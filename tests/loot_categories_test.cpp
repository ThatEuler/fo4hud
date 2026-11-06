#include "../loot_categories.h"

#include <cassert>

int main() {
    // Verified against live Fallout4.exe 1.11.240 ACTI bases. 0x19 is not ACTI.
    assert(fo4hud_scripted_loot_category(0x1B, 0x0003000C) == Fo4LootOtherMisc);
    assert(fo4hud_scripted_loot_category(0x1B, 0x001715CD) == Fo4LootAid);
    assert(fo4hud_scripted_loot_category(0x19, 0x0003000C) == 0);
    assert(fo4hud_scripted_loot_category(0x19, 0x001715CD) == 0);
    assert(fo4hud_scripted_loot_category(0x1B, 0x0016579B) == 0);
    assert(fo4hud_loot_category_for_form(0x1D, false) == Fo4LootApparel);
    assert(fo4hud_loot_category_for_form(0x1E, false) == Fo4LootBooks);
    assert(fo4hud_loot_category_for_form(0x21, false) == Fo4LootIngredients);
    assert(fo4hud_loot_category_for_form(0x23, true) == Fo4LootJunk);
    assert(fo4hud_loot_category_for_form(0x23, false) == Fo4LootOtherMisc);
    assert(fo4hud_misc_loot_category(0, 2) == Fo4LootJunk);
    assert(fo4hud_misc_loot_category(0, 0) == Fo4LootOtherMisc);
    assert(fo4hud_misc_loot_category(0x80, 2) == Fo4LootOtherMisc); // loose mod
    assert(fo4hud_loot_category_for_form(0x2B, false) == Fo4LootWeapons);
    assert(fo4hud_loot_category_for_form(0x2C, false) == Fo4LootAmmo);
    assert(fo4hud_loot_category_for_form(0x2F, false) == Fo4LootKeys);
    assert(fo4hud_loot_category_for_form(0x30, false) == Fo4LootAid);
    assert(fo4hud_loot_category_for_form(0x32, false) == Fo4LootNotes);
    assert(fo4hud_loot_category_for_form(0x1F, false) == 0); // container, not an item
    assert(fo4hud_loot_category_for_form(0x41, false) == 0); // actor
}
