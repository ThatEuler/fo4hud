#include "../game_entities.cpp"
#include "../lockpick_overlay.h"
#include <sys/mman.h>
#include <cassert>
#include <limits>

template <class T> void put(void* bytes, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(bytes) + offset, &value, sizeof(value));
}

int main() {
    auto* image = static_cast<unsigned char*>(mmap(nullptr, kImageSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    alignas(8) unsigned char ui[0x260]{}, menu[0x1C0]{}, unrelated[16]{};
    const uintptr_t ui_address = reinterpret_cast<uintptr_t>(ui);
    const uintptr_t menu_address = reinterpret_cast<uintptr_t>(menu);
    uintptr_t stack[] = {reinterpret_cast<uintptr_t>(unrelated), menu_address};
    put(image, kUiPointerRva, ui_address);
    put(ui, 0, base + kUiVtableRva);
    put(ui, 0x190, reinterpret_cast<uintptr_t>(stack));
    put(ui, 0x198, uint32_t(2)); put(ui, 0x1A0, uint32_t(2));
    put(menu, 0, base + kLockpickVtableRva);
    put(menu, 0x58, uint32_t(0x40)); put(menu, 0x1B0, uint8_t(1));
    put(menu, 0x188, float(-30)); put(menu, 0x1A0, float(20)); put(menu, 0x1A4, float(12));
    Fo4LockpickState state;
    assert(read_lockpick_state(base, state));
    assert(state.pick_degrees == -30 && state.sweet_spot_degrees == 20 && state.sweet_spot_width == 12);
    assert(!fo4hud_lockpick_aligned(state));
    state.pick_degrees = 26;
    assert(fo4hud_lockpick_aligned(state)); // The game uses the full width / 2.
    state.pick_degrees = 26.01f;
    assert(!fo4hud_lockpick_aligned(state));
    assert(std::cos(fo4hud_lockpick_screen_angle(90)) < -0.99f);
    assert(std::cos(fo4hud_lockpick_screen_angle(-90)) > 0.99f);
    assert(std::sin(fo4hud_lockpick_screen_angle(0)) < -0.99f);

    put(menu, 0x58, uint32_t(0)); assert(!read_lockpick_state(base, state));
    put(menu, 0x58, uint32_t(0x40));
    put(menu, 0x1B0, uint8_t(0)); assert(!read_lockpick_state(base, state));
    put(menu, 0x1B0, uint8_t(1));
    put(image, kLockpickUnlockTimeRva, uint64_t(123)); assert(!read_lockpick_state(base, state));
    put(image, kLockpickUnlockTimeRva, uint64_t(0));
    put(menu, 0x188, std::numeric_limits<float>::max()); assert(!read_lockpick_state(base, state));
    put(menu, 0x188, float(-30));
    put(menu, 0x1A0, std::numeric_limits<float>::quiet_NaN()); assert(!read_lockpick_state(base, state));
    put(menu, 0x1A0, float(89)); assert(!read_lockpick_state(base, state)); // Band exceeds legal range.
    put(menu, 0x1A0, float(20));
    put(menu, 0x1A4, float(-1)); assert(!read_lockpick_state(base, state));
    put(menu, 0x1A4, float(12));
    put(ui, 0x1A0, uint32_t(3)); assert(!read_lockpick_state(base, state)); // Count > capacity.
    put(ui, 0x198, uint32_t(65)); put(ui, 0x1A0, uint32_t(65)); assert(!read_lockpick_state(base, state));
    put(ui, 0x198, uint32_t(2)); put(ui, 0x1A0, uint32_t(1)); assert(!read_lockpick_state(base, state)); // Other menu only.
    put(ui, 0x1A0, uint32_t(2));
    stack[1] = 1; assert(!read_lockpick_state(base, state)); // Stale/unmapped menu.
    stack[1] = menu_address;
    assert(read_lockpick_state(base, state));
    put(ui, 0, uintptr_t(0)); assert(!read_lockpick_state(base, state));
    put(ui, 0, base + kUiVtableRva);

    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (const ImVec2 display : {ImVec2(640, 480), ImVec2(1920, 1080), ImVec2(3440, 1440)}) {
        io.DisplaySize = display; io.DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        state = {-30, 20, 12};
        fo4hud_draw_lockpick_overlay(state, display);
        assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        ImGui::Render();
        const ImDrawData* data = ImGui::GetDrawData();
        assert(data && data->TotalVtxCount > 0);
        for (int i = 0; i < data->CmdListsCount; ++i)
            for (const auto& vertex : data->CmdLists[i]->VtxBuffer) {
                assert(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y));
                assert(vertex.pos.x >= 0 && vertex.pos.x <= display.x);
                assert(vertex.pos.y >= 0 && vertex.pos.y <= display.y);
            }
    }
    ImGui::DestroyContext();
    assert(munmap(image, kImageSize) == 0);
}
