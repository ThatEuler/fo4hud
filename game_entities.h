#pragma once

#include "radar_map.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class Fo4InventoryState : uint8_t { Unknown, Empty, HasItems };

struct Fo4Entity {
    uintptr_t address = 0;
    uint32_t handle = 0;
    uint8_t process_level = 0;
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    uint8_t radar_category = 0; // 1 loading entrance, 2 container, 3 item, 4 actor, 5 flora
    Fo4InventoryState inventory_state = Fo4InventoryState::Unknown;
    // Presentation retains the last confirmed result while a new check runs.
    // Loot eligibility uses inventory_state, which can expire to Unknown.
    Fo4InventoryState inventory_display_state = Fo4InventoryState::Unknown;
    Fo4InventoryState display_inventory_state() const {
        return inventory_display_state == Fo4InventoryState::Unknown ? inventory_state : inventory_display_state;
    }
    uintptr_t cell_address = 0;
    uintptr_t worldspace_address = 0;
    bool cell_is_interior = false;
    float position[3]{};
    std::string name;
};

struct Fo4EntitySnapshot {
    std::string status;
    std::string actor_status;
    uintptr_t image_address = 0;
    uintptr_t player_address = 0;
    float player_position[3]{};
    float player_heading_radians = 0.0f;
    bool player_heading_valid = false;
    uintptr_t cell_address = 0;
    uintptr_t worldspace_address = 0;
    bool cell_is_interior = false;
    uintptr_t entries_address = 0;
    uintptr_t process_lists_address = 0;
    uintptr_t handle_table_address = 0;
    uint32_t cell_form_id = 0;
    uint32_t capacity = 0;
    uint32_t total_references = 0;
    uint32_t actor_handle_counts[4]{};
    std::vector<Fo4Entity> entities;
    std::vector<Fo4Entity> actors;
    std::vector<Fo4Entity> trap_candidates;
};

enum class Fo4SynthStatus : uint8_t { Unknown, Confirmed };

// Missing values stay unavailable rather than being presented as zero.
struct Fo4CompanionState {
    uintptr_t address = 0;
    uint32_t handle = 0;
    uint32_t form_id = 0;
    std::string name;
    Fo4SynthStatus synth_status = Fo4SynthStatus::Unknown;
    std::string race = "Unknown";
    bool is_companion = false;
    std::optional<float> affinity, health, max_health, action_points, max_action_points;
    std::array<std::optional<float>, 7> special;
    std::optional<float> inventory_weight, carry_weight, damage_resistance, energy_resistance;
};
// Native actor-value getters are sampled only from PlayerCharacter::Update.
void fo4hud_companion_tick(uintptr_t image, void* player);
bool fo4hud_read_companion_state(Fo4CompanionState& state);

struct Fo4Projection {
    float matrix[4][4]{};
    float port[4]{};
};

struct Fo4BoneSegment {
    float from[3]{};
    float to[3]{};
};

enum class Fo4BodyPart : uint8_t { Head, Chest, LeftArm, RightArm, LeftLeg, RightLeg };
struct Fo4AimPoint {
    Fo4BodyPart part{};
    float position[3]{};
};

// Returns zero unless the loaded Fallout4.exe matches the supported build.
uintptr_t fo4hud_supported_game_image();
// The verified game's MenuCursor registration state; false if unavailable.
bool fo4hud_game_menu_cursor_active();

struct Fo4LockpickState {
    float pick_degrees = 0;
    float sweet_spot_degrees = 0;
    float sweet_spot_width = 0; // Full angular width, not half-width.
};
// Checked, read-only view of the active 1.11.240 lockpicking menu.
bool fo4hud_read_lockpick_state(Fo4LockpickState& state);

struct Fo4HackingState {
    std::array<char, 384> board{}; // Two columns of 16 rows, 12 characters per row.
    std::string password;
    uint32_t password_position = 0;
};
bool fo4hud_read_hacking_state(Fo4HackingState& state);

// Snapshot actors and references in the player's cell plus loaded exterior
// cells in the same worldspace. No game functions are called.
Fo4EntitySnapshot fo4hud_read_current_cell();

struct Fo4EntityScanOptions {
    float actor_range = 0;
    float radar_range = 0;
    float loot_range = 0;
    float trap_range = 0;
    uint8_t radar_categories = 0; // bit N selects radar category N
    bool actor_names = false;
    bool corpse_inventories = false; // Empty-corpse colors for world labels.
    bool loot_items = false;
    bool loot_inventories = false;
    bool harvest = false;
    bool operator==(const Fo4EntityScanOptions& other) const;
};

struct Fo4EntityScanStats {
    uint64_t actor_passes = 0;
    uint64_t world_passes = 0;
    uint64_t distant_skipped = 0;
    uint64_t inventory_steps = 0;
    uint64_t inventory_probes = 0;
    size_t cached_names = 0;
    size_t cached_forms = 0;
    double last_slice_ms = 0;
};

// Incremental overlay view. Each update runs bounded units of checked reads
// until its soft time budget expires. Completed lists replace previous lists;
// changing player/cell/worldspace cancels pending work and clears stale data.
// The synchronous full snapshot above remains available to explicit MCP reads.
class Fo4EntityRefresh {
public:
    using Clock = std::chrono::steady_clock;
    Fo4EntityRefresh();
    ~Fo4EntityRefresh();
    bool update(const Fo4EntityScanOptions& options,
                std::chrono::microseconds budget = std::chrono::microseconds(1500),
                Clock::time_point now = Clock::now());
    const Fo4EntitySnapshot& snapshot() const;
    const Fo4EntityScanStats& stats() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
std::string fo4hud_entity_log_path();
bool fo4hud_append_entity_log(const Fo4EntitySnapshot& snapshot, std::string& error);
bool fo4hud_read_actor_position(const Fo4Entity& actor, float position[3]);
// Checks whether a snapshotted world reference is still present and enabled.
bool fo4hud_world_reference_visible(const Fo4Entity& entity);
// A FLOR reference with produce that has not already been harvested.
bool fo4hud_flora_harvestable(uintptr_t reference);
// Recognize only the supported scripted cap-stash activator base form.
bool fo4hud_is_cap_stash_base(uintptr_t base);
// Category of a supported scripted loot activator, or zero.
uint32_t fo4hud_scripted_loot_category_for_base(uintptr_t base);
// A modeled DOOR with ExtraTeleport to another cell; excludes instant
// teleports and invisible/model-less placeholder doors.
bool fo4hud_reference_has_loading_transition(uintptr_t reference, uintptr_t source_cell,
                                             uintptr_t handle_table = 0);
// Rejects workshop workbenches and settlement resource containers. An
// unreadable container is rejected so auto loot cannot drain it by mistake.
bool fo4hud_container_auto_loot_safe(uintptr_t base);
// Game-thread only. Every bound script must be reviewed and in a safe state;
// unavailable inspection is unsafe. Does not execute scripts or activate the reference.
bool fo4hud_reference_scripts_allow_auto_loot(uintptr_t image, uintptr_t reference);
// Game-thread only. Approves a nonempty collection-alias binding set containing
// only recollectionaliasscript and/or DefaultCollectionAliasOnLoad.
bool fo4hud_collection_alias_scripts_allow_auto_loot(uintptr_t image, uintptr_t alias);
// Conservative ownership checks for auto loot. Missing or inconsistent
// ownership data is treated as unsafe.
bool fo4hud_extra_list_unowned(uintptr_t extra_list);
bool fo4hud_reference_unowned(uintptr_t reference, uintptr_t cell);
bool fo4hud_base_container_unowned(uintptr_t base, uint8_t base_type);
bool fo4hud_player_in_snapshot_cell(const Fo4EntitySnapshot& snapshot);
bool fo4hud_actor_in_player_space(const Fo4EntitySnapshot& snapshot, const Fo4Entity& actor);
bool fo4hud_read_actor_life_state(const Fo4Entity& actor, uint8_t& life_state);
bool fo4hud_read_actor_label_position(const Fo4Entity& actor, const float origin[3],
                                      float position[3]);
bool fo4hud_read_actor_head_position(const Fo4Entity& actor, const float origin[3],
                                     float position[3]);
bool fo4hud_read_actor_skeleton(const Fo4Entity& actor, const float origin[3],
                                std::vector<Fo4BoneSegment>& segments,
                                std::vector<Fo4AimPoint>* aim_points = nullptr);
bool fo4hud_read_player_position(const Fo4EntitySnapshot& snapshot, float position[3]);
bool fo4hud_read_player_heading(const Fo4EntitySnapshot& snapshot, float& heading_radians);
bool fo4hud_read_projection(const Fo4EntitySnapshot& snapshot, Fo4Projection& projection);
bool fo4hud_projection_camera_position(const Fo4Projection& projection, float position[3]);
bool fo4hud_project_world(const Fo4Projection& projection, const float position[3],
                          float width, float height, float screen[2]);

// Read the exterior worldspace's Pip-Boy texture and map-cell bounds.
bool fo4hud_read_world_map(const Fo4EntitySnapshot& snapshot, Fo4MapInfo& map);

enum class Fo4TrapKind : uint8_t { None, Mine, Wire, PressurePlate, Gun, Flame, Electric, Radiation, Explosive, Alarm, Other, Monkey };
enum class Fo4TrapActivity : uint8_t { Unknown, Armed, Inactive, Sprung };
struct Fo4TrapMarker {
    Fo4Entity entity;
    Fo4TrapKind kind = Fo4TrapKind::None;
    Fo4TrapActivity activity = Fo4TrapActivity::Unknown;
};
// Candidates come from the bounded cell scanner. Script inspection runs on
// the game thread; render-time validation never calls the engine.
void fo4hud_submit_trap_candidates(const Fo4EntitySnapshot& snapshot, bool enabled);
void fo4hud_traps_tick(uintptr_t image, void* player);
std::vector<Fo4TrapMarker> fo4hud_read_trap_markers();
bool fo4hud_trap_reference_live(const Fo4Entity& entity, float position[3], bool allow_destroyed = false);
