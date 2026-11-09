#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <limits>

static unsigned checked_reads = 0;
static ssize_t counted_read(pid_t pid, const iovec* local, unsigned long local_count,
                            const iovec* remote, unsigned long remote_count,
                            unsigned long flags) {
    ++checked_reads;
    return process_vm_readv(pid, local, local_count, remote, remote_count, flags);
}
#define process_vm_readv counted_read
#include "../game_entities.cpp"
#undef process_vm_readv

template <typename T> void put(void* storage, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(storage) + offset, &value, sizeof(value));
}

void check_native_loot_binding() {
    // Live Carla chest 001679C7 exposes a plain ObjectReference binding,
    // even though it has no custom container script in its plugin record.
    std::array<unsigned char, 0x20> object{}, reference_type{}, form_type{}, root_type{}, custom_type{};
    std::array<unsigned char, 0x60> reference_name{}, form_name{}, root_name{}, custom_name{}, state{};
    auto string = [](auto& text, const char* name) {
        put(text.data(), 0x10, uint32_t(std::strlen(name)));
        std::strcpy(reinterpret_cast<char*>(text.data()+0x18), name);
    };
    auto type = [&](auto& node, auto& text, const char* name, uintptr_t parent) {
        string(text, name);
        put(node.data(), 0x10, uintptr_t(text.data()));
        put(node.data(), 0x18, parent);
    };
    type(root_type, root_name, "ScriptObject", 0);
    type(form_type, form_name, "Form", uintptr_t(root_type.data()));
    type(reference_type, reference_name, "ObjectReference", uintptr_t(form_type.data()));
    put(object.data(), 0, uint32_t(1));
    put(object.data(), 8, uintptr_t(reference_type.data()));
    assert(loot_script_allows_auto_loot(uintptr_t(object.data())));
    // Only the exact native hierarchy and empty state qualify.
    string(state, "CustomState");
    put(object.data(), 0x10, uintptr_t(state.data()));
    assert(!loot_script_allows_auto_loot(uintptr_t(object.data())));
    put(object.data(), 0x10, uintptr_t(0));
    type(custom_type, custom_name, "UnreviewedContainer", uintptr_t(reference_type.data()));
    put(object.data(), 8, uintptr_t(custom_type.data()));
    assert(!loot_script_allows_auto_loot(uintptr_t(object.data())));
    // A safe native binding cannot approve an unsafe sibling binding.
    LootScriptVisitor visitor{nullptr};
    uintptr_t binding = uintptr_t(object.data());
    visit_loot_script(&visitor, &binding);
    put(object.data(), 8, uintptr_t(reference_type.data()));
    visit_loot_script(&visitor, &binding);
    assert(!visitor.clear && visitor.count == 2);
    put(form_type.data(), 0x18, uintptr_t(reference_type.data()));
    assert(!loot_script_allows_auto_loot(uintptr_t(object.data())));
}

int main() {
    check_native_loot_binding();
    const size_t page_size = sysconf(_SC_PAGESIZE);
    auto* pages = static_cast<unsigned char*>(mmap(nullptr, page_size * 2,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(pages != MAP_FAILED);
    assert(mprotect(pages + page_size, page_size, PROT_NONE) == 0);

    // The batched reference read ends exactly at the final position field.
    // An inaccessible next page must not affect this valid reference.
    auto* ref = pages + page_size - 0xDC;
    put(ref, 0x14, uint32_t(0x1234));
    put(ref, 0x1A, uint8_t(64));
    put(ref, 0xD0, float(1));
    put(ref, 0xD4, float(2));
    put(ref, 0xD8, float(3));
    Fo4Entity entity;
    checked_reads = 0;
    assert(read_entity(reinterpret_cast<uintptr_t>(ref), entity));
    assert(checked_reads == 1);
    assert(entity.form_id == 0x1234 && entity.form_type == 64);
    assert(entity.position[0] == 1 && entity.position[1] == 2 && entity.position[2] == 3);
    assert(!read_entity(reinterpret_cast<uintptr_t>(ref + 1), entity));
    uint64_t value = 0;
    assert(!read_value(reinterpret_cast<uintptr_t>(pages + page_size), value));
    assert(!read_memory(std::numeric_limits<uintptr_t>::max() - 1, &value, 8));

    // Pooled strings retain redirect, encoding, and length validation.
    alignas(8) unsigned char text[0x30]{};
    alignas(8) unsigned char redirect[0x18]{};
    put(text, 0x10, uint32_t(4));
    std::memcpy(text + 0x18, "Ada\n", 4);
    uintptr_t field = reinterpret_cast<uintptr_t>(text);
    assert(read_pooled_string(reinterpret_cast<uintptr_t>(&field)) == "Ada ");
    put(redirect, 8, uint16_t(0x4000));
    put(redirect, 0x10, reinterpret_cast<uintptr_t>(text));
    field = reinterpret_cast<uintptr_t>(redirect);
    assert(read_pooled_string(reinterpret_cast<uintptr_t>(&field)) == "Ada ");
    put(text, 8, uint16_t(0x8000));
    assert(read_pooled_string(reinterpret_cast<uintptr_t>(&field)).empty());
    put(text, 8, uint16_t(0));
    put(text, 0x10, uint32_t(256));
    assert(read_pooled_string(reinterpret_cast<uintptr_t>(&field)).empty());

    // Inventory batch reads still fail closed on invalid counts and cycles.
    alignas(8) unsigned char reference[0x100]{};
    alignas(8) unsigned char inventory[0x70]{};
    alignas(8) unsigned char stack[0x28]{};
    alignas(8) unsigned char item[0x2B0]{};
    put(item, 0x1A, uint8_t(0x23));
    uintptr_t entries[] = {reinterpret_cast<uintptr_t>(item), reinterpret_cast<uintptr_t>(stack)};
    put(reference, 0xF8, reinterpret_cast<uintptr_t>(inventory));
    put(inventory, 0x58, reinterpret_cast<uintptr_t>(entries));
    put(inventory, 0x60, uint32_t(1));
    put(inventory, 0x68, uint32_t(1));
    put(stack, 0x20, uint32_t(1));
    const uintptr_t reference_address = reinterpret_cast<uintptr_t>(reference);
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::HasItems);
    // Scripted loot must be discovered as loose loot without container mode.
    put(reference, 0x1A, uint8_t(64));
    put(reference, 0xE0, reinterpret_cast<uintptr_t>(item));
    for (uint32_t id : {0x0003000Cu, 0x001715CDu}) {
        put(item, 0x1A, uint8_t(0x1B));
        put(item, 0x14, id);
        Fo4Entity scripted;
        scripted.address = reference_address;
        scripted.form_type = 64;
        classify_radar_reference(scripted);
        assert(scripted.radar_category == 3);
        assert(scripted.name == (id == 0x0003000C ? "Cap stash" : "Mirelurk egg"));
    }
    put(item, 0x14, uint32_t(0x0016579B));
    Fo4Entity hatching;
    hatching.address = reference_address;
    hatching.form_type = 64;
    classify_radar_reference(hatching);
    assert(hatching.radar_category == 0);
    // Mr Handy keeps hidden Pincher/Saw weapons and robot armor after looting.
    put(item, 0x1A, uint8_t(0x2B)); put(item, 0x2A8, uint32_t(0x1A0100));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(item, 0x1A, uint8_t(0x1D)); put(item, 0x10, uint32_t(0xD));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(item, 0x10, uint32_t(0x9));
    // Live SkinFeralGhoulGlowing (00020BCA): playable flags, no FULL name.
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(text, 0x10, uint32_t(4));
    put(item, 0x70, reinterpret_cast<uintptr_t>(text));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::HasItems);
    // The game uses a non-null shared empty pooled string for this skin.
    put(text, 0x10, uint32_t(0));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    field = reinterpret_cast<uintptr_t>(redirect);
    put(item, 0x70, field);
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(item, 0x70, uintptr_t(1));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Unknown);
    put(item, 0x1A, uint8_t(0x2C)); put(item, 0x16C, uint8_t(2));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(item, 0x1A, uint8_t(0x23));
    put(stack, 0x20, uint32_t(0));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Empty);
    put(stack, 0x10, reinterpret_cast<uintptr_t>(stack));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Unknown);
    put(inventory, 0x68, uint32_t(2));
    assert(read_inventory_state(reference_address, 0, 0) == Fo4InventoryState::Unknown);

    // Only an accepted executable enables direct reads, and only whitelisted
    // fixed ranges bypass the syscall. A pointer's target remains checked.
    auto* image = static_cast<unsigned char*>(mmap(nullptr, kImageSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t image_address = reinterpret_cast<uintptr_t>(image);
    assert(!known_game_image(image_address));
    assert(!fixed_game_storage(image_address + kPlayerPointerRva, 8));
    put(image, 0, uint16_t(0x5A4D));
    put(image, 0x3C, uint32_t(0x100));
    put(image, 0x100, uint32_t(0x00004550));
    put(image, 0x108, kTimestamp);
    put(image, 0x130, uint64_t(kImageBase));
    put(image, 0x150, kImageSize);
    assert(known_game_image(image_address));
    put(image, kPlayerPointerRva, reinterpret_cast<uintptr_t>(pages + page_size));
    checked_reads = 0;
    uintptr_t player = 0;
    assert(read_value(image_address + kPlayerPointerRva, player));
    assert(checked_reads == 0);
    assert(!read_value(player, value));
    assert(checked_reads == 1);
    assert(fixed_game_storage(image_address + kProjectionMatrixRva, 64));
    assert(!fixed_game_storage(image_address + kProjectionMatrixRva, 65));
    assert(!fixed_game_storage(image_address + kPlayerPointerRva - 1, 8));
    assert(!fixed_game_storage(image_address + kPlayerPointerRva, 9));
    assert(!fixed_game_storage(image_address + kProcessListsVtableRva, 8));
    verified_image.store(0);
    assert(munmap(image, kImageSize) == 0);
    assert(munmap(pages, page_size * 2) == 0);
}
