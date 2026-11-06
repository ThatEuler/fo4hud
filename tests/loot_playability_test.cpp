#include "../game_combat.cpp"
#include <array>
#include <cassert>

template<class T, size_t N>
void put(std::array<unsigned char, N>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template<size_t N> uintptr_t address(std::array<unsigned char, N>& bytes) {
    return reinterpret_cast<uintptr_t>(bytes.data());
}

int main() {
    std::array<unsigned char, 0x300> weapon{}, armor{}, ammo{}, aid{};
    put(weapon, 0x1A, uint8_t(0x2B));
    put(armor, 0x1A, uint8_t(0x1D));
    put(ammo, 0x1A, uint8_t(0x2C));
    put(aid, 0x1A, uint8_t(0x30));
    // Live Sonic Attack flags, plus the different armor and ammo layouts.
    put(weapon, 0x2A8, uint32_t(0x120000));
    put(armor, 0x10, uint32_t(13));
    put(ammo, 0x16C, uint8_t(2));
    assert(!loot_category_for_object(address(weapon)));
    assert(!loot_category_for_object(address(armor)));
    assert(!loot_category_for_object(address(ammo)));
    assert(loot_category_for_object(address(aid)) == Fo4LootAid);
    put(weapon, 0x2A8, uint32_t(0x20000));
    put(armor, 0x10, uint32_t(9));
    put(ammo, 0x16C, uint8_t(1));
    // Generic form flags must not replace the weapon-specific check.
    put(weapon, 0x10, uint32_t(13));
    assert(loot_category_for_object(address(weapon)) == Fo4LootWeapons);
    assert(loot_category_for_object(address(armor)) == Fo4LootApparel);
    assert(loot_category_for_object(address(ammo)) == Fo4LootAmmo);
    assert(!loot_category_for_object(1)); // Unreadable objects fail closed.

    std::array<unsigned char, 0x108> source{};
    std::array<unsigned char, 0x70> inventory{};
    std::array<unsigned char, 0x28> first{}, second{}, aid_stack{};
    std::array<unsigned char, 0x30> extra{}, node{};
    std::array<unsigned char, 0x138> instance{};
    uintptr_t entries[] = {address(weapon), address(first), address(aid), address(aid_stack)};
    put(source, 0xF8, address(inventory));
    put(inventory, 0x58, reinterpret_cast<uintptr_t>(entries));
    put(inventory, 0x60, uint32_t(2)); put(inventory, 0x68, uint32_t(2));
    put(first, 0x20, uint32_t(7)); put(first, 0x10, address(second));
    put(second, 0x20, uint32_t(3)); put(second, 0x18, address(extra));
    put(aid_stack, 0x20, uint32_t(1));
    put(extra, 8, address(node)); put(node, 0x12, uint8_t(0xBA));
    put(node, 0x20, address(instance));
    auto_loot_categories = kFo4LootAllCategories;
    uintptr_t object = 0; int32_t count = 0;
    for (bool steal : {false, true}) {
        put(instance, 0x110, uint32_t(0x100000));
        // RemoveItem can choose any stack. Skip the entire weapon entry
        // when a later stack has a non-playable instance; continue to aid.
        assert(first_inventory_stack(address(source), steal, object, count));
        assert(object == address(aid) && count == 1);
        put(instance, 0x110, uint32_t(0));
        assert(first_inventory_stack(address(source), steal, object, count));
        assert(object == address(weapon) && count == 7);
        put(weapon, 0x2A8, uint32_t(0x100000));
        assert(first_inventory_stack(address(source), steal, object, count));
        assert(object == address(aid)); // Base exclusion also covers containers.
        put(weapon, 0x2A8, uint32_t(0));
    }
    assert(loot_weapon_extra_playable(0));
    assert(!loot_weapon_extra_playable(1));
    put(node, 0x20, uintptr_t(1)); assert(!loot_weapon_extra_playable(address(extra)));
    put(node, 0x20, address(instance));
    put(node, 8, address(node)); assert(!loot_weapon_extra_playable(address(extra)));
    put(node, 8, uintptr_t(0));
    put(instance, 0x110, uint32_t(0x100000));
    auto_loot_categories = Fo4LootWeapons;
    assert(!first_inventory_stack(address(source), true, object, count));
}
