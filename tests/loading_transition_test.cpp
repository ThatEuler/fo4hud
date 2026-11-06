#include "../game_entities.h"

#include <array>
#include <cassert>
#include <cstring>

template <size_t N, class T>
void put(std::array<unsigned char, N>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

int main() {
    alignas(8) std::array<unsigned char, 0x110> reference{};
    alignas(8) std::array<unsigned char, 0x20> extras{};
    alignas(8) std::array<unsigned char, 0x20> unrelated{};
    alignas(8) std::array<unsigned char, 0x20> teleport{};
    alignas(8) std::array<unsigned char, 0x28> data{};
    alignas(8) std::array<unsigned char, 0x20> source_cell{};
    alignas(8) std::array<unsigned char, 0x20> destination_cell{};
    alignas(8) std::array<unsigned char, 0xC0> destination_door{};
    alignas(8) std::array<unsigned char, 16 * 4> handle_slots{};
    alignas(8) std::array<unsigned char, 0x90> base{};
    alignas(8) std::array<unsigned char, 0x80> model{};
    const uintptr_t ref = reinterpret_cast<uintptr_t>(reference.data());
    const uintptr_t source = reinterpret_cast<uintptr_t>(source_cell.data());
    const uintptr_t destination = reinterpret_cast<uintptr_t>(destination_cell.data());
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(reference, 0xE0, reinterpret_cast<uintptr_t>(base.data()));
    put(base, 0x1A, uint8_t(0x20));
    const char path[] = "SetDressing\\Doors\\TrapDoorLoad01.nif";
    put(model, 0x10, uint32_t(sizeof(path) - 1));
    std::memcpy(model.data() + 0x18, path, sizeof(path));
    put(base, 0x80, reinterpret_cast<uintptr_t>(model.data()));
    put(reference, 0x100, reinterpret_cast<uintptr_t>(extras.data()));
    put(extras, 0x08, reinterpret_cast<uintptr_t>(unrelated.data()));
    put(unrelated, 0x12, uint8_t(0x21));
    put(unrelated, 0x08, reinterpret_cast<uintptr_t>(teleport.data()));
    put(teleport, 0x12, uint8_t(0x2B));
    put(teleport, 0x18, reinterpret_cast<uintptr_t>(data.data()));
    put(data, 0x00, destination);
    put(destination_cell, 0x1A, uint8_t(63));
    assert(fo4hud_reference_has_loading_transition(ref, source));
    // Regression for 0024A1A7: real teleport data, but no physical door model.
    put(base, 0x80, uintptr_t(0));
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(base, 0x80, reinterpret_cast<uintptr_t>(model.data()));
    put(model, 0x10, uint32_t(0)); // Empty pooled model path is also a placeholder.
    model[0x18] = 0;
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(model, 0x10, uint32_t(sizeof(path) - 1));
    std::memcpy(model.data() + 0x18, path, sizeof(path));
    assert(fo4hud_reference_has_loading_transition(ref, source)); // No display name is required.
    put(data, 0x24, uint8_t(1)); // Instant teleport; no loading entrance.
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(data, 0x24, uint8_t(0));
    put(data, 0x00, source);
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(data, 0x00, destination);
    put(destination_cell, 0x1A, uint8_t(64));
    assert(!fo4hud_reference_has_loading_transition(ref, source));
    put(destination_cell, 0x1A, uint8_t(63));
    // Live shelter hatch layout: cached cell is null; linked door is a handle.
    put(data, 0, uintptr_t(0));
    constexpr uint32_t handle = 0x200003;
    put(data, 8, handle);
    put(handle_slots, 3 * 16, uint32_t(0x4200003));
    put(handle_slots, 3 * 16 + 8, reinterpret_cast<uintptr_t>(destination_door.data()) + 32);
    put(destination_door, 0x28, uint32_t(3 << 11));
    put(destination_door, 0x1A, uint8_t(64));
    put(destination_door, 0xB8, destination);
    const uintptr_t table = reinterpret_cast<uintptr_t>(handle_slots.data());
    assert(fo4hud_reference_has_loading_transition(ref, source, table));
    put(data, 0x24, uint8_t(1));
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
    put(data, 0x24, uint8_t(0));
    put(destination_door, 0xB8, source);
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
    put(destination_door, 0xB8, destination);
    put(handle_slots, 3 * 16, uint32_t(0x4400003)); // Recycled handle age.
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
    put(handle_slots, 3 * 16, uint32_t(0x4200003));
    put(destination_door, 0x28, uint32_t(4 << 11)); // Reused object address.
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
    put(destination_door, 0x28, uint32_t(3 << 11));
    put(destination_door, 0x1A, uint8_t(65)); // Actors cannot be linked doors.
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
    put(destination_door, 0x1A, uint8_t(64));
    put(data, 8, uint32_t(0));
    assert(!fo4hud_reference_has_loading_transition(ref, source, table));
}
