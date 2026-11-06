#include "../game_entities.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

template <size_t N, class T>
void put(std::array<unsigned char, N>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

int main() {
    alignas(8) std::array<unsigned char, 0x120> reference{};
    alignas(8) std::array<unsigned char, 0x60> cell{};
    alignas(8) std::array<unsigned char, 0x28> extras{};
    alignas(8) std::array<unsigned char, 0x18> node{};
    const uintptr_t ref = reinterpret_cast<uintptr_t>(reference.data());
    const uintptr_t cell_address = reinterpret_cast<uintptr_t>(cell.data());
    const uintptr_t extra_address = reinterpret_cast<uintptr_t>(extras.data());
    const uintptr_t node_address = reinterpret_cast<uintptr_t>(node.data());
    put(cell, 0x1A, uint8_t(63));
    assert(fo4hud_reference_unowned(ref, cell_address));

    put(reference, 0x100, extra_address);
    put(extras, 0x08, node_address);
    put(node, 0x12, uint8_t(0x21)); // ExtraOwnership
    assert(!fo4hud_reference_unowned(ref, cell_address));
    put(node, 0x12, uint8_t(0x24)); // ExtraCount, unrelated
    assert(fo4hud_reference_unowned(ref, cell_address));

    put(reference, 0x100, uintptr_t(0));
    put(cell, 0x48, extra_address);
    put(node, 0x12, uint8_t(0x21));
    assert(!fo4hud_reference_unowned(ref, cell_address));
    put(cell, 0x48, uintptr_t(0));

    alignas(8) std::array<unsigned char, 0x110> container{};
    alignas(8) std::array<unsigned char, 0x18> item{};
    alignas(8) std::array<unsigned char, 0x18> item_extra{};
    const uintptr_t item_address = reinterpret_cast<uintptr_t>(item.data());
    const uintptr_t entries[] = {item_address};
    put(container, 0x70, reinterpret_cast<uintptr_t>(entries));
    put(container, 0x78, uint32_t(1));
    assert(fo4hud_base_container_unowned(
        reinterpret_cast<uintptr_t>(container.data()), 0x1F));
    put(item, 0x10, reinterpret_cast<uintptr_t>(item_extra.data()));
    put(item_extra, 0x00, uintptr_t(0x1234));
    assert(!fo4hud_base_container_unowned(
        reinterpret_cast<uintptr_t>(container.data()), 0x1F));
}
