#pragma once

#include <cstdint>

// Fallout 4 TESForm type values. Keep these independent of the radar's
// reference categories: a container can hold items from many loot categories.
enum Fo4LootCategory : uint32_t {
    Fo4LootJunk        = 1u << 0,
    Fo4LootWeapons     = 1u << 1,
    Fo4LootApparel     = 1u << 2,
    Fo4LootAid         = 1u << 3,
    Fo4LootAmmo        = 1u << 4,
    Fo4LootBooks       = 1u << 5,
    Fo4LootIngredients = 1u << 6,
    Fo4LootKeys        = 1u << 7,
    Fo4LootNotes       = 1u << 8,
    Fo4LootOtherMisc   = 1u << 9,
};
constexpr uint32_t kFo4LootAllCategories = (1u << 10) - 1;
constexpr uint32_t kFo4LootDefaultCategories =
    kFo4LootAllCategories & ~(Fo4LootBooks | Fo4LootKeys | Fo4LootNotes);

constexpr uint32_t fo4hud_misc_loot_category(uint32_t form_flags, uint32_t component_count) {
    return (form_flags & 0x80u) || component_count == 0 ?
        Fo4LootOtherMisc : Fo4LootJunk;
}

constexpr uint32_t fo4hud_loot_category_for_form(uint8_t type, bool has_components) {
    switch (type) {
        case 0x1D: return Fo4LootApparel;     // ARMO
        case 0x1E: return Fo4LootBooks;       // BOOK
        case 0x21: return Fo4LootIngredients; // INGR
        case 0x23: return has_components ? Fo4LootJunk : Fo4LootOtherMisc; // MISC
        case 0x2B: return Fo4LootWeapons;     // WEAP
        case 0x2C: return Fo4LootAmmo;        // AMMO
        case 0x2F: return Fo4LootKeys;        // KEYM
        case 0x30: return Fo4LootAid;         // ALCH
        case 0x32: return Fo4LootNotes;       // NOTE
        default: return 0;
    }
}

// Fallout4.esm: loot_capsStash_tin (ACTI), using bobbyPinBoxSCRIPT to
// award a leveled caps amount and disable/delete the activated reference.
// ACTI is runtime type 0x1B, verified in Fallout4.exe 1.11.240.
// Match the complete base-game form ID; unrelated activators stay excluded.
constexpr bool fo4hud_is_cap_stash_form(uint8_t type, uint32_t form_id) {
    return type == 0x1B && form_id == 0x0003000C;
}

// MirelurkEgg_Harvestable runs MirelurkHarvestableScript to award the ALCH
// MirelurkEgg (0x0023E9D4). The hatching egg (0x0016579B) is not loot.
constexpr bool fo4hud_is_mirelurk_egg_form(uint8_t type, uint32_t form_id) {
    return type == 0x1B && form_id == 0x001715CD;
}

constexpr uint32_t fo4hud_scripted_loot_category(uint8_t type, uint32_t form_id) {
    return fo4hud_is_cap_stash_form(type, form_id) ? Fo4LootOtherMisc :
           fo4hud_is_mirelurk_egg_form(type, form_id) ? Fo4LootAid : 0;
}
