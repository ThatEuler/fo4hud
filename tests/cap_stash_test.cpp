#include "../game_combat.cpp"
#include <array>
#include <cassert>

namespace {
int activation_calls = 0;
void* expected_reference = reinterpret_cast<void*>(0x123400);
void* expected_player = reinterpret_cast<void*>(0x123500);
bool __attribute__((ms_abi)) activate_stash(void* reference, void* player, void* third,
                                          int32_t count, bool default_only, int16_t sixth, bool seventh) {
    assert(reference == expected_reference && player == expected_player);
    assert(third == nullptr && count == 1 && !default_only && sixth == 1 && !seventh);
    ++activation_calls;
    return false; // A script can handle OnActivate even when native default activation is blocked.
}
template<class T, size_t N>
void put(std::array<unsigned char, N>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
}

int main() {
    char directory[] = "/tmp/fo4hud-stash-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
    std::array<unsigned char, 0x20> base{};
    const uintptr_t base_address = reinterpret_cast<uintptr_t>(base.data());
    put(base, 0x1A, uint8_t(0x1B));
    put(base, 0x14, uint32_t(0x0003000C));
    assert(fo4hud_is_cap_stash_base(base_address));
    assert(loot_category_for_object(base_address) == Fo4LootOtherMisc);
    auto_loot_categories = kFo4LootAllCategories & ~Fo4LootOtherMisc;
    assert(!loot_category_enabled(base_address));
    auto_loot_categories = kFo4LootAllCategories;
    assert(loot_category_enabled(base_address));
    put(base, 0x14, uint32_t(0x0103000C));
    assert(!fo4hud_is_cap_stash_base(base_address));
    assert(loot_category_for_object(base_address) == 0); // Unrelated mod activator.
    put(base, 0x14, uint32_t(0x0003000C));
    put(base, 0x1A, uint8_t(0x1F));
    assert(!fo4hud_is_cap_stash_base(base_address));
    put(base, 0x1A, uint8_t(0x1B));

    auto_loot = true;
    auto_loot_inventories = false;
    Fo4Entity entity;
    entity.address = reinterpret_cast<uintptr_t>(expected_reference);
    entity.cell_address = 0x123600;
    entity.form_id = 0x123700;
    entity.radar_category = 3;
    assert(fo4hud_combat_queue_auto_loot(entity));
    const AutoLootCandidate candidate = pending_auto_loot;
    pending_auto_loot = {};
    auto_loot = false;
    assert(!fo4hud_combat_queue_auto_loot(entity));

    void* memory = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const uintptr_t image = reinterpret_cast<uintptr_t>(memory);
    auto* entry = reinterpret_cast<unsigned char*>(image + kActivateReferenceRva);
    // Preserve the checked native prefix (shadow-space stores only), followed
    // by a jump to our MS-ABI stub to verify all seven activation arguments.
    unsigned char code[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x44, 0x89, 0x4C, 0x24, 0x20,
                            0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const uintptr_t target = reinterpret_cast<uintptr_t>(activate_stash);
    std::memcpy(code + 12, &target, sizeof(target));
    std::memcpy(entry, code, sizeof(code));
    assert(mprotect(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) & ~uintptr_t(4095)),
                    4096, PROT_READ | PROT_EXEC) == 0);
    const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);
    (void)activate_scripted_loot_on_game_thread(image, candidate, base_address, expected_player, now);
    assert(activation_calls == 1);
    (void)activate_scripted_loot_on_game_thread(image, candidate, base_address, expected_player, now + std::chrono::seconds(1));
    assert(activation_calls == 1); // Do not flood the asynchronous loot script.
    put(base, 0x14, uint32_t(0x0103000C));
    (void)activate_scripted_loot_on_game_thread(image, candidate, base_address, expected_player, now + std::chrono::seconds(6));
    assert(activation_calls == 1); // Only cap stashes can invoke this activation path.
    // Harvestable eggs use the same script-dispatching activation, gated by Aid.
    put(base, 0x14, uint32_t(0x001715CD));
    assert(loot_category_for_object(base_address) == Fo4LootAid);
    auto_loot_categories = kFo4LootAllCategories & ~Fo4LootAid;
    assert(!loot_category_enabled(base_address));
    auto_loot_categories = Fo4LootAid;
    assert(loot_category_enabled(base_address));
    auto egg = candidate;
    ++egg.form_id;
    (void)activate_scripted_loot_on_game_thread(image, egg, base_address, expected_player, now);
    assert(activation_calls == 2);
    put(base, 0x14, uint32_t(0x0016579B)); // Hatching eggs must stay excluded.
    assert(loot_category_for_object(base_address) == 0);
    (void)activate_scripted_loot_on_game_thread(image, egg, base_address, expected_player, now + std::chrono::seconds(6));
    assert(activation_calls == 2);
    munmap(memory, kImageSize);
    std::filesystem::remove_all(directory);
}
