#pragma once

#include <string>
#include <cstdint>

struct OverlaySettings {
    bool overlay_visible = false;
    bool lockpick_guide = true;
    bool hacking_guide = true;
    bool companion_information = true;
    bool dialogue_affinity = true;
    bool trap_warnings = true;
    float trap_warning_range = 2000.0f;
    bool world_labels = true;
    bool world_skeletons = false;
    bool hide_dead_actors = true;
    float label_radius = 2000.0f;
    int label_limit = 12;
    std::string name_filter;
    bool aim_assist = false;
    float aim_radius = 160.0f;
    float aim_distance = 3000.0f;
    bool instant_kill = false;
    bool invulnerable = false;
    bool unlimited_action_points = false;
    bool no_recoil = false;
    bool loot_vendor_chests = false;
    bool auto_loot = false;
    bool auto_loot_inventories = false;
    bool auto_harvest = false;
    bool auto_loot_steal = false;
    float auto_loot_range = 200.0f;
    bool loot_junk = true;
    bool loot_weapons = true;
    bool loot_apparel = true;
    bool loot_aid = true;
    bool loot_ammo = true;
    bool loot_books = false;
    bool loot_ingredients = true;
    bool loot_keys = false;
    bool loot_notes = false;
    bool loot_other_misc = true;
    bool radar_map_background = true;
    float radar_map_opacity = 0.85f;
    bool radar_doors = true;
    bool radar_containers = true;
    bool radar_items = true;
    bool radar_actors = true;
    bool radar_crops = true;
    float radar_range = 2000.0f;
    float radar_interior_range = 2000.0f;
    float& radar_range_for(bool interior) { return interior ? radar_interior_range : radar_range; }
    float radar_range_for(bool interior) const { return interior ? radar_interior_range : radar_range; }
};

std::string fo4hud_settings_path();
std::string fo4hud_imgui_ini_path();
bool fo4hud_load_settings(OverlaySettings& settings, std::string& error);
bool fo4hud_save_settings(const OverlaySettings& settings, std::string& error);
uint32_t fo4hud_loot_category_mask(const OverlaySettings& settings);
