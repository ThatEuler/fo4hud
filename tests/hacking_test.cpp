#include "../game_entities.cpp"
#include "../hacking_overlay.h"
#include <sys/mman.h>
#include <cassert>

template <class T> void put(void* bytes, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(bytes) + offset, &value, sizeof(value));
}

int main() {
    auto* image = static_cast<unsigned char*>(mmap(nullptr, kImageSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    alignas(8) unsigned char ui[0x260]{}, menu[0x270]{}, entry[16]{}, string[40]{}, external[24]{};
    std::array<char, 640> text;
    uintptr_t stack[] = {reinterpret_cast<uintptr_t>(menu)};
    put(image, kUiPointerRva, reinterpret_cast<uintptr_t>(ui));
    put(ui, 0, base + kUiVtableRva);
    put(ui, 0x190, reinterpret_cast<uintptr_t>(stack));
    put(ui, 0x198, uint32_t(1)); put(ui, 0x1A0, uint32_t(1));
    put(menu, 0, base + kTerminalVtableRva);
    put(menu, 0x58, uint32_t(0x40)); put(menu, 0x240, uint32_t(1));
    put(image, kHackingMenuRva, stack[0]);
    put(image, kHackingFlagsRva, uint8_t(1));
    put(image, kHackingLengthRva, uint32_t(4));
    put(image, kHackingPasswordRva, uint32_t(0));
    put(image, kHackingWordsRva, reinterpret_cast<uintptr_t>(entry));
    put(image, kHackingWordsRva + 8, uint32_t(1));
    put(image, kHackingWordsRva + 0x10, uint32_t(1));
    put(image, kHackingBoardRva, reinterpret_cast<uintptr_t>(text.data()));
    put(entry, 0, reinterpret_cast<uintptr_t>(string));
    std::memcpy(string + 24, "CODE", 5);
    Fo4HackingState state;
    // Exercise row wraps, the column boundary, and the last possible word.
    for (uint32_t position : {0u, 10u, 190u, 192u, 380u}) {
        text.fill('!'); text.back() = 0;
        put(entry, 8, position);
        for (uint32_t j = 0; j < 4; ++j) {
            const uint32_t i = position + j;
            text[40 * ((i % 192) / 12) + 20 * (i / 192) + i % 12 + 7] = "CODE"[j];
        }
        assert(read_hacking_state(base, state));
        assert(state.password == "CODE" && state.password_position == position);
        assert(std::string(state.board.data() + position, 4) == "CODE");
    }
    auto rejected = [&] {
        assert(!read_hacking_state(base, state));
        assert(state.password.empty());
    };
    put(menu, 0x240, uint32_t(3)); rejected(); put(menu, 0x240, uint32_t(1));
    put(menu, 0x58, uint32_t(0)); rejected(); put(menu, 0x58, uint32_t(0x40));
    put(image, kHackingFlagsRva, uint8_t(5)); rejected();
    put(image, kHackingFlagsRva, uint8_t(0)); rejected();
    put(image, kHackingFlagsRva, uint8_t(1));
    put(image, kHackingPasswordRva, uint32_t(-1)); rejected();
    put(image, kHackingPasswordRva, uint32_t(0));
    put(image, kHackingLengthRva, uint32_t(13)); rejected();
    put(image, kHackingLengthRva, uint32_t(0)); rejected();
    put(image, kHackingLengthRva, uint32_t(4));
    put(entry, 8, uint32_t(383)); rejected(); put(entry, 8, uint32_t(380));
    put(entry, 12, uint8_t(1)); rejected(); put(entry, 12, uint8_t(0));
    put(image, kHackingWordsRva + 8, uint32_t(0)); rejected();
    put(image, kHackingWordsRva + 8, uint32_t(1));
    put(ui, 0x1A0, uint32_t(2)); rejected(); put(ui, 0x1A0, uint32_t(1));
    stack[0] = 1; rejected(); stack[0] = reinterpret_cast<uintptr_t>(menu);
    put(entry, 0, uintptr_t(1)); rejected();
    put(entry, 0, reinterpret_cast<uintptr_t>(external));
    put(external, 8, uint32_t(0x4000));
    put(external, 16, reinterpret_cast<uintptr_t>(external)); rejected();
    put(external, 16, reinterpret_cast<uintptr_t>(string));
    assert(read_hacking_state(base, state));
    string[24] = 'X'; rejected(); string[24] = 'C';
    put(image, kHackingBoardRva, uintptr_t(1)); rejected();
    put(image, kHackingBoardRva, reinterpret_cast<uintptr_t>(text.data()));
    assert(read_hacking_state(base, state));

    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (const ImVec2 display : {ImVec2(320, 200), ImVec2(640, 480), ImVec2(1920, 1080), ImVec2(3440, 1440)}) {
        io.DisplaySize = display; io.DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        fo4hud_draw_hacking_overlay(state, display);
        assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        ImGui::Render();
        const auto* data = ImGui::GetDrawData();
        assert(data && data->TotalVtxCount > 0);
        bool highlight = false;
        for (int i = 0; i < data->CmdListsCount; ++i)
            for (const auto& vertex : data->CmdLists[i]->VtxBuffer) {
                assert(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y));
                assert(vertex.pos.x >= 0 && vertex.pos.x <= display.x);
                assert(vertex.pos.y >= 0 && vertex.pos.y <= display.y);
                highlight |= vertex.col == IM_COL32(255, 225, 90, 255);
            }
        assert(highlight);
    }
    ImGui::DestroyContext();
    munmap(image, kImageSize);
}
