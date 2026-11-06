#include "game_combat.h"
#include "game_entities.h"
#include "game_workshop.h"
#include "logging.h"
#include "loot_categories.h"
#include "loot_playability.h"
#include "loot_safety.h"

#include <sys/mman.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
// Fallout4.exe 1.11.240 (PE timestamp 6A727FCC). Actor::HandleHealthDamage
// is virtual slot 0x110. It runs after health changes and before the normal
// death path. All addresses are RVAs and are checked before patching.
constexpr uintptr_t kActorVtableRva = 0x25683B0;
constexpr uintptr_t kHealthHandlerRva = 0x0CD2AE0;
// Queued weapon hits call CombatDamage even when only a limb is damaged and
// the normal health-damage callback is skipped (notably on robots).
constexpr uintptr_t kCombatDamageCallRva = 0x0C4DC17;
constexpr uintptr_t kCombatDamageRva = 0x0CD2CA0;
// PlayerCharacter's ActorValueOwner subobject (+0x58), virtual slot 6.
constexpr uintptr_t kPlayerValueVtableRva = 0x256D450;
constexpr uintptr_t kPlayerValueModifierRva = 0x0CC9B40;
// Actor::AlterActorValue calls Actor slot 0x131 for every kDamage change,
// including direct callers (combat and falling) that bypass AV-owner slot 6.
// Player's implementation also implements the native god/immortal AV checks.
constexpr uintptr_t kPlayerAdjustDamageRva = 0x0D75170;
constexpr uintptr_t kPlayerUpdateRva = 0x0D58F50;
constexpr uintptr_t kSetPerkCountRva = 0x0D75FF0;
constexpr uintptr_t kPerkCountOffset = 0xCF1;
constexpr uintptr_t kPlayerPointerRva = 0x32DD370;
constexpr uintptr_t kHandleTablePointerRva = 0x30E5490;
constexpr uintptr_t kActorValueListRva = 0x30E7460;
constexpr uintptr_t kHealthActorValueOffset = 0xD8;
// Actor-value registration at 0x6F9400, Fallout4.exe 1.11.240.
// Perception/Endurance, attacks, mobility, rotation and brain condition.
// These describe body parts, independently of Health and power armor pieces.
constexpr uintptr_t kLimbActorValueOffsets[] = {
    0x178, 0xC8, 0x118, 0x1F8, 0x128, 0x208, 0x210, 0x78
};
// Actor-value registration at 0x6F9400: Rads is +464, flags 0x04000030.
// Unlike Health, its kDamage modifier accumulates in the positive direction.
constexpr uintptr_t kRadsActorValueOffset = 0x1D0;
constexpr uintptr_t kActionPointsActorValueOffset = 0x08;
constexpr uintptr_t kCarryWeightActorValueOffset = 0x80;
constexpr uintptr_t kSpeedMultActorValueOffset = 0x238;
constexpr uintptr_t kPlayerGetValueRva = 0x0CC93D0;
constexpr uintptr_t kPlayerGetModifierRva = 0x0CC9E00;
constexpr uintptr_t kGoldObjectPointerRva = 0x30E7918;
// Static float GMST records in Fallout4.exe 1.11.240.
constexpr uintptr_t kJumpSettingRva = 0x2F1D580;
constexpr uintptr_t kFallSettingRva = 0x2F1D490;
constexpr uintptr_t kJumpNameRva = 0x24F55E0;
constexpr uintptr_t kFallNameRva = 0x24F54E8;
constexpr uintptr_t kFloatSettingVtableRva = 0x2467CA8;
constexpr uintptr_t kPlayerVtableRva = 0x256C938;
constexpr uintptr_t kPlayerRemoveItemRva = 0x0C9ACD0;
constexpr uintptr_t kPlayerAddItemRva = 0x0C7AA20;
constexpr uintptr_t kActorGetGoldAmountRva = 0x0501D40;
// A live 98,810-cap addition produced 33,274: a loss of exactly 65,536.
// Keep each operation well below that boundary and verify the result.
constexpr int kCapsStep = 30000;
constexpr uintptr_t kImageSize = 0x425A000;
// 1.11.240: bhkPickData(from,to), TESObjectCELL::Pick, HasHit.
// Verified against the installed Fallout4.exe disassembly. The pick result's
// hit fraction is at +0x80 (hknpCollisionResult::fraction).
constexpr uintptr_t kPickInitRva = 0x02DC570;
constexpr uintptr_t kCellPickRva = 0x04CAB60;
constexpr uintptr_t kPickHasHitRva = 0x1878C50;
// Actor::IsHostileToActor, also called by the game's Papyrus wrapper at
// 0x1107A50 in Fallout4.exe 1.11.240.
constexpr uintptr_t kIsHostileToActorRva = 0x0C7BBA0;
// TESObjectWEAP::Fire calls Projectile::Launch at this instruction. Hooking
// this call covers ordinary weapon shots without changing the camera.
constexpr uintptr_t kWeaponLaunchCallRva = 0x047A096;
constexpr uintptr_t kProjectileLaunchRva = 0x0E421E0;
constexpr uintptr_t kDirectionToAnglesRva = 0x0EFAD10;
// Called by the 1.11.240 Actor.WouldBeStealing Papyrus wrapper at 0x10FC9E6.
// WouldBeStealing(ref) is !TESObjectREFR::IsAnOwner(player, true, false).
constexpr uintptr_t kIsAnOwnerRva = 0x0562C90;
// ObjectReference.Activate(player, false) uses this routine via the Papyrus
// wrapper at 0x1152CB0. It dispatches OnActivate for scripted loot activators.
constexpr uintptr_t kActivateReferenceRva = 0x05144B0;
// Protected actors remain killable by the player. Only essential and
// invulnerable actors are excluded from player-attributed instant kills.
constexpr uint32_t kRuntimeInstantKillExcluded = (1u << 18);
constexpr uint32_t kBaseInstantKillExcluded = (1u << 1) | (1u << 31);

using HealthFn = void (__attribute__((ms_abi)) *)(void*, void*);
using CombatDamageFn = uintptr_t (__attribute__((ms_abi)) *)(void*, void*);
uintptr_t __attribute__((ms_abi)) combat_damage_hook(void* actor, void* hit);
using GetValueFn = float (__attribute__((ms_abi)) *)(void*, void*);
using ModValueFn = void (__attribute__((ms_abi)) *)(void*, int32_t, void*, float);
using AdjustDamageFn = float (__attribute__((ms_abi)) *)(void*, void*, float);
using GetModifierFn = float (__attribute__((ms_abi)) *)(void*, int32_t, void*);
using SetPerkCountFn = void (__attribute__((ms_abi)) *)(void*, uint8_t);
using PlayerUpdateFn = void (__attribute__((ms_abi)) *)(void*, float);
using IsHostileToActorFn = bool (__attribute__((ms_abi)) *)(void*, void*);
using ProjectileLaunchFn = void* (__attribute__((ms_abi)) *)(void*, void*);
using DirectionToAnglesFn = void (__attribute__((ms_abi)) *)(const float*, float*, float*);
using IsAnOwnerFn = bool (__attribute__((ms_abi)) *)(void*, void*, bool, bool);
// AddObjectToContainer takes the smart pointer by value in the MS ABI; the
// nontrivial smart pointer is passed as a pointer to a temporary.
using AddItemFn = void (__attribute__((ms_abi)) *)(void*, void*, uintptr_t*, int32_t, void*, int32_t);
// RemoveItem returns an ObjectRefHandle through the hidden second argument.
using RemoveItemFn = void (__attribute__((ms_abi)) *)(void*, uint32_t*, void*);
using GetGoldAmountFn = int32_t (__attribute__((ms_abi)) *)(void*);
using PickUpObjectFn = void (__attribute__((ms_abi)) *)(void*, void*, int32_t, bool);
// TESFlora::Activate (TESForm virtual slot 0x40) uses the game's harvest path.
using ActivateReferenceFn = bool (__attribute__((ms_abi)) *)(void*, void*, void*, int32_t,
                                                             bool, int16_t, bool);
using ActivateFloraFn = bool (__attribute__((ms_abi)) *)(void*, void*, void*, uint8_t,
                                                         void*, int32_t);

std::mutex install_mutex;
std::atomic<bool> installed{false};
std::atomic<void (*)(uintptr_t, void*)> control_tick_callback{nullptr};
std::atomic<bool> enabled{false};
std::atomic<bool> invulnerable{false};
std::atomic<bool> unlimited_action_points{false};
std::atomic<bool> no_recoil{false};
std::atomic<bool> auto_loot{false};
std::atomic<bool> auto_harvest{false};
std::atomic<bool> auto_loot_inventories{false};
std::atomic<bool> auto_loot_steal{false};
std::atomic<float> auto_loot_range{200.0f};
std::atomic<uint32_t> auto_loot_categories{kFo4LootDefaultCategories};
std::mutex auto_loot_mutex;
std::mutex loot_access_mutex;
std::vector<Fo4LootAccessCandidate> loot_access_candidates;
uintptr_t loot_access_player = 0, loot_access_player_cell = 0;
size_t loot_access_cursor = 0;
std::chrono::steady_clock::time_point loot_access_requested{}, loot_access_last_check{};
struct LootAccessEntry {
    uint32_t form_id = 0;
    uintptr_t cell = 0;
    Fo4LootAccess access = Fo4LootAccess::Unknown;
    std::chrono::steady_clock::time_point checked{};
    const char* manual_reason = nullptr;
};
std::unordered_map<uintptr_t, LootAccessEntry> loot_access_results;
std::mutex visibility_mutex;
struct VisibilityRequest {
    uintptr_t actor = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
    uint64_t sequence = 0;
    float eye[3]{};
    std::vector<Fo4AimPoint> points;
};
VisibilityRequest pending_visibility;
Fo4VisibilityResult visibility_result;
uint64_t visibility_sequence = 0;
std::mutex hostility_mutex;
std::vector<Fo4HostilityCandidate> pending_hostility;
struct HostilityEntry {
    uint32_t form_id = 0;
    uintptr_t cell = 0;
    bool hostile = false;
    std::chrono::steady_clock::time_point checked{};
};
std::unordered_map<uintptr_t, HostilityEntry> hostility_results;
std::mutex shot_target_mutex;
Fo4ShotTarget shot_target;
std::chrono::steady_clock::time_point shot_target_updated{};
std::atomic<bool> shot_redirect_installed{false};
uint8_t* shot_launch_stub = nullptr;
uintptr_t shot_launch_call = 0;
int32_t original_launch_displacement = 0;
uint8_t* combat_damage_stub = nullptr;
uintptr_t combat_damage_call = 0;
int32_t original_combat_displacement = 0;
CombatDamageFn original_combat_damage = nullptr;
struct AutoLootCandidate {
    uintptr_t address = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
    uint8_t category = 0;
};
AutoLootCandidate pending_auto_loot;
std::atomic<uint64_t> instant_hits{0};
std::atomic<uint64_t> protected_hits{0};
std::atomic<uint64_t> blocked_damage{0};
std::atomic<uint64_t> blocked_rads{0};
std::atomic<uint64_t> blocked_ap_drains{0};
std::atomic<int> status{0}; // 0 not installed, 1 active, 2 unsupported, 3 patch failed
std::atomic<int> pending_carry_weight{-1};
std::atomic<int> carry_weight_status{0}; // 0 idle, 1 queued, 2 applied, 3 failed
std::atomic<int> pending_speed_mult{-1};
std::atomic<int> speed_mult_status{0};
std::atomic<int> pending_caps{-1};
std::atomic<int> active_caps_target{-1};
std::atomic<int> caps_status{0};
std::atomic<int> game_caps{-1};
std::atomic<uint64_t> pending_movement{UINT64_MAX};
std::atomic<int> movement_status{0};
uintptr_t image_base = 0;
void** patched_slot = nullptr;
void** patched_player_value_slot = nullptr;
void** patched_player_update_slot = nullptr;
void** patched_player_damage_slot = nullptr;
HealthFn original_health = nullptr;
ModValueFn original_player_value = nullptr;
PlayerUpdateFn original_player_update = nullptr;
AdjustDamageFn original_player_damage = nullptr;
thread_local bool changing_health = false;
struct CombatHitContext {
    void* actor;
    bool health_handled;
    CombatHitContext* previous;
};
thread_local CombatHitContext* active_combat_hit = nullptr;

struct RecoilModel {
    uintptr_t address;
    uintptr_t weapon;
    float min_degrees;
    float max_degrees;
};
// Only the player's equipped runtime models are touched. This is accessed on
// the game thread through PlayerCharacter::Update, not from the render thread.
std::vector<RecoilModel> saved_recoil_models;

bool read_memory(uintptr_t address, void* destination, size_t size) {
    if (!address || address > std::numeric_limits<uintptr_t>::max() - size) return false;
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

template <typename T> bool read_value(uintptr_t address, T& value) {
    return read_memory(address, &value, sizeof(value));
}

int page_protection(uintptr_t address) {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], perms[5];
    unsigned long start, end;
    int result = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (address >= start && address < end) {
            if (perms[0] == 'r') result |= PROT_READ;
            if (perms[1] == 'w') result |= PROT_WRITE;
            if (perms[2] == 'x') result |= PROT_EXEC;
            break;
        }
    }
    std::fclose(maps);
    return result;
}

bool set_slot(void** slot, void* expected, void* replacement) {
    const int protection = page_protection(reinterpret_cast<uintptr_t>(slot));
    if (!(protection & PROT_READ)) return false;
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return false;
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~(uintptr_t(size) - 1));
    if (mprotect(page, size, protection | PROT_WRITE) != 0) return false;
    void* previous = __sync_val_compare_and_swap(slot, expected, replacement);
    const bool replaced = previous == expected;
    const bool restored = mprotect(page, size, protection) == 0;
    return replaced && restored;
}

std::string combat_log_path() {
    std::string path = fo4hud_entity_log_path();
    const auto slash = path.rfind('/');
    return (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) + "combat.log";
}

void log_event(const char* action, uint32_t form_id = 0, float health = 0.0f) {
    const std::string path = combat_log_path();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (error) return;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    std::time_t now = std::time(nullptr);
    char line[192];
    const int count = std::snprintf(line, sizeof(line), "time=%lld pid=%ld action=%s form=%08X health=%.2f\n",
                                    static_cast<long long>(now), long(getpid()), action, form_id, health);
    if (count > 0) (void)write(fd, line, std::min(size_t(count), sizeof(line) - 1));
    close(fd);
}

bool pick_functions_match(uintptr_t image) {
    const uint8_t init[] = {0x48, 0x83, 0xEC, 0x58};
    const uint8_t cell[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
    const uint8_t hit[] = {0x83, 0xB9, 0xBC, 0x00, 0x00, 0x00, 0x00};
    uint8_t bytes[7]{};
    return read_memory(image + kPickInitRva, bytes, sizeof(init)) &&
           std::memcmp(bytes, init, sizeof(init)) == 0 &&
           read_memory(image + kCellPickRva, bytes, sizeof(cell)) &&
           std::memcmp(bytes, cell, sizeof(cell)) == 0 &&
           read_memory(image + kPickHasHitRva, bytes, sizeof(hit)) &&
           std::memcmp(bytes, hit, sizeof(hit)) == 0;
}

bool hostility_function_matches(uintptr_t image) {
    const uint8_t prolog[] = {0x48, 0x89, 0x5C, 0x24, 0x08,
                              0x48, 0x89, 0x74, 0x24, 0x10};
    uint8_t bytes[sizeof(prolog)]{};
    return read_memory(image + kIsHostileToActorRva, bytes, sizeof(bytes)) &&
           std::memcmp(bytes, prolog, sizeof(bytes)) == 0;
}

void release_pick_world(uint8_t* query);

bool shot_path_clear(uintptr_t cell, const float origin[3], const float target[3]) {
    if (!pick_functions_match(image_base)) return false;
    using InitFn = void (__attribute__((ms_abi)) *)(void*, uint32_t, const float*, const float*);
    using PickFn = void* (__attribute__((ms_abi)) *)(void*, void*);
    using HasHitFn = bool (__attribute__((ms_abi)) *)(void*);
    alignas(16) uint8_t query[0x100]{};
    reinterpret_cast<InitFn>(image_base + kPickInitRva)(query, 0x3002A, origin, target);
    (void)reinterpret_cast<PickFn>(image_base + kCellPickRva)(
        reinterpret_cast<void*>(cell), query);
    float fraction = 1.0f;
    const bool hit = reinterpret_cast<HasHitFn>(image_base + kPickHasHitRva)(query);
    if (hit) std::memcpy(&fraction, query + 0x80, sizeof(fraction));
    uintptr_t cast_world = 0;
    std::memcpy(&cast_world, query + 0xC0, sizeof(cast_world));
    release_pick_world(query);
    if (!cast_world || !std::isfinite(fraction)) return false;
    float distance_squared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float delta = target[axis] - origin[axis];
        distance_squared += delta * delta;
    }
    if (distance_squared < 1.0f || !std::isfinite(distance_squared)) return false;
    return !hit || (fraction >= 0.0f &&
                    (1.0f - fraction) * std::sqrt(distance_squared) <= 25.0f);
}

void* __attribute__((ms_abi)) shot_launch_hook(void* result, void* raw_data) {
    const auto launch = reinterpret_cast<ProjectileLaunchFn>(image_base + kProjectileLaunchRva);
    Fo4ShotTarget target;
    std::chrono::steady_clock::time_point updated;
    {
        std::lock_guard<std::mutex> lock(shot_target_mutex);
        target = shot_target;
        updated = shot_target_updated;
    }
    const auto pass_through = [&](const char* reason) -> void* {
        if (fo4hud_aim_trace_enabled() && target.actor)
            fo4hud_log("aim", "shot unchanged form=%08X reason=%s", target.form_id, reason);
        return launch(result, raw_data);
    };
    const auto now = std::chrono::steady_clock::now();
    if (!shot_redirect_installed.load(std::memory_order_acquire) || !target.actor ||
        now - updated > std::chrono::milliseconds(150))
        return pass_through("no fresh target");

    const uintptr_t data = reinterpret_cast<uintptr_t>(raw_data);
    uintptr_t shooter = 0, player = 0, actor_cell = 0, player_cell = 0;
    uint32_t form_id = 0, state = 0;
    uint8_t type = 0;
    uint16_t cell_flags = 0;
    float origin[3]{};
    if (!read_value(image_base + kPlayerPointerRva, player) || !player ||
        !read_value(data + 0x20, shooter) || shooter != player ||
        !read_value(target.actor + 0x14, form_id) || form_id != target.form_id ||
        !read_value(target.actor + 0x1A, type) || type != 65 ||
        !read_value(target.actor + 0xB8, actor_cell) || actor_cell != target.cell ||
        !read_value(player + 0xB8, player_cell) || !player_cell ||
        !read_value(target.cell + 0x40, cell_flags) ||
        ((cell_flags & 1) && player_cell != target.cell) ||
        !read_value(target.actor + 0x130, state) || ((state >> 17) & 0xF) != 0 ||
        !read_memory(data, origin, sizeof(origin)) ||
        !hostility_function_matches(image_base))
        return pass_through("shooter or target changed");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(origin[i]) || !std::isfinite(target.position[i]))
            return pass_through("invalid position");
    if (!reinterpret_cast<IsHostileToActorFn>(image_base + kIsHostileToActorRva)(
            reinterpret_cast<void*>(target.actor), reinterpret_cast<void*>(player)))
        return pass_through("no longer hostile");
    if (!shot_path_clear(target.cell, origin, target.position))
        return pass_through("shot path blocked");

    float direction[3]{target.position[0] - origin[0],
                       target.position[1] - origin[1],
                       target.position[2] - origin[2]};
    float yaw = 0.0f, pitch = 0.0f;
    reinterpret_cast<DirectionToAnglesFn>(image_base + kDirectionToAnglesRva)(
        direction, &yaw, &pitch);
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return pass_through("invalid angle");
    std::memcpy(reinterpret_cast<void*>(data + 0x4C), &yaw, sizeof(yaw));
    std::memcpy(reinterpret_cast<void*>(data + 0x50), &pitch, sizeof(pitch));
    if (fo4hud_aim_trace_enabled())
        fo4hud_log("aim", "shot redirected form=%08X part=%u yaw=%.3f pitch=%.3f",
                   target.form_id, unsigned(target.part), yaw, pitch);
    return launch(result, raw_data);
}

bool patch_relative_call(uintptr_t call, int32_t displacement) {
    if (!call) return false;
    const int protection = page_protection(call);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (!(protection & PROT_EXEC) || page_size <= 0) return false;
    void* page = reinterpret_cast<void*>(call & ~(uintptr_t(page_size) - 1));
    if (mprotect(page, page_size, protection | PROT_WRITE) != 0) return false;
    std::memcpy(reinterpret_cast<void*>(call + 1), &displacement,
                sizeof(displacement));
    __builtin___clear_cache(reinterpret_cast<char*>(call),
                            reinterpret_cast<char*>(call + 5));
    return mprotect(page, page_size, protection) == 0;
}

bool install_relative_call(uintptr_t image, uintptr_t call_rva, uintptr_t target_rva,
                           uintptr_t hook, uintptr_t& site, int32_t& original, uint8_t*& stub) {
    if (stub) return false;
    site = image + call_rva;
    uint8_t call[5]{};
    if (!read_memory(site, call, sizeof(call)) || call[0] != 0xE8) return false;
    std::memcpy(&original, call + 1, sizeof(original));
    if (site + 5 + original != image + target_rva) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    // rel32 CALL cannot reach the ELF library from Fallout4.exe. Allocate a
    // small jump island inside the instruction's +/-2 GiB range.
    auto try_island = [&](uintptr_t candidate) {
        candidate &= ~(uintptr_t(page_size) - 1);
        const int64_t delta = int64_t(candidate) - int64_t(site + 5);
        if (delta > INT32_MAX || delta < INT32_MIN) return;
        void* mapped = mmap(reinterpret_cast<void*>(candidate), page_size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapped != MAP_FAILED) stub = static_cast<uint8_t*>(mapped);
    };
    for (int i = 0; i < 64 && !stub; ++i) {
        try_island(image + kImageSize + uintptr_t(i) * page_size);
        if (!stub)
            try_island(image - uintptr_t(i + 1) * page_size);
    }
    for (int i = 0; i < 96 && !stub; ++i) {
        try_island(image + 0x30000000u + uintptr_t(i) * 0x1000000u);
    }
    if (!stub) return false;
    const uint8_t prefix[] = {0x48, 0xB8}; // movabs rax, hook; jmp rax
    const uint8_t suffix[] = {0xFF, 0xE0};
    std::memcpy(stub, prefix, sizeof(prefix));
    std::memcpy(stub + 2, &hook, sizeof(hook));
    std::memcpy(stub + 10, suffix, sizeof(suffix));
    if (mprotect(stub, page_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(stub, page_size);
        stub = nullptr;
        return false;
    }
    const int32_t displacement = int32_t(reinterpret_cast<uintptr_t>(stub) -
                                         (site + 5));
    if (!patch_relative_call(site, displacement)) {
        // A failed restore of page permissions can leave the call patched.
        // Keep the island executable in that case so a fired shot stays safe.
        uint8_t current[5]{};
        if (read_memory(site, current, sizeof(current)) &&
            std::memcmp(current + 1, &displacement, sizeof(displacement)) == 0) {
            fo4hud_log("aim", "relative call patched but page restore failed");
            return false;
        }
        munmap(stub, page_size);
        stub = nullptr;
        return false;
    }
    return true;
}

bool install_shot_launch_hook(uintptr_t image) {
    shot_launch_call = image + kWeaponLaunchCallRva;
    uint8_t call[5]{};
    const uint8_t direction_prolog[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
    const uint8_t launch_prolog[] = {0x48, 0x89, 0x4C, 0x24, 0x08};
    uint8_t prolog[sizeof(direction_prolog)]{};
    if (!read_memory(shot_launch_call, call, sizeof(call)) || call[0] != 0xE8 ||
        !read_memory(image + kDirectionToAnglesRva, prolog, sizeof(prolog)) ||
        std::memcmp(prolog, direction_prolog, sizeof(prolog)) != 0 ||
        !read_memory(image + kProjectileLaunchRva, prolog, sizeof(prolog)) ||
        std::memcmp(prolog, launch_prolog, sizeof(prolog)) != 0) return false;
    std::memcpy(&original_launch_displacement, call + 1, sizeof(int32_t));
    if (shot_launch_call + 5 + original_launch_displacement !=
        image + kProjectileLaunchRva) return false;
    if (!install_relative_call(image, kWeaponLaunchCallRva, kProjectileLaunchRva,
                               reinterpret_cast<uintptr_t>(shot_launch_hook), shot_launch_call,
                               original_launch_displacement, shot_launch_stub)) return false;
    shot_redirect_installed.store(true, std::memory_order_release);
    fo4hud_log("aim", "shot redirect installed call=%p stub=%p launch=%p",
               reinterpret_cast<void*>(shot_launch_call), shot_launch_stub,
               reinterpret_cast<void*>(image + kProjectileLaunchRva));
    return true;
}

void uninstall_shot_launch_hook() {
    shot_redirect_installed.store(false, std::memory_order_release);
    if (!shot_launch_stub) return;
    if (!patch_relative_call(shot_launch_call, original_launch_displacement)) {
        fo4hud_log("aim", "shot redirect call restore failed; retaining jump island");
        return;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) munmap(shot_launch_stub, page_size);
    shot_launch_stub = nullptr;
    shot_launch_call = 0;
}

void check_hostility_on_game_thread(void* player) {
    static auto last_check = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_check < std::chrono::milliseconds(75)) return;
    std::vector<Fo4HostilityCandidate> candidates;
    {
        std::lock_guard<std::mutex> lock(hostility_mutex);
        if (pending_hostility.empty()) return;
        candidates.swap(pending_hostility);
    }
    last_check = now;
    if (!hostility_function_matches(image_base)) {
        static bool warned = false;
        if (!warned) {
            fo4hud_log("aim", "hostility unavailable: game function changed");
            warned = true;
        }
        return;
    }
    const auto is_hostile = reinterpret_cast<IsHostileToActorFn>(image_base + kIsHostileToActorRva);
    std::unordered_map<uintptr_t, HostilityEntry> checked;
    size_t hostile_count = 0;
    for (const auto& candidate : candidates) {
        uint32_t form_id = 0;
        uintptr_t cell = 0;
        uint8_t type = 0;
        if (!candidate.actor || candidate.actor == reinterpret_cast<uintptr_t>(player) ||
            !read_value(candidate.actor + 0x14, form_id) || form_id != candidate.form_id ||
            !read_value(candidate.actor + 0x1A, type) || type != 65 ||
            !read_value(candidate.actor + 0xB8, cell) || cell != candidate.cell) continue;
        const bool hostile = is_hostile(reinterpret_cast<void*>(candidate.actor), player);
        hostile_count += hostile;
        checked[candidate.actor] = {form_id, cell, hostile, now};
    }
    if (fo4hud_aim_trace_enabled()) {
        static auto last_trace = std::chrono::steady_clock::time_point{};
        if (now - last_trace >= std::chrono::milliseconds(500)) {
            last_trace = now;
            fo4hud_log("aim", "hostility candidates=%zu checked=%zu hostile=%zu",
                       candidates.size(), checked.size(), hostile_count);
        }
    }
    std::lock_guard<std::mutex> lock(hostility_mutex);
    for (const auto& entry : checked) hostility_results[entry.first] = entry.second;
    for (auto it = hostility_results.begin(); it != hostility_results.end();) {
        if (now - it->second.checked > std::chrono::seconds(1))
            it = hostility_results.erase(it);
        else ++it;
    }
}

// The game's stack allocated bhkPickData destructors release castWorld at
// +0xC0. Match that release after each pick to avoid retaining Havok worlds.
void release_pick_world(uint8_t* query) {
    void* world = nullptr;
    std::memcpy(&world, query + 0xC0, sizeof(world));
    if (!world) return;
    auto* references = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(world) + 8);
    uint32_t old = __atomic_load_n(references, __ATOMIC_ACQUIRE);
    while (uint16_t(old >> 16) != 0) {
        const uint32_t next = (old & 0xFFFF0000u) | uint16_t(old - 1);
        if (__atomic_compare_exchange_n(references, &old, next, true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (uint16_t(next) == 0) {
                void** vtable = *reinterpret_cast<void***>(world);
                using DestroyFn = void (__attribute__((ms_abi)) *)(void*);
                reinterpret_cast<DestroyFn>(vtable[3])(world);
            }
            break;
        }
    }
}

void check_visibility_on_game_thread(void* player) {
    static uint64_t processed = 0;
    static auto last_check = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_check < std::chrono::milliseconds(40)) return;
    VisibilityRequest request;
    {
        std::lock_guard<std::mutex> lock(visibility_mutex);
        if (!pending_visibility.actor || pending_visibility.sequence == processed) return;
        request = pending_visibility;
    }
    processed = request.sequence;
    last_check = now;
    Fo4VisibilityResult result;
    result.actor = request.actor;
    result.cell = request.cell;
    result.checked = now;
    if (!pick_functions_match(image_base)) {
        static bool warned = false;
        if (!warned) { fo4hud_log("aim", "visibility unavailable: pick functions changed"); warned = true; }
        return;
    }
    uint32_t current_form = 0;
    uintptr_t player_cell = 0;
    uint8_t cell_type = 0;
    if (!read_value(request.actor + 0x14, current_form) ||
        current_form != request.form_id ||
        !read_value(reinterpret_cast<uintptr_t>(player) + 0xB8, player_cell) ||
        !player_cell || !read_value(request.cell + 0x1A, cell_type) || cell_type != 63)
        return;
    // For exteriors, Pick uses the attached Havok world shared by cells.
    // An interior's physics world is separate, so require the exact cell.
    uint16_t cell_flags = 0;
    if (!read_value(request.cell + 0x40, cell_flags) ||
        ((cell_flags & 1) && player_cell != request.cell)) return;
    using InitFn = void (__attribute__((ms_abi)) *)(void*, uint32_t, const float*, const float*);
    using PickFn = void* (__attribute__((ms_abi)) *)(void*, void*);
    using HasHitFn = bool (__attribute__((ms_abi)) *)(void*);
    const auto init = reinterpret_cast<InitFn>(image_base + kPickInitRva);
    const auto pick = reinterpret_cast<PickFn>(image_base + kCellPickRva);
    const auto has_hit = reinterpret_cast<HasHitFn>(image_base + kPickHasHitRva);
    for (const Fo4AimPoint& point : request.points) {
        alignas(16) uint8_t query[0x100]{};
        init(query, 0x3002A, request.eye, point.position);
        (void)pick(reinterpret_cast<void*>(request.cell), query);
        float fraction = 1.0f;
        const bool hit = has_hit(query);
        if (hit) std::memcpy(&fraction, query + 0x80, sizeof(fraction));
        uintptr_t cast_world = 0;
        std::memcpy(&cast_world, query + 0xC0, sizeof(cast_world));
        release_pick_world(query);
        float distance_squared = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float delta = point.position[axis] - request.eye[axis];
            distance_squared += delta * delta;
        }
        if (!cast_world || !std::isfinite(fraction) || distance_squared < 1.0f) continue;
        const float distance = std::sqrt(distance_squared);
        Fo4VisibilityPoint checked;
        checked.part = static_cast<uint8_t>(point.part);
        std::copy(point.position, point.position + 3, checked.position);
        checked.exposed = !hit || (fraction >= 0.0f &&
                                   (1.0f - fraction) * distance <= 25.0f);
        result.points.push_back(checked);
    }
    if (fo4hud_aim_trace_enabled()) {
        static auto last_trace = std::chrono::steady_clock::time_point{};
        if (now - last_trace >= std::chrono::milliseconds(250)) {
            last_trace = now;
            unsigned mask = 0;
            for (const auto& point : result.points)
                if (point.exposed) mask |= 1u << point.part;
            fo4hud_log("aim", "visibility form=%08X sampled=%zu exposed_mask=%02X eye=(%.1f,%.1f,%.1f)",
                       request.form_id, result.points.size(), mask,
                       request.eye[0], request.eye[1], request.eye[2]);
        }
    }
    std::lock_guard<std::mutex> lock(visibility_mutex);
    visibility_result = std::move(result);
}

// Returns false on incomplete data. In that case the cheat never adds damage.
bool actor_flags(void* actor, uint32_t& actor_flags_out, uint32_t& base_flags_out) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(actor);
    uintptr_t base = 0;
    uint8_t actor_type = 0, base_type = 0;
    return read_value(address + 0x1A, actor_type) && actor_type == 65 &&
           read_value(address + 0x43C, actor_flags_out) &&
           read_value(address + 0xE0, base) && base &&
           read_value(base + 0x1A, base_type) && base_type == 45 &&
           read_value(base + 0x70, base_flags_out);
}

bool read_player(uintptr_t& image, uintptr_t& player) {
    image = image_base ? image_base : fo4hud_supported_game_image();
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    return image && read_value(image + kPlayerPointerRva, player) && player &&
           read_value(player + 0x14, form_id) && form_id == 0x14 &&
           read_value(player + 0x1A, form_type) && form_type == 65;
}

struct CurrentRecoilModel {
    uintptr_t address;
    uintptr_t weapon;
};

bool player_recoil_models(uintptr_t player, std::vector<CurrentRecoilModel>& result) {
    uintptr_t process = 0, middle_high = 0, items = 0;
    uint32_t capacity = 0, count = 0;
    // Actor::currentProcess, AIProcess::middleHigh, then the equippedItems
    // BSTArray in MiddleHighProcessData (CommonLibF4/F4SE 1.11.240 layout).
    if (!read_value(player + 0x300, process) || !process ||
        !read_value(process + 0x08, middle_high) || !middle_high ||
        !read_value(middle_high + 0x288, items) ||
        !read_value(middle_high + 0x290, capacity) ||
        !read_value(middle_high + 0x298, count) ||
        count > capacity || count > 16 || (count && !items)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t item = items + uintptr_t(i) * 0x28;
        uintptr_t weapon = 0, data = 0, model = 0, owner = 0;
        uint8_t type = 0;
        if (!read_value(item, weapon) || !weapon ||
            !read_value(weapon + 0x1A, type) || type != 0x2B ||
            !read_value(item + 0x20, data) || !data ||
            !read_value(data + 0x20, model) || !model ||
            !read_value(model + 0x88, owner) || owner != player) continue;
        if (std::none_of(result.begin(), result.end(), [model](const auto& entry) {
                return entry.address == model;
            })) result.push_back({model, weapon});
    }
    return true;
}

void update_player_recoil(uintptr_t player) {
    const bool suppress = no_recoil.load(std::memory_order_acquire);
    if (!suppress && saved_recoil_models.empty()) return;
    std::vector<CurrentRecoilModel> current;
    if (!player_recoil_models(player, current)) return;
    // An unequipped model may already have been destroyed. Never dereference
    // a saved pointer unless it is still in the player's equipped item array.
    saved_recoil_models.erase(std::remove_if(saved_recoil_models.begin(), saved_recoil_models.end(),
        [&current](const RecoilModel& saved) {
            return std::none_of(current.begin(), current.end(), [&saved](const CurrentRecoilModel& active) {
                return active.address == saved.address && active.weapon == saved.weapon;
            });
        }), saved_recoil_models.end());
    for (const auto& active : current) {
        float min_degrees = 0, max_degrees = 0;
        if (!read_value(active.address + 0x24, min_degrees) ||
            !read_value(active.address + 0x20, max_degrees) ||
            !std::isfinite(min_degrees) || !std::isfinite(max_degrees) ||
            min_degrees < 0 || max_degrees < 0 ||
            min_degrees > 1000 || max_degrees > 1000) continue;
        auto saved = std::find_if(saved_recoil_models.begin(), saved_recoil_models.end(),
            [&active](const RecoilModel& entry) { return entry.address == active.address; });
        if (!suppress) {
            if (saved != saved_recoil_models.end() && min_degrees == 0 && max_degrees == 0 &&
                (page_protection(active.address + 0x20) & PROT_WRITE) &&
                (page_protection(active.address + 0x24) & PROT_WRITE)) {
                *reinterpret_cast<float*>(active.address + 0x20) = saved->max_degrees;
                *reinterpret_cast<float*>(active.address + 0x24) = saved->min_degrees;
                log_event("recoil_restored");
            }
            continue;
        }
        if (min_degrees == 0 && max_degrees == 0) continue;
        if (!(page_protection(active.address + 0x20) & PROT_WRITE) ||
            !(page_protection(active.address + 0x24) & PROT_WRITE)) continue;
        const bool changed = saved == saved_recoil_models.end() ||
                             saved->min_degrees != min_degrees || saved->max_degrees != max_degrees;
        if (saved == saved_recoil_models.end()) {
            saved_recoil_models.push_back({active.address, active.weapon, min_degrees, max_degrees});
        } else {
            saved->min_degrees = min_degrees;
            saved->max_degrees = max_degrees;
        }
        *reinterpret_cast<float*>(active.address + 0x20) = 0;
        *reinterpret_cast<float*>(active.address + 0x24) = 0;
        if (changed) {
            char action[96];
            std::snprintf(action, sizeof(action), "recoil_suppressed_min_%.3f_max_%.3f",
                          min_degrees, max_degrees);
            log_event(action);
        }
    }
    if (!suppress) saved_recoil_models.clear();
}

bool read_gold_object(uintptr_t image, uintptr_t& gold) {
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    return read_value(image + kGoldObjectPointerRva, gold) && gold &&
           read_value(gold + 0x14, form_id) && form_id == 0xF &&
           read_value(gold + 0x1A, form_type) && form_type == 0x23;
}

bool read_game_caps_on_game_thread(uintptr_t image, void* player, int& caps) {
    // Actor::GetGoldAmount is also used by the game's Papyrus API. It walks
    // the inventory under the engine's lock and sums all caps stacks.
    uint32_t prologue = 0;
    if (!player || !read_value(image + kActorGetGoldAmountRva, prologue) ||
        prologue != 0x28EC8348) return false;
    const int result = reinterpret_cast<GetGoldAmountFn>(image + kActorGetGoldAmountRva)(player);
    if (result < 0 || result > 1000000000) return false;
    caps = result;
    game_caps.store(result, std::memory_order_release);
    return true;
}

bool movement_settings(uintptr_t& jump_data, uintptr_t& fall_data) {
    const uintptr_t image = fo4hud_supported_game_image();
    if (!image) return false;
    struct SettingSpec { uintptr_t record, name; const char* expected; };
    const SettingSpec specs[] = {
        {kJumpSettingRva, kJumpNameRva, "fJumpHeightMin"},
        {kFallSettingRva, kFallNameRva, "fJumpFallHeightMin"}
    };
    uintptr_t* outputs[] = {&jump_data, &fall_data};
    for (int i = 0; i < 2; ++i) {
        uintptr_t vtable = 0, name = 0;
        char actual[32]{};
        const auto& spec = specs[i];
        if (!read_value(image + spec.record, vtable) ||
            !read_value(image + spec.record + 0x10, name) ||
            vtable != image + kFloatSettingVtableRva || name != image + spec.name ||
            !read_memory(name, actual, std::strlen(spec.expected) + 1) ||
            std::strcmp(actual, spec.expected) != 0) return false;
        *outputs[i] = image + spec.record + 8;
    }
    return true;
}

bool apply_movement_on_game_thread(float jump, float fall) {
    uintptr_t jump_data = 0, fall_data = 0;
    if (!movement_settings(jump_data, fall_data) ||
        !(page_protection(jump_data) & PROT_WRITE) ||
        !(page_protection(fall_data) & PROT_WRITE)) return false;
    *reinterpret_cast<float*>(jump_data) = jump;
    *reinterpret_cast<float*>(fall_data) = fall;
    float updated_jump = 0, updated_fall = 0;
    if (!fo4hud_combat_read_movement(updated_jump, updated_fall) ||
        updated_jump != jump || updated_fall != fall) return false;
    char action[96];
    std::snprintf(action, sizeof(action), "movement_jump_%.1f_fall_%.1f", jump, fall);
    log_event(action);
    return true;
}

// AP loss is the negative kDamage modifier of the player's ActionPoints AV.
// Restore that modifier through the native setter so the engine updates its
// dependent values and HUD. This also fills AP already spent when toggled on.
void refill_action_points() {
    if (!unlimited_action_points.load(std::memory_order_acquire) ||
        !installed.load(std::memory_order_acquire) || !original_player_value) return;
    uintptr_t image = 0, player = 0, ap_info = 0, vtable = 0, getter = 0;
    if (!read_player(image, player) || image != image_base ||
        !read_value(image + kActorValueListRva + kActionPointsActorValueOffset, ap_info) || !ap_info ||
        !read_value(player + 0x58, vtable) || vtable != image + kPlayerValueVtableRva ||
        !read_value(vtable + 7 * sizeof(void*), getter) ||
        getter != image + kPlayerGetModifierRva) return;
    void* owner = reinterpret_cast<void*>(player + 0x58);
    float spent = reinterpret_cast<GetModifierFn>(getter)(owner, 2, reinterpret_cast<void*>(ap_info));
    if (std::isfinite(spent) && spent < -0.001f && spent > -1000000.0f)
        original_player_value(owner, 2, reinterpret_cast<void*>(ap_info), -spent);
}

// Undo damage through the native modifier so dependent values and the HUD
// update too. Radiation must be cleared before restoring health's damage.
void clear_damage_modifier(void* owner, void* info, GetModifierFn getter, bool positive_damage) {
    const float damage = getter(owner, 2, info);
    if (std::isfinite(damage) &&
        (positive_damage ? damage > 0.0f : damage < 0.0f))
        original_player_value(owner, 2, info, -damage);
}

void clear_player_damage(void* actor) {
    if (!invulnerable.load(std::memory_order_acquire) || !original_player_value) return;
    uintptr_t player = 0, vtable = 0, getter = 0, health = 0, rads = 0;
    if (!read_value(image_base + kPlayerPointerRva, player) || !player ||
        reinterpret_cast<uintptr_t>(actor) != player ||
        !read_value(player + 0x58, vtable) || vtable != image_base + kPlayerValueVtableRva ||
        !read_value(vtable + 7 * sizeof(void*), getter) ||
        getter != image_base + kPlayerGetModifierRva ||
        !read_value(image_base + kActorValueListRva + kHealthActorValueOffset, health) || !health ||
        !read_value(image_base + kActorValueListRva + kRadsActorValueOffset, rads) || !rads) return;
    void* owner = reinterpret_cast<void*>(player + 0x58);
    clear_damage_modifier(owner, reinterpret_cast<void*>(rads), reinterpret_cast<GetModifierFn>(getter), true);
    clear_damage_modifier(owner, reinterpret_cast<void*>(health), reinterpret_cast<GetModifierFn>(getter), false);
    for (uintptr_t offset : kLimbActorValueOffsets) {
        uintptr_t limb = 0;
        if (read_value(image_base + kActorValueListRva + offset, limb) && limb)
            clear_damage_modifier(owner, reinterpret_cast<void*>(limb), reinterpret_cast<GetModifierFn>(getter), false);
    }
}

// This receives Actor*, not the ActorValueOwner subobject. Returning zero
// here prevents the damage write in Actor::AlterActorValue. Delegate health/limb
// healing, rad removal, and unrelated actor values to the native filter.
float __attribute__((ms_abi)) player_damage_hook(void* actor, void* value_info, float delta) {
    if (invulnerable.load(std::memory_order_relaxed) &&
        installed.load(std::memory_order_acquire) && image_base) {
        uintptr_t player = 0, info = 0;
        if (read_value(image_base + kPlayerPointerRva, player) && player &&
            reinterpret_cast<uintptr_t>(actor) == player) {
            if (delta < 0.0f &&
                read_value(image_base + kActorValueListRva + kHealthActorValueOffset, info) &&
                info && reinterpret_cast<uintptr_t>(value_info) == info) {
                blocked_damage.fetch_add(1, std::memory_order_relaxed);
                return 0.0f;
            }
            if (delta < 0.0f) {
                for (uintptr_t offset : kLimbActorValueOffsets) {
                    if (read_value(image_base + kActorValueListRva + offset, info) &&
                        info && reinterpret_cast<uintptr_t>(value_info) == info) {
                        blocked_damage.fetch_add(1, std::memory_order_relaxed);
                        return 0.0f;
                    }
                }
            }
            if (delta > 0.0f &&
                read_value(image_base + kActorValueListRva + kRadsActorValueOffset, info) &&
                info && reinterpret_cast<uintptr_t>(value_info) == info) {
                blocked_rads.fetch_add(1, std::memory_order_relaxed);
                return 0.0f;
            }
        }
    }
    return original_player_damage ? original_player_damage(actor, value_info, delta) : delta;
}

void __attribute__((ms_abi)) player_value_hook(void* owner, int32_t modifier,
                                                void* value_info, float delta) {
    if (!original_player_value) return;
    if (unlimited_action_points.load(std::memory_order_relaxed) &&
        installed.load(std::memory_order_acquire) && image_base &&
        modifier == 2 && std::isfinite(delta) && delta < 0.0f) {
        uintptr_t player = 0, value_expected = 0;
        if (read_value(image_base + kPlayerPointerRva, player) && player &&
            reinterpret_cast<uintptr_t>(owner) == player + 0x58) {
            if (unlimited_action_points.load(std::memory_order_relaxed) &&
                read_value(image_base + kActorValueListRva + kActionPointsActorValueOffset, value_expected) &&
                reinterpret_cast<uintptr_t>(value_info) == value_expected) {
                blocked_ap_drains.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    original_player_value(owner, modifier, value_info, delta);
}

bool apply_carry_weight_on_game_thread(void* actor, int weight) {
    uintptr_t image = 0, player = 0, info = 0, vtable = 0, modifier = 0;
    if (!read_player(image, player) || actor != reinterpret_cast<void*>(player) ||
        !read_value(image + kActorValueListRva + kCarryWeightActorValueOffset, info) || !info ||
        !read_value(player + 0x58, vtable) || vtable != image + kPlayerValueVtableRva ||
        !read_value(vtable + 6 * sizeof(void*), modifier) ||
        modifier != reinterpret_cast<uintptr_t>(player_value_hook)) return false;
    float current = 0.0f;
    if (!fo4hud_combat_read_carry_weight(current)) return false;
    const float delta = static_cast<float>(weight) - current;
    if (std::fabs(delta) > 0.01f)
        original_player_value(reinterpret_cast<void*>(player + 0x58), 0,
                              reinterpret_cast<void*>(info), delta);
    float updated = 0.0f;
    if (!fo4hud_combat_read_carry_weight(updated) ||
        std::fabs(updated - static_cast<float>(weight)) > 0.5f) return false;
    char action[80];
    std::snprintf(action, sizeof(action), "carry_weight_%.0f_to_%d", current, weight);
    log_event(action);
    return true;
}

bool apply_speed_mult_on_game_thread(void* actor, int speed_mult) {
    uintptr_t image = 0, player = 0, info = 0, vtable = 0, modifier = 0;
    if (!read_player(image, player) || actor != reinterpret_cast<void*>(player) ||
        !read_value(image + kActorValueListRva + kSpeedMultActorValueOffset, info) || !info ||
        !read_value(player + 0x58, vtable) || vtable != image + kPlayerValueVtableRva ||
        !read_value(vtable + 6 * sizeof(void*), modifier) ||
        modifier != reinterpret_cast<uintptr_t>(player_value_hook)) return false;
    float current = 0.0f;
    if (!fo4hud_combat_read_speed_mult(current)) return false;
    const float delta = static_cast<float>(speed_mult) - current;
    if (std::fabs(delta) > 0.01f)
        original_player_value(reinterpret_cast<void*>(player + 0x58), 0,
                              reinterpret_cast<void*>(info), delta);
    float updated = 0.0f;
    if (!fo4hud_combat_read_speed_mult(updated) ||
        std::fabs(updated - static_cast<float>(speed_mult)) > 0.5f) return false;
    char action[80];
    std::snprintf(action, sizeof(action), "speed_mult_%.0f_to_%d", current, speed_mult);
    log_event(action);
    return true;
}

enum class CapsStepResult { Failed, Progress, Complete };

CapsStepResult apply_caps_step_on_game_thread(void* actor, int caps) {
    uintptr_t image = 0, player = 0, gold = 0, vtable = 0, add = 0, remove = 0;
    if (!read_player(image, player) || actor != reinterpret_cast<void*>(player) ||
        !read_gold_object(image, gold) ||
        !read_value(player, vtable) || vtable != image + kPlayerVtableRva ||
        !read_value(vtable + 0x7A * sizeof(void*), add) || add != image + kPlayerAddItemRva ||
        !read_value(vtable + 0x6D * sizeof(void*), remove) ||
        remove != image + kPlayerRemoveItemRva) {
        log_event("caps_native_function_unavailable");
        return CapsStepResult::Failed;
    }
    int current = 0;
    if (!read_game_caps_on_game_thread(image, actor, current)) {
        log_event("caps_game_read_before_failed");
        return CapsStepResult::Failed;
    }
    if (current == caps) return CapsStepResult::Complete;
    const int difference = caps - current;
    const int step = std::clamp(difference, -kCapsStep, kCapsStep);
    if (step > 0) {
        uintptr_t no_extra_data = 0;
        reinterpret_cast<AddItemFn>(add)(reinterpret_cast<void*>(player),
            reinterpret_cast<void*>(gold), &no_extra_data, step, nullptr, 0);
    } else {
        alignas(8) unsigned char remove_data[0x48]{};
        std::memcpy(remove_data + 0x20, &gold, sizeof(gold));
        const int count = -step;
        std::memcpy(remove_data + 0x28, &count, sizeof(count));
        uint32_t result_handle = 0;
        reinterpret_cast<RemoveItemFn>(remove)(reinterpret_cast<void*>(player),
                                                &result_handle, remove_data);
    }
    int updated = -1;
    if (!read_game_caps_on_game_thread(image, actor, updated)) {
        log_event("caps_game_read_after_failed");
        return CapsStepResult::Failed;
    }
    int raw_count = -1;
    (void)fo4hud_combat_read_caps(raw_count);
    char action[128];
    std::snprintf(action, sizeof(action), "caps_step_from_%d_to_%d_target_%d_raw_%d",
                  current, updated, caps, raw_count);
    log_event(action);
    if (updated != current + step) return CapsStepResult::Failed;
    return updated == caps ? CapsStepResult::Complete : CapsStepResult::Progress;
}

bool loot_object_playable(uintptr_t object, uint8_t type) {
    return fo4hud_item_playable(object, type, [](uintptr_t address, auto& value) {
        return read_value(address, value);
    }).value_or(false);
}

bool loot_weapon_extra_playable(uintptr_t extra) {
    return fo4hud_weapon_instance_playable(extra, [](uintptr_t address, auto& value) {
        return read_value(address, value);
    }).value_or(false);
}

uint32_t loot_category_for_object(uintptr_t object) {
    uint8_t type = 0;
    if (!object || !read_value(object + 0x1A, type)) return 0;
    if (!loot_object_playable(object, type)) return 0;
    if (const uint32_t category = fo4hud_scripted_loot_category_for_base(object))
        return category;
    if (type != 0x23) return fo4hud_loot_category_for_form(type, false);

    // MISC with component data is scrap junk. Keep caps, loose mods and
    // other MISC objects in a separate category. An unreadable array is
    // unknown and is never looted through a category filter.
    uint32_t flags = 0;
    if (!read_value(object + 0x10, flags)) return 0;
    if (flags & 0x80u) return Fo4LootOtherMisc; // TESObjectMISC::IsLooseMod
    uintptr_t components = 0;
    if (!read_value(object + 0x158, components)) return 0;
    if (!components) return Fo4LootOtherMisc;
    uintptr_t entries = 0;
    uint32_t capacity = 0, count = 0;
    if (!read_value(components, entries) ||
        !read_value(components + 0x08, capacity) ||
        !read_value(components + 0x10, count) ||
        count > capacity || capacity > 1024 || (count && !entries)) return 0;
    return fo4hud_misc_loot_category(flags, count);
}

bool loot_category_enabled(uintptr_t object) {
    const uint32_t category = loot_category_for_object(object);
    return category && (auto_loot_categories.load(std::memory_order_acquire) & category);
}

bool loot_alias_scripts_clear(uintptr_t alias) {
    return fo4hud_collection_alias_scripts_allow_auto_loot(image_base, alias);
}

const char* loot_reference_reason(uintptr_t reference, bool inspect_scripts = true) {
    return fo4hud_loot_reference_reason(reference, [](uintptr_t address, auto& value) {
        return read_value(address, value);
    }, inspect_scripts ? loot_alias_scripts_clear : nullptr);
}

bool reference_no_crime(uintptr_t reference, bool& no_crime) {
    no_crime = false;
    uintptr_t extras = 0, node = 0;
    if (!read_value(reference + 0x100, extras)) return false;
    if (!extras) return true;
    if (!read_value(extras + 8, node)) return false;
    // ExtraDataList::HasNoCrime (27FEA0) finds ExtraOwnership (21) and
    // checks its +0x24 byte. TESProduceForm::Activate (31C640), normal pickup
    // (C7A000), and the activation prompt (518100) all honor this override
    // even when IsAnOwner is false. Graygarden mutfruit uses this flag.
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t type = 0, flag = 0;
        uintptr_t next = 0;
        if (!read_value(node + 0x12, type) || !read_value(node + 8, next) || next == node) return false;
        if (type == 0x21) {
            if (!read_value(node + 0x24, flag)) return false;
            no_crime = flag != 0;
            return true;
        }
        node = next;
    }
    return node == 0;
}

bool reference_would_be_stealing(uintptr_t image, uintptr_t player,
                                 uintptr_t reference, bool& would_steal) {
    bool no_crime = false;
    if (!reference_no_crime(reference, no_crime)) return false;
    if (no_crime) { would_steal = false; return true; }
    // Match the pickup/harvest theft decision, including its explicit
    // no-crime exception, rather than treating ownership alone as theft.
    // If the native permission check changes, fail closed.
    const uint8_t prolog[] = {0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56};
    uint8_t bytes[sizeof(prolog)]{};
    if (!read_memory(image + kIsAnOwnerRva, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, prolog, sizeof(prolog)) != 0) return false;
    const auto is_an_owner = reinterpret_cast<IsAnOwnerFn>(image + kIsAnOwnerRva);
    would_steal = !is_an_owner(reinterpret_cast<void*>(reference),
                              reinterpret_cast<void*>(player), true, false);
    return true;
}

void check_loot_access_on_game_thread(void* player,
        bool (*check)(uintptr_t, uintptr_t, uintptr_t, bool&) = reference_would_be_stealing) {
    const auto now = std::chrono::steady_clock::now();
    std::vector<Fo4LootAccessCandidate> batch;
    uintptr_t player_cell = 0;
    if (!read_value(reinterpret_cast<uintptr_t>(player) + 0xB8, player_cell) || !player_cell) return;
    {
        std::lock_guard<std::mutex> lock(loot_access_mutex);
        if (loot_access_player != reinterpret_cast<uintptr_t>(player) || loot_access_player_cell != player_cell ||
            now - loot_access_requested > std::chrono::seconds(1) || loot_access_candidates.empty() ||
            now - loot_access_last_check < std::chrono::milliseconds(25)) return;
        loot_access_last_check = now;
        for (size_t i = 0, n = loot_access_candidates.size(); i < n && batch.size() < 16; ++i) {
            const auto& c = loot_access_candidates[loot_access_cursor++ % n];
            const auto it = loot_access_results.find(c.reference);
            if (it != loot_access_results.end() && it->second.form_id == c.form_id &&
                it->second.cell == c.cell && now - it->second.checked < std::chrono::milliseconds(750)) continue;
            batch.push_back(c);
        }
    }
    std::unordered_map<uintptr_t, LootAccessEntry> checked;
    for (const auto& c : batch) {
        uintptr_t cell = 0;
        uint32_t id = 0, flags = 0;
        uint8_t type = 0;
        bool stealing = false;
        Fo4LootAccess access = Fo4LootAccess::Unknown;
        const bool live = read_value(c.reference + 0x14, id) && id == c.form_id &&
            read_value(c.reference + 0x1A, type) && (type == 64 || type == 65) &&
            read_value(c.reference + 0x10, flags) && !(flags & ((1u << 5) | (1u << 11))) &&
            read_value(c.reference + 0xB8, cell) && cell == c.cell;
        if (live && check(image_base, reinterpret_cast<uintptr_t>(player), c.reference, stealing))
            access = stealing ? Fo4LootAccess::Stealing : Fo4LootAccess::Allowed;
        const char* reason = loot_reference_reason(c.reference);
        uintptr_t base = 0;
        uint8_t base_type = 0;
        if (live && !reason && read_value(c.reference + 0xE0, base) &&
            read_value(base + 0x1A, base_type) && base_type == 0x1F &&
            !fo4hud_reference_scripts_allow_auto_loot(image_base, c.reference))
            reason = "Unreviewed/unfinished script or script check unavailable";
        checked[c.reference] = {c.form_id, c.cell, access, now, reason};
    }
    uintptr_t confirmed_cell = 0;
    if (!read_value(reinterpret_cast<uintptr_t>(player) + 0xB8, confirmed_cell) || confirmed_cell != player_cell) return;
    std::lock_guard<std::mutex> lock(loot_access_mutex);
    if (loot_access_player != reinterpret_cast<uintptr_t>(player) || loot_access_player_cell != player_cell) return;
    for (const auto& e : checked) loot_access_results[e.first] = e.second;
    for (auto it = loot_access_results.begin(); it != loot_access_results.end();) {
        if (now - it->second.checked > std::chrono::seconds(3)) it = loot_access_results.erase(it);
        else ++it;
    }
}

using LootClock = std::chrono::steady_clock;
constexpr size_t kInventoryLootBatchSize = 8;
constexpr auto kInventoryLootBudget = std::chrono::milliseconds(2);
struct InventoryLootCursor { uint32_t form_id; size_t next; };
std::unordered_map<uintptr_t, InventoryLootCursor> inventory_loot_cursors;

bool first_inventory_stack(uintptr_t source, bool allow_stealing,
                           uintptr_t& object, int32_t& count, size_t* cursor = nullptr,
                           LootClock::time_point deadline = LootClock::time_point::max(),
                           const std::unordered_set<uintptr_t>* attempted = nullptr) {
    uintptr_t inventory = 0, data = 0;
    uint32_t capacity = 0, size = 0;
    if (!read_value(source + 0xF8, inventory) || !inventory ||
        !read_value(inventory + 0x58, data) ||
        !read_value(inventory + 0x60, capacity) ||
        !read_value(inventory + 0x68, size) || size > capacity ||
        size > 10000 || (size && !data)) return false;
    struct InventoryEntry { uintptr_t object, stack; };
    std::vector<InventoryEntry> entries(size);
    if (size && !read_memory(data, entries.data(), size * sizeof(InventoryEntry))) return false;
    const size_t start = size && cursor ? *cursor % size : 0;
    for (size_t offset = 0; offset < entries.size(); ++offset) {
        if (LootClock::now() >= deadline) return false;
        const size_t index = (start + offset) % entries.size();
        if (cursor) *cursor = (index + 1) % entries.size();
        const auto& entry = entries[index];
        if (!entry.object || (attempted && attempted->count(entry.object)) ||
            !loot_category_enabled(entry.object)) continue;
        uint8_t type = 0;
        if (!read_value(entry.object + 0x1A, type)) return false;
        uintptr_t stack = entry.stack;
        int32_t safe_count = 0;
        bool unsafe_object = false;
        for (unsigned depth = 0; stack && depth < 10000; ++depth) {
            if (LootClock::now() >= deadline) return false;
            uint32_t stack_count = 0;
            uintptr_t next = 0, extra = 0;
            if (!read_value(stack + 0x20, stack_count) ||
                !read_value(stack + 0x10, next) || next == stack ||
                !read_value(stack + 0x18, extra)) return false;
            if (fo4hud_loot_extra_reason(extra, [](uintptr_t address, auto& value) {
                    return read_value(address, value);
                })) unsafe_object = true;
            if (!allow_stealing && !fo4hud_extra_list_unowned(extra))
                unsafe_object = true;
            if (type == 0x2B && !loot_weapon_extra_playable(extra))
                unsafe_object = true;
            if (stack_count && !safe_count)
                safe_count = static_cast<int32_t>(std::min(stack_count, uint32_t(1000)));
            stack = next;
        }
        if (stack) return false;
        // RemoveItemData without an explicit stack ID may choose any stack
        // of this object. The reference theft check does not resolve a
        // different owner or non-playable instance on an individual stack,
        // so reject the object if any of its stacks are unsafe.
        if (safe_count && !unsafe_object) {
            object = entry.object;
            count = safe_count;
            return true;
        }
    }
    return false;
}

void loot_inventory_on_game_thread(uintptr_t image, uintptr_t source,
                                   uintptr_t base, uint8_t base_type,
                                   uintptr_t player, uint32_t source_form_id,
                                   bool allow_stealing,
                                   bool (*scripts_clear)(uintptr_t, uintptr_t) = fo4hud_reference_scripts_allow_auto_loot) {
    if (loot_reference_reason(source) ||
        (base_type == 0x1F && !scripts_clear(image, source))) return;
    uintptr_t source_vtable = 0, remove = 0, inventory = 0;
    if (!read_value(source, source_vtable) ||
        source_vtable < image || source_vtable >= image + kImageSize ||
        !read_value(source_vtable + 0x6D * sizeof(void*), remove) ||
        remove < image || remove >= image + kImageSize ||
        !read_value(source + 0xF8, inventory)) return;
    if (!inventory) {
        const uintptr_t container_offset = base_type == 0x1F ? 0x68 : 0xD0;
        uint32_t base_count = 0;
        uintptr_t create = 0;
        if (!read_value(base + container_offset + 0x10, base_count) || !base_count ||
            !read_value(source_vtable + 0xA7 * sizeof(void*), create) ||
            create < image || create >= image + kImageSize) return;
        using CreateInventoryFn = void (__attribute__((ms_abi)) *)(void*, void*);
        reinterpret_cast<CreateInventoryFn>(create)(reinterpret_cast<void*>(source),
            reinterpret_cast<void*>(base + container_offset));
    }
    uintptr_t source_cell = 0, player_cell = 0;
    if (!read_value(source + 0xB8, source_cell) || !source_cell ||
        !read_value(player + 0xB8, player_cell) || !player_cell) return;
    if (inventory_loot_cursors.size() >= 2048 && !inventory_loot_cursors.count(source))
        inventory_loot_cursors.clear();
    auto& cursor = inventory_loot_cursors[source];
    if (cursor.form_id != source_form_id) cursor = {source_form_id, 0};
    const auto deadline = LootClock::now() + kInventoryLootBudget;
    std::unordered_set<uintptr_t> attempted;
    for (size_t i = 0; i < kInventoryLootBatchSize && LootClock::now() < deadline; ++i) {
        // RemoveItem may invoke scripts. Recheck identity, range and permission
        // after each transfer, and never retain inventory/stack pointers across it.
        uint32_t id = 0, flags = 0, state = 0;
        uintptr_t live_base = 0, cell = 0, live_player_cell = 0;
        float source_position[3]{}, player_position[3]{};
        if (!auto_loot.load(std::memory_order_acquire) ||
            !auto_loot_inventories.load(std::memory_order_acquire) ||
            fo4hud_game_menu_cursor_active() ||
            loot_reference_reason(source) ||
            !read_value(source + 0x14, id) || id != source_form_id ||
            !read_value(source + 0xE0, live_base) || live_base != base ||
            !read_value(source + 0x10, flags) || (flags & ((1u << 5) | (1u << 11))) ||
            !read_value(source + 0xB8, cell) || cell != source_cell ||
            !read_value(player + 0xB8, live_player_cell) || live_player_cell != player_cell ||
            !read_memory(source + 0xD0, source_position, sizeof(source_position)) ||
            !read_memory(player + 0xD0, player_position, sizeof(player_position))) break;
        if (base_type == 45 && (!read_value(source + 0x130, state) ||
            (((state >> 17) & 0xF) != 1 && ((state >> 17) & 0xF) != 2))) break;
        if (base_type == 0x1F && !scripts_clear(image, source)) break;
        float distance_squared = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float delta = source_position[axis] - player_position[axis];
            distance_squared += delta * delta;
        }
        const float range = auto_loot_range.load(std::memory_order_acquire);
        if (!std::isfinite(distance_squared) || distance_squared > range * range) break;
        // A setting change can revoke permission even partway through a batch.
        const bool steal = allow_stealing && auto_loot_steal.load(std::memory_order_acquire);
        bool would_steal = false;
        if (!steal && (!reference_would_be_stealing(image, player, source, would_steal) || would_steal)) break;
        uintptr_t object = 0;
        int32_t count = 0;
        if (!first_inventory_stack(source, steal, object, count, &cursor.next, deadline, &attempted)) break;
        attempted.insert(object); // A refused transfer must not block the remaining objects.
        if (LootClock::now() >= deadline) break;
        alignas(8) unsigned char remove_data[0x48]{};
        std::memcpy(remove_data + 0x20, &object, sizeof(object));
        std::memcpy(remove_data + 0x28, &count, sizeof(count));
        const int32_t reason = 4; // ITEM_REMOVE_REASON::kStoreContainer
        std::memcpy(remove_data + 0x2C, &reason, sizeof(reason));
        std::memcpy(remove_data + 0x30, &player, sizeof(player));
        uint32_t result_handle = 0;
        reinterpret_cast<RemoveItemFn>(remove)(reinterpret_cast<void*>(source),
                                                &result_handle, remove_data);
    }
    if (!attempted.empty()) log_event("auto_loot_inventory_batch", source_form_id);

}

// The game's harvested flag controls readiness, including settlement regrowth.
// Keep only a short retry delay so an unsuccessful activation cannot suppress
// a ready crop for minutes. Called only on the player update/game thread.
struct ReferenceActivationAttempt {
    uintptr_t address = 0;
    std::chrono::steady_clock::time_point when{};
};
std::unordered_map<uint32_t, ReferenceActivationAttempt> harvest_attempts;

bool harvest_flora_on_game_thread(uintptr_t image, const AutoLootCandidate& candidate,
                                  uintptr_t base, void* actor,
                                  std::chrono::steady_clock::time_point now) {
    if (!fo4hud_flora_harvestable(candidate.address)) return false;
    const auto prior = harvest_attempts.find(candidate.form_id);
    if (prior != harvest_attempts.end() && prior->second.address == candidate.address &&
        now - prior->second.when < std::chrono::seconds(2)) return false;
    uintptr_t base_vtable = 0, activate = 0;
    if (!read_value(base, base_vtable) ||
        !read_value(base_vtable + 0x40 * sizeof(void*), activate) ||
        activate < image || activate >= image + kImageSize) return false;
    if (harvest_attempts.size() >= 16384) harvest_attempts.clear();
    harvest_attempts[candidate.form_id] = {candidate.address, now};
    // TESFlora::Activate tail-calls TESProduceForm::Activate, which awards
    // produce and sets the harvested flag. Never force or clear that state.
    const bool harvested = reinterpret_cast<ActivateFloraFn>(activate)(
        reinterpret_cast<void*>(base), reinterpret_cast<void*>(candidate.address),
        actor, 0, nullptr, 1);
    log_event(harvested ? "auto_harvest_success" : "auto_harvest_failed", candidate.form_id);
    return harvested;
}

bool activate_scripted_loot_on_game_thread(uintptr_t image, const AutoLootCandidate& candidate,
                                        uintptr_t base, void* player,
                                        std::chrono::steady_clock::time_point now) {
    const uint32_t category = fo4hud_scripted_loot_category_for_base(base);
    if (!category) return false;
    // Scripts run asynchronously. Give OnActivate time to award the loot and
    // disable/delete the object before attempting the same reference again.
    static std::unordered_map<uint32_t, ReferenceActivationAttempt> attempts;
    const auto prior = attempts.find(candidate.form_id);
    if (prior != attempts.end() && prior->second.address == candidate.address &&
        now - prior->second.when < std::chrono::seconds(5)) return false;
    const uint8_t prolog[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x44, 0x89, 0x4C, 0x24, 0x20};
    uint8_t actual[sizeof(prolog)]{};
    if (!read_memory(image + kActivateReferenceRva, actual, sizeof(actual)) ||
        std::memcmp(actual, prolog, sizeof(actual)) != 0) return false;
    if (attempts.size() >= 16384) attempts.clear();
    attempts[candidate.form_id] = {candidate.address, now};
    // The same arguments as ObjectReference.Activate(player, false): default
    // processing only must be false, otherwise its loot script is bypassed.
    const bool accepted = reinterpret_cast<ActivateReferenceFn>(image + kActivateReferenceRva)(
        reinterpret_cast<void*>(candidate.address), player, nullptr, 1, false, 1, false);
    // A script-blocked activator can return false after sending OnActivate.
    // Do not interpret this return as confirmation that loot was transferred.
    log_event(category == Fo4LootAid ? "auto_loot_mirelurk_egg_attempt" :
              "auto_loot_cap_stash_attempt", candidate.form_id);
    return accepted;
}

void auto_loot_on_game_thread(void* actor) {
    static thread_local bool in_pickup = false;
    if (in_pickup) return;
    if ((!auto_loot.load(std::memory_order_acquire) &&
         !auto_harvest.load(std::memory_order_acquire)) ||
        fo4hud_game_menu_cursor_active()) return;
    static auto last_attempt = std::chrono::steady_clock::time_point{};
    static uintptr_t last_reference = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_attempt < kFo4AutoLootInterval) return;
    AutoLootCandidate candidate;
    {
        std::lock_guard<std::mutex> lock(auto_loot_mutex);
        candidate = pending_auto_loot;
        pending_auto_loot = {};
    }
    const auto repeat_interval = candidate.category == 2 || candidate.category == 4 ?
        kFo4AutoLootInterval : kFo4AutoLootRepeatInterval;
    if (!candidate.address ||
        (candidate.address == last_reference && now - last_attempt < repeat_interval)) return;
    last_attempt = now;
    last_reference = candidate.address;

    uintptr_t image = 0, player = 0, vtable = 0, player_cell = 0;
    uint32_t form_id = 0, flags = 0;
    uint32_t player_state = 0;
    uint8_t type = 0, base_type = 0;
    uintptr_t base = 0, item_cell = 0;
    float player_position[3]{}, item_position[3]{};
    const bool allow_stealing = auto_loot_steal.load(std::memory_order_acquire);
    if (!read_player(image, player) || actor != reinterpret_cast<void*>(player) ||
        !read_value(player, vtable) || vtable != image + kPlayerVtableRva ||
        !read_value(player + 0x130, player_state) ||
        (((player_state >> 17) & 0xF) != 0) ||
        !read_value(candidate.address + 0x14, form_id) || form_id != candidate.form_id ||
        !read_value(candidate.address + 0x1A, type) ||
        type != (candidate.category == 4 ? 65 : 64) ||
        !read_value(candidate.address + 0x10, flags) ||
        (flags & ((1u << 5) | (1u << 11))) ||
        !read_value(candidate.address + 0xE0, base) || !base ||
        !read_value(base + 0x1A, base_type) ||
        !read_value(player + 0xB8, player_cell) || !player_cell ||
        !read_value(candidate.address + 0xB8, item_cell) ||
        item_cell != candidate.cell ||
        !read_memory(player + 0xD0, player_position, sizeof(player_position)) ||
        !read_memory(candidate.address + 0xD0, item_position, sizeof(item_position))) return;

    if (loot_reference_reason(candidate.address)) return;

    if (candidate.category == 5) {
        uintptr_t produce = 0;
        if (!auto_harvest.load(std::memory_order_acquire) ||
            !fo4hud_flora_harvestable(candidate.address) || base_type != 0x29 ||
            !read_value(base + 0x158, produce) || !produce) return;
    } else if (candidate.category == 3) {
        if (!auto_loot.load(std::memory_order_acquire)) return;
        if (!loot_category_enabled(base)) return;
        uintptr_t extra = 0;
        if (base_type == 0x2B &&
            (!read_value(candidate.address + 0x100, extra) ||
             !loot_weapon_extra_playable(extra))) return;
    } else {
        if (!auto_loot.load(std::memory_order_acquire) ||
            !auto_loot_inventories.load(std::memory_order_acquire) ||
            (candidate.category == 2 && base_type != 0x1F) ||
            (candidate.category == 4 && base_type != 45) ||
            (candidate.category != 2 && candidate.category != 4)) return;
        if (candidate.category == 2 && !fo4hud_container_auto_loot_safe(base))
            return;
        if (candidate.category == 4) {
            uint32_t state_word = 0;
            if (!read_value(candidate.address + 0x130, state_word)) return;
            const uint8_t life_state = static_cast<uint8_t>((state_word >> 17) & 0xF);
            if (life_state != 1 && life_state != 2) return;
        }
    }

    // Match the game's player theft decision, including faction/relationship
    // permission. This replaces the raw ownership flags that rejected items
    // the player is allowed to take. Direct transfers do not show a prompt.
    bool would_steal = false;
    if (!allow_stealing &&
        (!reference_would_be_stealing(image, player, candidate.address, would_steal) ||
         would_steal)) return;

    if (player_cell != item_cell) {
        uint16_t player_flags = 0, item_flags = 0;
        uintptr_t player_world = 0, item_world = 0;
        if (!read_value(player_cell + 0x40, player_flags) || (player_flags & 1) ||
            !read_value(item_cell + 0x40, item_flags) || (item_flags & 1) ||
            !read_value(player_cell + 0xC8, player_world) || !player_world ||
            !read_value(item_cell + 0xC8, item_world) || player_world != item_world)
            return;
    }
    float distance_squared = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float delta = item_position[axis] - player_position[axis];
        distance_squared += delta * delta;
    }
    const float range = auto_loot_range.load(std::memory_order_acquire);
    if (!std::isfinite(distance_squared) || distance_squared > range * range) return;
    in_pickup = true;
    if (candidate.category == 5) {
        (void)harvest_flora_on_game_thread(image, candidate, base, actor, now);
    } else if (candidate.category == 3 && fo4hud_scripted_loot_category_for_base(base)) {
        (void)activate_scripted_loot_on_game_thread(image, candidate, base, actor, now);
    } else if (candidate.category == 3) {
        uintptr_t pickup = 0;
        if (read_value(vtable + 0xEC * sizeof(void*), pickup) &&
            pickup >= image && pickup < image + kImageSize) {
            // The game's world-reference pickup path.
            reinterpret_cast<PickUpObjectFn>(pickup)(actor,
                reinterpret_cast<void*>(candidate.address), 1, true);
            log_event("auto_loot_attempt", form_id);
        }
    } else {
        loot_inventory_on_game_thread(image, candidate.address, base, base_type,
                                      player, form_id, allow_stealing);
    }
    in_pickup = false;
}

void __attribute__((ms_abi)) player_update_hook(void* player, float delta) {
    if (original_player_update) original_player_update(player, delta);
    if (!installed.load(std::memory_order_acquire) || !player || !image_base) return;
    if (auto callback = control_tick_callback.load(std::memory_order_acquire)) callback(image_base, player);
    check_hostility_on_game_thread(player);
    check_loot_access_on_game_thread(player);
    check_visibility_on_game_thread(player);
    fo4hud_workshop_tick(image_base, player);
    fo4hud_companion_tick(image_base, player);
    fo4hud_traps_tick(image_base, player);
    static unsigned gold_refresh_ticks = 0;
    if (++gold_refresh_ticks >= 60) {
        gold_refresh_ticks = 0;
        int current_gold = 0;
        (void)read_game_caps_on_game_thread(image_base, player, current_gold);
    }
    update_player_recoil(reinterpret_cast<uintptr_t>(player));
    auto_loot_on_game_thread(player);
    // Actor-value changes can send game/UI events. Run them from the game's
    // player update, never inside the Vulkan present hook's render mutex.
    clear_player_damage(player);
    refill_action_points();
    const int requested = pending_carry_weight.exchange(-1, std::memory_order_acq_rel);
    if (requested >= 0) {
        const bool applied = apply_carry_weight_on_game_thread(player, requested);
        carry_weight_status.store(applied ? 2 : 3, std::memory_order_release);
        if (!applied) log_event("carry_weight_change_failed");
    }
    const int requested_speed = pending_speed_mult.exchange(-1, std::memory_order_acq_rel);
    if (requested_speed >= 0) {
        const bool applied = apply_speed_mult_on_game_thread(player, requested_speed);
        speed_mult_status.store(applied ? 2 : 3, std::memory_order_release);
        if (!applied) log_event("speed_mult_change_failed");
    }
    const int requested_caps = pending_caps.exchange(-1, std::memory_order_acq_rel);
    if (requested_caps >= 0) active_caps_target.store(requested_caps, std::memory_order_release);
    const int caps_target = active_caps_target.load(std::memory_order_acquire);
    if (caps_target >= 0) {
        const CapsStepResult result = apply_caps_step_on_game_thread(player, caps_target);
        if (result == CapsStepResult::Complete) {
            active_caps_target.store(-1, std::memory_order_release);
            caps_status.store(2, std::memory_order_release);
            log_event("caps_change_complete");
        } else if (result == CapsStepResult::Failed) {
            active_caps_target.store(-1, std::memory_order_release);
            caps_status.store(3, std::memory_order_release);
            log_event("caps_change_failed");
        } else {
            caps_status.store(4, std::memory_order_release);
        }
    }
    const uint64_t requested_movement = pending_movement.exchange(UINT64_MAX, std::memory_order_acq_rel);
    if (requested_movement != UINT64_MAX) {
        const uint32_t jump_bits = static_cast<uint32_t>(requested_movement);
        const uint32_t fall_bits = static_cast<uint32_t>(requested_movement >> 32);
        float jump = 0, fall = 0;
        std::memcpy(&jump, &jump_bits, sizeof(jump));
        std::memcpy(&fall, &fall_bits, sizeof(fall));
        const bool applied = apply_movement_on_game_thread(jump, fall);
        movement_status.store(applied ? 2 : 3, std::memory_order_release);
        if (!applied) log_event("movement_change_failed");
    }
}

void __attribute__((ms_abi)) health_hook(void* actor, void* attacker) {
    if (!original_health) return;
    if (changing_health) return; // ModActorValue may enter this callback again.
    if (!enabled.load(std::memory_order_relaxed) || !installed.load(std::memory_order_acquire) ||
        !actor || !attacker || !image_base) {
        original_health(actor, attacker);
        return;
    }

    uintptr_t player = 0;
    uint32_t runtime_flags = 0, base_flags = 0, form_id = 0;
    if (!read_value(image_base + kPlayerPointerRva, player) ||
        reinterpret_cast<uintptr_t>(attacker) != player ||
        reinterpret_cast<uintptr_t>(actor) == player ||
        !actor_flags(actor, runtime_flags, base_flags) ||
        !read_value(reinterpret_cast<uintptr_t>(actor) + 0x14, form_id)) {
        original_health(actor, attacker);
        return;
    }

    for (auto* hit = active_combat_hit; hit; hit = hit->previous)
        if (hit->actor == actor) hit->health_handled = true;

    void* health_info = nullptr;
    void* owner_vtable = nullptr;
    const uintptr_t owner = reinterpret_cast<uintptr_t>(actor) + 0x58;
    if (!read_value(image_base + kActorValueListRva + kHealthActorValueOffset, health_info) ||
        !health_info || !read_value(owner, owner_vtable) || !owner_vtable) {
        original_health(actor, attacker);
        return;
    }
    uintptr_t getter = 0, modifier = 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(owner_vtable);
    if (!read_value(vtable + sizeof(void*), getter) ||
        !read_value(vtable + 6 * sizeof(void*), modifier) ||
        getter < image_base || getter >= image_base + kImageSize ||
        modifier < image_base || modifier >= image_base + kImageSize) {
        original_health(actor, attacker);
        return;
    }
    float health = reinterpret_cast<GetValueFn>(getter)(reinterpret_cast<void*>(owner), health_info);
    if (!std::isfinite(health) || health < -1000000.0f || health > 1000000.0f) {
        original_health(actor, attacker);
        return;
    }

    const bool excluded_actor = (runtime_flags & kRuntimeInstantKillExcluded) ||
                                (base_flags & kBaseInstantKillExcluded);
    if (excluded_actor) {
        // Preserve the existing guard for essential and invulnerable actors.
        // Protected-only actors continue through the player instant-kill path.
        if (health <= 0.0f) {
            changing_health = true;
            reinterpret_cast<ModValueFn>(modifier)(reinterpret_cast<void*>(owner), 2, health_info, 1.0f - health);
            changing_health = false;
        }
        protected_hits.fetch_add(1, std::memory_order_relaxed);
        original_health(actor, attacker);
        return;
    }

    if (health > 0.0f) {
        changing_health = true;
        reinterpret_cast<ModValueFn>(modifier)(reinterpret_cast<void*>(owner), 2, health_info, -health - 1.0f);
        changing_health = false;
        instant_hits.fetch_add(1, std::memory_order_relaxed);
        log_event("instant_hit", form_id, health);
    }
    original_health(actor, attacker);
}
// Preserve vanilla hit processing, then cover confirmed player hits that did
// not alter Health (e.g. a robot limb). Never infer attribution from proximity.
bool player_combat_hit(void* hit, uintptr_t& player) {
    uint32_t handle = 0;
    float health_damage = 0, limb_damage = 0;
    const auto address = reinterpret_cast<uintptr_t>(hit);
    if (!hit || !read_value(address + 0x40, handle) ||
        !read_value(address + 0x90, health_damage) || !read_value(address + 0x9C, limb_damage) ||
        !read_value(image_base + kPlayerPointerRva, player) || !player) return false;
    if (!handle || !std::isfinite(health_damage) || !std::isfinite(limb_damage) ||
        (health_damage <= 0.0f && limb_damage <= 0.0f)) return false;
    uintptr_t table = 0, object = 0;
    uint32_t slot_flags = 0, flags = 0;
    const uint32_t index = handle & 0x1FFFFF;
    return read_value(image_base + kHandleTablePointerRva, table) && table &&
        read_value(table + uintptr_t(index) * 16, slot_flags) && (slot_flags & 0x4000000) &&
        (slot_flags & 0x3E00000) == (handle & 0x3E00000) &&
        read_value(table + uintptr_t(index) * 16 + 8, object) && object == player + 0x20 &&
        read_value(player + 0x28, flags) && flags >> 11 == index;
}

uintptr_t __attribute__((ms_abi)) combat_damage_hook(void* actor, void* hit) {
    uintptr_t player = 0;
    const bool player_hit = enabled.load(std::memory_order_relaxed) &&
        installed.load(std::memory_order_acquire) && image_base && actor &&
        player_combat_hit(hit, player) && reinterpret_cast<uintptr_t>(actor) != player;
    CombatHitContext context{actor, false, active_combat_hit};
    active_combat_hit = &context;
    const auto result = original_combat_damage ? original_combat_damage(actor, hit) : 0;
    active_combat_hit = context.previous;
    if (player_hit && !context.health_handled) {
        uint32_t state = 0;
        if (read_value(reinterpret_cast<uintptr_t>(actor) + 0x130, state)) {
            const auto life = (state >> 17) & 0xF;
            if (life != 1 && life != 2) health_hook(actor, reinterpret_cast<void*>(player));
        }
    }
    return result;
}

} // namespace

void fo4hud_combat_queue_loot_access(const std::vector<Fo4LootAccessCandidate>& candidates,
                                    uintptr_t player, uintptr_t player_cell) {
    if (!fo4hud_combat_active()) return;
    std::lock_guard<std::mutex> lock(loot_access_mutex);
    if (loot_access_player != player || loot_access_player_cell != player_cell) {
        loot_access_results.clear();
        loot_access_cursor = 0;
    }
    loot_access_player = player;
    loot_access_player_cell = player_cell;
    loot_access_requested = std::chrono::steady_clock::now();
    loot_access_candidates.assign(candidates.begin(), candidates.begin() + std::min<size_t>(candidates.size(), 512));
}

Fo4LootAccess fo4hud_combat_loot_access(const Fo4Entity& entity, uintptr_t player, uintptr_t player_cell) {
    std::lock_guard<std::mutex> lock(loot_access_mutex);
    if (!fo4hud_combat_active() || loot_access_player != player || loot_access_player_cell != player_cell)
        return Fo4LootAccess::Unknown;
    const auto it = loot_access_results.find(entity.address);
    if (it == loot_access_results.end() || it->second.form_id != entity.form_id || it->second.cell != entity.cell_address ||
        std::chrono::steady_clock::now() - it->second.checked > std::chrono::seconds(3)) return Fo4LootAccess::Unknown;
    return it->second.access;
}

const char* fo4hud_combat_manual_loot_reason(const Fo4Entity& item) {
    // The render thread consumes the game-thread verdict; never enter the
    // Papyrus VM iterator here. A missing verdict stays conservatively manual.
    std::lock_guard<std::mutex> lock(loot_access_mutex);
    const auto it = loot_access_results.find(item.address);
    if (it != loot_access_results.end() && it->second.form_id == item.form_id &&
        it->second.cell == item.cell_address &&
        std::chrono::steady_clock::now() - it->second.checked < std::chrono::seconds(1))
        return it->second.manual_reason;
    return loot_reference_reason(item.address, false);
}

void fo4hud_combat_queue_hostility(const std::vector<Fo4HostilityCandidate>& candidates) {
    if (!fo4hud_combat_active()) return;
    std::lock_guard<std::mutex> lock(hostility_mutex);
    pending_hostility.assign(candidates.begin(),
        candidates.begin() + std::min<size_t>(candidates.size(), 64));
}

bool fo4hud_combat_actor_hostile(uintptr_t actor, uint32_t form_id, uintptr_t cell) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(hostility_mutex);
    const auto it = hostility_results.find(actor);
    return it != hostility_results.end() && it->second.form_id == form_id &&
           it->second.cell == cell && it->second.hostile &&
           now - it->second.checked < std::chrono::milliseconds(300);
}

void fo4hud_combat_queue_visibility(uintptr_t actor, uint32_t form_id, uintptr_t cell,
                                    const float eye[3], const std::vector<Fo4AimPoint>& points) {
    if (!fo4hud_combat_active() || !actor || !cell || points.empty() || points.size() > 6) return;
    std::lock_guard<std::mutex> lock(visibility_mutex);
    pending_visibility.actor = actor;
    pending_visibility.form_id = form_id;
    pending_visibility.cell = cell;
    std::copy(eye, eye + 3, pending_visibility.eye);
    pending_visibility.points = points;
    pending_visibility.sequence = ++visibility_sequence;
}

Fo4VisibilityResult fo4hud_combat_visibility_result() {
    std::lock_guard<std::mutex> lock(visibility_mutex);
    return visibility_result;
}

void fo4hud_combat_set_shot_target(const Fo4ShotTarget& target) {
    std::lock_guard<std::mutex> lock(shot_target_mutex);
    shot_target = target;
    shot_target_updated = target.actor ? std::chrono::steady_clock::now() :
                                   std::chrono::steady_clock::time_point{};
}

bool fo4hud_combat_shot_redirect_active() {
    return shot_redirect_installed.load(std::memory_order_acquire);
}

void fo4hud_combat_clear_visibility() {
    std::lock_guard<std::mutex> lock(visibility_mutex);
    pending_visibility.actor = 0;
    ++visibility_sequence;
    visibility_result = {};
}

bool fo4hud_combat_install() {
    std::lock_guard<std::mutex> lock(install_mutex);
    if (installed.load()) return true;
    const uintptr_t image = fo4hud_supported_game_image();
    if (!image) { status = 2; return false; }
    void** slot = reinterpret_cast<void**>(image + kActorVtableRva + 0x110 * sizeof(void*));
    void** player_slot = reinterpret_cast<void**>(image + kPlayerValueVtableRva + 6 * sizeof(void*));
    void** update_slot = reinterpret_cast<void**>(image + kPlayerVtableRva + 0xCF * sizeof(void*));
    void** damage_slot = reinterpret_cast<void**>(image + kPlayerVtableRva + 0x131 * sizeof(void*));
    void* damage_expected = reinterpret_cast<void*>(image + kPlayerAdjustDamageRva);
    void* damage_current = nullptr;
    void* expected = reinterpret_cast<void*>(image + kHealthHandlerRva);
    void* player_expected = reinterpret_cast<void*>(image + kPlayerValueModifierRva);
    void* update_expected = reinterpret_cast<void*>(image + kPlayerUpdateRva);
    void* current = nullptr, *player_current = nullptr, *update_current = nullptr;
    if (!read_value(reinterpret_cast<uintptr_t>(slot), current) || current != expected ||
        !read_value(reinterpret_cast<uintptr_t>(player_slot), player_current) ||
        player_current != player_expected ||
        !read_value(reinterpret_cast<uintptr_t>(update_slot), update_current) ||
        update_current != update_expected ||
        !read_value(reinterpret_cast<uintptr_t>(damage_slot), damage_current) ||
        damage_current != damage_expected) {
        status = 3;
        return false;
    }
    original_health = reinterpret_cast<HealthFn>(expected);
    original_player_value = reinterpret_cast<ModValueFn>(player_expected);
    original_player_update = reinterpret_cast<PlayerUpdateFn>(update_expected);
    original_player_damage = reinterpret_cast<AdjustDamageFn>(damage_expected);
    image_base = image;
    if (!set_slot(slot, expected, reinterpret_cast<void*>(health_hook))) {
        status = 3;
        return false;
    }
    if (!set_slot(player_slot, player_expected, reinterpret_cast<void*>(player_value_hook))) {
        (void)set_slot(slot, reinterpret_cast<void*>(health_hook), expected);
        status = 3;
        return false;
    }
    if (!set_slot(update_slot, update_expected, reinterpret_cast<void*>(player_update_hook))) {
        (void)set_slot(player_slot, reinterpret_cast<void*>(player_value_hook), player_expected);
        (void)set_slot(slot, reinterpret_cast<void*>(health_hook), expected);
        status = 3;
        return false;
    }
    if (!set_slot(damage_slot, damage_expected, reinterpret_cast<void*>(player_damage_hook))) {
        (void)set_slot(update_slot, reinterpret_cast<void*>(player_update_hook), update_expected);
        (void)set_slot(player_slot, reinterpret_cast<void*>(player_value_hook), player_expected);
        (void)set_slot(slot, reinterpret_cast<void*>(health_hook), expected);
        status = 3;
        return false;
    }
    patched_player_damage_slot = damage_slot;
    patched_slot = slot;
    patched_player_value_slot = player_slot;
    patched_player_update_slot = update_slot;
    installed.store(true, std::memory_order_release);
    status = 1;
    log_event("hook_installed");
    original_combat_damage = reinterpret_cast<CombatDamageFn>(image + kCombatDamageRva);
    if (!install_relative_call(image, kCombatDamageCallRva, kCombatDamageRva,
                               reinterpret_cast<uintptr_t>(combat_damage_hook), combat_damage_call,
                               original_combat_displacement, combat_damage_stub))
        log_event("combat_hit_hook_unavailable");
    if (!install_shot_launch_hook(image))
        fo4hud_log("aim", "shot redirect unavailable: call site or jump island failed");
    return true;
}

void fo4hud_combat_uninstall() {
    std::lock_guard<std::mutex> lock(install_mutex);
    enabled.store(false, std::memory_order_release);
    invulnerable.store(false, std::memory_order_release);
    unlimited_action_points.store(false, std::memory_order_release);
    no_recoil.store(false, std::memory_order_release);
    auto_loot.store(false, std::memory_order_release);
    auto_harvest.store(false, std::memory_order_release);
    auto_loot_steal.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> pending_lock(auto_loot_mutex);
        pending_auto_loot = {};
    }
    fo4hud_combat_clear_visibility();
    {
        std::lock_guard<std::mutex> lock(loot_access_mutex);
        loot_access_candidates.clear();
        loot_access_results.clear();
        loot_access_player = loot_access_player_cell = 0;
        loot_access_cursor = 0;
        loot_access_requested = loot_access_last_check = {};
    }
    fo4hud_workshop_reset();
    fo4hud_combat_set_shot_target({});
    uninstall_shot_launch_hook();
    if (combat_damage_stub && patch_relative_call(combat_damage_call, original_combat_displacement)) {
        // A game thread may already have fetched the CALL; retain its island.
        combat_damage_stub = nullptr;
        combat_damage_call = 0;
    }
    {
        std::lock_guard<std::mutex> hostility_lock(hostility_mutex);
        pending_hostility.clear();
        hostility_results.clear();
    }
    pending_carry_weight.store(-1, std::memory_order_release);
    carry_weight_status.store(0, std::memory_order_release);
    pending_speed_mult.store(-1, std::memory_order_release);
    speed_mult_status.store(0, std::memory_order_release);
    pending_caps.store(-1, std::memory_order_release);
    active_caps_target.store(-1, std::memory_order_release);
    caps_status.store(0, std::memory_order_release);
    game_caps.store(-1, std::memory_order_release);
    pending_movement.store(UINT64_MAX, std::memory_order_release);
    movement_status.store(0, std::memory_order_release);
    const bool was_installed = installed.load(std::memory_order_acquire);
    if (patched_player_damage_slot && original_player_damage)
        (void)set_slot(patched_player_damage_slot, reinterpret_cast<void*>(player_damage_hook),
                       reinterpret_cast<void*>(original_player_damage));
    if (patched_player_update_slot && original_player_update)
        (void)set_slot(patched_player_update_slot, reinterpret_cast<void*>(player_update_hook),
                       reinterpret_cast<void*>(original_player_update));
    if (patched_player_value_slot && original_player_value)
        (void)set_slot(patched_player_value_slot, reinterpret_cast<void*>(player_value_hook),
                       reinterpret_cast<void*>(original_player_value));
    if (patched_slot && original_health)
        (void)set_slot(patched_slot, reinterpret_cast<void*>(health_hook),
                       reinterpret_cast<void*>(original_health));
    patched_slot = nullptr;
    patched_player_value_slot = nullptr;
    patched_player_update_slot = nullptr;
    patched_player_damage_slot = nullptr;
    installed.store(false, std::memory_order_release);
    status = 0;
    if (was_installed) log_event("hook_uninstalled");
}

bool fo4hud_combat_active() { return installed.load(std::memory_order_acquire); }
bool fo4hud_combat_enabled() { return enabled.load(std::memory_order_acquire); }
void fo4hud_combat_set_enabled(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    enabled.store(value && fo4hud_combat_active(), std::memory_order_release);
    log_event(value ? "instant_kill_enabled" : "instant_kill_disabled");
}
bool fo4hud_combat_invulnerable() { return invulnerable.load(std::memory_order_acquire); }
void fo4hud_combat_set_invulnerable(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    invulnerable.store(value && fo4hud_combat_active(), std::memory_order_release);
    log_event(value ? "invulnerable_enabled" : "invulnerable_disabled");
}
bool fo4hud_combat_unlimited_action_points() {
    return unlimited_action_points.load(std::memory_order_acquire);
}
void fo4hud_combat_set_unlimited_action_points(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    unlimited_action_points.store(value && fo4hud_combat_active(), std::memory_order_release);
    log_event(value ? "unlimited_ap_enabled" : "unlimited_ap_disabled");
}
bool fo4hud_combat_no_recoil() { return no_recoil.load(std::memory_order_acquire); }
void fo4hud_combat_set_no_recoil(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    no_recoil.store(value && fo4hud_combat_active(), std::memory_order_release);
    log_event(value ? "no_recoil_enabled" : "no_recoil_disabled");
}
bool fo4hud_combat_auto_loot() { return auto_loot.load(std::memory_order_acquire); }
bool fo4hud_combat_auto_harvest() { return auto_harvest.load(std::memory_order_acquire); }
void fo4hud_combat_set_auto_loot(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    auto_loot.store(value && fo4hud_combat_active(), std::memory_order_release);
    if (!value && !auto_harvest.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(auto_loot_mutex);
        pending_auto_loot = {};
    }
    log_event(value ? "auto_loot_enabled" : "auto_loot_disabled");
}
void fo4hud_combat_set_auto_harvest(bool value) {
    if (value && !fo4hud_combat_active()) fo4hud_combat_install();
    auto_harvest.store(value && fo4hud_combat_active(), std::memory_order_release);
    if (!value && !auto_loot.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(auto_loot_mutex);
        pending_auto_loot = {};
    }
    log_event(value ? "auto_harvest_enabled" : "auto_harvest_disabled");
}
void fo4hud_combat_set_auto_loot_inventories(bool value) {
    auto_loot_inventories.store(value, std::memory_order_release);
}
void fo4hud_combat_set_auto_loot_steal(bool value) {
    auto_loot_steal.store(value, std::memory_order_release);
    log_event(value ? "auto_loot_steal_enabled" : "auto_loot_steal_disabled");
}
void fo4hud_combat_set_auto_loot_range(float world_units) {
    if (std::isfinite(world_units))
        auto_loot_range.store(std::clamp(world_units, 50.0f, 1000.0f),
                              std::memory_order_release);
}
void fo4hud_combat_set_auto_loot_categories(uint32_t mask) {
    auto_loot_categories.store(mask & kFo4LootAllCategories, std::memory_order_release);
}
bool fo4hud_combat_queue_auto_loot(const Fo4Entity& item) {
    const bool harvest = item.radar_category == 5 &&
        auto_harvest.load(std::memory_order_acquire);
    const bool loot = auto_loot.load(std::memory_order_acquire) &&
        (item.radar_category == 3 ||
         (auto_loot_inventories.load(std::memory_order_acquire) &&
          (item.radar_category == 2 || item.radar_category == 4)));
    if ((!harvest && !loot) ||
        !item.address || !item.cell_address) return false;
    std::lock_guard<std::mutex> lock(auto_loot_mutex);
    if (pending_auto_loot.address) return false;
    pending_auto_loot = {item.address, item.cell_address, item.form_id,
                         item.radar_category};
    return true;
}
uint64_t fo4hud_combat_blocked_action_point_drains() {
    return blocked_ap_drains.load(std::memory_order_relaxed);
}
const char* fo4hud_combat_status() {
    switch (status.load(std::memory_order_acquire)) {
        case 1: return "Hook active";
        case 2: return "Unsupported Fallout 4 executable";
        case 3: return "Damage hook unavailable";
        default: return "Waiting for game";
    }
}
uint64_t fo4hud_combat_instant_hits() { return instant_hits.load(std::memory_order_relaxed); }
uint64_t fo4hud_combat_protected_hits() { return protected_hits.load(std::memory_order_relaxed); }
uint64_t fo4hud_combat_blocked_rads() { return blocked_rads.load(std::memory_order_relaxed); }
uint64_t fo4hud_combat_blocked_damage() { return blocked_damage.load(std::memory_order_relaxed); }

bool fo4hud_combat_read_perk_points(int& points) {
    uintptr_t image = 0, player = 0;
    int8_t count = -1;
    if (!read_player(image, player) || !read_value(player + kPerkCountOffset, count) || count < 0)
        return false;
    points = count;
    return true;
}

bool fo4hud_combat_set_perk_points(int points, std::string& error) {
    if (points < 0 || points > 127) {
        error = "Enter a value from 0 to 127";
        return false;
    }
    uintptr_t image = 0, player = 0;
    if (!read_player(image, player)) {
        error = "Player data is unavailable";
        return false;
    }
    // Confirm the native setter's entry bytes before calling into this build.
    constexpr unsigned char signature[] = {
        0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x88, 0x91, 0xF1, 0x0C, 0x00, 0x00
    };
    unsigned char actual[sizeof(signature)]{};
    if (!read_memory(image + kSetPerkCountRva, actual, sizeof(actual)) ||
        std::memcmp(actual, signature, sizeof(signature)) != 0) {
        error = "Perk point setter does not match this executable";
        return false;
    }
    int previous = 0;
    if (!fo4hud_combat_read_perk_points(previous)) {
        error = "Cannot read current perk points";
        return false;
    }
    reinterpret_cast<SetPerkCountFn>(image + kSetPerkCountRva)(
        reinterpret_cast<void*>(player), static_cast<uint8_t>(points));
    int updated = -1;
    if (!fo4hud_combat_read_perk_points(updated) || updated != points) {
        error = "Game did not retain the new perk point value";
        return false;
    }
    char action[64];
    std::snprintf(action, sizeof(action), "perk_points_%d_to_%d", previous, points);
    log_event(action);
    error.clear();
    return true;
}

bool fo4hud_combat_read_carry_weight(float& weight) {
    uintptr_t image = 0, player = 0, info = 0, vtable = 0, getter = 0;
    if (!read_player(image, player) ||
        !read_value(image + kActorValueListRva + kCarryWeightActorValueOffset, info) || !info ||
        !read_value(player + 0x58, vtable) || vtable != image + kPlayerValueVtableRva ||
        !read_value(vtable + sizeof(void*), getter) || getter != image + kPlayerGetValueRva)
        return false;
    weight = reinterpret_cast<GetValueFn>(getter)(reinterpret_cast<void*>(player + 0x58),
                                                  reinterpret_cast<void*>(info));
    return std::isfinite(weight) && weight >= 0.0f && weight <= 1000000.0f;
}

bool fo4hud_combat_set_carry_weight(int weight, std::string& error) {
    if (weight < 0 || weight > 100000) {
        error = "Enter a value from 0 to 100000";
        return false;
    }
    if (!fo4hud_combat_active()) {
        error = "Game update hook is unavailable";
        return false;
    }
    float current = 0.0f;
    if (!fo4hud_combat_read_carry_weight(current)) {
        error = "Cannot read current carry weight";
        return false;
    }
    carry_weight_status.store(1, std::memory_order_release);
    pending_carry_weight.store(weight, std::memory_order_release);
    log_event("carry_weight_change_queued");
    error.clear();
    return true;
}

const char* fo4hud_combat_carry_weight_status() {
    switch (carry_weight_status.load(std::memory_order_acquire)) {
        case 1: return "Carry weight change queued";
        case 2: return "Carry weight updated";
        case 3: return "Carry weight change failed";
        default: return "";
    }
}

bool fo4hud_combat_read_speed_mult(float& speed_mult) {
    uintptr_t image = 0, player = 0, info = 0, vtable = 0, getter = 0;
    if (!read_player(image, player) ||
        !read_value(image + kActorValueListRva + kSpeedMultActorValueOffset, info) || !info ||
        !read_value(player + 0x58, vtable) || vtable != image + kPlayerValueVtableRva ||
        !read_value(vtable + sizeof(void*), getter) || getter != image + kPlayerGetValueRva)
        return false;
    speed_mult = reinterpret_cast<GetValueFn>(getter)(reinterpret_cast<void*>(player + 0x58),
                                                      reinterpret_cast<void*>(info));
    return std::isfinite(speed_mult) && speed_mult >= 0.0f && speed_mult <= 1000000.0f;
}

bool fo4hud_combat_set_speed_mult(int speed_mult, std::string& error) {
    if (speed_mult < 1 || speed_mult > 1000) {
        error = "Enter a value from 1 to 1000";
        return false;
    }
    if (!fo4hud_combat_active()) {
        error = "Game update hook is unavailable";
        return false;
    }
    float current = 0.0f;
    if (!fo4hud_combat_read_speed_mult(current)) {
        error = "Cannot read current SpeedMult";
        return false;
    }
    speed_mult_status.store(1, std::memory_order_release);
    pending_speed_mult.store(speed_mult, std::memory_order_release);
    log_event("speed_mult_change_queued");
    error.clear();
    return true;
}

const char* fo4hud_combat_speed_mult_status() {
    switch (speed_mult_status.load(std::memory_order_acquire)) {
        case 1: return "SpeedMult change queued";
        case 2: return "SpeedMult updated";
        case 3: return "SpeedMult change failed";
        default: return "";
    }
}

bool fo4hud_combat_read_caps(int& caps) {
    uintptr_t image = 0, player = 0, gold = 0, inventory = 0, data = 0;
    uint32_t capacity = 0, size = 0;
    if (!read_player(image, player) || !read_gold_object(image, gold) ||
        !read_value(player + 0xF8, inventory) || !inventory ||
        !read_value(inventory + 0x58, data) ||
        !read_value(inventory + 0x60, capacity) ||
        !read_value(inventory + 0x68, size) ||
        size > capacity || size > 10000 || (size && !data)) return false;

    struct InventoryEntry { uintptr_t object, stack; };
    static_assert(sizeof(InventoryEntry) == 0x10);
    std::vector<InventoryEntry> entries(size);
    if (size && !read_memory(data, entries.data(), size * sizeof(InventoryEntry))) return false;
    uint64_t total = 0;
    for (const InventoryEntry& entry : entries) {
        if (entry.object != gold) continue;
        uintptr_t stack = entry.stack;
        // A large caps balance can occupy many inventory stacks when added
        // in steps below the observed 65,536 boundary.
        for (unsigned i = 0; stack && i < 50000; ++i) {
            uint32_t count = 0;
            uintptr_t next = 0;
            if (!read_value(stack + 0x20, count) || !read_value(stack + 0x10, next) ||
                next == stack) return false;
            total += count;
            if (total > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
            stack = next;
        }
        if (stack) return false;
    }
    caps = static_cast<int>(total);
    return true;
}

bool fo4hud_combat_set_caps(int caps, std::string& error) {
    if (caps < 0 || caps > 1000000000) {
        error = "Enter a value from 0 to 1000000000";
        return false;
    }
    if (!fo4hud_combat_active()) {
        error = "Game update hook is unavailable";
        return false;
    }
    int current = 0;
    if (!fo4hud_combat_read_caps(current)) {
        error = "Cannot read current caps";
        return false;
    }
    caps_status.store(1, std::memory_order_release);
    pending_caps.store(caps, std::memory_order_release);
    char action[96];
    std::snprintf(action, sizeof(action), "caps_change_%d_to_%d_queued", current, caps);
    log_event(action);
    error.clear();
    return true;
}

bool fo4hud_combat_game_caps(int& caps) {
    const int cached = game_caps.load(std::memory_order_acquire);
    if (cached < 0) return false;
    caps = cached;
    return true;
}

const char* fo4hud_combat_caps_status() {
    switch (caps_status.load(std::memory_order_acquire)) {
        case 1: return "Caps change queued";
        case 2: return "Caps updated";
        case 3: return "Caps change failed";
        case 4: return "Caps updating";
        default: return "";
    }
}

bool fo4hud_combat_read_movement(float& jump_height, float& safe_fall_height) {
    uintptr_t jump_data = 0, fall_data = 0;
    if (!movement_settings(jump_data, fall_data) ||
        !read_value(jump_data, jump_height) || !read_value(fall_data, safe_fall_height)) return false;
    return std::isfinite(jump_height) && std::isfinite(safe_fall_height) &&
           jump_height >= 0.0f && safe_fall_height >= 0.0f;
}

bool fo4hud_combat_set_movement(float jump_height, float safe_fall_height, std::string& error) {
    if (!std::isfinite(jump_height) || !std::isfinite(safe_fall_height) ||
        jump_height < 0.0f || jump_height > 100000.0f ||
        safe_fall_height < 0.0f || safe_fall_height > 100000.0f) {
        error = "Enter values from 0 to 100000";
        return false;
    }
    if (!fo4hud_combat_active()) {
        error = "Game update hook is unavailable";
        return false;
    }
    float current_jump = 0, current_fall = 0;
    if (!fo4hud_combat_read_movement(current_jump, current_fall)) {
        error = "Cannot read movement settings";
        return false;
    }
    uint32_t jump_bits = 0, fall_bits = 0;
    std::memcpy(&jump_bits, &jump_height, sizeof(jump_bits));
    std::memcpy(&fall_bits, &safe_fall_height, sizeof(fall_bits));
    movement_status.store(1, std::memory_order_release);
    pending_movement.store((uint64_t(fall_bits) << 32) | jump_bits, std::memory_order_release);
    log_event("movement_change_queued");
    error.clear();
    return true;
}

const char* fo4hud_combat_movement_status() {
    switch (movement_status.load(std::memory_order_acquire)) {
        case 1: return "Movement change queued";
        case 2: return "Movement settings updated";
        case 3: return "Movement change failed";
        default: return "";
    }
}

void fo4hud_combat_set_control_tick(void (*callback)(uintptr_t, void*)) {
    control_tick_callback.store(callback, std::memory_order_release);
}
