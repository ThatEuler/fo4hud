#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

// Keep render-thread scheduling and game-thread processing at the same rate.
inline constexpr auto kFo4AutoLootInterval = std::chrono::milliseconds(50);
inline constexpr auto kFo4AutoLootRepeatInterval = std::chrono::milliseconds(150);

struct Fo4Entity;
struct Fo4AimPoint;

enum class Fo4LootAccess : uint8_t { Unknown, Allowed, Stealing };
struct Fo4LootAccessCandidate {
    uintptr_t reference = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
};
// Ownership decisions run on the game thread; the radar only reads results.
void fo4hud_combat_queue_loot_access(const std::vector<Fo4LootAccessCandidate>& candidates,
                                    uintptr_t player, uintptr_t player_cell);
Fo4LootAccess fo4hud_combat_loot_access(const Fo4Entity& entity, uintptr_t player, uintptr_t player_cell);

struct Fo4VisibilityPoint {
    uint8_t part = 0;
    float position[3]{};
    bool exposed = false;
};
struct Fo4VisibilityResult {
    uintptr_t actor = 0;
    uintptr_t cell = 0;
    std::chrono::steady_clock::time_point checked{};
    std::vector<Fo4VisibilityPoint> points;
};
struct Fo4HostilityCandidate {
    uintptr_t actor = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
};
struct Fo4ShotTarget {
    uintptr_t actor = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
    uint8_t part = 0;
    float position[3]{};
};
void fo4hud_combat_set_shot_target(const Fo4ShotTarget& target);
bool fo4hud_combat_shot_redirect_active();
void fo4hud_combat_queue_hostility(const std::vector<Fo4HostilityCandidate>& candidates);
bool fo4hud_combat_actor_hostile(uintptr_t actor, uint32_t form_id, uintptr_t cell);
void fo4hud_combat_queue_visibility(uintptr_t actor, uint32_t form_id, uintptr_t cell,
                                    const float eye[3], const std::vector<Fo4AimPoint>& points);
Fo4VisibilityResult fo4hud_combat_visibility_result();
void fo4hud_combat_clear_visibility();

bool fo4hud_combat_install();
void fo4hud_combat_uninstall();
bool fo4hud_combat_active();
bool fo4hud_combat_enabled();
void fo4hud_combat_set_enabled(bool enabled);
bool fo4hud_combat_invulnerable();
void fo4hud_combat_set_invulnerable(bool enabled);
bool fo4hud_combat_unlimited_action_points();
void fo4hud_combat_set_unlimited_action_points(bool enabled);
bool fo4hud_combat_no_recoil();
void fo4hud_combat_set_no_recoil(bool enabled);
bool fo4hud_combat_auto_loot();
bool fo4hud_combat_loot_vendor_chests();
void fo4hud_combat_set_loot_vendor_chests(bool enabled);
bool fo4hud_combat_auto_harvest();
void fo4hud_combat_set_auto_loot(bool enabled);
void fo4hud_combat_set_auto_harvest(bool enabled);
void fo4hud_combat_set_auto_loot_inventories(bool enabled);
void fo4hud_combat_set_auto_loot_steal(bool enabled);
void fo4hud_combat_set_auto_loot_range(float world_units);
void fo4hud_combat_set_auto_loot_categories(uint32_t mask);
bool fo4hud_combat_queue_auto_loot(const Fo4Entity& item);
// Presentation only; every action performs fresh safety checks.
const char* fo4hud_combat_manual_loot_reason(const Fo4Entity& item);
uint64_t fo4hud_combat_blocked_action_point_drains();
const char* fo4hud_combat_status();
uint64_t fo4hud_combat_instant_hits();
uint64_t fo4hud_combat_protected_hits();
uint64_t fo4hud_combat_blocked_damage();
uint64_t fo4hud_combat_blocked_rads();
bool fo4hud_combat_read_perk_points(int& points);
bool fo4hud_combat_set_perk_points(int points, std::string& error);
bool fo4hud_combat_read_carry_weight(float& weight);
bool fo4hud_combat_set_carry_weight(int weight, std::string& error);
const char* fo4hud_combat_carry_weight_status();
bool fo4hud_combat_read_speed_mult(float& speed_mult);
bool fo4hud_combat_set_speed_mult(int speed_mult, std::string& error);
const char* fo4hud_combat_speed_mult_status();
bool fo4hud_combat_read_caps(int& caps);
bool fo4hud_combat_game_caps(int& caps);
bool fo4hud_combat_set_caps(int caps, std::string& error);
const char* fo4hud_combat_caps_status();
bool fo4hud_combat_read_movement(float& jump_height, float& safe_fall_height);
bool fo4hud_combat_set_movement(float jump_height, float safe_fall_height, std::string& error);
const char* fo4hud_combat_movement_status();

// Optional game-thread consumer used by the local MCP bridge.
void fo4hud_combat_set_control_tick(void (*callback)(uintptr_t, void*));
