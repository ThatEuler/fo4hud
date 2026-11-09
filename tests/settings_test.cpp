#include "../settings.h"
#include "../loot_categories.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>

int main() {
    char directory[] = "/tmp/fo4hud-settings-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    const std::string path = fo4hud_settings_path();
    OverlaySettings defaults;
    std::string error;
    assert(fo4hud_load_settings(defaults, error));
    assert(defaults.world_labels && !defaults.no_recoil && !defaults.auto_loot_steal);
    assert(defaults.lockpick_guide && defaults.hacking_guide && defaults.companion_information && defaults.dialogue_affinity);
    assert(!defaults.overlay_visible);
    assert(fo4hud_loot_category_mask(defaults) == kFo4LootDefaultCategories);

    assert(defaults.trap_warnings && defaults.trap_warning_range == 2000);
    OverlaySettings chosen;
    chosen.overlay_visible = true;
    chosen.trap_warnings = false;
    chosen.trap_warning_range = 1500;
    chosen.hacking_guide = false;
    chosen.companion_information = false;
    chosen.dialogue_affinity = false;
    chosen.world_labels = false;
    chosen.world_skeletons = true;
    chosen.hide_dead_actors = false;
    chosen.label_radius = 75000.0f;
    chosen.label_limit = 27;
    chosen.name_filter = "raider,-dead";
    chosen.aim_assist = true;
    chosen.aim_radius = 333.0f;
    chosen.aim_distance = 12345.0f;
    chosen.instant_kill = true;
    chosen.invulnerable = true;
    chosen.unlimited_action_points = true;
    chosen.no_recoil = true;
    assert(!chosen.loot_vendor_chests);
    chosen.loot_vendor_chests = true;
    chosen.auto_loot = true;
    chosen.auto_loot_inventories = true;
    chosen.auto_harvest = true;
    chosen.auto_loot_steal = true;
    chosen.auto_loot_range = 425.0f;
    chosen.loot_junk = false;
    chosen.loot_weapons = false;
    chosen.loot_aid = false;
    chosen.loot_keys = true;
    chosen.loot_books = true;
    chosen.loot_notes = true;
    chosen.radar_map_background = false;
    chosen.radar_map_opacity = 0.4f;
    chosen.radar_doors = false;
    chosen.radar_containers = false;
    chosen.radar_items = true;
    chosen.radar_actors = false;
    chosen.radar_crops = false;
    chosen.radar_range = 7500.0f;
    chosen.radar_range_for(true) = 1250.0f;
    assert(chosen.radar_range_for(false) == 7500.0f);
    assert(fo4hud_save_settings(chosen, error));
    {
        std::ifstream saved(path);
        const std::string text((std::istreambuf_iterator<char>(saved)),
                               std::istreambuf_iterator<char>());
        assert(text.find("aim_strength=") == std::string::npos);
        assert(text.find("auto_loot_steal=1") != std::string::npos);
        assert(text.find("auto_loot_owned=") == std::string::npos);
    }
    struct stat metadata{};
    assert(stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600);

    OverlaySettings loaded;
    assert(fo4hud_load_settings(loaded, error));
    assert(loaded.overlay_visible);
    assert(loaded.loot_keys && loaded.loot_books && loaded.loot_notes);
    assert(!loaded.trap_warnings && loaded.trap_warning_range == 1500);
    assert(!loaded.hacking_guide && !loaded.companion_information && !loaded.dialogue_affinity);
    assert(!loaded.world_labels && loaded.world_skeletons && !loaded.hide_dead_actors);
    assert(loaded.label_radius == 75000.0f && loaded.label_limit == 27);
    assert(loaded.name_filter == "raider,-dead");
    assert(loaded.aim_assist && loaded.aim_radius == 333.0f &&
           loaded.aim_distance == 12345.0f);
    assert(loaded.instant_kill && loaded.invulnerable &&
           loaded.unlimited_action_points && loaded.no_recoil);
    assert(loaded.loot_vendor_chests);
    assert(loaded.auto_loot && loaded.auto_loot_inventories && loaded.auto_harvest &&
           loaded.auto_loot_steal &&
           loaded.auto_loot_range == 425.0f);
    assert(!loaded.loot_junk && !loaded.loot_weapons && !loaded.loot_aid &&
           loaded.loot_keys && loaded.loot_apparel && loaded.loot_ammo &&
           loaded.loot_books && loaded.loot_ingredients && loaded.loot_notes &&
           loaded.loot_other_misc);
    assert(fo4hud_loot_category_mask(loaded) ==
           (kFo4LootAllCategories & ~(Fo4LootJunk | Fo4LootWeapons | Fo4LootAid)));
    assert(!loaded.radar_map_background && loaded.radar_map_opacity == 0.4f);
    assert(!loaded.radar_doors && !loaded.radar_containers && loaded.radar_items &&
           !loaded.radar_actors && !loaded.radar_crops && loaded.radar_range == 7500.0f);
    assert(loaded.radar_range_for(true) == 1250.0f);

    std::ofstream file(path, std::ios::app);
    file << "label_radius=999999\nlabel_limit=-50\naim_distance=99999\n"
            "aim_strength=0.1\nno_recoil=maybe\nradar_map_opacity=50\nhacking_guide=1\nradar_interior_range=-50\n";
    file.close();
    assert(fo4hud_load_settings(loaded, error));
    assert(loaded.label_radius == 100000.0f && loaded.label_limit == 1);
    assert(loaded.aim_distance == 15000.0f);
    assert(loaded.no_recoil);
    assert(loaded.hacking_guide);
    assert(loaded.radar_map_opacity == 1.0f);
    assert(loaded.radar_interior_range == 100.0f);
    {
        std::ofstream legacy(path, std::ios::trunc);
        legacy << "auto_loot_owned=1\nloot_books=1\nloot_keys=1\nloot_notes=1\n";
    }
    OverlaySettings migrated;
    assert(fo4hud_load_settings(migrated, error));
    assert(migrated.auto_loot_steal);
    assert(!migrated.loot_books && !migrated.loot_keys && !migrated.loot_notes);
    assert(!migrated.overlay_visible);
    assert(migrated.radar_range_for(true) == 2000.0f);
    std::filesystem::remove_all(directory);
}
