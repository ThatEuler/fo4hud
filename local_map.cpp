#include "local_map.h"
#include "logging.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using FinishFn = uintptr_t (__attribute__((ms_abi)) *)();
using CreateFn = uintptr_t (__attribute__((ms_abi)) *)(uint8_t);
using RenderFn = uintptr_t (__attribute__((ms_abi)) *)(uintptr_t, uint8_t);
using DestroyFn = void (__attribute__((ms_abi)) *)(uintptr_t);
using FreeFn = void (__attribute__((ms_abi)) *)(uintptr_t, size_t);
using CaptureFn = int32_t (__attribute__((ms_abi)) *)(uintptr_t, uintptr_t, uintptr_t, void*);
using ImageFn = uintptr_t (__attribute__((ms_abi)) *)(void*, size_t, size_t, size_t);
using ConvertFn = int32_t (__attribute__((ms_abi)) *)(uintptr_t, uint32_t, uint32_t, float, void*);
using ReleaseFn = void (__attribute__((ms_abi)) *)(void*);
using BoundsFn = void (__attribute__((ms_abi)) *)(uintptr_t, const float*, const float*);
using SizeFn = void (__attribute__((ms_abi)) *)(uintptr_t, float, float);
using CenterFn = void (__attribute__((ms_abi)) *)(uintptr_t, const float*);
struct NativeApi {
    FinishFn finish = nullptr;
    CreateFn create = nullptr;
    RenderFn render = nullptr;
    DestroyFn destroy = nullptr;
    FreeFn free = nullptr;
    CaptureFn capture = nullptr;
    ImageFn image = nullptr;
    ConvertFn convert = nullptr;
    ReleaseFn release = nullptr;
    BoundsFn bounds = nullptr;
    SizeFn size = nullptr;
    CenterFn center = nullptr;
} api;
struct Request {
    uintptr_t player = 0, cell = 0;
    uint32_t cell_form_id = 0;
    Clock::time_point updated{};
};
std::mutex state_mutex, hook_mutex, lifecycle_mutex;
Request request;
std::shared_ptr<const Fo4LocalMapFrame> latest;
std::atomic<bool> enabled{false};
uintptr_t image_base = 0;
uint8_t* jump_island = nullptr;
uintptr_t patched_call = 0;
int32_t original_displacement = 0;
uint64_t sequence = 0;
Clock::time_point last_attempt{};
constexpr uintptr_t kRenderCall = 0xC383F6;
constexpr uintptr_t kRenderFinish = 0x21FA3B0;

bool read_memory(uintptr_t address, void* data, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    iovec local{data, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
}
template<class T> bool read_value(uintptr_t address, T& value) {
    return read_memory(address, &value, sizeof(value));
}

bool current_player(const Request& r, float position[3]) {
    uintptr_t player = 0, cell = 0, tes = 0, interior = 0;
    uint32_t id = 0;
    uint16_t flags = 0;
    uint8_t type = 0, loaded = 0;
    if (!r.player || !r.cell ||
        !read_value(image_base + 0x32DD370, player) || player != r.player ||
        !read_value(player + 0x1A, type) || type != 65 ||
        !read_value(player + 0xB8, cell) || cell != r.cell ||
        !read_value(cell + 0x14, id) || id != r.cell_form_id ||
        !read_value(cell + 0x40, flags) || !(flags & 1) ||
        !read_value(cell + 0x44, loaded) || loaded != 8 ||
        !read_value(image_base + 0x32DD158, tes) || !tes ||
        !read_value(tes + 88, interior) || interior != cell ||
        !read_memory(player + 0xD0, position, 3 * sizeof(float))) return false;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(position[i])) return false;
    return true;
}

bool same_floor(const Fo4LocalMapFrame& frame, const float position[3]) {
    return std::abs(frame.position[2] - position[2]) < 128.0f;
}

// Mirrors PipboyMapMenu::GetLocalMapCorners (ADAE70). The same NiCamera
// axes/frustum produced the captured pixels; no north-marker guess is needed.
bool camera_map(uintptr_t renderer, Fo4MapInfo& map) {
    uintptr_t camera = 0;
    float axes[12]{}, frustum[4]{};
    if (!read_value(renderer + 504, camera) || !camera ||
        !read_memory(camera + 128, axes, sizeof(axes)) ||
        !read_memory(camera + 352, frustum, sizeof(frustum))) return false;
    map.texture = "local";
    map.oriented = true;
    for (int i = 0; i < 3; ++i) {
        const float right = frustum[i == 1 ? 1 : 0];
        const float up = frustum[i == 2 ? 3 : 2];
        map.corners[i] = {axes[8] + axes[4] * right + axes[0] * up,
                          axes[9] + axes[5] * right + axes[1] * up};
    }
    return map.valid();
}

struct NativeImage {
    size_t width, height;
    uint32_t format, padding;
    size_t row_pitch, slice_pitch;
    uintptr_t pixels;
};
static_assert(sizeof(NativeImage) == 48);
struct ScratchImage {
    uint64_t storage[12]{};
    ~ScratchImage() { api.release(storage); }
};

bool copy_pixels(uintptr_t address, Fo4LocalMapFrame& frame) {
    NativeImage img{};
    if (!read_value(address, img) || !img.width || !img.height ||
        img.width > 4096 || img.height > 4096 ||
        img.row_pitch < img.width * 4 || img.row_pitch > 65536 ||
        img.slice_pitch < img.row_pitch * img.height ||
        (img.format != 28 && img.format != 29) || !img.pixels) return false;
    frame.width = int(img.width);
    frame.height = int(img.height);
    frame.pixels.resize(img.width * img.height * 4);
    for (size_t y = 0; y < img.height; ++y)
        if (!read_memory(img.pixels + y * img.row_pitch,
                         frame.pixels.data() + y * img.width * 4, img.width * 4)) return false;
    return true;
}

bool capture_pixels(Fo4LocalMapFrame& frame) {
    // BSRenderTargetManager maps logical LocalMap=19 to a graphics target.
    // This is the exact source used by OutputLocalMapPictures, without files.
    uint32_t target = 0;
    uintptr_t texture = 0, renderer = 0, device = 0, context = 0;
    if (!read_value(image_base + 0x2F4D750 + 3572 + 19 * 4, target) || target >= 128 ||
        !read_value(image_base + 0x3A25D00 + 2664 + 48 * target, texture) || !texture ||
        !read_value(image_base + 0x38E13A0, renderer) || !renderer ||
        !read_value(renderer + 72, device) || !device ||
        !read_value(renderer + 80, context) || !context) return false;
    ScratchImage captured, converted;
    if (api.capture(device, context, texture, captured.storage) < 0) return false;
    uintptr_t source = api.image(captured.storage, 0, 0, 0);
    NativeImage img{};
    if (!source || !read_value(source, img)) return false;
    if (img.format != 28 && img.format != 29) {
        if (api.convert(source, 28, 0, 0.5f, converted.storage) < 0) return false;
        source = api.image(converted.storage, 0, 0, 0);
    }
    return source && copy_pixels(source, frame);
}

struct LocalRenderer {
    uintptr_t address = 0;
    ~LocalRenderer() {
        if (address) { api.destroy(address); api.free(address, 688); }
    }
};

// TESObjectCELL::Get3D (4C9110) -> loaded data +448 -> scene root.
// NiAVObject's world bound at +176 is a sphere encompassing the loaded cell.
// Configure only our private camera; never change the player's zoom or cell.
bool frame_interior(uintptr_t renderer, uintptr_t cell) {
    uintptr_t loaded = 0, root = 0;
    float bound[4]{};
    if (!read_value(cell + 208, loaded) || !loaded ||
        !read_value(loaded + 448, root) || !root ||
        !read_memory(root + 176, bound, sizeof(bound))) return false;
    for (float value : bound) if (!std::isfinite(value)) return false;
    if (bound[3] <= 0 || bound[3] > 500000) return false;
    const float radius = std::max(256.0f, bound[3] * 1.05f);
    float minimum[3], maximum[3];
    for (int i = 0; i < 3; ++i) {
        minimum[i] = bound[i] - radius;
        maximum[i] = bound[i] + radius;
    }
    // Native local-map cameras use a 16:9 frustum. Fit the short dimension
    // too, so distant rooms do not depend on the player's current position.
    api.size(renderer + 416, radius * 2 / 0.5625f, radius * 2);
    api.center(renderer + 416, bound);
    api.bounds(renderer + 416, maximum, minimum);
    return true;
}

struct RadarFogScope {
    uintptr_t address = image_base + 0x2EDDBE0;
    uint8_t saved = 0;
    bool ready = false;
    RadarFogScope() {
        // Only LocalMap::Render and the fog console command read this flag
        // (371B5F / 5F2DD0). The render thread cannot overlap its own Pip-Boy
        // pass. Restore immediately afterward; exploration data is untouched.
        if (read_value(address, saved) && saved <= 1) {
            *reinterpret_cast<volatile uint8_t*>(address) = 0;
            ready = true;
        }
    }
    ~RadarFogScope() {
        if (ready) *reinterpret_cast<volatile uint8_t*>(address) = saved;
    }
};

void render_requested_map() {
    const auto now = Clock::now();
    Request r;
    std::shared_ptr<const Fo4LocalMapFrame> previous;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        r = request;
        previous = latest;
    }
    if (!r.player || now - r.updated > std::chrono::seconds(1) ||
        now - last_attempt < std::chrono::milliseconds(750)) return;
    float position[3]{};
    if (!current_player(r, position) || fo4hud_game_menu_cursor_active()) return;
    if (previous && previous->cell == r.cell && previous->cell_form_id == r.cell_form_id &&
        same_floor(*previous, position) &&
        std::hypot(previous->position[0] - position[0], previous->position[1] - position[1]) < 256 &&
        now - last_attempt < std::chrono::seconds(3)) return;
    last_attempt = now;
    auto frame = std::make_shared<Fo4LocalMapFrame>();
    frame->cell = r.cell;
    frame->cell_form_id = r.cell_form_id;
    std::copy(position, position + 3, frame->position);
    bool okay = false;
    {
        // A private renderer leaves the Pip-Boy's camera, pan and zoom alone.
        // Constructor configures loaded bounds, aspect and cell north marker.
        LocalRenderer renderer{api.create(0)};
        if (renderer.address) {
            // Bounds may be temporarily unavailable while geometry attaches;
            // in that case retain the native framing and retry next capture.
            (void)frame_interior(renderer.address, r.cell);
            {
                RadarFogScope fog;
                if (fog.ready) {
                    api.render(renderer.address, 0);
                    okay = camera_map(renderer.address, frame->map);
                }
            }
            if (okay) okay = capture_pixels(*frame);
        }
    }
    float confirmed[3]{};
    if (!current_player(r, confirmed) || !same_floor(*frame, confirmed)) return;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (request.cell != r.cell || request.cell_form_id != r.cell_form_id ||
        Clock::now() - request.updated > std::chrono::seconds(1)) return;
    if (okay) {
        frame->sequence = ++sequence;
        latest = frame;
        if (!previous || previous->cell != r.cell)
            fo4hud_log("radar", "local map cell=%08X captured %dx%d", r.cell_form_id, frame->width, frame->height);
    } else if (!previous) {
        // Attempts are throttled; no unbounded per-frame log on unsupported data.
        static auto last_log = Clock::time_point{};
        if (now - last_log > std::chrono::seconds(10)) {
            last_log = now;
            fo4hud_log("radar", "local map capture unavailable cell=%08X", r.cell_form_id);
        }
    }
}

uintptr_t __attribute__((ms_abi)) render_stage_hook() {
    std::lock_guard<std::mutex> lock(hook_mutex);
    if (enabled.load(std::memory_order_acquire)) {
        try { render_requested_map(); }
        catch (...) { fo4hud_log("radar", "local map capture failed"); }
    }
    // The original stage resets engine render state after the local-map pass.
    return api.finish();
}

bool patch_call(int32_t displacement) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (!patched_call || page_size <= 0) return false;
    const uintptr_t page = patched_call & ~(uintptr_t(page_size) - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    std::memcpy(reinterpret_cast<void*>(patched_call + 1), &displacement, sizeof(displacement));
    __builtin___clear_cache(reinterpret_cast<char*>(patched_call), reinterpret_cast<char*>(patched_call + 5));
    return mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_EXEC) == 0;
}

bool verify_code(uintptr_t image) {
    struct Signature { uintptr_t rva; std::vector<uint8_t> bytes; };
    const Signature signatures[] = {
        {kRenderCall, {0xE8, 0xB5, 0x1F, 0x5C, 0x01}},
        {0xADB360, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57}},
        {0x371350, {0x48, 0x8B, 0xC4, 0x88, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x53}},
        {0x3712D0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}},
        {0x184DF90, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}}
    };
    for (const auto& s : signatures) {
        std::vector<uint8_t> bytes(s.bytes.size());
        if (!read_memory(image + s.rva, bytes.data(), bytes.size()) || bytes != s.bytes) return false;
    }
    return true;
}
}

bool fo4hud_local_map_install() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (enabled.load()) return true;
    const uintptr_t image = fo4hud_supported_game_image();
    if (!image || !verify_code(image)) return false;
    image_base = image;
    api = {reinterpret_cast<FinishFn>(image + kRenderFinish),
           reinterpret_cast<CreateFn>(image + 0xADB360),
           reinterpret_cast<RenderFn>(image + 0x371350),
           reinterpret_cast<DestroyFn>(image + 0x3712D0),
           reinterpret_cast<FreeFn>(image + 0x22BE6D8),
           reinterpret_cast<CaptureFn>(image + 0x184DF90),
           reinterpret_cast<ImageFn>(image + 0x1846EA0),
           reinterpret_cast<ConvertFn>(image + 0x18473C0),
           reinterpret_cast<ReleaseFn>(image + 0x1846DD0),
           reinterpret_cast<BoundsFn>(image + 0x370FE0),
           reinterpret_cast<SizeFn>(image + 0x371020),
           reinterpret_cast<CenterFn>(image + 0x370D90)};
    patched_call = image + kRenderCall;
    if (!read_value(patched_call + 1, original_displacement)) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    if (!jump_island) {
        auto try_page = [&](uintptr_t candidate) {
            if (jump_island) return;
            candidate &= ~(uintptr_t(page_size) - 1);
            const int64_t delta = int64_t(candidate) - int64_t(patched_call + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) return;
            void* p = mmap(reinterpret_cast<void*>(candidate), page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) jump_island = static_cast<uint8_t*>(p);
        };
        // Wine reserves large heap arenas above the PE. Probe the small gaps
        // immediately around it first, as the shot-redirect hook does.
        for (int i = 0; i < 64 && !jump_island; ++i) {
            try_page(image + 0x425A000 + uintptr_t(i) * page_size);
            try_page(image - uintptr_t(i + 1) * page_size);
        }
        for (int i = 0; i < 96 && !jump_island; ++i)
            try_page(image + 0x30000000 + uintptr_t(i) * 0x1000000);
        if (!jump_island) {
            fo4hud_log("radar", "local map hook: no free executable page within CALL range");
            return false;
        }
        const uintptr_t callback = reinterpret_cast<uintptr_t>(render_stage_hook);
        const uint8_t stub[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        std::memcpy(jump_island, stub, sizeof(stub));
        std::memcpy(jump_island + 2, &callback, sizeof(callback));
        if (mprotect(jump_island, page_size, PROT_READ | PROT_EXEC)) return false;
    }
    const int64_t delta = int64_t(reinterpret_cast<uintptr_t>(jump_island)) - int64_t(patched_call + 5);
    if (delta < INT32_MIN || delta > INT32_MAX || !patch_call(int32_t(delta))) return false;
    enabled.store(true, std::memory_order_release);
    fo4hud_log("radar", "interior local map renderer installed");
    return true;
}

void fo4hud_local_map_uninstall() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
    enabled.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> active(hook_mutex);
    if (patched_call) {
        int32_t current = 0;
        if (read_value(patched_call + 1, current) && jump_island &&
            patched_call + 5 + current == reinterpret_cast<uintptr_t>(jump_island)) {
            if (!patch_call(original_displacement))
                fo4hud_log("radar", "local map hook restore failed; retaining inactive jump island");
        }
    }
    // Retain the tiny island: another thread could already have fetched CALL.
    // A late callback remains valid and forwards straight to the original stage.
    std::lock_guard<std::mutex> state(state_mutex);
    request = {};
    latest.reset();
    last_attempt = {};
}

std::shared_ptr<const Fo4LocalMapFrame> fo4hud_local_map_request(const Fo4EntitySnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!enabled.load(std::memory_order_acquire) || snapshot.status != "Ready" || !snapshot.cell_is_interior ||
        !fo4hud_player_in_snapshot_cell(snapshot)) {
        request = {};
        latest.reset();
        return {};
    }
    request = {snapshot.player_address, snapshot.cell_address, snapshot.cell_form_id, Clock::now()};
    float position[3]{};
    if (!current_player(request, position)) { latest.reset(); return {}; }
    if (latest && (latest->cell != snapshot.cell_address || latest->cell_form_id != snapshot.cell_form_id)) latest.reset();
    return latest;
}
