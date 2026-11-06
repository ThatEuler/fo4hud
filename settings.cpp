#include "settings.h"
#include "loot_categories.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
std::string settings_directory() {
    const char* config_home = std::getenv("XDG_CONFIG_HOME");
    if (config_home && config_home[0] == '/') return std::string(config_home) + "/fo4hud";
    const char* home = std::getenv("HOME");
    if (home && home[0] == '/') return std::string(home) + "/.config/fo4hud";
    return "/tmp/fo4hud-config-" + std::to_string(getuid());
}

bool read_bool(const std::string& value, bool& out) {
    if (value == "1") { out = true; return true; }
    if (value == "0") { out = false; return true; }
    return false;
}

bool read_float(const std::string& value, float& out, float low, float high) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno || end == value.c_str() || *end || !std::isfinite(parsed)) return false;
    out = std::clamp(parsed, low, high);
    return true;
}

bool read_int(const std::string& value, int& out, int low, int high) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno || end == value.c_str() || *end) return false;
    out = static_cast<int>(std::clamp(parsed, long(low), long(high)));
    return true;
}
} // namespace

std::string fo4hud_settings_path() { return settings_directory() + "/settings.ini"; }
std::string fo4hud_imgui_ini_path() { return settings_directory() + "/imgui.ini"; }

uint32_t fo4hud_loot_category_mask(const OverlaySettings& settings) {
    return (settings.loot_junk ? Fo4LootJunk : 0u) |
           (settings.loot_weapons ? Fo4LootWeapons : 0u) |
           (settings.loot_apparel ? Fo4LootApparel : 0u) |
           (settings.loot_aid ? Fo4LootAid : 0u) |
           (settings.loot_ammo ? Fo4LootAmmo : 0u) |
           (settings.loot_books ? Fo4LootBooks : 0u) |
           (settings.loot_ingredients ? Fo4LootIngredients : 0u) |
           (settings.loot_keys ? Fo4LootKeys : 0u) |
           (settings.loot_notes ? Fo4LootNotes : 0u) |
           (settings.loot_other_misc ? Fo4LootOtherMisc : 0u);
}

bool fo4hud_load_settings(OverlaySettings& settings, std::string& error) {
    error.clear();
    const std::string path = fo4hud_settings_path();
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        if (code) error = "Cannot inspect settings: " + code.message();
        return !code;
    }
    const auto size = std::filesystem::file_size(path, code);
    if (code || size > 16384) {
        error = code ? "Cannot inspect settings: " + code.message() : "Settings file is too large";
        return false;
    }
    std::ifstream file(path);
    if (!file) { error = "Cannot open settings: " + std::string(std::strerror(errno)); return false; }
    bool quest_loot_defaults = false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split);
        const std::string value = line.substr(split + 1);
        if (key == "quest_loot_defaults") read_bool(value, quest_loot_defaults);
        else if (key == "overlay_visible") read_bool(value, settings.overlay_visible);
        else if (key == "lockpick_guide") read_bool(value, settings.lockpick_guide);
        else if (key == "dialogue_affinity") read_bool(value, settings.dialogue_affinity);
        else if (key == "companion_information") read_bool(value, settings.companion_information);
        else if (key == "hacking_guide") read_bool(value, settings.hacking_guide);
        else if (key == "trap_warnings") read_bool(value, settings.trap_warnings);
        else if (key == "trap_warning_range") read_float(value, settings.trap_warning_range, 100.0f, 10000.0f);
        else if (key == "world_labels") read_bool(value, settings.world_labels);
        else if (key == "world_skeletons") read_bool(value, settings.world_skeletons);
        else if (key == "hide_dead_actors") read_bool(value, settings.hide_dead_actors);
        else if (key == "label_radius") read_float(value, settings.label_radius, 500.0f, 100000.0f);
        else if (key == "label_limit") read_int(value, settings.label_limit, 1, 30);
        else if (key == "name_filter") {
            if (value.size() < 256 && value.find('\0') == std::string::npos)
                settings.name_filter = value;
        }
        else if (key == "aim_assist") read_bool(value, settings.aim_assist);
        else if (key == "aim_radius") read_float(value, settings.aim_radius, 40.0f, 500.0f);
        else if (key == "aim_distance") read_float(value, settings.aim_distance, 100.0f, 15000.0f);
        else if (key == "instant_kill") read_bool(value, settings.instant_kill);
        else if (key == "invulnerable") read_bool(value, settings.invulnerable);
        else if (key == "unlimited_action_points") read_bool(value, settings.unlimited_action_points);
        else if (key == "no_recoil") read_bool(value, settings.no_recoil);
        else if (key == "auto_loot") read_bool(value, settings.auto_loot);
        else if (key == "auto_loot_inventories") read_bool(value, settings.auto_loot_inventories);
        else if (key == "auto_harvest") read_bool(value, settings.auto_harvest);
        else if (key == "auto_loot_owned" || key == "auto_loot_steal")
            read_bool(value, settings.auto_loot_steal);
        else if (key == "auto_loot_range") read_float(value, settings.auto_loot_range, 50.0f, 1000.0f);
        else if (key == "loot_junk") read_bool(value, settings.loot_junk);
        else if (key == "loot_weapons") read_bool(value, settings.loot_weapons);
        else if (key == "loot_apparel") read_bool(value, settings.loot_apparel);
        else if (key == "loot_aid") read_bool(value, settings.loot_aid);
        else if (key == "loot_ammo") read_bool(value, settings.loot_ammo);
        else if (key == "loot_books") read_bool(value, settings.loot_books);
        else if (key == "loot_ingredients") read_bool(value, settings.loot_ingredients);
        else if (key == "loot_keys") read_bool(value, settings.loot_keys);
        else if (key == "loot_notes") read_bool(value, settings.loot_notes);
        else if (key == "loot_other_misc") read_bool(value, settings.loot_other_misc);
        else if (key == "radar_map_background") read_bool(value, settings.radar_map_background);
        else if (key == "radar_map_opacity") read_float(value, settings.radar_map_opacity, 0.15f, 1.0f);
        else if (key == "radar_doors") read_bool(value, settings.radar_doors);
        else if (key == "radar_containers") read_bool(value, settings.radar_containers);
        else if (key == "radar_items") read_bool(value, settings.radar_items);
        else if (key == "radar_actors") read_bool(value, settings.radar_actors);
        else if (key == "radar_crops") read_bool(value, settings.radar_crops);
        else if (key == "radar_range") read_float(value, settings.radar_range, 100.0f, 100000.0f);
        else if (key == "radar_interior_range") read_float(value, settings.radar_interior_range, 100.0f, 100000.0f);
    }
    if (file.bad()) { error = "Cannot read settings"; return false; }
    // Old files enabled every category by default. Migrate once; after the
    // next save, explicit category opt-ins are preserved.
    if (!quest_loot_defaults) settings.loot_books = settings.loot_keys = settings.loot_notes = false;
    return true;
}

bool fo4hud_save_settings(const OverlaySettings& settings, std::string& error) {
    error.clear();
    const std::string path = fo4hud_settings_path();
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), code);
    if (code) { error = "Cannot create settings directory: " + code.message(); return false; }
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) { error = "Cannot create settings: " + std::string(std::strerror(errno)); return false; }
    FILE* file = fdopen(fd, "w");
    if (!file) {
        error = "Cannot open settings stream: " + std::string(std::strerror(errno));
        close(fd);
        unlink(temporary.c_str());
        return false;
    }
    std::string filter = settings.name_filter.substr(0, 255);
    std::replace(filter.begin(), filter.end(), '\n', ' ');
    std::replace(filter.begin(), filter.end(), '\r', ' ');
    const int written = std::fprintf(file,
        "# fo4hud overlay preferences\n"
        "overlay_visible=%d\n"
        "lockpick_guide=%d\n"
        "hacking_guide=%d\n"
        "companion_information=%d\ndialogue_affinity=%d\n"
        "trap_warnings=%d\ntrap_warning_range=%.9g\n"
        "world_labels=%d\nworld_skeletons=%d\nhide_dead_actors=%d\n"
        "label_radius=%.9g\nlabel_limit=%d\nname_filter=%s\n"
        "aim_assist=%d\naim_radius=%.9g\naim_distance=%.9g\n"
        "instant_kill=%d\ninvulnerable=%d\nunlimited_action_points=%d\nno_recoil=%d\n"
        "quest_loot_defaults=1\nauto_loot=%d\nauto_loot_inventories=%d\nauto_harvest=%d\nauto_loot_steal=%d\nauto_loot_range=%.9g\n"
        "loot_junk=%d\nloot_weapons=%d\nloot_apparel=%d\nloot_aid=%d\nloot_ammo=%d\n"
        "loot_books=%d\nloot_ingredients=%d\nloot_keys=%d\nloot_notes=%d\nloot_other_misc=%d\n"
        "radar_map_background=%d\nradar_map_opacity=%.9g\n"
        "radar_doors=%d\nradar_containers=%d\nradar_items=%d\nradar_actors=%d\nradar_crops=%d\nradar_range=%.9g\nradar_interior_range=%.9g\n",
        settings.overlay_visible,
        settings.lockpick_guide,
        settings.hacking_guide,
        settings.companion_information, settings.dialogue_affinity,
        settings.trap_warnings, settings.trap_warning_range,
        settings.world_labels, settings.world_skeletons, settings.hide_dead_actors,
        settings.label_radius, settings.label_limit, filter.c_str(),
        settings.aim_assist, settings.aim_radius, settings.aim_distance,
        settings.instant_kill, settings.invulnerable,
        settings.unlimited_action_points, settings.no_recoil,
        settings.auto_loot, settings.auto_loot_inventories, settings.auto_harvest,
        settings.auto_loot_steal,
        settings.auto_loot_range,
        settings.loot_junk, settings.loot_weapons, settings.loot_apparel,
        settings.loot_aid, settings.loot_ammo, settings.loot_books,
        settings.loot_ingredients, settings.loot_keys, settings.loot_notes,
        settings.loot_other_misc,
        settings.radar_map_background, settings.radar_map_opacity,
        settings.radar_doors, settings.radar_containers, settings.radar_items,
        settings.radar_actors, settings.radar_crops, settings.radar_range, settings.radar_interior_range);
    const int flushed = std::fflush(file);
    const int synced = flushed == 0 ? fsync(fd) : -1;
    const int closed = std::fclose(file);
    const bool good = written > 0 && flushed == 0 && synced == 0 && closed == 0;
    if (!good) {
        error = "Cannot write settings: " + std::string(std::strerror(errno));
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        error = "Cannot replace settings: " + std::string(std::strerror(errno));
        unlink(temporary.c_str());
        return false;
    }
    return true;
}
