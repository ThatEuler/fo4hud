#include "game_workshop.h"
#include "logging.h"

#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace {
// Fallout4.exe 1.11.240.0. These are the functions used by the game's
// ScrapAll command and F4SE's ObjectReference.Scrap implementation.
constexpr uintptr_t kFindWorkshopRva = 0x0385720;
constexpr uintptr_t kCanScrapRva = 0x0384910;
constexpr uintptr_t kInWorkshopRva = 0x0384810;
constexpr uintptr_t kScrapReferenceRva = 0x0393F30;
constexpr uintptr_t kContextInitRva = 0x03897F0;
constexpr uintptr_t kMakeHandleRva = 0x05194C0;
constexpr uintptr_t kResolveHandleRva = 0x022D060;
constexpr uintptr_t kCurrentWorkshopRva = 0x30F7698;
constexpr uintptr_t kTesPointerRva = 0x32DD158;
constexpr uintptr_t kTesVtableRva = 0x2473B58;
constexpr uintptr_t kGridVtableRva = 0x247C7F8;
constexpr uintptr_t kImageSize = 0x425A000;
constexpr size_t kMaxCellRefs = 16384;
constexpr size_t kMaxTotalRefs = 50000;

using FindWorkshopFn = void* (__attribute__((ms_abi)) *)(void*);
using CanScrapFn = bool (__attribute__((ms_abi)) *)(void*, bool);
using InWorkshopFn = bool (__attribute__((ms_abi)) *)(void*, void*);
using ContextInitFn = void* (__attribute__((ms_abi)) *)(void*, void*);
using MakeHandleFn = uint32_t* (__attribute__((ms_abi)) *)(void*, uint32_t*);
using ResolveHandleFn = bool (__attribute__((ms_abi)) *)(uint32_t*, uintptr_t*);
using ScrapReferenceFn = void (__attribute__((ms_abi)) *)(void*, uintptr_t*, void*);

std::mutex preview_mutex;
std::mutex work_mutex;
Fo4WorkshopScrapPreview preview;
std::atomic<bool> preview_requested{false};
std::atomic<uint64_t> scrap_requested{0};
std::atomic<bool> scrap_cancelled{false};
std::vector<uint8_t> pending_selection;
std::vector<Fo4ScrapReference> work;
size_t work_cursor = 0;
uintptr_t work_workshop = 0;
uint32_t work_workshop_form = 0;
uintptr_t work_player_cell = 0;
bool functions_checked = false;
bool functions_valid = false;

bool read_memory(uintptr_t address, void* destination, size_t size) {
    if (!address || address > std::numeric_limits<uintptr_t>::max() - size) return false;
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

template <typename T> bool read_value(uintptr_t address, T& value) {
    return read_memory(address, &value, sizeof(value));
}

bool prolog_matches(uintptr_t address, const uint8_t* expected, size_t size) {
    uint8_t bytes[8]{};
    return size <= sizeof(bytes) && read_memory(address, bytes, size) &&
           std::memcmp(bytes, expected, size) == 0;
}

std::string base_name(uintptr_t image, uintptr_t base) {
    uintptr_t vtable = 0, function = 0;
    if (!read_value(base, vtable) ||
        !read_value(vtable + 0x35 * sizeof(void*), function) ||
        function < image || function >= image + kImageSize) return {};
    using NameFn = const char* (__attribute__((ms_abi)) *)(void*);
    const char* pointer = reinterpret_cast<NameFn>(function)(reinterpret_cast<void*>(base));
    if (!pointer) return {};
    char bytes[96]{};
    if (!read_memory(reinterpret_cast<uintptr_t>(pointer), bytes, sizeof(bytes))) {
        if (!read_memory(reinterpret_cast<uintptr_t>(pointer), bytes, 32)) return {};
    }
    const size_t length = strnlen(bytes, sizeof(bytes));
    if (!length || length == sizeof(bytes)) return {};
    std::string result(bytes, length);
    for (char& ch : result)
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    return result;
}

bool functions_match(uintptr_t image) {
    if (functions_checked) return functions_valid;
    functions_checked = true;
    const uint8_t find[] = {0x4C, 0x8B, 0xDC, 0x56};
    const uint8_t can[] = {0x48, 0x89, 0x5C, 0x24, 0x18};
    const uint8_t area[] = {0x4C, 0x8B, 0x8A, 0xB8};
    const uint8_t scrap[] = {0x48, 0x8B, 0xC4, 0x4C};
    const uint8_t context[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
    const uint8_t handle[] = {0x40, 0x53, 0x48, 0x83};
    const uint8_t resolve[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
    functions_valid = prolog_matches(image + kFindWorkshopRva, find, sizeof(find)) &&
        prolog_matches(image + kCanScrapRva, can, sizeof(can)) &&
        prolog_matches(image + kInWorkshopRva, area, sizeof(area)) &&
        prolog_matches(image + kScrapReferenceRva, scrap, sizeof(scrap)) &&
        prolog_matches(image + kContextInitRva, context, sizeof(context)) &&
        prolog_matches(image + kMakeHandleRva, handle, sizeof(handle)) &&
        prolog_matches(image + kResolveHandleRva, resolve, sizeof(resolve));
    if (!functions_valid) fo4hud_log("workshop", "workshop functions changed; scrap disabled");
    return functions_valid;
}

bool collect_cell(uintptr_t cell, uintptr_t world, bool interior,
                  std::unordered_set<uintptr_t>& seen,
                  std::vector<Fo4ScrapReference>& out) {
    uint8_t type = 0;
    uint16_t flags = 0;
    uintptr_t cell_world = 0, entries = 0;
    uint32_t capacity = 0, count = 0;
    if (!read_value(cell + 0x1A, type) || type != 63 ||
        !read_value(cell + 0x40, flags) || bool(flags & 1) != interior ||
        (!interior && (!read_value(cell + 0xC8, cell_world) || cell_world != world)) ||
        !read_value(cell + 0x70, entries) || !read_value(cell + 0x78, capacity) ||
        !read_value(cell + 0x80, count) || count > capacity || count > kMaxCellRefs ||
        (count && !entries) || out.size() + count > kMaxTotalRefs) return false;
    std::vector<uintptr_t> refs(count);
    if (count && !read_memory(entries, refs.data(), count * sizeof(uintptr_t))) return false;
    uintptr_t current_entries = 0;
    uint32_t current_count = 0;
    if (!read_value(cell + 0x70, current_entries) ||
        !read_value(cell + 0x80, current_count) ||
        current_entries != entries || current_count != count) return false;
    for (uintptr_t ref : refs) {
        if (!ref || !seen.insert(ref).second) continue;
        Fo4ScrapReference candidate;
        candidate.address = ref;
        candidate.cell = cell;
        uint8_t ref_type = 0;
        uintptr_t actual_cell = 0, base = 0;
        if (!read_value(ref + 0x14, candidate.form_id) ||
            !read_value(ref + 0x1A, ref_type) || ref_type != 64 ||
            !read_value(ref + 0xB8, actual_cell) || actual_cell != cell ||
            !read_value(ref + 0xE0, base) || !base ||
            !read_value(base + 0x14, candidate.base_form_id) ||
            !read_memory(ref + 0xD0, candidate.position, sizeof(candidate.position))) continue;
        out.push_back(candidate);
    }
    return true;
}

bool collect_loaded_refs(uintptr_t image, uintptr_t player_cell,
                         std::vector<Fo4ScrapReference>& out) {
    uint16_t flags = 0;
    uintptr_t world = 0;
    if (!read_value(player_cell + 0x40, flags)) return false;
    const bool interior = (flags & 1) != 0;
    if (!interior && (!read_value(player_cell + 0xC8, world) || !world)) return false;
    std::unordered_set<uintptr_t> seen;
    if (!collect_cell(player_cell, world, interior, seen, out)) return false;
    if (interior) return true;

    uintptr_t tes = 0, tes_vtable = 0, grid = 0, grid_vtable = 0, cells = 0;
    uint32_t dimension = 0;
    if (!read_value(image + kTesPointerRva, tes) || !tes ||
        !read_value(tes, tes_vtable) || tes_vtable != image + kTesVtableRva ||
        !read_value(tes + 0x18, grid) || !grid ||
        !read_value(grid, grid_vtable) || grid_vtable != image + kGridVtableRva ||
        !read_value(grid + 0x10, dimension) || dimension < 1 || dimension > 11 ||
        !read_value(grid + 0x18, cells) || !cells) return false;
    std::vector<uintptr_t> loaded(dimension * dimension);
    if (!read_memory(cells, loaded.data(), loaded.size() * sizeof(uintptr_t))) return false;
    std::unordered_set<uintptr_t> visited{player_cell};
    for (uintptr_t cell : loaded) {
        if (!cell || !visited.insert(cell).second) continue;
        uint8_t type = 0;
        uint16_t cell_flags = 0;
        uintptr_t cell_world = 0;
        if (!read_value(cell + 0x1A, type) || type != 63 ||
            !read_value(cell + 0x40, cell_flags) || (cell_flags & 1) ||
            !read_value(cell + 0xC8, cell_world) || cell_world != world) continue;
        if (!collect_cell(cell, world, false, seen, out)) return false;
    }
    return true;
}

bool valid_candidate(uintptr_t image, uintptr_t workshop,
                     const Fo4ScrapReference& candidate) {
    if (!candidate.address || candidate.address == workshop) return false;
    uint32_t form_id = 0, flags = 0;
    uint8_t type = 0;
    uintptr_t cell = 0;
    if (!read_value(candidate.address + 0x14, form_id) ||
        form_id != candidate.form_id ||
        !read_value(candidate.address + 0x1A, type) || type != 64 ||
        !read_value(candidate.address + 0x10, flags) || (flags & ((1u << 5) | (1u << 11))) ||
        !read_value(candidate.address + 0xB8, cell) || cell != candidate.cell) return false;
    const auto can_scrap = reinterpret_cast<CanScrapFn>(image + kCanScrapRva);
    const auto in_workshop = reinterpret_cast<InWorkshopFn>(image + kInWorkshopRva);
    return can_scrap(reinterpret_cast<void*>(candidate.address), false) &&
           in_workshop(reinterpret_cast<void*>(workshop),
                       reinterpret_cast<void*>(candidate.address));
}

struct CurrentWorkshopScope {
    uint32_t* slot = nullptr;
    uint32_t previous = 0;
    bool active = false;

    CurrentWorkshopScope(uintptr_t image, uintptr_t workshop) {
        uint32_t handle = 0;
        reinterpret_cast<MakeHandleFn>(image + kMakeHandleRva)(
            reinterpret_cast<void*>(workshop), &handle);
        if (!handle) return;
        slot = reinterpret_cast<uint32_t*>(image + kCurrentWorkshopRva);
        previous = *slot;
        *slot = handle;
        active = true;
    }
    ~CurrentWorkshopScope() { if (active) *slot = previous; }
};

void publish(Fo4WorkshopScrapPreview next) {
    std::lock_guard<std::mutex> lock(preview_mutex);
    next.generation = preview.generation + 1;
    preview = std::move(next);
}

void build_preview(uintptr_t image, void* player) {
    Fo4WorkshopScrapPreview result;
    if (!functions_match(image)) {
        result.status = "Workshop functions are unavailable for this build";
        publish(std::move(result));
        return;
    }
    uintptr_t player_cell = 0;
    if (!read_value(reinterpret_cast<uintptr_t>(player) + 0xB8, player_cell) || !player_cell) {
        result.status = "Player cell is unavailable";
        publish(std::move(result));
        return;
    }
    auto find = reinterpret_cast<FindWorkshopFn>(image + kFindWorkshopRva);
    result.workshop = reinterpret_cast<uintptr_t>(find(player));
    result.player_cell = player_cell;
    auto within = reinterpret_cast<InWorkshopFn>(image + kInWorkshopRva);
    if (!result.workshop || !read_value(result.workshop + 0x14, result.workshop_form_id) ||
        !within(reinterpret_cast<void*>(result.workshop), player)) {
        result.status = "Stand inside a settlement's buildable area";
        publish(std::move(result));
        return;
    }
    std::vector<Fo4ScrapReference> loaded;
    if (!collect_loaded_refs(image, player_cell, loaded)) {
        result.status = "Loaded cell references changed; retry the preview";
        publish(std::move(result));
        return;
    }
    {
        CurrentWorkshopScope current(image, result.workshop);
        if (!current.active) {
            result.status = "Could not resolve workshop handle";
            publish(std::move(result));
            return;
        }
        for (auto& candidate : loaded)
            if (valid_candidate(image, result.workshop, candidate)) {
                uintptr_t base = 0;
                if (read_value(candidate.address + 0xE0, base) && base)
                    candidate.name = base_name(image, base);
                result.references.push_back(std::move(candidate));
            }
    }
    uintptr_t current_cell = 0;
    uint32_t current_workshop_form = 0;
    if (!read_value(reinterpret_cast<uintptr_t>(player) + 0xB8, current_cell) ||
        current_cell != player_cell ||
        !read_value(result.workshop + 0x14, current_workshop_form) ||
        current_workshop_form != result.workshop_form_id) {
        result.references.clear();
        result.status = "Player or workshop changed during scan; retry";
        publish(std::move(result));
        return;
    }
    result.ready = true;
    result.status = result.references.empty() ? "No scrappable references found" :
                    "Preview ready";
    fo4hud_log("workshop", "preview workshop=%08X loaded=%zu scrappable=%zu",
               result.workshop_form_id, loaded.size(), result.references.size());
    publish(std::move(result));
}

void release_reference(uintptr_t ref) {
    if (!ref) return;
    auto* count = reinterpret_cast<uint32_t*>(ref + 40);
    if ((__atomic_sub_fetch(count, 1, __ATOMIC_ACQ_REL) & 0x3FF) == 0) {
        uintptr_t vtable = 0, destroy = 0;
        if (read_value(ref + 32, vtable) && read_value(vtable + 8, destroy)) {
            using DestroyFn = void (__attribute__((ms_abi)) *)(void*);
            reinterpret_cast<DestroyFn>(destroy)(reinterpret_cast<void*>(ref + 32));
        }
    }
}

bool scrap_one(uintptr_t image, uintptr_t player, uintptr_t workshop,
               const Fo4ScrapReference& candidate) {
    auto make_handle = reinterpret_cast<MakeHandleFn>(image + kMakeHandleRva);
    auto resolve = reinterpret_cast<ResolveHandleFn>(image + kResolveHandleRva);
    uint32_t workshop_handle = 0, ref_handle = 0;
    make_handle(reinterpret_cast<void*>(workshop), &workshop_handle);
    make_handle(reinterpret_cast<void*>(candidate.address), &ref_handle);
    if (!workshop_handle || !ref_handle) return false;
    uintptr_t smart_ref = 0;
    if (!resolve(&ref_handle, &smart_ref) || smart_ref != candidate.address) {
        release_reference(smart_ref);
        return false;
    }
    uint32_t* current_workshop = reinterpret_cast<uint32_t*>(image + kCurrentWorkshopRva);
    const uint32_t prior = *current_workshop;
    alignas(16) uint8_t context[0x30]{};
    reinterpret_cast<ContextInitFn>(image + kContextInitRva)(
        context, reinterpret_cast<void*>(player));
    *current_workshop = workshop_handle;
    reinterpret_cast<ScrapReferenceFn>(image + kScrapReferenceRva)(context, &smart_ref, nullptr);
    *current_workshop = prior;
    release_reference(smart_ref);
    return true;
}

void process_scrap(uintptr_t image, uintptr_t player) {
    if (work.empty()) return;
    if (scrap_cancelled.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(preview_mutex);
        preview.status = "Scrap pass stopped";
        preview.running = false;
        preview.ready = false;
        work.clear();
        return;
    }
    uintptr_t player_cell = 0;
    uint32_t workshop_form = 0;
    auto within = reinterpret_cast<InWorkshopFn>(image + kInWorkshopRva);
    if (!read_value(player + 0xB8, player_cell) || player_cell != work_player_cell ||
        !read_value(work_workshop + 0x14, workshop_form) ||
        workshop_form != work_workshop_form ||
        !within(reinterpret_cast<void*>(work_workshop), reinterpret_cast<void*>(player))) {
        std::lock_guard<std::mutex> lock(preview_mutex);
        preview.status = "Stopped: player left the settlement";
        preview.running = false;
        work.clear();
        return;
    }
    CurrentWorkshopScope current(image, work_workshop);
    if (!current.active) {
        std::lock_guard<std::mutex> lock(preview_mutex);
        preview.status = "Stopped: workshop handle is unavailable";
        preview.running = false;
        work.clear();
        return;
    }
    for (int batch = 0; batch < 4 && work_cursor < work.size(); ++batch, ++work_cursor) {
        const auto& candidate = work[work_cursor];
        bool applied = valid_candidate(image, work_workshop, candidate) &&
                       scrap_one(image, player, work_workshop, candidate);
        {
            std::lock_guard<std::mutex> lock(preview_mutex);
            if (applied) ++preview.processed;
            else ++preview.skipped;
        }
        if (applied) fo4hud_log("workshop", "scrap call ref=%08X base=%08X",
                                 candidate.form_id, candidate.base_form_id);
    }
    if (work_cursor == work.size()) {
        std::lock_guard<std::mutex> lock(preview_mutex);
        preview.running = false;
        preview.ready = false;
        preview.status = "Scrap pass complete; refresh preview to check remaining objects";
        fo4hud_log("workshop", "scrap pass complete workshop=%08X processed=%zu skipped=%zu",
                   work_workshop_form, preview.processed, preview.skipped);
        work.clear();
    }
}
} // namespace

void fo4hud_workshop_request_preview() {
    preview_requested.store(true, std::memory_order_release);
}

Fo4WorkshopScrapPreview fo4hud_workshop_scrap_preview() {
    std::lock_guard<std::mutex> lock(preview_mutex);
    return preview;
}

bool fo4hud_workshop_begin_scrap(uint64_t generation,
                                const std::vector<uint8_t>& selected) {
    std::lock_guard<std::mutex> lock(preview_mutex);
    if (!preview.ready || preview.running || preview.references.empty() ||
        preview.generation != generation ||
        selected.size() != preview.references.size()) return false;
    const size_t count = std::count_if(selected.begin(), selected.end(),
                                      [](uint8_t value) { return value != 0; });
    if (!count) return false;
    pending_selection = selected;
    preview.running = true;
    preview.status = "Scrapping selected settlement objects";
    preview.processed = 0;
    preview.skipped = 0;
    preview.selected_count = count;
    scrap_cancelled.store(false, std::memory_order_release);
    scrap_requested.store(generation, std::memory_order_release);
    return true;
}

void fo4hud_workshop_cancel_scrap() {
    scrap_cancelled.store(true, std::memory_order_release);
}

void fo4hud_workshop_tick(uintptr_t image, void* player) {
    if (!image || !player) return;
    std::lock_guard<std::mutex> work_lock(work_mutex);
    if (!work.empty()) {
        process_scrap(image, reinterpret_cast<uintptr_t>(player));
        return;
    }
    const uint64_t requested = scrap_requested.exchange(0, std::memory_order_acq_rel);
    if (requested) {
        std::lock_guard<std::mutex> lock(preview_mutex);
        if (preview.generation == requested && preview.running &&
            pending_selection.size() == preview.references.size() &&
            functions_match(image)) {
            work.clear();
            work.reserve(preview.selected_count);
            for (size_t i = 0; i < preview.references.size(); ++i)
                if (pending_selection[i]) work.push_back(preview.references[i]);
            work_cursor = 0;
            work_workshop = preview.workshop;
            work_workshop_form = preview.workshop_form_id;
            work_player_cell = preview.player_cell;
        } else {
            preview.running = false;
            preview.status = "Scrap request could not start; refresh preview";
        }
        pending_selection.clear();
        return;
    }
    if (preview_requested.exchange(false, std::memory_order_acq_rel)) {
        {
            std::lock_guard<std::mutex> lock(preview_mutex);
            if (preview.running) return;
            preview.status = "Scanning loaded settlement references...";
            preview.ready = false;
        }
        build_preview(image, player);
    }
}

void fo4hud_workshop_reset() {
    std::lock_guard<std::mutex> work_lock(work_mutex);
    preview_requested.store(false, std::memory_order_release);
    scrap_requested.store(0, std::memory_order_release);
    scrap_cancelled.store(false, std::memory_order_release);
    work.clear();
    std::lock_guard<std::mutex> lock(preview_mutex);
    pending_selection.clear();
    preview = {};
}
