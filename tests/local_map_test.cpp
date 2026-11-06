// Exercise the real request/render/readback pipeline with a small fake engine.
// No native functions are called in a game, and all allocations are local.
#include "../local_map.cpp"
#include <cassert>
#include <filesystem>

namespace {
bool menu_open = false;
int creates = 0, renders = 0, destroys = 0, frees = 0, captures = 0, releases = 0, converts = 0, finishes = 0;
bool capture_ok = true, change_cell_during_render = false;
bool throw_during_render = false;
float framed_center[3]{}, framed_maximum[3]{}, framed_minimum[3]{}, framed_width = 0, framed_height = 0;
std::array<unsigned char, 688> renderer_data{};
std::array<unsigned char, 416> camera_data{};
std::array<unsigned char, 0xE0> player_data{}, cell_data{}, tes_data{}, graphics_data{};
std::array<unsigned char, 32> pixel_data{
    1,2,3,255, 4,5,6,255, 7,8,9,255, 99,99,99,99,
    10,11,12,255, 13,14,15,255, 16,17,18,255, 99,99,99,99};
NativeImage native_image{3, 2, 28, 0, 16, 32, reinterpret_cast<uintptr_t>(pixel_data.data())};
template<class T> void put(uintptr_t address, const T& value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
template<class T, size_t N> void put(std::array<unsigned char, N>& data, size_t off, T value) {
    put(reinterpret_cast<uintptr_t>(data.data()) + off, value);
}
uintptr_t __attribute__((ms_abi)) mock_create(uint8_t mode) { assert(mode == 0); ++creates; return reinterpret_cast<uintptr_t>(renderer_data.data()); }
uintptr_t __attribute__((ms_abi)) mock_render(uintptr_t r, uint8_t image) {
    assert(r == reinterpret_cast<uintptr_t>(renderer_data.data()) && image == 0);
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 0); // Radar pass has no exploration mask.
    ++renders;
    if (throw_during_render) throw std::runtime_error("render failure");
    if (change_cell_during_render) put(player_data, 0xB8, uintptr_t(0));
    return 0;
}
void __attribute__((ms_abi)) mock_destroy(uintptr_t r) { assert(r == reinterpret_cast<uintptr_t>(renderer_data.data())); ++destroys; }
void __attribute__((ms_abi)) mock_free(uintptr_t r, size_t size) { assert(r == reinterpret_cast<uintptr_t>(renderer_data.data()) && size == 688); ++frees; }
int32_t __attribute__((ms_abi)) mock_capture(uintptr_t dev, uintptr_t ctx, uintptr_t tex, void* scratch) {
    assert(dev == 0x1234 && ctx == 0x5678 && tex == 0x9ABC); ++captures;
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 1); // Pip-Boy fog was already restored.
    static_cast<uint64_t*>(scratch)[0] = 1;
    return capture_ok ? 0 : -1;
}
uintptr_t __attribute__((ms_abi)) mock_image(void* scratch, size_t a, size_t b, size_t c) {
    assert(a == 0 && b == 0 && c == 0 && static_cast<uint64_t*>(scratch)[0]);
    return reinterpret_cast<uintptr_t>(&native_image);
}
int32_t __attribute__((ms_abi)) mock_convert(uintptr_t img, uint32_t format, uint32_t filter, float threshold, void* out) {
    assert(img == reinterpret_cast<uintptr_t>(&native_image) && format == 28 && filter == 0 && threshold == 0.5f);
    ++converts; native_image.format = 28; static_cast<uint64_t*>(out)[0] = 2; return 0;
}
void __attribute__((ms_abi)) mock_release(void*) { ++releases; }
uintptr_t __attribute__((ms_abi)) mock_finish() { ++finishes; return 0xACDC; }
void __attribute__((ms_abi)) mock_bounds(uintptr_t camera, const float* maximum, const float* minimum) {
    assert(camera == reinterpret_cast<uintptr_t>(renderer_data.data()) + 416);
    std::copy(maximum, maximum + 3, framed_maximum);
    std::copy(minimum, minimum + 3, framed_minimum);
}
void __attribute__((ms_abi)) mock_size(uintptr_t camera, float width, float height) {
    assert(camera == reinterpret_cast<uintptr_t>(renderer_data.data()) + 416);
    framed_width = width; framed_height = height;
}
void __attribute__((ms_abi)) mock_center(uintptr_t camera, const float* center) {
    assert(camera == reinterpret_cast<uintptr_t>(renderer_data.data()) + 416);
    std::copy(center, center + 3, framed_center);
}
void fresh_attempt() { last_attempt = Clock::now() - std::chrono::seconds(4); }
}

uintptr_t fo4hud_supported_game_image() { return image_base; }
bool fo4hud_game_menu_cursor_active() { return menu_open; }
bool fo4hud_player_in_snapshot_cell(const Fo4EntitySnapshot& s) {
    uintptr_t cell = 0;
    return read_value(s.player_address + 0xB8, cell) && cell == s.cell_address;
}

int main() {
    char state_directory[] = "/tmp/fo4hud-local-map-test-XXXXXX";
    assert(mkdtemp(state_directory));
    assert(setenv("XDG_STATE_HOME", state_directory, 1) == 0);
    constexpr size_t image_size = 0x4000000;
    void* mapped = mmap(nullptr, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapped != MAP_FAILED);
    image_base = reinterpret_cast<uintptr_t>(mapped);
    api = {mock_finish, mock_create, mock_render, mock_destroy, mock_free,
           mock_capture, mock_image, mock_convert, mock_release, mock_bounds, mock_size, mock_center};
    put(image_base + 0x2EDDBE0, uint8_t(1));
    {
        RadarFogScope fog;
        assert(fog.ready && *reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 0);
    }
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 1);
    put(image_base + 0x2EDDBE0, uint8_t(0));
    { RadarFogScope fog; assert(fog.ready); }
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 0); // Preserve an already-disabled setting too.
    put(image_base + 0x2EDDBE0, uint8_t(1));
    enabled.store(true);
    const uintptr_t player = reinterpret_cast<uintptr_t>(player_data.data());
    const uintptr_t cell = reinterpret_cast<uintptr_t>(cell_data.data());
    put(image_base + 0x32DD370, player);
    put(image_base + 0x32DD158, reinterpret_cast<uintptr_t>(tes_data.data()));
    put(tes_data, 88, cell);
    put(player_data, 0x1A, uint8_t(65));
    put(player_data, 0xB8, cell);
    put(player_data, 0xD0, 1000.0f); put(player_data, 0xD4, 2000.0f); put(player_data, 0xD8, 0.0f);
    put(cell_data, 0x14, uint32_t(0x123));
    put(cell_data, 0x40, uint16_t(1)); put(cell_data, 0x44, uint8_t(8));
    put(renderer_data, 504, reinterpret_cast<uintptr_t>(camera_data.data()));
    // An interior with north rotated 90 degrees, and a non-square frustum.
    const float axes[] = {1,0,0,0, 0,1,0,0, 1000,2000,3000,0};
    const float frustum[] = {-400,400,200,-200};
    std::memcpy(camera_data.data() + 128, axes, sizeof(axes));
    std::memcpy(camera_data.data() + 352, frustum, sizeof(frustum));
    put(image_base + 0x2F4D750 + 3572 + 19 * 4, uint32_t(7));
    put(image_base + 0x3A25D00 + 2664 + 48 * 7, uintptr_t(0x9ABC));
    put(image_base + 0x38E13A0, reinterpret_cast<uintptr_t>(graphics_data.data()));
    put(graphics_data, 72, uintptr_t(0x1234)); put(graphics_data, 80, uintptr_t(0x5678));
    Fo4EntitySnapshot snapshot;
    snapshot.status = "Ready"; snapshot.player_address = player;
    snapshot.cell_address = cell; snapshot.cell_form_id = 0x123; snapshot.cell_is_interior = true;
    assert(!fo4hud_local_map_request(snapshot));
    assert(render_stage_hook() == 0xACDC);
    auto frame = fo4hud_local_map_request(snapshot);
    assert(frame && frame->width == 3 && frame->height == 2 && frame->sequence == 1);
    assert(creates == 1 && renders == 1 && destroys == 1 && frees == 1 && captures == 1 && releases == 2);
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 1);
    // Frame a whole interior independent of player location, covering both
    // dimensions of its scene sphere. Invalid/absent bounds keep native framing.
    std::array<unsigned char, 456> loaded{};
    std::array<unsigned char, 192> root{};
    put(cell_data, 208, reinterpret_cast<uintptr_t>(loaded.data()));
    put(loaded, 448, reinterpret_cast<uintptr_t>(root.data()));
    put(root, 176, 5000.0f); put(root, 180, -8000.0f); put(root, 184, 200.0f); put(root, 188, 10000.0f);
    assert(frame_interior(reinterpret_cast<uintptr_t>(renderer_data.data()), cell));
    assert(framed_center[0] == 5000 && framed_center[1] == -8000 && framed_center[2] == 200);
    assert(framed_height == 21000 && std::abs(framed_width * 0.5625f - framed_height) < 0.01f);
    for (int i = 0; i < 3; ++i) {
        assert(framed_maximum[i] == framed_center[i] + 10500);
        assert(framed_minimum[i] == framed_center[i] - 10500);
    }
    put(root, 188, std::numeric_limits<float>::infinity());
    assert(!frame_interior(reinterpret_cast<uintptr_t>(renderer_data.data()), cell));
    put(cell_data, 208, uintptr_t(0));
    const std::vector<unsigned char> expected{1,2,3,255,4,5,6,255,7,8,9,255,10,11,12,255,13,14,15,255,16,17,18,255};
    assert(frame->pixels == expected); // Row padding must not become image pixels.
    assert(frame->map.corners[0].x == 1200 && frame->map.corners[0].y == 1600);
    for (int i = 0; i < 3; ++i) {
        const auto p = frame->map.corners[i];
        const auto uv = frame->map.world_to_uv(p.x, p.y);
        assert(uv.x == (i == 1 ? 1 : 0) && uv.y == (i == 2 ? 1 : 0));
    }
    const auto uv = frame->map.world_to_uv(1000, 2000);
    assert(uv.x == 0.5f && uv.y == 0.5f);
    for (float heading : {0.0f, 1.5707963f, -2.1f}) {
        const Fo4RadarTransform t{1000,2000,heading,600,200};
        const auto polygon = fo4hud_radar_map_polygon(frame->map, t);
        assert(polygon.size() >= 3);
        for (const auto& v : polygon) {
            const auto& corners = frame->map.corners;
            const auto projected = t.project(corners[0].x + v.u * (corners[1].x - corners[0].x) + v.v * (corners[2].x - corners[0].x),
                                              corners[0].y + v.u * (corners[1].y - corners[0].y) + v.v * (corners[2].y - corners[0].y));
            assert(std::abs(projected.x - v.x) < 0.001f && std::abs(projected.y - v.y) < 0.001f);
            assert(v.u >= -1e-6f && v.u <= 1.000001f && v.v >= -1e-6f && v.v <= 1.000001f);
        }
    }
    // Frequent present calls do not cause frequent readbacks.
    render_stage_hook(); assert(captures == 1 && finishes == 2);
    menu_open = true; fresh_attempt(); render_stage_hook(); assert(captures == 1);
    menu_open = false;
    request.updated = Clock::now() - std::chrono::seconds(2);
    render_stage_hook(); assert(captures == 1); // Abandoned radar request.
    (void)fo4hud_local_map_request(snapshot);
    put(player_data, 0xD8, 200.0f);
    assert(fo4hud_local_map_request(snapshot) == frame); // Keep the whole-cell map while elevation refreshes.
    fresh_attempt(); render_stage_hook(); assert(captures == 2);
    frame = fo4hud_local_map_request(snapshot); assert(frame && frame->position[2] == 200);
    native_image.format = 87;
    fresh_attempt(); render_stage_hook(); assert(converts == 1);
    assert(fo4hud_local_map_request(snapshot));
    capture_ok = false;
    fresh_attempt(); render_stage_hook();
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 1);
    assert(creates == destroys && destroys == frees && releases == captures * 2);
    throw_during_render = true;
    fresh_attempt(); render_stage_hook();
    assert(*reinterpret_cast<uint8_t*>(image_base + 0x2EDDBE0) == 1);
    assert(creates == destroys && destroys == frees);
    throw_during_render = false;
    capture_ok = true; change_cell_during_render = true;
    fresh_attempt(); render_stage_hook();
    assert(!fo4hud_local_map_request(snapshot)); // A render spanning a door transition cannot publish.
    assert(creates == destroys && destroys == frees);
    change_cell_during_render = false; put(player_data, 0xB8, cell);
    snapshot.cell_is_interior = false;
    assert(!fo4hud_local_map_request(snapshot) && !latest && !request.player);
    snapshot.cell_is_interior = true;
    put(cell_data, 0x14, uint32_t(0x456));
    assert(!fo4hud_local_map_request(snapshot)); // Reused cell address, different form.
    put(cell_data, 0x14, snapshot.cell_form_id);
    put(cell_data, 0x44, uint8_t(7));
    assert(!fo4hud_local_map_request(snapshot)); // Cell still loading.
    Fo4LocalMapFrame invalid;
    native_image.row_pitch = 1;
    assert(!copy_pixels(reinterpret_cast<uintptr_t>(&native_image), invalid));
    enabled.store(false);
    assert(render_stage_hook() == 0xACDC); // Disabled hook always forwards.
    // A mismatched instruction must prevent installation. With all checked
    // signatures present, installation/restoration changes only the CALL.
    const NativeApi mock_api = api;
    assert(!fo4hud_local_map_install());
    const std::pair<uintptr_t, std::vector<uint8_t>> signatures[] = {
        {kRenderCall, {0xE8, 0xB5, 0x1F, 0x5C, 0x01}},
        {0xADB360, {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57}},
        {0x371350, {0x48,0x8B,0xC4,0x88,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x53}},
        {0x3712D0, {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20}},
        {0x184DF90, {0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}}
    };
    for (const auto& s : signatures)
        std::memcpy(reinterpret_cast<void*>(image_base + s.first), s.second.data(), s.second.size());
    assert(fo4hud_local_map_install());
    api = mock_api;
    int32_t displacement = 0;
    assert(read_value(image_base + kRenderCall + 1, displacement));
    assert(image_base + kRenderCall + 5 + displacement == reinterpret_cast<uintptr_t>(jump_island));
    request = {};
    assert(reinterpret_cast<FinishFn>(jump_island)() == 0xACDC);
    fo4hud_local_map_uninstall();
    uint8_t restored[5]{};
    assert(read_memory(image_base + kRenderCall, restored, sizeof(restored)));
    assert(std::memcmp(restored, signatures[0].second.data(), sizeof(restored)) == 0);
    assert(!latest && !request.player && !enabled.load());
    // A CPU that fetched the old CALL just before restoration can still use
    // the retained island safely; it forwards to the original render stage.
    assert(reinterpret_cast<FinishFn>(jump_island)() == 0xACDC);
    munmap(mapped, image_size);
    std::filesystem::remove_all(state_directory);
}
