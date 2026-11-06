#include "../game_combat.cpp"
#include <array>
#include <cassert>

namespace {
bool native_stealing = true, native_available = true, change_cell = false;
int native_calls = 0;
uintptr_t test_player = 0;
template<class T, size_t N>
void put(std::array<unsigned char, N>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
bool mock_check(uintptr_t, uintptr_t player, uintptr_t, bool& stealing) {
    assert(player == test_player);
    ++native_calls;
    stealing = native_stealing;
    if (change_cell) {
        const uintptr_t changed = 0x2222;
        std::memcpy(reinterpret_cast<void*>(player + 0xB8), &changed, sizeof(changed));
    }
    return native_available;
}
void tick() {
    loot_access_last_check = {};
    check_loot_access_on_game_thread(reinterpret_cast<void*>(test_player), mock_check);
}
}

int main() {
    // Actual Graygarden crop layout: ownership is present but no-crime=1.
    // This must override ownership for harvesting and ordinary pickup alike.
    std::array<unsigned char, 0x110> crop{};
    std::array<unsigned char, 0x18> extras{};
    std::array<unsigned char, 0x28> ownership{}, unrelated{};
    const uintptr_t crop_address = reinterpret_cast<uintptr_t>(crop.data());
    put(crop, 0x100, reinterpret_cast<uintptr_t>(extras.data()));
    put(extras, 8, reinterpret_cast<uintptr_t>(unrelated.data()));
    put(unrelated, 0x12, uint8_t(0x16));
    put(unrelated, 8, reinterpret_cast<uintptr_t>(ownership.data()));
    put(ownership, 0x12, uint8_t(0x21));
    put(ownership, 0x18, uintptr_t(0x123456)); // Owner identity is irrelevant to the exception.
    put(ownership, 0x24, uint8_t(1));
    bool no_crime = false, would_steal = true;
    assert(reference_no_crime(crop_address, no_crime) && no_crime);
    assert(reference_would_be_stealing(0, 0, crop_address, would_steal) && !would_steal);
    put(ownership, 0x24, uint8_t(0));
    assert(reference_no_crime(crop_address, no_crime) && !no_crime);
    assert(!reference_would_be_stealing(0, 0, crop_address, would_steal)); // Native owner check still required.
    put(ownership, 8, reinterpret_cast<uintptr_t>(ownership.data()));
    assert(!reference_no_crime(crop_address, no_crime)); // Corrupt list fails closed.
    put(ownership, 8, uintptr_t(0));
    put(crop, 0x100, uintptr_t(1));
    assert(!reference_no_crime(crop_address, no_crime));
    put(crop, 0x100, uintptr_t(0));
    assert(reference_no_crime(crop_address, no_crime) && !no_crime);
    std::array<unsigned char, 0xC0> player{}, reference{};
    test_player = reinterpret_cast<uintptr_t>(player.data());
    const uintptr_t address = reinterpret_cast<uintptr_t>(reference.data());
    constexpr uintptr_t cell = 0x1111;
    put(player, 0xB8, cell);
    put(reference, 0x14, uint32_t(0x22B4A4));
    put(reference, 0x1A, uint8_t(64));
    put(reference, 0xB8, cell);
    Fo4Entity entity;
    entity.address = address; entity.form_id = 0x22B4A4; entity.cell_address = cell;
    const std::vector<Fo4LootAccessCandidate> candidates{{address, cell, entity.form_id}};
    installed.store(true);
    assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
    fo4hud_combat_queue_loot_access(candidates, test_player, cell);
    tick();
    assert(native_calls == 1 && fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Stealing);
    tick(); assert(native_calls == 1); // Repeated presents reuse fresh decisions.
    native_stealing = false; // The game grants permission, even for an owned reference.
    loot_access_results[address].checked -= std::chrono::seconds(1);
    tick();
    assert(native_calls == 2 && fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Allowed);
    // The appearance does not depend on whether automatic stealing is enabled.
    auto_loot_steal.store(true);
    assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Allowed);
    auto_loot_steal.store(false);
    loot_access_results[address].checked -= std::chrono::seconds(4);
    assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
    native_available = false; tick();
    assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
    native_available = true; native_stealing = true;
    loot_access_results.clear();
    put(reference, 0x14, uint32_t(0x9999));
    const int before = native_calls; tick();
    assert(native_calls == before && fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
    put(reference, 0x14, entity.form_id);
    loot_access_results.clear();
    put(reference, 0xB8, uintptr_t(0x3333)); tick();
    assert(native_calls == before); // Stale cell identity cannot use ownership from an old location.
    put(reference, 0xB8, cell);
    loot_access_results.clear();
    loot_access_requested -= std::chrono::seconds(2); tick();
    assert(native_calls == before); // Hiding radar expires its work queue.
    fo4hud_combat_queue_loot_access(candidates, test_player, cell);
    change_cell = true; tick();
    assert(loot_access_results.empty()); // A door transition during the native call cannot publish.
    change_cell = false; put(player, 0xB8, cell);
    tick(); assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Stealing);
    fo4hud_combat_queue_loot_access(candidates, test_player, 0x2222);
    assert(loot_access_results.empty() && fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
    // Bound native calls per tick and rotate through the whole submitted batch.
    std::array<std::array<unsigned char, 0xC0>, 40> references{};
    std::vector<Fo4LootAccessCandidate> many;
    for (size_t i = 0; i < references.size(); ++i) {
        put(references[i], 0x14, uint32_t(i + 1));
        put(references[i], 0x1A, uint8_t(64));
        put(references[i], 0xB8, cell);
        many.push_back({reinterpret_cast<uintptr_t>(references[i].data()), cell, uint32_t(i + 1)});
    }
    fo4hud_combat_queue_loot_access(many, test_player, cell);
    const int start = native_calls;
    tick(); assert(native_calls == start + 16);
    tick(); assert(native_calls == start + 32);
    tick(); assert(native_calls == start + 40);
    installed.store(false);
    assert(fo4hud_combat_loot_access(entity, test_player, cell) == Fo4LootAccess::Unknown);
}
