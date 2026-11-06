#pragma once

#include <cstdint>
#include <optional>

// Checked equivalents of TESForm::IsPlayable in Fallout 4 1.11.240.
// A failed read stays unknown; built-in creature equipment is hidden loot.
template<class Reader>
std::optional<bool> fo4hud_item_playable(uintptr_t object, uint8_t type, Reader&& read) {
    uint32_t flags = 0;
    uint8_t ammo_flags = 0;
    switch (type) {
        case 0x1D:
            if (!read(object + 0x10, flags)) return {};
            return !(flags & 4u);
        case 0x2B:
            if (!read(object + 0x2A8, flags)) return {};
            return !(flags & 0x100000u);
        case 0x2C:
            if (!read(object + 0x16C, ammo_flags)) return {};
            return !(ammo_flags & 2u);
        default: return true;
    }
}

template<class Reader>
std::optional<bool> fo4hud_weapon_instance_playable(uintptr_t extra, Reader&& read) {
    if (!extra) return true;
    uintptr_t node = 0;
    if (!read(extra + 8, node)) return {};
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t type = 0;
        uintptr_t next = 0;
        if (!read(node + 0x12, type) || !read(node + 8, next) || next == node) return {};
        if (type == 0xBA) { // ExtraInstanceData, instance at +0x20.
            uintptr_t instance = 0;
            uint32_t flags = 0;
            if (!read(node + 0x20, instance)) return {};
            if (!instance) return true;
            if (!read(instance + 0x110, flags)) return {};
            return !(flags & 0x100000u);
        }
        node = next;
    }
    if (node) return {};
    return true;
}
