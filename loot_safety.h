#pragma once

#include <cstdint>

template<class Reader>
bool fo4hud_loot_text_matches(uintptr_t text, const char* expected, Reader& read) {
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t c = 0;
        if (!read(text + i, c)) return false;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != static_cast<uint8_t>(expected[i])) return false;
        if (!c) return true;
    }
    return false;
}

template<class Reader>
bool fo4hud_loot_alias_exception(uintptr_t quest, uintptr_t alias, Reader& read,
                                bool (*scripts_clear)(uintptr_t) = nullptr) {
    uint32_t id = 0, alias_id = 0;
    uint8_t type = 0;
    uintptr_t owner = 0, sources = 0, files = 0, file = 0;
    uint64_t file_count = 0;
    if (!quest || !alias || !read(quest + 0x1A, type) || type != 0x50 ||
        !read(quest + 0x14, id) || !read(alias + 0x10, owner) || owner != quest ||
        !read(alias + 0x18, alias_id)) return false;
    // The game-thread VM inspector approves the actual bound script set,
    // independent of quest/plugin/name. Unknown siblings reject this path.
    if (scripts_clear && scripts_clear(alias)) return true;
    const char* name = nullptr;
    const char* plugin = "dlccoast.esm";
    if ((id & 0xFFFFFF) == 0x001B40 && alias_id == 13) name = "trappers";
    else if ((id & 0xFFFFFF) == 0x043822 && alias_id == 9) name = "enemies_wave2";
    // DLC03_ADV011: DefaultCollectionAliasOnDeath counts kills, then stage 50
    // enables/activates QueenEnableMarker. No inventory or looting handlers.
    else if ((id & 0xFFFFFF) == 0x032D34 && alias_id == 3) name = "mirelurkcollection";
    else return false;

    // Match the originating plugin, not a particular load-order prefix or an
    // unrelated mod with the same local FormID. Overrides retain this origin.
    if (!read(quest + 8, sources) || !sources || !read(sources, files) || !files ||
        !read(sources + 8, file_count) || !file_count || file_count > 4096 ||
        !read(files, file) || !file ||
        !fo4hud_loot_text_matches(file + 0x70, plugin, read)) return false;

    uintptr_t entry = 0;
    if (!read(alias + 8, entry)) return false;
    for (unsigned depth = 0; entry && depth < 4; ++depth) {
        uint16_t flags = 0;
        if (!read(entry + 8, flags)) return false;
        if (flags & 0x4000) {
            if (!read(entry + 0x10, entry)) return false;
            continue;
        }
        uint32_t length = 0;
        unsigned expected_length = 0;
        while (name[expected_length]) ++expected_length;
        return !(flags & 0x8000) && read(entry + 0x10, length) && length == expected_length &&
               fo4hud_loot_text_matches(entry + 0x18, name, read);
    }
    return false;
}

template<class Reader>
bool fo4hud_loot_alias_array_exempt(uintptr_t node, Reader& read,
                                   bool (*scripts_clear)(uintptr_t) = nullptr) {
    uintptr_t entries = 0;
    uint32_t capacity = 0, count = 0;
    if (!read(node + 0x18, entries) || !entries || !read(node + 0x20, capacity) ||
        !read(node + 0x28, count) || !count || count > capacity || capacity > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t quest = 0, alias = 0;
        if (!read(entries + i * 24, quest) || !read(entries + i * 24 + 8, alias) ||
            !fo4hud_loot_alias_exception(quest, alias, read, scripts_clear)) return false;
    }
    return true;
}

// Unknown/empty alias data stays manual. Exceptions apply only to corpse
// reference attachments, never to protected inventory stacks or loose objects.
template<class Reader>
const char* fo4hud_loot_extra_reason(uintptr_t extra, Reader&& read,
                                   bool corpse_alias_exceptions = false,
                                   bool (*scripts_clear)(uintptr_t) = nullptr,
                                   bool allow_locks = false) {
    if (!extra) return nullptr;
    uintptr_t node = 0;
    if (!read(extra + 8, node)) return "Safety data unavailable";
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t type = 0;
        uintptr_t next = 0;
        if (!read(node + 0x12, type) || !read(node + 8, next) || next == node)
            return "Safety data unavailable";
        switch (type) {
            case 0x88: // ExtraAliasInstanceArray
                if (corpse_alias_exceptions && fo4hud_loot_alias_array_exempt(node, read, scripts_clear)) break;
                return "Quest-linked";
            case 0x95: // ExtraFromAlias
                return "Quest-linked";
            case 0x2A: // ExtraLock: leave lock-bearing objects manual, even unlocked.
            case 0xA3: // ExtraLockList
                if (allow_locks) break;
                return "Lock interaction required";
            case 0x52: // ExtraActivateRef
            case 0x53: // ExtraActivateRefChildren
            case 0x68: // ExtraOpenCloseActivateRef
                return "Activation interaction required";
            case 0xA0: { // ExtraFlags, tested by Activate at 0x5144B0.
                uint32_t flags = 0;
                if (!read(node + 0x18, flags)) return "Safety data unavailable";
                if (flags & 9u) return "Activation blocked";
                break;
            }
        }
        node = next;
    }
    return node ? "Safety data unavailable" : nullptr;
}

template<class Reader>
const char* fo4hud_loot_reference_reason(uintptr_t reference, Reader&& read,
                                        bool (*scripts_clear)(uintptr_t) = nullptr,
                                        bool allow_locks = false) {
    uintptr_t extra = 0;
    if (!reference || !read(reference + 0x100, extra)) return "Safety data unavailable";
    uint8_t type = 0;
    uint32_t state = 0;
    const bool corpse = read(reference + 0x1A, type) && type == 65 &&
        read(reference + 0x130, state) &&
        (((state >> 17) & 0xF) == 1 || ((state >> 17) & 0xF) == 2);
    return fo4hud_loot_extra_reason(extra, read, corpse, scripts_clear, allow_locks);
}
