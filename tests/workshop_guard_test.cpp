#include "../game_entities.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
    alignas(8) std::array<unsigned char, 0x180> container{};
    auto write = [&](size_t offset, auto value) {
        std::memcpy(container.data() + offset, &value, sizeof(value));
    };
    write(0x14, uint32_t(0x00123456));
    write(0x1A, uint8_t(0x1F));
    const uintptr_t base = reinterpret_cast<uintptr_t>(container.data());
    assert(fo4hud_container_auto_loot_safe(base));

    // Settlement workbench and hidden resource-container base forms.
    write(0x14, uint32_t(0x000C1AEB));
    assert(!fo4hud_container_auto_loot_safe(base));
    write(0x14, uint32_t(0x0022BF75));
    assert(!fo4hud_container_auto_loot_safe(base));
    write(0x14, uint32_t(0x00123456));

    alignas(8) std::array<unsigned char, 0x20> keyword{};
    const uint32_t workshop_keyword = 0x00054BA7;
    std::memcpy(keyword.data() + 0x14, &workshop_keyword, sizeof(workshop_keyword));
    const uintptr_t keyword_ptr = reinterpret_cast<uintptr_t>(keyword.data());
    const uintptr_t keywords[] = {keyword_ptr};
    write(0x108, reinterpret_cast<uintptr_t>(keywords));
    write(0x110, uint32_t(1));
    assert(!fo4hud_container_auto_loot_safe(base));

    // Corrupt keyword metadata must fail closed.
    write(0x108, uintptr_t(0));
    assert(!fo4hud_container_auto_loot_safe(base));
}
