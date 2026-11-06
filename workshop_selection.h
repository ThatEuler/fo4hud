#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The preview's initial selection is intentionally conservative. A missed
// object can still be checked manually before the scrap pass starts.
inline bool fo4hud_workshop_default_selected(std::string_view name, uint32_t ref_id) {
    // Newly placed settlement objects have save-game reference IDs. Their
    // display names may be generic or supplied by a mod.
    if (name.empty() || (ref_id >> 24) == 0xFF) return false;

    std::string lower;
    lower.reserve(name.size());
    for (unsigned char ch : name)
        lower.push_back(ch >= 'A' && ch <= 'Z' ? char(ch + ('a' - 'A')) : char(ch));

    constexpr std::string_view protected_names[] = {
        // Crafting and settlement control.
        "workbench", "workshop", "crafting", "station", "chemistry", "cooking",
        "power armor", "power armour", "armor bench", "armour bench", "weapon bench",
        // Utilities, production, and defense.
        "generator", "purifier", "water pump", "water tower", "water well",
        "turret", "guard post", "defense post", "defence post", "artillery",
        "recruitment beacon", "radio beacon", "scavenging", "brahmin trough",
        "feed trough", "conduit", "pylon", "power switch", "power connector",
        "terminal", "fusion core", "power plant",
        // Stores, storage, beds, and food crops.
        "store", "shop", "trading post", "trader", "vendor", "clinic",
        "container", "locker", "safe", "chest", "dresser",
        "bed", "sleeping bag", "crop", "planter", "mutfruit", "razorgrain",
        "tato", "corn", "carrot", "gourd", "melon",
    };
    for (std::string_view term : protected_names)
        if (lower.find(term) != std::string::npos) return false;
    return true;
}
