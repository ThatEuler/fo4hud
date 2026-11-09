#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "vendor_chest_records.h"

// Read TESFile identities through the winning file's master table. This also
// handles overrides and light plugins without assuming DLC load-order slots.
template<class Reader>
bool fo4hud_vendor_form_matches(uintptr_t form, const char* plugin, uint32_t local_id, Reader& read) {
    uint32_t id = 0, count = 0;
    uintptr_t sources = 0, files = 0, file = 0;
    if (!read(form + 0x14, id) ||
        (id & ((id >> 24) == 0xFE ? 0xFFF : 0xFFFFFF)) != local_id ||
        !read(form + 8, sources) || !sources || !read(sources, files) || !files ||
        !read(sources + 8, count) || !count || count > 256 ||
        !read(files + 8 * (count - 1), file)) return false;
    std::vector<uintptr_t> pending{file}, visited;
    while (!pending.empty() && visited.size() < 256) {
        file = pending.back(); pending.pop_back();
        if (!file || std::find(visited.begin(), visited.end(), file) != visited.end()) continue;
        visited.push_back(file);
        uint8_t prefix = 0;
        uint16_t light = 0;
        if (!read(file + 0x370, prefix)) return false;
        if (prefix == (id >> 24) && (prefix != 0xFE ||
            (read(file + 0x372, light) && light == ((id >> 12) & 0xFFF)))) {
            for (size_t i = 0; i < 260; ++i) {
                char c = 0;
                if (!read(file + 0x70 + i, c)) return false;
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != plugin[i]) return false;
                if (!c) return true;
            }
            return false;
        }
        uintptr_t masters = 0;
        if (!read(file + 0x358, count) || count > 256 ||
            !read(file + 0x360, masters) || (count && !masters)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t master = 0;
            if (!read(masters + 8*i, master)) return false;
            pending.push_back(master);
        }
    }
    return false;
}

template<class Reader>
const char* fo4hud_vendor_chest_name(uintptr_t reference, Reader&& read) {
    uint8_t type = 0;
    uintptr_t base = 0;
    uint32_t id = 0, base_id = 0;
    if (!reference || !read(reference + 0x1A, type) || type != 64 ||
        !read(reference + 0xE0, base) || !base ||
        !read(base + 0x1A, type) || type != 0x1F ||
        !read(reference + 0x14, id) || !read(base + 0x14, base_id)) return nullptr;
    for (const auto& r : kFo4VendorChests) {
        if ((id & ((id >> 24) == 0xFE ? 0xFFF : 0xFFFFFF)) == r.reference_id &&
            (base_id & ((base_id >> 24) == 0xFE ? 0xFFF : 0xFFFFFF)) == r.base_id &&
            fo4hud_vendor_form_matches(reference, r.reference_plugin, r.reference_id, read) &&
            fo4hud_vendor_form_matches(base, r.base_plugin, r.base_id, read)) return r.name;
    }
    // Script-created stock (including settlement vendors) has a dynamic
    // reference ID, but still uses a known vendor-only container base.
    for (const auto& r : kFo4VendorChestBases) {
        if ((base_id & ((base_id >> 24) == 0xFE ? 0xFFF : 0xFFFFFF)) == r.id &&
            fo4hud_vendor_form_matches(base, r.plugin, r.id, read)) return r.name;
    }
    return nullptr;
}
