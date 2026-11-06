#include "../workshop_selection.h"

#include <cassert>

int main() {
    constexpr uint32_t existing_ref = 0x00054BB0;
    assert(!fo4hud_workshop_default_selected("Weapons Workbench", existing_ref));
    assert(!fo4hud_workshop_default_selected("Power Armor Station", existing_ref));
    assert(!fo4hud_workshop_default_selected("Cooking Station", existing_ref));
    assert(!fo4hud_workshop_default_selected("Industrial Water Purifier", existing_ref));
    assert(!fo4hud_workshop_default_selected("Machinegun Turret", existing_ref));
    assert(!fo4hud_workshop_default_selected("Mutfruit Plant", existing_ref));
    assert(fo4hud_workshop_default_selected("Cabinet", existing_ref));
    assert(fo4hud_workshop_default_selected("Medicine Cabinet", existing_ref));
    assert(!fo4hud_workshop_default_selected("", existing_ref));
    assert(!fo4hud_workshop_default_selected("Wooden Crate", 0xFF00B608));
    assert(fo4hud_workshop_default_selected("Ruined House", existing_ref));
    assert(fo4hud_workshop_default_selected("Broken Chair", existing_ref));
}
