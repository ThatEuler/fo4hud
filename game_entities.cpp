#include "game_entities.h"
#include "loot_categories.h"
#include "loot_playability.h"

#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr uintptr_t kPlayerPointerRva = 0x032DD370;
constexpr uintptr_t kProcessListsPointerRva = 0x030E71C0;
constexpr uintptr_t kProcessListsVtableRva = 0x0256EF78;
constexpr uintptr_t kHandleTablePointerRva = 0x030E5490;
constexpr uintptr_t kTesPointerRva = 0x032DD158;
constexpr uintptr_t kTesVtableRva = 0x02473B58;
constexpr uintptr_t kGridCellVtableRva = 0x0247C7F8;
constexpr uintptr_t kMenuCursorPointerRva = 0x0327A568;
constexpr uintptr_t kMenuCursorVtableRva = 0x02728B28;
// World-to-screen globals used by Fallout4.exe's WorldToScreen_Internal.
constexpr uintptr_t kProjectionMatrixRva = 0x03279F80;
constexpr uintptr_t kProjectionReadyRva = 0x03279FC0;
constexpr uintptr_t kProjectionPortRva = 0x03279FD8;
constexpr uint32_t kTimestamp = 0x6A727FCC;
constexpr uint32_t kImageSize = 0x0425A000;
constexpr uintptr_t kImageBase = 0x140000000;
constexpr uint32_t kMaxReferences = 16384;

// Only fixed storage inside the verified main executable is read directly.
// Heap pointers loaded from these globals still go through checked reads.
std::atomic<uintptr_t> verified_image{0};

bool fixed_game_storage(uintptr_t address, size_t size) {
    const uintptr_t image = verified_image.load(std::memory_order_acquire);
    if (!image || address < image) return false;
    const uintptr_t rva = address - image;
    if (rva < kHandleTablePointerRva || rva > kPlayerPointerRva + 8) return false;
    struct Range { uintptr_t rva; size_t size; };
    constexpr Range ranges[] = {
        {kPlayerPointerRva, 8}, {kProcessListsPointerRva, 8},
        {kHandleTablePointerRva, 8}, {kTesPointerRva, 8},
        {kMenuCursorPointerRva, 8}, {kProjectionMatrixRva, 64},
        {kProjectionReadyRva, 1}, {kProjectionPortRva, 16},
    };
    for (const auto& range : ranges)
        if (rva >= range.rva && rva - range.rva <= range.size &&
            size <= range.size - (rva - range.rva)) return true;
    return false;
}

bool read_memory(uintptr_t address, void* destination, size_t size) {
    if (!address || address > std::numeric_limits<uintptr_t>::max() - size) return false;
    if (fixed_game_storage(address, size)) {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    }
    iovec local{destination, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

template <typename T> bool read_value(uintptr_t address, T& value) {
    return read_memory(address, &value, sizeof(value));
}

// Copy related fields in one checked read; unpack only our local copy.
// Exact bounds avoid extending reads past the last field that is needed.
template <size_t Size> struct ReadBlock {
    std::array<unsigned char, Size> bytes{};
    bool read(uintptr_t address) { return read_memory(address, bytes.data(), Size); }
    template <typename T, size_t Offset> T get() const {
        static_assert(Offset + sizeof(T) <= Size);
        T value;
        std::memcpy(&value, bytes.data() + Offset, sizeof(T));
        return value;
    }
};

uintptr_t find_game_image() {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[1024];
    uintptr_t image = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0, offset = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%lx-%lx %4s %lx", &start, &end, permissions, &offset) != 4) continue;
        if (offset == 0 && permissions[0] == 'r' && std::strstr(line, "Fallout4.exe")) {
            image = start;
            break;
        }
    }
    std::fclose(maps);
    return image;
}

bool known_game_image(uintptr_t image) {
    uint16_t mz = 0;
    uint32_t pe_offset = 0, signature = 0, timestamp = 0, size = 0;
    uint64_t preferred_base = 0;
    const bool valid = image && read_value(image, mz) && mz == 0x5A4D &&
           read_value(image + 0x3C, pe_offset) && pe_offset < 0x1000 &&
           read_value(image + pe_offset, signature) && signature == 0x00004550 &&
           read_value(image + pe_offset + 8, timestamp) && timestamp == kTimestamp &&
           read_value(image + pe_offset + 0x30, preferred_base) && preferred_base == kImageBase &&
           read_value(image + pe_offset + 0x50, size) && size == kImageSize;
    if (valid) verified_image.store(image, std::memory_order_release);
    return valid;
}

bool read_entity(uintptr_t address, Fo4Entity& entity) {
    entity.address = address;
    ReadBlock<0xC8> data;
    if (!data.read(address + 0x14)) return false;
    entity.form_id = data.template get<uint32_t, 0>();
    entity.form_type = data.template get<uint8_t, 6>();
    std::memcpy(entity.position, data.bytes.data() + 0xBC, sizeof(entity.position));
    return true;
}

bool read_reference_space(uintptr_t reference, Fo4Entity& entity) {
    uintptr_t cell = 0;
    ReadBlock<0x28> data;
    if (!read_value(reference + 0xB8, cell) || !cell ||
        !data.read(cell + 0x1A) || data.template get<uint8_t, 0>() != 63) return false;
    const uint16_t flags = data.template get<uint16_t, 0x26>();
    entity.cell_address = cell;
    entity.cell_is_interior = (flags & 1) != 0;
    if (!entity.cell_is_interior)
        (void)read_value(cell + 0xC8, entity.worldspace_address);
    return true;
}

std::optional<std::string> read_checked_pooled_string(uintptr_t field);
std::string read_pooled_string(uintptr_t field);

uint8_t radar_category_for_base(uint8_t type) {
    if (type == 0x1F) return 2; // CONT
    switch (type) {
        case 0x1D: // ARMO
        case 0x1E: // BOOK
        case 0x21: // INGR
        case 0x23: // MISC
        case 0x2B: // WEAP
        case 0x2C: // AMMO
        case 0x2F: // KEYM
        case 0x30: // ALCH
        case 0x32: // NOTE
            return 3;
        default: return 0;
    }
}

// Triggers can be attached to an activator, container, door or movable static.
bool trap_script_candidate_base(uint8_t type) {
    return type == 27 || type == 31 || type == 32 || type == 36;
}

// Inventory colors describe loot the player can see, independent of the
// auto-loot category selection. Robot weapons/armor remain after looting.
Fo4InventoryState inventory_item_state(uintptr_t object, uint8_t& type) {
    if (!object || !read_value(object + 0x1A, type)) return Fo4InventoryState::Unknown;
    if (!fo4hud_loot_category_for_form(type, false)) return Fo4InventoryState::Empty;
    const auto playable = fo4hud_item_playable(object, type, [](uintptr_t address, auto& value) {
        return read_value(address, value);
    });
    if (!playable) return Fo4InventoryState::Unknown;
    if (!*playable) return Fo4InventoryState::Empty;
    if (type == 0x1D) {
        // Creature skins (e.g. SkinFeralGhoulGlowing) can pass IsPlayable
        // while having no FULL name. They never appear in the loot panel.
        const auto name = read_checked_pooled_string(object + 0x70);
        if (!name) return Fo4InventoryState::Unknown;
        if (name->empty()) return Fo4InventoryState::Empty;
    }
    return Fo4InventoryState::HasItems;
}

Fo4InventoryState inventory_stack_state(uint8_t type, uintptr_t extra) {
    if (type != 0x2B) return Fo4InventoryState::HasItems;
    const auto playable = fo4hud_weapon_instance_playable(extra, [](uintptr_t address, auto& value) {
        return read_value(address, value);
    });
    if (!playable) return Fo4InventoryState::Unknown;
    return *playable ? Fo4InventoryState::HasItems : Fo4InventoryState::Empty;
}

Fo4InventoryState read_inventory_state(uintptr_t reference, uintptr_t base,
                                       uint8_t base_type) {
    uintptr_t inventory = 0;
    if (!read_value(reference + 0xF8, inventory)) return Fo4InventoryState::Unknown;
    if (inventory) {
        // BGSInventoryList is built from the base container, then tracks the
        // reference's actual contents. A null list needs the base fallback.
        uintptr_t data = 0;
        uint32_t capacity = 0, size = 0;
        ReadBlock<0x14> header;
        if (!header.read(inventory + 0x58)) return Fo4InventoryState::Unknown;
        data = header.template get<uintptr_t, 0>();
        capacity = header.template get<uint32_t, 8>();
        size = header.template get<uint32_t, 0x10>();
        if (size > capacity ||
            size > 10000 || (size && !data)) return Fo4InventoryState::Unknown;
        struct InventoryEntry { uintptr_t object, stack; };
        static_assert(sizeof(InventoryEntry) == 0x10);
        std::vector<InventoryEntry> entries(size);
        if (size && !read_memory(data, entries.data(), size * sizeof(InventoryEntry)))
            return Fo4InventoryState::Unknown;
        bool has_items = false;
        for (const InventoryEntry& entry : entries) {
            if (!entry.object) continue;
            uint8_t type = 0;
            const auto item = inventory_item_state(entry.object, type);
            if (item == Fo4InventoryState::Unknown) return item;
            if (item == Fo4InventoryState::Empty) continue;
            uintptr_t stack = entry.stack;
            for (unsigned depth = 0; stack && depth < 10000; ++depth) {
                uint32_t count = 0;
                uintptr_t next = 0;
                ReadBlock<0x14> stack_data;
                if (!stack_data.read(stack + 0x10)) return Fo4InventoryState::Unknown;
                next = stack_data.template get<uintptr_t, 0>();
                count = stack_data.template get<uint32_t, 0x10>();
                if (next == stack) return Fo4InventoryState::Unknown;
                if (count) {
                    const auto state = inventory_stack_state(type, stack_data.get<uintptr_t, 8>());
                    if (state == Fo4InventoryState::Unknown) return state;
                    if (state == Fo4InventoryState::HasItems) has_items = true;
                }
                stack = next;
            }
            if (stack) return Fo4InventoryState::Unknown;
        }
        uintptr_t confirmed_data = 0;
        uint32_t confirmed_size = 0;
        if (!read_value(inventory + 0x58, confirmed_data) ||
            !read_value(inventory + 0x68, confirmed_size) ||
            confirmed_data != data || confirmed_size != size)
            return Fo4InventoryState::Unknown;
        return has_items ? Fo4InventoryState::HasItems : Fo4InventoryState::Empty;
    }

    // TESObjectCONT and TESNPC both embed TESContainer. This fallback keeps
    // never-opened containers with base-form items from appearing empty.
    const uintptr_t container_offset = base_type == 0x1F ? 0x68 :
                                       base_type == 45 ? 0xD0 : 0;
    if (!base || !container_offset) return Fo4InventoryState::Unknown;
    uintptr_t objects = 0;
    uint32_t count = 0;
    if (!read_value(base + container_offset + 0x08, objects) ||
        !read_value(base + container_offset + 0x10, count) ||
        count > 10000 || (count && !objects)) return Fo4InventoryState::Unknown;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t object = 0;
        int32_t quantity = 0;
        if (!read_value(objects + uintptr_t(i) * 8, object) || !object ||
            !read_value(object, quantity)) return Fo4InventoryState::Unknown;
        if (quantity > 0) {
            uintptr_t item = 0; uint8_t type = 0;
            if (!read_value(object + 8, item)) return Fo4InventoryState::Unknown;
            const auto state = inventory_item_state(item, type);
            if (state != Fo4InventoryState::Empty) return state;
        }
    }
    return Fo4InventoryState::Empty;
}

void classify_radar_reference(Fo4Entity& entity) {
    if (entity.form_type == 65) { // ACHR, including corpses no longer in process lists
        uint32_t state_word = 0;
        if (!read_value(entity.address + 0x130, state_word)) return;
        const uint8_t life_state = static_cast<uint8_t>((state_word >> 17) & 0xF);
        if (life_state != 1 && life_state != 2) return;
        uintptr_t base = 0;
        uint8_t base_type = 0;
        if (!read_value(entity.address + 0xE0, base) || !base ||
            !read_value(base + 0x1A, base_type) || base_type != 45) return;
        entity.radar_category = 4;
        entity.inventory_state = read_inventory_state(entity.address, base, base_type);
        (void)read_reference_space(entity.address, entity);
        return;
    }
    if (entity.form_type != 64) return; // REFR
    ReadBlock<0xD8> reference_data;
    if (!reference_data.read(entity.address + 0x10)) return;
    const uint32_t flags = reference_data.template get<uint32_t, 0>();
    if (flags & ((1u << 5) | (1u << 11))) return;
    const uintptr_t base = reference_data.template get<uintptr_t, 0xD0>();
    ReadBlock<7> base_data;
    if (!base || !base_data.read(base + 0x14)) return;
    const uint8_t type = base_data.template get<uint8_t, 6>();
    const uint32_t form_id = base_data.template get<uint32_t, 0>();
    if (fo4hud_scripted_loot_category(type, form_id)) {
        entity.radar_category = 3;
        entity.name = fo4hud_is_cap_stash_form(type, form_id) ? "Cap stash" : "Mirelurk egg";
        return;
    }
    if (type == 0x29) { // FLOR; TESProduceForm::produceItem at 0x158.
        uintptr_t produce = 0;
        if (read_value(base + 0x158, produce) && produce) {
            entity.radar_category = 5;
            entity.name = read_pooled_string(base + 0x70);
        }
        return;
    }
    entity.radar_category = fo4hud_reference_has_loading_transition(
        entity.address, entity.cell_address) ? 1 : radar_category_for_base(type);
    if (!entity.radar_category) return;
    if (entity.radar_category == 2)
        entity.inventory_state = read_inventory_state(entity.address, base, type);
    // TESFullName contains a BGSLocalizedString eight bytes into the mixin.
    const uintptr_t name_offset = type == 0x1F ? 0x88 : type == 0x32 ? 0xC0 : 0x70;
    entity.name = read_pooled_string(base + name_offset);
}

std::optional<std::string> read_checked_pooled_string(uintptr_t field) {
    uintptr_t entry = 0;
    if (!read_value(field, entry)) return {};
    if (!entry) return std::string{};
    for (int depth = 0; entry && depth < 4; ++depth) {
        ReadBlock<0x10> header;
        if (!header.read(entry + 8)) return {};
        const uint16_t flags = header.template get<uint16_t, 0>();
        if (flags & 0x4000) {
            entry = header.template get<uintptr_t, 8>();
            continue;
        }
        if (flags & 0x8000) return {};
        const uint32_t length = header.template get<uint32_t, 8>();
        if (length == 0) return std::string{};
        if (length > 255) return {};
        std::string name(length, '\0');
        if (!read_memory(entry + 0x18, name.data(), length) ||
            name.find('\0') != std::string::npos) return {};
        for (char& character : name)
            if (static_cast<unsigned char>(character) < 0x20) character = ' ';
        return name;
    }
    return {};
}

std::string read_pooled_string(uintptr_t field) {
    return read_checked_pooled_string(field).value_or(std::string{});
}

std::string read_actor_name(uintptr_t actor) {
    uintptr_t base = 0;
    uint8_t type = 0;
    if (!read_value(actor + 0xE0, base) || !base ||
        !read_value(base + 0x1A, type) || type != 45) return {};
    std::string name = read_pooled_string(base + 0x128);
    if (name.empty()) name = read_pooled_string(base + 0x298);
    return name;
}

void read_process_actors(uintptr_t image, Fo4EntitySnapshot& result) {
    uintptr_t process_lists = 0, handle_table = 0, vtable = 0;
    if (!read_value(image + kProcessListsPointerRva, process_lists) || !process_lists ||
        !read_value(process_lists, vtable) || vtable != image + kProcessListsVtableRva) {
        result.actor_status = "Actor process lists are not available";
        return;
    }
    result.process_lists_address = process_lists;
    if (!read_value(image + kHandleTablePointerRva, handle_table) || !handle_table) {
        result.actor_status = "Actor handle table is not available";
        return;
    }
    result.handle_table_address = handle_table;

    constexpr uintptr_t list_offsets[4] = {0x40, 0x58, 0x70, 0x88};
    std::unordered_set<uint32_t> seen;
    for (uint8_t level = 0; level < 4; ++level) {
        uintptr_t entries = 0;
        uint32_t capacity = 0, count = 0;
        uintptr_t list = process_lists + list_offsets[level];
        ReadBlock<0x14> header;
        if (!header.read(list)) {
            result.actor_status = "Actor process lists are not readable";
            result.actors.clear();
            return;
        }
        entries = header.template get<uintptr_t, 0>();
        capacity = header.template get<uint32_t, 8>();
        count = header.template get<uint32_t, 0x10>();
        if (count > capacity ||
            count > kMaxReferences || (count && !entries)) {
            result.actor_status = "Actor process list layout is unavailable";
            result.actors.clear();
            return;
        }
        result.actor_handle_counts[level] = count;
        std::vector<uint32_t> handles(count);
        if (count && !read_memory(entries, handles.data(), count * sizeof(uint32_t))) {
            result.actor_status = "Actor process list changed during snapshot";
            result.actors.clear();
            return;
        }
        for (uint32_t handle : handles) {
            if (!handle || !seen.insert(handle).second) continue;
            uint32_t index = handle & 0x1FFFFF;
            uintptr_t slot = handle_table + uintptr_t(index) * 16;
            uint32_t slot_flags = 0, object_flags = 0;
            uintptr_t object_data = 0;
            ReadBlock<16> slot_data;
            if (!slot_data.read(slot)) continue;
            slot_flags = slot_data.template get<uint32_t, 0>();
            object_data = slot_data.template get<uintptr_t, 8>();
            if (!object_data || object_data < 32 ||
                !(slot_flags & 0x4000000) ||
                (slot_flags & 0x3E00000) != (handle & 0x3E00000)) continue;
            uintptr_t actor = object_data - 32;
            if (!read_value(actor + 40, object_flags) || object_flags >> 11 != index) continue;
            Fo4Entity entity;
            if (!read_entity(actor, entity) || entity.form_type != 65) continue;
            entity.radar_category = 4;
            uint32_t state_word = 0;
            if (read_value(actor + 0x130, state_word)) {
                const uint8_t life_state = static_cast<uint8_t>((state_word >> 17) & 0xF);
                if (life_state == 1 || life_state == 2) {
                    uintptr_t base = 0;
                    uint8_t base_type = 0;
                    if (read_value(actor + 0xE0, base) && base &&
                        read_value(base + 0x1A, base_type))
                        entity.inventory_state = read_inventory_state(actor, base, base_type);
                }
            }
            (void)read_reference_space(actor, entity);
            entity.name = read_actor_name(actor);
            entity.handle = handle;
            entity.process_level = level;
            result.actors.push_back(entity);
        }
    }
    result.actor_status = "Ready";
}
}

bool fo4hud_reference_has_loading_transition(uintptr_t reference, uintptr_t source_cell,
                                             uintptr_t handle_table) {
    if (!reference || !source_cell) return false;
    uintptr_t base = 0;
    uint8_t base_type = 0;
    // Teleport links also exist on invisible placeholder doors. For example,
    // 0024A1A7 uses ccZSEFO4002_BldWoodPDoorLoad02 (0024A1A8), with no
    // MODL/FULL and zero bounds, behind a boarded-up opening. A teleport link
    // alone is not a physical entrance the player can use. DOOR embeds its
    // TESModelTextureSwap at +78; its pooled model path is at +80.
    if (!read_value(reference + 0xE0, base) || !base ||
        !read_value(base + 0x1A, base_type) || base_type != 0x20 ||
        read_pooled_string(base + 0x80).empty()) return false;
    uintptr_t extras = 0, node = 0;
    if (!read_value(reference + 0x100, extras) || !extras ||
        !read_value(extras + 0x08, node)) return false;
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t type = 0;
        uintptr_t next = 0;
        // ExtraTeleport (0x2B) contains DoorTeleportData at +0x18.
        ReadBlock<0xB> node_data;
        if (!node_data.read(node + 8)) return false;
        next = node_data.template get<uintptr_t, 0>();
        type = node_data.template get<uint8_t, 0xA>();
        if (next == node) return false;
        if (type == 0x2B) {
            uintptr_t data = 0, destination = 0;
            uint8_t destination_type = 0, flags = 0;
            if (!read_value(node + 0x18, data) || !data ||
                !read_value(data + 0x24, flags) || (flags & 1) || // kInstant
                !read_value(data, destination)) return false;
            // DoorTeleportData has an optional cached cell at +0 and the
            // linked door's ObjectRefHandle at +8. The nearby shelter hatch
            // (00191F25) has no cached cell; the handle resolves its entrance.
            // ExtraTeleport::Compare (2A3A20) confirms both fields' layout.
            if (destination && read_value(destination + 0x1A, destination_type) && destination_type == 63)
                return destination != source_cell;
            uint32_t handle = 0;
            if (!read_value(data + 8, handle) || !handle) return false;
            if (!handle_table) {
                const uintptr_t image = verified_image.load(std::memory_order_acquire);
                if (!image || !read_value(image + kHandleTablePointerRva, handle_table) || !handle_table)
                    return false;
            }
            const uint32_t index = handle & 0x1FFFFF;
            ReadBlock<16> slot;
            if (!slot.read(handle_table + uintptr_t(index) * 16)) return false;
            const uint32_t slot_flags = slot.template get<uint32_t, 0>();
            const uintptr_t object = slot.template get<uintptr_t, 8>();
            if (object < 32 || !(slot_flags & 0x4000000) ||
                (slot_flags & 0x3E00000) != (handle & 0x3E00000)) return false;
            const uintptr_t door = object - 32;
            uint32_t object_flags = 0;
            return read_value(door + 0x28, object_flags) && object_flags >> 11 == index &&
                   read_value(door + 0x1A, destination_type) && destination_type == 64 &&
                   read_value(door + 0xB8, destination) && destination && destination != source_cell &&
                   read_value(destination + 0x1A, destination_type) && destination_type == 63;
        }
        node = next;
    }
    return false;
}

uintptr_t fo4hud_supported_game_image() {
    const uintptr_t image = find_game_image();
    return known_game_image(image) ? image : 0;
}

bool fo4hud_game_menu_cursor_active() {
    static uintptr_t image = 0;
    if (!image) image = fo4hud_supported_game_image();
    if (!image) return false;
    uintptr_t cursor = 0, vtable = 0;
    uint32_t registrations = 0;
    return read_value(image + kMenuCursorPointerRva, cursor) && cursor &&
           read_value(cursor, vtable) && vtable == image + kMenuCursorVtableRva &&
           read_value(cursor + 0x50, registrations) && registrations > 0 &&
           registrations < 100;
}

namespace {
constexpr uintptr_t kUiPointerRva = 0x030E8930;
constexpr uintptr_t kUiVtableRva = 0x0271DAF8;
constexpr uintptr_t kLockpickVtableRva = 0x025B0A48;
constexpr uintptr_t kLockpickUnlockTimeRva = 0x033130A0;
constexpr uintptr_t kTerminalVtableRva = 0x02540AB8;
constexpr uintptr_t kHackingFlagsRva = 0x0328FB76;
constexpr uintptr_t kHackingMenuRva = 0x0328FB88;
constexpr uintptr_t kHackingLengthRva = 0x0328FB94;
constexpr uintptr_t kHackingWordsRva = 0x0328FCC0;
constexpr uintptr_t kHackingPasswordRva = 0x02F2E19C;
constexpr uintptr_t kHackingBoardRva = 0x0328FD08;

// ValidateHackingWord (RVA B124F0) compares the selected word index with
// 2F2E19C. GenerateBoard (B10560) stores 16-byte word records: BSFixedString,
// character position +8, removed flag +12. B12DB0 maps positions to text.
bool read_hacking_state(uintptr_t image, Fo4HackingState& state) {
    state = {};
    uintptr_t ui = 0, vtable = 0, menu = 0;
    uint8_t flags = 0;
    uint32_t mode = 0, menu_flags = 0, length = 0, index = 0;
    if (!image || !read_value(image + kUiPointerRva, ui) || !ui ||
        !read_value(ui, vtable) || vtable != image + kUiVtableRva ||
        !read_value(image + kHackingMenuRva, menu) || !menu ||
        !read_value(menu, vtable) || vtable != image + kTerminalVtableRva ||
        !read_value(menu + 0x58, menu_flags) || !(menu_flags & 0x40) ||
        !read_value(menu + 0x240, mode) || mode != 1 ||
        !read_value(image + kHackingFlagsRva, flags) || (flags & 5) != 1 ||
        !read_value(image + kHackingLengthRva, length) || !length || length > 12 ||
        !read_value(image + kHackingPasswordRva, index)) return false;
    ReadBlock<0x14> stack, words;
    if (!stack.read(ui + 0x190) || !words.read(image + kHackingWordsRva)) return false;
    const uint32_t count = stack.get<uint32_t, 0x10>();
    const uint32_t word_count = words.get<uint32_t, 0x10>();
    const uintptr_t data = stack.get<uintptr_t, 0>();
    const uintptr_t word_data = words.get<uintptr_t, 0>();
    if (!data || !count || count > 64 || count > stack.get<uint32_t, 8>() ||
        !word_data || !word_count || word_count > 64 || index >= word_count ||
        word_count > words.get<uint32_t, 8>()) return false;
    std::array<uintptr_t, 64> menus{};
    if (!read_memory(data, menus.data(), count * sizeof(uintptr_t)) ||
        std::find(menus.begin(), menus.begin() + count, menu) == menus.begin() + count) return false;
    ReadBlock<16> entry;
    if (!entry.read(word_data + 16 * index) || entry.get<uint8_t, 12>() != 0) return false;
    const uint32_t position = entry.get<uint32_t, 8>();
    if (position >= 384 || length > 384 - position) return false;
    // Resolve BSFixedString's external-data chain with a strict depth bound.
    uintptr_t string = entry.get<uintptr_t, 0>();
    std::array<char, 13> password{};
    bool resolved = false;
    for (unsigned depth = 0; string && depth < 8; ++depth) {
        uint32_t string_flags = 0;
        if (!read_value(string + 8, string_flags)) return false;
        if (!(string_flags & 0x4000)) {
            if (!read_memory(string + 24, password.data(), length + 1)) return false;
            resolved = true;
            break;
        }
        if (!read_value(string + 16, string)) return false;
    }
    if (!resolved || password[length] != '\0') return false;
    uintptr_t board = 0;
    std::array<char, 639> text{};
    if (!read_value(image + kHackingBoardRva, board) || !board ||
        !read_memory(board, text.data(), text.size())) return false;
    Fo4HackingState next;
    for (uint32_t i = 0; i < 384; ++i) {
        const uint32_t offset = 40 * ((i % 192) / 12) + 20 * (i / 192) + i % 12 + 7;
        const unsigned char ch = text[offset];
        if (ch < 33 || ch > 126) return false;
        next.board[i] = char(ch);
    }
    for (uint32_t i = 0; i < length; ++i)
        if (password[i] < 'A' || password[i] > 'Z' || next.board[position + i] != password[i]) return false;
    // Reject a menu close or puzzle regeneration during the checked reads.
    ReadBlock<0x14> confirmed_stack, confirmed_words;
    ReadBlock<16> confirmed_entry;
    uintptr_t confirmed = 0;
    uint32_t confirmed_length = 0, confirmed_index = 0;
    uint8_t confirmed_flags = 0;
    if (!confirmed_stack.read(ui + 0x190) || confirmed_stack.bytes != stack.bytes ||
        !confirmed_words.read(image + kHackingWordsRva) || confirmed_words.bytes != words.bytes ||
        !confirmed_entry.read(word_data + 16 * index) || confirmed_entry.bytes != entry.bytes ||
        !read_value(image + kUiPointerRva, confirmed) || confirmed != ui ||
        !read_value(image + kHackingMenuRva, confirmed) || confirmed != menu ||
        !read_value(image + kHackingBoardRva, confirmed) || confirmed != board ||
        !read_value(image + kHackingLengthRva, confirmed_length) || confirmed_length != length ||
        !read_value(image + kHackingPasswordRva, confirmed_index) || confirmed_index != index ||
        !read_value(image + kHackingFlagsRva, confirmed_flags) || (confirmed_flags & 5) != 1 ||
        !read_value(menu, confirmed) || confirmed != image + kTerminalVtableRva ||
        !read_value(menu + 0x240, mode) || mode != 1 ||
        !read_value(menu + 0x58, menu_flags) || !(menu_flags & 0x40)) return false;
    next.password.assign(password.data(), length);
    next.password_position = position;
    state = std::move(next);
    return true;
}

bool read_lockpick_state(uintptr_t image, Fo4LockpickState& state) {
    state = {};
    uintptr_t ui = 0, vtable = 0;
    uint64_t unlock_time = 0;
    if (!image || !read_value(image + kUiPointerRva, ui) || !ui ||
        !read_value(ui, vtable) || vtable != image + kUiVtableRva ||
        !read_value(image + kLockpickUnlockTimeRva, unlock_time) || unlock_time) return false;
    // UI's active IMenu pointer array: data +0x190, capacity +0x198,
    // size +0x1A0. Confirmed in UI's message pump and stack iteration.
    ReadBlock<0x14> stack;
    if (!stack.read(ui + 0x190)) return false;
    const uintptr_t data = stack.get<uintptr_t, 0>();
    const uint32_t count = stack.get<uint32_t, 0x10>();
    if (!count || count > 64 || count > stack.get<uint32_t, 8>() || !data) return false;
    std::array<uintptr_t, 64> menus{};
    if (!read_memory(data, menus.data(), count * sizeof(uintptr_t))) return false;
    for (uint32_t i = count; i > 0; --i) {
        const uintptr_t menu = menus[i - 1];
        if (!menu || !read_value(menu, vtable) || vtable != image + kLockpickVtableRva) continue;
        uint32_t flags = 0;
        uint8_t initialized = 0;
        if (!read_value(menu + 0x58, flags) || !(flags & 0x40) ||
            !read_value(menu + 0x1B0, initialized) || initialized != 1) return false;
        // UpdatePickAngle uses +0x188. UpdateSweetSpot writes center +0x1A0
        // and full width +0x1A4; the torque handler accepts abs(error)<=width/2.
        ReadBlock<0x20> angles;
        if (!angles.read(menu + 0x188)) return false;
        Fo4LockpickState next{angles.get<float, 0>(), angles.get<float, 0x18>(), angles.get<float, 0x1C>()};
        if (!std::isfinite(next.pick_degrees) || !std::isfinite(next.sweet_spot_degrees) ||
            !std::isfinite(next.sweet_spot_width) || std::abs(next.pick_degrees) > 90.01f ||
            std::abs(next.sweet_spot_degrees) > 90.01f || next.sweet_spot_width < 0 ||
            next.sweet_spot_width > 180.01f ||
            std::abs(next.sweet_spot_degrees) + next.sweet_spot_width * 0.5f > 90.01f) return false;
        ReadBlock<0x14> confirmed;
        uintptr_t current_ui = 0;
        if (!confirmed.read(ui + 0x190) || confirmed.get<uintptr_t, 0>() != data ||
            confirmed.get<uint32_t, 0x10>() != count ||
            !read_value(image + kUiPointerRva, current_ui) || current_ui != ui ||
            !read_value(menu, vtable) || vtable != image + kLockpickVtableRva) return false;
        state = next;
        return true;
    }
    return false;
}
}

bool fo4hud_read_lockpick_state(Fo4LockpickState& state) {
    // The executable stays mapped for the process lifetime. Heap menu objects
    // are rediscovered and checked each frame, never cached or dereferenced.
    static uintptr_t image = 0;
    if (!image) image = fo4hud_supported_game_image();
    return read_lockpick_state(image, state);
}

bool fo4hud_read_hacking_state(Fo4HackingState& state) {
    static uintptr_t image = 0;
    if (!image) image = fo4hud_supported_game_image();
    return read_hacking_state(image, state);
}

namespace {
// Fallout4.exe 1.11.240: DialogueMenu constructor 1042EE0,
// TESTopicManager::GetDialogueTarget BB06B0 and SetDialogueTarget BAEFC0.
constexpr uintptr_t kDialogueVtableRva = 0x025AD118;
constexpr uintptr_t kTopicManagerRva = 0x030E6E50;
constexpr uintptr_t kActorValuesRva = 0x030E7460;
constexpr uintptr_t kActorValueOwnerVtableRva = 0x02568EA8;
constexpr uint32_t kAffinityForm = 0x000A1B80; // Fallout4.esm CA_Affinity

bool resolve_information_actor(uintptr_t image, uint32_t handle, Fo4Entity& target) {
    uintptr_t table = 0;
    if (!read_value(image + kHandleTablePointerRva, table) || !table) return false;
    ReadBlock<16> slot;
    const uint32_t index = handle & 0x1FFFFF;
    if (!slot.read(table + uintptr_t(index) * 16)) return false;
    const auto flags = slot.get<uint32_t, 0>();
    const auto object = slot.get<uintptr_t, 8>();
    uint32_t object_flags = 0, reference_flags = 0;
    if (!(flags & 0x4000000) || (flags & 0x3E00000) != (handle & 0x3E00000) || object < 32 ||
        !read_value(object - 32 + 40, object_flags) || object_flags >> 11 != index ||
        !read_entity(object - 32, target) || target.form_type != 65 || target.form_id == 0x14 ||
        !read_value(target.address + 0x10, reference_flags) || (reference_flags & 0x820)) return false;
    target.handle = handle;
    return true;
}


bool companion_dialogue_target(uintptr_t image, Fo4Entity& target) {
    target = {};
    uintptr_t ui = 0, vtable = 0, manager = 0;
    uint8_t talking = 0;
    uint32_t handle = 0;
    if (!image || !read_value(image + kUiPointerRva, ui) || !ui ||
        !read_value(ui, vtable) || vtable != image + kUiVtableRva ||
        !read_value(image + kTopicManagerRva, manager) || !manager ||
        !read_value(manager + 0x4C, talking) || talking != 1 ||
        !read_value(manager + 0x14, handle) || !handle) return false;
    ReadBlock<0x14> stack;
    if (!stack.read(ui + 0x190)) return false;
    const auto count = stack.get<uint32_t, 0x10>();
    const auto data = stack.get<uintptr_t, 0>();
    if (!data || !count || count > 64 || count > stack.get<uint32_t, 8>()) return false;
    std::array<uintptr_t, 64> menus{};
    if (!read_memory(data, menus.data(), count * 8)) return false;
    bool active = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t flags = 0;
        uint8_t enabled = 0;
        if (menus[i] && read_value(menus[i], vtable) && vtable == image + kDialogueVtableRva &&
            read_value(menus[i] + 0x58, flags) && (flags & 0x40) &&
            read_value(menus[i] + 0x164, enabled) && enabled == 1) active = true;
    }
    return active && resolve_information_actor(image, handle, target);
}

// HUDDataModel (9DF910) owns HUDRolloverModel at +0x18. Its activation
// event source (+0x580) publishes the interaction target handle at +8 and
// validity at +12 (A20920). This is the actual game pick, not a nearby guess.
constexpr uintptr_t kHudDataPointerRva = 0x032745F8;
constexpr uintptr_t kHudDataVtableRva = 0x02531498;
constexpr uintptr_t kRolloverVtableRva = 0x02533770;
constexpr uintptr_t kActivateSourceVtableRva = 0x02533758;
constexpr uintptr_t kViewCastSinkVtableRva = 0x02532AC0;
constexpr uintptr_t kActivatePickLengthRva = 0x02F2B100; // fActivatePickLength:Interface

bool nearby_information_actor(uintptr_t image, Fo4Entity& target) {
    target = {};
    uintptr_t cursor = 0, vtable = 0, hud = 0, player = 0;
    uint32_t registrations = 0, handle = 0;
    uint8_t valid = 0;
    if (!image || !read_value(image + kMenuCursorPointerRva, cursor) || !cursor ||
        !read_value(cursor, vtable) || vtable != image + kMenuCursorVtableRva ||
        !read_value(cursor + 0x50, registrations) || registrations != 0 ||
        !read_value(image + kHudDataPointerRva, hud) || !hud ||
        !read_value(hud, vtable) || vtable != image + kHudDataVtableRva ||
        !read_value(hud + 0x18, vtable) || vtable != image + kRolloverVtableRva ||
        !read_value(hud + 0x598, vtable) || vtable != image + kActivateSourceVtableRva ||
        !read_value(hud + 0x5A4, valid) || valid > 1 ||
        !read_value(hud + 0x5A0, handle) ||
        !read_value(image + kPlayerPointerRva, player) || !player) return false;
    if (!valid || !handle) {
        // ViewCasterUpdateEvent is HUDRolloverModel's sink at +0x3B0
        // (A1EE60). Its first handle is the direct view pick (A273E0,
        // A24CD0), even for creatures without a usable activation prompt.
        if (!read_value(hud + 0x3C8, vtable) || vtable != image + kViewCastSinkVtableRva ||
            !read_value(hud + 0x408, valid) || valid != 1 ||
            !read_value(hud + 0x3D0, handle) || !handle) return false;
    }
    if (!resolve_information_actor(image, handle, target)) return false;
    Fo4Entity player_space;
    float position[3]{};
    float range = 0;
    uint32_t life = 0;
    if (!read_value(target.address + 0x130, life) || ((life >> 17) & 0xF) == 1 || ((life >> 17) & 0xF) == 2 ||
        !read_reference_space(player, player_space) || !read_reference_space(target.address, target) ||
        !read_memory(player + 0xD0, position, sizeof(position)) ||
        !read_value(image + kActivatePickLengthRva, range) || !std::isfinite(range) || range < 1 || range > 5000)
        return false;
    if (player_space.cell_address != target.cell_address &&
        (player_space.cell_is_interior || target.cell_is_interior || !player_space.worldspace_address ||
         player_space.worldspace_address != target.worldspace_address)) return false;
    double distance = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double delta = double(target.position[axis]) - position[axis];
        if (!std::isfinite(delta)) return false;
        distance += delta * delta;
    }
    return distance <= double(range) * range;
}

bool information_actor_target(uintptr_t image, Fo4Entity& target) {
    return companion_dialogue_target(image, target) || nearby_information_actor(image, target);
}

// TESActorBaseData faction records: base+0xB8 data, +0xC8 size (30AA70).
// ExtraFactionChanges (type 91) overrides the base rank (C8EDF0, 2A9370).
// Recognize potential, current and former companions, including DLC/mod NPCs
// using the game's companion factions; never identify an NPC by display name.
bool companion_factions(uintptr_t actor) {
    bool companion = false;
    auto scan = [&](uintptr_t data, uint32_t count) {
        if (count > 1024 || (count && !data)) return false;
        std::array<unsigned char, 16 * 1024> records{};
        if (count && !read_memory(data, records.data(), size_t(count) * 16)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t faction = 0;
            std::memcpy(&faction, records.data() + i * 16, 8);
            const int8_t rank = static_cast<int8_t>(records[i * 16 + 8]);
            uint32_t id = 0;
            uint8_t type = 0;
            if (!faction || !read_value(faction + 0x14, id) ||
                !read_value(faction + 0x1A, type) || type != 14) return false;
            if (rank >= 0 && (id == 0x1EC1B9 || id == 0x23C01 || id == 0xA1B85)) companion = true;
        }
        return true;
    };
    uintptr_t base = 0, data = 0;
    uint8_t type = 0;
    uint32_t count = 0;
    if (!read_value(actor + 0xE0, base) || !base ||
        !read_value(base + 0x1A, type) || type != 45 ||
        !read_value(base + 0xB8, data) || !read_value(base + 0xC8, count) || !scan(data, count)) return false;
    uint32_t base_id = 0;
    if (read_value(base + 0x14, base_id) && base_id == 0x1D15C) return true; // Dogmeat has separate factions.
    // PotentialCompanionFaction is a capability marker and does not require
    // an active follower. Runtime ranks allow recruited custom companions too.
    if (companion) return true;
    uintptr_t extras = 0, node = 0;
    if (!read_value(actor + 0x100, extras)) return false;
    if (!extras) return false;
    if (!read_value(extras + 8, node)) return false;
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t extra_type = 0;
        uintptr_t next = 0;
        if (!read_value(node + 0x12, extra_type) || !read_value(node + 8, next) || next == node) return false;
        if (extra_type == 91) {
            if (!read_value(node + 0x20, data) || !read_value(node + 0x30, count) || !scan(data, count)) return false;
            return companion;
        }
        node = next;
    }
    return false;
}

// Confirm only explicit identity markers, never a human race, display name,
// friendly faction, or a synth component transferred into an inventory.
Fo4SynthStatus companion_synth_status(uintptr_t actor) {
    constexpr auto confirmed = Fo4SynthStatus::Confirmed;
    constexpr auto unknown = Fo4SynthStatus::Unknown;
    uintptr_t npc = 0;
    uint8_t type = 0;
    if (!read_value(actor + 0x1A, type) || type != 65 ||
        !read_value(actor + 0xE0, npc) || !npc ||
        !read_value(npc + 0x1A, type) || type != 45) return unknown;

    auto has_synth_keyword = [](uintptr_t form, uintptr_t component) {
        // BGSKeywordForm: array +0x10, count +0x18 (303F20).
        // GetKeywordForm (303DC0): NPC +0x158, race +0x78.
        uintptr_t data = 0;
        uint32_t count = 0;
        if (!read_value(form + component + 0x10, data) ||
            !read_value(form + component + 0x18, count) || count > 1024 ||
            (count && !data)) return false;
        std::array<uintptr_t, 1024> keywords{};
        if (count && !read_memory(data, keywords.data(), size_t(count) * 8)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = 0;
            uint8_t keyword_type = 0;
            if (keywords[i] && read_value(keywords[i] + 0x1A, keyword_type) && keyword_type == 4 &&
                read_value(keywords[i] + 0x14, id) && id == 0x10C3CE) return true; // ActorTypeSynth
        }
        return false;
    };
    // Actor.GetRace (1107900) returns actor+0x418. ActorBase.GetRace
    // (114A670) uses NPC+0x1B8; use it only when the runtime race is absent.
    uintptr_t race = 0;
    if (!read_value(actor + 0x418, race) || !race) (void)read_value(npc + 0x1B8, race);
    uint32_t race_id = 0;
    if (race && read_value(race + 0x1A, type) && type == 17) {
        if (read_value(race + 0x14, race_id) &&
            (race_id == 0xE8D09 || race_id == 0x10BD65 || race_id == 0x2261A4)) return confirmed;
        if (has_synth_keyword(race, 0x78)) return confirmed;
    }
    if (has_synth_keyword(npc, 0x158)) return confirmed;

    // TESNPC's INAM loader (64F230) stores the death item at +0x88.
    // Fallout4.esm: LL_SynthDeathItem and BoSPaladinDanseDeathItem each
    // explicitly contain SynthDeathItem. This identifies human-race synths
    // even before their death loot is instantiated in the live inventory.
    uintptr_t death_item = 0;
    uint32_t death_id = 0;
    if (read_value(npc + 0x88, death_item) && death_item &&
        read_value(death_item + 0x1A, type) && type == 56 && // LVLI
        read_value(death_item + 0x14, death_id) &&
        (death_id == 0x1357E6 || death_id == 0xD10AC)) return confirmed;
    return unknown;
}

std::string information_actor_race(uintptr_t actor, Fo4SynthStatus synth) {
    if (synth == Fo4SynthStatus::Confirmed) return "Synth";
    uintptr_t race = 0, npc = 0;
    if (!read_value(actor + 0x418, race) || !race) {
        if (!read_value(actor + 0xE0, npc) || !npc || !read_value(npc + 0x1B8, race)) return "Unknown";
    }
    uint32_t id = 0;
    uint8_t type = 0;
    if (!race || !read_value(race + 0x1A, type) || type != 17 || !read_value(race + 0x14, id)) return "Unknown";
    switch (id) {
        case 0x13746: return "Human";
        case 0x11D83F: return "Human (child)";
        case 0x1A009: return "Super mutant";
        case 0xBB7D9: return "Super mutant behemoth";
        case 0x6B4EC: return "Feral ghoul";
        case 0xA96BF: return "Feral ghoul (glowing)";
        case 0xEAFB6: case 0x11EB96: return "Ghoul";
        case 0x1D698: case 0x3578A: case 0x187AF9: return "Dog";
    }
    // TESRace embeds TESFullName at +0x20 (6865E0); retain custom race names.
    std::string name = read_pooled_string(race + 0x28);
    return name.empty() ? "Unknown" : name;
}

// CA_Affinity is a plugin-defined AVIF and is absent from ActorValueList's
// built-in array. Game.GetForm (1129EC0) uses TESForm::LookupByID (311B80).
// Resolve on the game thread under the engine's own form-table lock.
uintptr_t companion_affinity_info(uintptr_t image) {
    using Lookup = void* (__attribute__((ms_abi)) *)(uint32_t);
    const auto info = reinterpret_cast<uintptr_t>(reinterpret_cast<Lookup>(image + 0x311B80)(kAffinityForm));
    uint32_t id = 0;
    uint8_t type = 0;
    return info && read_value(info + 0x14, id) && id == kAffinityForm &&
        read_value(info + 0x1A, type) && type == 98 ? info : 0;
}

std::optional<float> companion_inventory_weight(uintptr_t image, uintptr_t actor) {
    // TESObjectREFR::GetWeight (507DA0) fills BGSInventoryList's weight cache
    // using CalcContainerWeight, including stack extras, equipment and perks.
    // Its NPC return value is capped at capacity; read the computed cache to
    // preserve the actual load when a companion is over capacity.
    uintptr_t inventory = 0;
    if (!read_value(actor + 0xF8, inventory)) return {};
    using GetWeight = float (__attribute__((ms_abi)) *)(void*);
    float weight = reinterpret_cast<GetWeight>(image + 0x507DA0)(reinterpret_cast<void*>(actor));
    uintptr_t confirmed = 0;
    if (!read_value(actor + 0xF8, confirmed) || confirmed != inventory) return {};
    if (inventory && !read_value(inventory + 0x70, weight)) return {};
    return std::isfinite(weight) && weight >= 0 && weight <= 1000000 ?
        std::optional<float>(weight) : std::nullopt;
}

// Called on the game thread: GetValue may touch TLS and engine AV caches.
bool sample_companion(uintptr_t image, Fo4CompanionState& state) {
    state = {};
    Fo4Entity target;
    if (!information_actor_target(image, target)) return false;
    uintptr_t vtable = 0, getter = 0, modifier = 0;
    const uintptr_t owner = target.address + 0x58;
    if (!read_value(owner, vtable) || vtable != image + kActorValueOwnerVtableRva ||
        !read_value(vtable + 8, getter) || getter != image + 0xCC93D0 ||
        !read_value(vtable + 7 * 8, modifier) || modifier != image + 0xCC9E00) return false;
    using Getter = float (__attribute__((ms_abi)) *)(void*, void*);
    using Modifier = float (__attribute__((ms_abi)) *)(void*, int32_t, void*);
    auto value = [&](uintptr_t info) -> std::optional<float> {
        uint8_t type = 0;
        if (!info || !read_value(info + 0x1A, type) || type != 98) return {};
        const float v = reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(owner), reinterpret_cast<void*>(info));
        return std::isfinite(v) && std::abs(v) <= 1000000 ? std::optional<float>(v) : std::nullopt;
    };
    auto info_at = [&](uintptr_t offset) {
        uintptr_t info = 0;
        read_value(image + kActorValuesRva + offset, info);
        return info;
    };
    auto maximum = [&](uintptr_t info, std::optional<float> current) -> std::optional<float> {
        if (!current) return {};
        const float damage = reinterpret_cast<Modifier>(modifier)(reinterpret_cast<void*>(owner), 2, reinterpret_cast<void*>(info));
        const float max = *current - damage;
        return std::isfinite(max) && max > 0 && max <= 1000000 ? std::optional<float>(max) : std::nullopt;
    };
    Fo4CompanionState next;
    next.address = target.address; next.form_id = target.form_id; next.handle = target.handle;
    next.name = read_actor_name(target.address);
    next.synth_status = companion_synth_status(target.address);
    next.race = information_actor_race(target.address, next.synth_status);
    next.is_companion = companion_factions(target.address);
    uintptr_t npc = 0;
    uint32_t npc_id = 0;
    if (next.is_companion && read_value(target.address + 0xE0, npc) && read_value(npc + 0x14, npc_id) && npc_id != 0x1D15C)
        next.affinity = value(companion_affinity_info(image)); // Dogmeat does not use affinity.
    const auto health = info_at(0xD8), ap = info_at(0x08);
    next.health = value(health); next.max_health = maximum(health, next.health);
    next.action_points = value(ap); next.max_action_points = maximum(ap, next.action_points);
    // ActorValueList constructor (6F9400), in SPECIAL order.
    constexpr uintptr_t special[] = {0x248, 0x170, 0xC0, 0x88, 0x100, 0x18, 0x140};
    for (unsigned i = 0; i < 7; ++i) next.special[i] = value(info_at(special[i]));
    next.carry_weight = value(info_at(0x80));
    next.inventory_weight = companion_inventory_weight(image, target.address);
    next.damage_resistance = value(info_at(0xA8));
    next.energy_resistance = value(info_at(0x318));
    Fo4Entity confirmed;
    if (!information_actor_target(image, confirmed) || confirmed.address != target.address ||
        confirmed.form_id != target.form_id || confirmed.handle != target.handle) return false;
    state = std::move(next);
    return true;
}

std::mutex companion_mutex;
Fo4CompanionState companion_state;
std::chrono::steady_clock::time_point companion_sample_time{};

bool read_cached_companion(uintptr_t image, Fo4CompanionState& state,
                           std::chrono::steady_clock::time_point now) {
    state = {};
    Fo4Entity target;
    if (!information_actor_target(image, target)) return false;
    std::lock_guard<std::mutex> lock(companion_mutex);
    if (!companion_state.address || target.address != companion_state.address ||
        target.form_id != companion_state.form_id || target.handle != companion_state.handle ||
        now - companion_sample_time > std::chrono::seconds(1)) return false;
    state = companion_state;
    return true;
}
}

void fo4hud_companion_tick(uintptr_t image, void* player) {
    if (!image || !player) return;
    const auto now = std::chrono::steady_clock::now();
    static auto last = std::chrono::steady_clock::time_point{};
    if (now - last < std::chrono::milliseconds(200)) return;
    last = now;
    Fo4CompanionState next;
    sample_companion(image, next);
    std::lock_guard<std::mutex> lock(companion_mutex);
    companion_state = std::move(next);
    companion_sample_time = now;
}

bool fo4hud_read_companion_state(Fo4CompanionState& state) {
    state = {};
    static uintptr_t image = 0;
    if (!image) image = fo4hud_supported_game_image();
    if (!image) return false;
    return read_cached_companion(image, state, std::chrono::steady_clock::now());
}

Fo4EntitySnapshot fo4hud_read_current_cell() {
    Fo4EntitySnapshot result;
    uintptr_t image = find_game_image();
    result.image_address = image;
    if (!image) { result.status = "Fallout4.exe is not mapped"; return result; }
    if (!known_game_image(image)) { result.status = "Unsupported Fallout 4 executable"; return result; }
    read_process_actors(image, result);

    uintptr_t player = 0, cell = 0;
    if (!read_value(image + kPlayerPointerRva, player) || !player) {
        result.status = "Player is not loaded";
        return result;
    }
    result.player_address = player;
    read_memory(player + 0xD0, result.player_position, sizeof(result.player_position));
    result.player_heading_valid = read_value(player + 0xC8, result.player_heading_radians) &&
                                  std::isfinite(result.player_heading_radians);
    uint8_t player_type = 0;
    if (!read_value(player + 0x1A, player_type) || player_type != 65) {
        result.status = "Player layout does not match F4SE";
        return result;
    }
    if (!read_value(player + 0xB8, cell) || !cell) {
        result.status = "Player has no current cell";
        return result;
    }
    result.cell_address = cell;
    uint8_t cell_type = 0;
    if (!read_value(cell + 0x1A, cell_type) || cell_type != 63) {
        result.status = "Cell layout does not match F4SE";
        return result;
    }
    read_value(cell + 0x14, result.cell_form_id);
    uint16_t cell_flags = 0;
    if (!read_value(cell + 0x40, cell_flags)) {
        result.status = "Current cell flags are unavailable";
        return result;
    }
    result.cell_is_interior = (cell_flags & 1) != 0;
    if (!result.cell_is_interior)
        (void)read_value(cell + 0xC8, result.worldspace_address);

    uintptr_t entries = 0;
    uint32_t capacity = 0, count = 0;
    if (!read_value(cell + 0x70, entries) || !read_value(cell + 0x78, capacity) ||
        !read_value(cell + 0x80, count) || count > capacity || count > kMaxReferences ||
        (count && !entries)) {
        result.status = "Current cell list is unavailable";
        return result;
    }
    result.total_references = count;
    result.entries_address = entries;
    result.capacity = capacity;
    std::vector<uintptr_t> references(count);
    if (count && !read_memory(entries, references.data(), count * sizeof(uintptr_t))) {
        result.status = "Current cell list changed during snapshot";
        return result;
    }

    result.entities.reserve(count);
    for (uintptr_t reference : references) {
        if (!reference) continue;
        Fo4Entity entity;
        if (!read_entity(reference, entity)) continue;
        entity.cell_address = cell;
        classify_radar_reference(entity);
        result.entities.push_back(entity);
    }

    uintptr_t current_cell = 0, current_entries = 0;
    uint32_t current_count = 0;
    if (!read_value(player + 0xB8, current_cell) || !read_value(cell + 0x70, current_entries) ||
        !read_value(cell + 0x80, current_count) || current_cell != cell ||
        current_entries != entries || current_count != count) {
        result.entities.clear();
        result.status = "Current cell changed during snapshot; refresh again";
        return result;
    }
    if (!result.cell_is_interior && result.worldspace_address) {
        uintptr_t tes = 0, tes_vtable = 0, grid = 0, grid_vtable = 0, grid_data = 0;
        uint32_t dimension = 0;
        if (read_value(image + kTesPointerRva, tes) && tes &&
            read_value(tes, tes_vtable) && tes_vtable == image + kTesVtableRva &&
            read_value(tes + 0x18, grid) && grid &&
            read_value(grid, grid_vtable) && grid_vtable == image + kGridCellVtableRva &&
            read_value(grid + 0x10, dimension) && dimension >= 1 && dimension <= 11 &&
            read_value(grid + 0x18, grid_data) && grid_data) {
            std::vector<uintptr_t> loaded_cells(dimension * dimension);
            if (read_memory(grid_data, loaded_cells.data(), loaded_cells.size() * sizeof(uintptr_t))) {
                std::unordered_set<uintptr_t> seen(references.begin(), references.end());
                for (uintptr_t nearby_cell : loaded_cells) {
                    if (!nearby_cell || nearby_cell == cell) continue;
                    uint8_t nearby_type = 0;
                    uint16_t nearby_flags = 0;
                    uintptr_t nearby_world = 0, nearby_entries = 0;
                    uint32_t nearby_capacity = 0, nearby_count = 0;
                    if (!read_value(nearby_cell + 0x1A, nearby_type) || nearby_type != 63 ||
                        !read_value(nearby_cell + 0x40, nearby_flags) || (nearby_flags & 1) ||
                        !read_value(nearby_cell + 0xC8, nearby_world) ||
                        nearby_world != result.worldspace_address ||
                        !read_value(nearby_cell + 0x70, nearby_entries) ||
                        !read_value(nearby_cell + 0x78, nearby_capacity) ||
                        !read_value(nearby_cell + 0x80, nearby_count) ||
                        nearby_count > nearby_capacity || nearby_count > kMaxReferences ||
                        (nearby_count && !nearby_entries)) continue;
                    std::vector<uintptr_t> nearby_refs(nearby_count);
                    if (nearby_count &&
                        !read_memory(nearby_entries, nearby_refs.data(), nearby_count * sizeof(uintptr_t)))
                        continue;
                    uintptr_t confirmed_entries = 0;
                    uint32_t confirmed_count = 0;
                    if (!read_value(nearby_cell + 0x70, confirmed_entries) ||
                        !read_value(nearby_cell + 0x80, confirmed_count) ||
                        confirmed_entries != nearby_entries || confirmed_count != nearby_count)
                        continue;
                    result.total_references += nearby_count;
                    for (uintptr_t reference : nearby_refs) {
                        if (!reference || !seen.insert(reference).second) continue;
                        Fo4Entity entity;
                        if (!read_entity(reference, entity)) continue;
                        entity.cell_address = nearby_cell;
                        classify_radar_reference(entity);
                        if (entity.radar_category) result.entities.push_back(std::move(entity));
                    }
                }
            }
        }
    }
    result.status = "Ready";
    std::sort(result.actors.begin(), result.actors.end(), [&](const Fo4Entity& left, const Fo4Entity& right) {
        auto distance_squared = [&](const Fo4Entity& actor) {
            double squared = 0;
            for (int i = 0; i < 3; ++i) {
                double delta = double(actor.position[i]) - result.player_position[i];
                squared += delta * delta;
            }
            return std::isfinite(squared) ? squared : std::numeric_limits<double>::infinity();
        };
        return distance_squared(left) < distance_squared(right);
    });
    return result;
}

bool Fo4EntityScanOptions::operator==(const Fo4EntityScanOptions& o) const {
    return actor_range == o.actor_range && radar_range == o.radar_range &&
           loot_range == o.loot_range && trap_range == o.trap_range && radar_categories == o.radar_categories &&
           actor_names == o.actor_names && corpse_inventories == o.corpse_inventories && loot_items == o.loot_items &&
           loot_inventories == o.loot_inventories && harvest == o.harvest;
}

struct Fo4EntityRefresh::State {
    using Time = Clock::time_point;
    static constexpr size_t cache_limit = 2048;
    Fo4EntitySnapshot view;
    Fo4EntityScanOptions options;
    Fo4EntityScanStats metrics;
    Time now{}, actor_due{}, world_due{}, inventory_due{};
    bool changed = false;
    unsigned turn = 0;
    struct Name { uintptr_t entry; std::string text; Time expires; };
    struct Form { uint32_t id; uint8_t type, category; uintptr_t name_field; Time expires; };
    struct Inventory { uint32_t id; Fo4InventoryState state, display_state; Time expires; };
    std::unordered_map<uintptr_t, Name> names;
    std::unordered_map<uintptr_t, Form> forms;
    std::unordered_map<uintptr_t, Inventory> inventories;

    struct Actors {
        bool active = false;
        uintptr_t lists = 0, table = 0;
        unsigned level = 0;
        size_t cursor = 0;
        std::vector<uint32_t> handles;
        std::unordered_set<uint32_t> seen;
        std::vector<Fo4Entity> output;
    } actors;
    struct World {
        bool active = false, grid_complete = false;
        size_t cell_cursor = 0, cursor = 0;
        uintptr_t cell = 0;
        uint32_t cell_id = 0;
        std::vector<uintptr_t> cells, references;
        std::unordered_set<uintptr_t> seen;
        std::vector<Fo4Entity> output, trap_output;
        uint32_t total = 0;
    } world;
    struct Probe {
        bool active = false, fallback = false;
        Fo4Entity entity;
        uintptr_t base = 0, inventory = 0, data = 0, stack = 0;
        uint32_t base_id = 0;
        uint8_t base_type = 0, item_type = 0;
        uint32_t size = 0, index = 0, nodes = 0;
        size_t base_offset = 0;
        std::unordered_set<uintptr_t> seen;
    } probe;
    size_t inventory_cursor = 0;

    bool want_actors() const { return options.actor_range > 0 || (options.radar_categories & (1 << 4)); }
    bool want_world() const { return (options.radar_categories & 0x2E) || options.loot_items || options.loot_inventories || options.harvest || options.trap_range > 0; }
    bool want_inventories() const { return options.corpse_inventories || options.loot_inventories || (options.radar_categories & ((1 << 2) | (1 << 4))); }
    double distance(const Fo4Entity& e, bool horizontal = false) const {
        double result = 0;
        for (int i = 0; i < (horizontal ? 2 : 3); ++i) {
            const double delta = double(e.position[i]) - view.player_position[i];
            result += delta * delta;
        }
        return result;
    }
    bool within(const Fo4Entity& e, float radius, bool horizontal = false) const {
        const double d = distance(e, horizontal);
        return radius > 0 && std::isfinite(d) && d <= double(radius) * radius;
    }
    bool radar_wants(const Fo4Entity& e, unsigned category) const {
        return (options.radar_categories & (1 << category)) && within(e, options.radar_range, true);
    }
    bool world_wants(const Fo4Entity& e, unsigned category) const {
        return radar_wants(e, category) ||
            (category == 4 && options.corpse_inventories && within(e, options.actor_range)) ||
            (within(e, options.loot_range) &&
             ((category == 3 && options.loot_items) ||
              (category == 2 && options.loot_inventories) ||
              (category == 4 && options.loot_inventories) ||
              (category == 5 && options.harvest)));
    }
    template <typename Map> void bound(Map& cache, uintptr_t key) {
        if (cache.size() >= cache_limit && cache.find(key) == cache.end()) cache.erase(cache.begin());
    }
    std::string name(uintptr_t field) {
        uintptr_t entry = 0;
        if (!read_value(field, entry) || !entry) { names.erase(field); return {}; }
        auto it = names.find(field);
        if (it != names.end() && it->second.entry == entry && now < it->second.expires)
            return it->second.text;
        std::string text = read_pooled_string(field);
        if (!text.empty()) {
            bound(names, field);
            names[field] = {entry, text, now + std::chrono::seconds(5)};
        } else names.erase(field);
        return text;
    }
    bool form(uintptr_t base, Form& result) {
        ReadBlock<7> identity;
        if (!base || !identity.read(base + 0x14)) { forms.erase(base); return false; }
        const uint32_t id = identity.get<uint32_t, 0>();
        const uint8_t type = identity.get<uint8_t, 6>();
        auto it = forms.find(base);
        if (it != forms.end() && it->second.id == id && it->second.type == type && now < it->second.expires) {
            result = it->second;
            return true;
        }
        result = {id, type, radar_category_for_base(type), 0, now + std::chrono::seconds(2)};
        if (fo4hud_scripted_loot_category(type, id)) result.category = 3;
        else if (type == 0x29) {
            uintptr_t produce = 0;
            if (read_value(base + 0x158, produce) && produce) result.category = 5;
        }
        result.name_field = base + (type == 45 ? 0x128 : type == 0x1F ? 0x88 : type == 0x32 ? 0xC0 : 0x70);
        bound(forms, base);
        forms[base] = result;
        return true;
    }
    void cached_inventory(Fo4Entity& e) {
        e.inventory_state = Fo4InventoryState::Unknown;
        e.inventory_display_state = Fo4InventoryState::Unknown;
        auto it = inventories.find(e.address);
        if (it != inventories.end() && it->second.id == e.form_id) {
            e.inventory_display_state = it->second.display_state;
            if (now < it->second.expires) e.inventory_state = it->second.state;
        }
    }
    bool context(Fo4EntitySnapshot& root) {
        root.image_address = view.image_address ? view.image_address : fo4hud_supported_game_image();
        if (!root.image_address) { root.status = "Supported Fallout 4 executable is not available"; return false; }
        ReadBlock<0xC8> player;
        if (!read_value(root.image_address + kPlayerPointerRva, root.player_address) || !root.player_address ||
            !player.read(root.player_address + 0x14) || player.get<uint8_t, 6>() != 65) {
            root.status = "Player is not loaded"; return false;
        }
        root.cell_address = player.get<uintptr_t, 0xA4>();
        std::memcpy(root.player_position, player.bytes.data() + 0xBC, sizeof(root.player_position));
        root.player_heading_radians = player.get<float, 0xB4>();
        root.player_heading_valid = std::isfinite(root.player_heading_radians);
        ReadBlock<0xBC> cell;
        if (!root.cell_address || !cell.read(root.cell_address + 0x14) || cell.get<uint8_t, 6>() != 63) {
            root.status = "Player has no valid current cell"; return false;
        }
        root.cell_form_id = cell.get<uint32_t, 0>();
        root.cell_is_interior = (cell.get<uint16_t, 0x2C>() & 1) != 0;
        if (!root.cell_is_interior) root.worldspace_address = cell.get<uintptr_t, 0xB4>();
        root.status = "Ready";
        root.actor_status = "Collecting actors";
        return true;
    }
    void cancel() {
        actors = {}; world = {}; probe = {};
        actor_due = world_due = inventory_due = {};
        inventory_cursor = 0;
    }
    void actor_failed() {
        actors = {};
        actor_due = now + std::chrono::milliseconds(100);
    }
    bool actor_step() {
        if (!want_actors()) return false;
        if (!actors.active) {
            if (now < actor_due) return false;
            actors = {};
            uintptr_t vtable = 0;
            if (!read_value(view.image_address + kProcessListsPointerRva, actors.lists) || !actors.lists ||
                !read_value(actors.lists, vtable) || vtable != view.image_address + kProcessListsVtableRva ||
                !read_value(view.image_address + kHandleTablePointerRva, actors.table) || !actors.table) {
                actor_failed(); return true;
            }
            actors.active = true;
            return true;
        }
        if (actors.cursor == actors.handles.size()) {
            if (actors.level == 4) {
                // No game reads in the comparator; positions were already filtered.
                std::sort(actors.output.begin(), actors.output.end(), [&](const auto& a, const auto& b) {
                    return distance(a) < distance(b);
                });
                // Inventory probes can finish while this list is being built.
                // Merge their latest result at publication, not collection.
                for (auto& e : actors.output) cached_inventory(e);
                view.actors = std::move(actors.output);
                view.process_lists_address = actors.lists;
                view.handle_table_address = actors.table;
                view.actor_status = "Ready";
                actors = {};
                actor_due = now + std::chrono::milliseconds(250);
                ++metrics.actor_passes; changed = true;
                return true;
            }
            ReadBlock<0x14> header;
            const uintptr_t list = actors.lists + 0x40 + actors.level * 0x18;
            if (!header.read(list)) { actor_failed(); return true; }
            const uintptr_t entries = header.get<uintptr_t, 0>();
            const uint32_t capacity = header.get<uint32_t, 8>(), count = header.get<uint32_t, 0x10>();
            if (count > capacity || count > kMaxReferences || (count && !entries)) { actor_failed(); return true; }
            actors.handles.resize(count); actors.cursor = 0;
            if (count && !read_memory(entries, actors.handles.data(), count * sizeof(uint32_t))) {
                actor_failed(); return true;
            }
            view.actor_handle_counts[actors.level++] = count;
            return true;
        }
        const uint32_t handle = actors.handles[actors.cursor++];
        if (!handle || !actors.seen.insert(handle).second) return true;
        const uint32_t index = handle & 0x1FFFFF;
        ReadBlock<16> slot;
        if (!slot.read(actors.table + uintptr_t(index) * 16)) return true;
        const uint32_t flags = slot.get<uint32_t, 0>();
        const uintptr_t object = slot.get<uintptr_t, 8>();
        if (object < 32 || !(flags & 0x4000000) || (flags & 0x3E00000) != (handle & 0x3E00000)) return true;
        const uintptr_t address = object - 32;
        uint32_t object_flags = 0;
        Fo4Entity e;
        if (!read_value(address + 40, object_flags) || object_flags >> 11 != index ||
            !read_entity(address, e) || e.form_type != 65 ||
            !read_reference_space(address, e) || !fo4hud_actor_in_player_space(view, e)) return true;
        if (!within(e, options.actor_range) && !radar_wants(e, 4)) { ++metrics.distant_skipped; return true; }
        e.radar_category = 4; e.handle = handle; e.process_level = actors.level - 1;
        if (options.actor_names) {
            uintptr_t base = 0; Form info;
            if (read_value(address + 0xE0, base) && form(base, info) && info.type == 45) {
                e.name = name(info.name_field);
                if (e.name.empty()) e.name = name(base + 0x298);
            }
        }
        actors.output.push_back(std::move(e));
        return true;
    }
    void begin_world() {
        world = {}; world.active = true;
        world.cells.push_back(view.cell_address);
        if (view.cell_is_interior || !view.worldspace_address) { world.grid_complete = true; return; }
        uintptr_t tes = 0, vtable = 0, grid = 0;
        ReadBlock<16> header;
        if (!read_value(view.image_address + kTesPointerRva, tes) || !tes ||
            !read_value(tes, vtable) || vtable != view.image_address + kTesVtableRva ||
            !read_value(tes + 0x18, grid) || !grid ||
            !read_value(grid, vtable) || vtable != view.image_address + kGridCellVtableRva ||
            !header.read(grid + 0x10)) return;
        const uint32_t dimension = header.get<uint32_t, 0>();
        const uintptr_t data = header.get<uintptr_t, 8>();
        if (dimension < 1 || dimension > 11 || !data) return;
        std::vector<uintptr_t> cells(dimension * dimension);
        if (read_memory(data, cells.data(), cells.size() * sizeof(uintptr_t))) {
            world.grid_complete = true;
            for (uintptr_t cell : cells)
                if (cell && std::find(world.cells.begin(), world.cells.end(), cell) == world.cells.end())
                    world.cells.push_back(cell);
        }
    }
    void world_failed() {
        world = {};
        world_due = now + std::chrono::milliseconds(100);
    }
    bool world_step() {
        if (!want_world()) return false;
        if (!world.active) {
            if (now < world_due) return false;
            begin_world(); return true;
        }
        if (world.cursor == world.references.size()) {
            if (world.cell_cursor == world.cells.size()) {
                // A missing exterior grid must not replace a complete nearby
                // snapshot with a current-cell-only scan. Bootstrap may still
                // show the current cell before the grid becomes available.
                if (!world.grid_complete && !view.entities.empty()) { world_failed(); return true; }
                for (auto& e : world.output) cached_inventory(e);
                view.entities = std::move(world.output);
                view.trap_candidates = std::move(world.trap_output);
                view.total_references = world.total;
                world = {};
                world_due = now + std::chrono::milliseconds(options.loot_items || options.harvest || options.trap_range > 0 ? 500 : 1000);
                ++metrics.world_passes; changed = true;
                return true;
            }
            world.cell = world.cells[world.cell_cursor++];
            world.references.clear(); world.cursor = 0;
            ReadBlock<0xBC> cell;
            if (!cell.read(world.cell + 0x14)) { world_failed(); return true; }
            if (cell.get<uint8_t, 6>() != 63) return true;
            const bool interior = (cell.get<uint16_t, 0x2C>() & 1) != 0;
            if (world.cell != view.cell_address && (interior || cell.get<uintptr_t, 0xB4>() != view.worldspace_address)) return true;
            world.cell_id = cell.get<uint32_t, 0>();
            const uintptr_t data = cell.get<uintptr_t, 0x5C>();
            const uint32_t capacity = cell.get<uint32_t, 0x64>(), count = cell.get<uint32_t, 0x6C>();
            if (count > capacity || count > kMaxReferences || (count && !data)) { world_failed(); return true; }
            world.references.resize(count);
            if (count && !read_memory(data, world.references.data(), count * sizeof(uintptr_t))) {
                world_failed(); return true;
            }
            // Confirm the copied pointer array before yielding. Later changes
            // are safe: every pointed-to reference is freshly checked below.
            ReadBlock<0x14> confirmed;
            if (!confirmed.read(world.cell + 0x70) || confirmed.get<uintptr_t, 0>() != data ||
                confirmed.get<uint32_t, 0x10>() != count) { world_failed(); return true; }
            world.total += count;
            return true;
        }
        const uintptr_t address = world.references[world.cursor++];
        if (!address || !world.seen.insert(address).second) return true;
        Fo4Entity e;
        if (!read_entity(address, e)) return true;
        bool nearby = within(e, options.trap_range);
        for (unsigned category : {1u, 2u, 3u, 4u, 5u}) nearby |= world_wants(e, category);
        if (!nearby) { ++metrics.distant_skipped; return true; }
        ReadBlock<0xD8> ref;
        if (!ref.read(address + 0x10) || ref.get<uint32_t, 4>() != e.form_id ||
            ref.get<uint8_t, 0xA>() != e.form_type || ref.get<uintptr_t, 0xA8>() != world.cell ||
            (ref.get<uint32_t, 0>() & ((1u << 5) | (1u << 11)))) return true;
        e.cell_address = world.cell;
        e.cell_is_interior = view.cell_is_interior;
        e.worldspace_address = view.worldspace_address;
        if (within(e, options.trap_range) && world.trap_output.size() < 1024) {
            uint8_t base_type = 0;
            if (read_value(ref.get<uintptr_t, 0xD0>() + 0x1A, base_type) &&
                ((e.form_type == 64 && trap_script_candidate_base(base_type)) || (e.form_type == 68 && base_type == 51)))
                world.trap_output.push_back(e);
        }
        if (e.form_type == 65) {
            uint8_t life = 0;
            if (!options.loot_inventories || !world_wants(e, 4) ||
                !fo4hud_read_actor_life_state(e, life) || (life != 1 && life != 2)) return true;
            e.radar_category = 4;
        } else if (e.form_type == 64) {
            Form info;
            if (!form(ref.get<uintptr_t, 0xD0>(), info)) return true;
            e.radar_category = info.category;
            // The extra-data walk is bounded to 256 nodes by the helper.
            if (radar_wants(e, 1) && fo4hud_reference_has_loading_transition(address, world.cell)) e.radar_category = 1;
            if (!e.radar_category || !world_wants(e, e.radar_category)) return true;
            if (radar_wants(e, e.radar_category))
                e.name = fo4hud_is_cap_stash_form(info.type, info.id) ? "Cap stash" :
                         fo4hud_is_mirelurk_egg_form(info.type, info.id) ? "Mirelurk egg" : name(info.name_field);
        } else return true;
        world.output.push_back(std::move(e));
        return true;
    }
    void finish_inventory(Fo4InventoryState result) {
        // Recheck identity and the inventory header after a multi-frame walk.
        Fo4Entity live;
        uintptr_t current = 0, current_base = 0, current_cell = 0;
        bool valid = read_entity(probe.entity.address, live) && live.form_id == probe.entity.form_id &&
                     live.form_type == probe.entity.form_type &&
                     read_value(live.address + 0xF8, current) && current == probe.inventory &&
                     read_value(live.address + 0xE0, current_base) && current_base == probe.base &&
                     read_value(live.address + 0xB8, current_cell) && current_cell == probe.entity.cell_address;
        if (valid) {
            ReadBlock<7> identity;
            valid = identity.read(probe.base + 0x14) && identity.get<uint32_t, 0>() == probe.base_id &&
                identity.get<uint8_t, 6>() == probe.base_type && read_reference_space(live.address, live) &&
                fo4hud_actor_in_player_space(view, live);
        }
        if (valid) {
            if (probe.fallback) {
                ReadBlock<0xC> header;
                valid = header.read(probe.base + probe.base_offset + 8) &&
                    header.get<uintptr_t, 0>() == probe.data && header.get<uint32_t, 8>() == probe.size;
            } else {
                ReadBlock<0x14> header;
                valid = probe.inventory && header.read(probe.inventory + 0x58) &&
                    header.get<uintptr_t, 0>() == probe.data && header.get<uint32_t, 0x10>() == probe.size;
            }
        }
        if (!valid) result = Fo4InventoryState::Unknown;
        const bool near = within(probe.entity, std::max(1500.0f, options.loot_range));
        auto display_state = result;
        const auto previous = inventories.find(probe.entity.address);
        if (result == Fo4InventoryState::Unknown && previous != inventories.end() &&
            previous->second.id == probe.entity.form_id)
            display_state = previous->second.display_state;
        bound(inventories, probe.entity.address);
        inventories[probe.entity.address] = {probe.entity.form_id, result, display_state,
            // Nearby inventories change as the player loots them, including
            // manual looting. Keep their colors responsive without moving any
            // stack walks out of the incremental frame budget.
            now + std::chrono::milliseconds(near ? 100 : 5000)};
        for (auto* list : {&view.entities, &view.actors})
            for (auto& e : *list)
                if (e.address == probe.entity.address && e.form_id == probe.entity.form_id) cached_inventory(e);
        probe = {};
        ++metrics.inventory_probes;
    }
    bool inventory_step() {
        if (!want_inventories()) return false;
        if (!probe.active) {
            if (now < inventory_due) return false;
            const size_t total = view.entities.size() + view.actors.size();
            if (!total) { inventory_due = now + std::chrono::milliseconds(100); return false; }
            if (inventory_cursor >= total) {
                inventory_cursor = 0; inventory_due = now + std::chrono::milliseconds(25); return false;
            }
            const size_t i = inventory_cursor++;
            const Fo4Entity& e = i < view.entities.size() ? view.entities[i] : view.actors[i - view.entities.size()];
            if (e.radar_category != 2 && e.radar_category != 4) return true;
            if (!world_wants(e, e.radar_category)) return true;
            if (e.radar_category == 4) {
                uint8_t life = 0;
                if (e.address == view.player_address || !fo4hud_read_actor_life_state(e, life) || (life != 1 && life != 2)) return true;
            }
            auto cached = inventories.find(e.address);
            if (cached != inventories.end() && cached->second.id == e.form_id && now < cached->second.expires) return true;
            Fo4Entity live;
            if (!read_entity(e.address, live) || live.form_id != e.form_id || live.form_type != e.form_type) return true;
            probe = {}; probe.entity = e; probe.active = true;
            Form info;
            if (!read_value(e.address + 0xE0, probe.base) || !form(probe.base, info) ||
                !read_value(e.address + 0xF8, probe.inventory)) { finish_inventory(Fo4InventoryState::Unknown); return true; }
            probe.base_id = info.id; probe.base_type = info.type;
            if (probe.inventory) {
                ReadBlock<0x14> header;
                if (!header.read(probe.inventory + 0x58)) { finish_inventory(Fo4InventoryState::Unknown); return true; }
                probe.data = header.get<uintptr_t, 0>(); probe.size = header.get<uint32_t, 0x10>();
                if (probe.size > header.get<uint32_t, 8>() || probe.size > 10000 || (probe.size && !probe.data))
                    { finish_inventory(Fo4InventoryState::Unknown); return true; }
            } else {
                probe.fallback = true; probe.base_offset = info.type == 0x1F ? 0x68 : info.type == 45 ? 0xD0 : 0;
                ReadBlock<0xC> header;
                if (!probe.base_offset || !header.read(probe.base + probe.base_offset + 8))
                    { finish_inventory(Fo4InventoryState::Unknown); return true; }
                probe.data = header.get<uintptr_t, 0>(); probe.size = header.get<uint32_t, 8>();
                if (probe.size > 10000 || (probe.size && !probe.data))
                    { finish_inventory(Fo4InventoryState::Unknown); return true; }
            }
            return true;
        }
        ++metrics.inventory_steps;
        if (++probe.nodes > 20000) { finish_inventory(Fo4InventoryState::Unknown); return true; }
        if (probe.stack) {
            ReadBlock<0x14> stack;
            if (!probe.seen.insert(probe.stack).second || !stack.read(probe.stack + 0x10))
                { finish_inventory(Fo4InventoryState::Unknown); return true; }
            if (stack.get<uint32_t, 0x10>()) {
                const auto state = inventory_stack_state(probe.item_type, stack.get<uintptr_t, 8>());
                if (state != Fo4InventoryState::Empty) { finish_inventory(state); return true; }
            }
            probe.stack = stack.get<uintptr_t, 0>();
            return true;
        }
        if (probe.index == probe.size) { finish_inventory(Fo4InventoryState::Empty); return true; }
        if (probe.fallback) {
            uintptr_t object = 0; int32_t count = 0;
            if (!read_value(probe.data + uintptr_t(probe.index++) * 8, object) || !object || !read_value(object, count))
                { finish_inventory(Fo4InventoryState::Unknown); return true; }
            if (count > 0) {
                uintptr_t item = 0;
                if (!read_value(object + 8, item)) { finish_inventory(Fo4InventoryState::Unknown); return true; }
                const auto state = inventory_item_state(item, probe.item_type);
                if (state != Fo4InventoryState::Empty) finish_inventory(state);
            }
        } else {
            ReadBlock<16> entry;
            if (!entry.read(probe.data + uintptr_t(probe.index++) * 16))
                { finish_inventory(Fo4InventoryState::Unknown); return true; }
            if (entry.get<uintptr_t, 0>()) {
                const auto state = inventory_item_state(entry.get<uintptr_t, 0>(), probe.item_type);
                if (state == Fo4InventoryState::Unknown) { finish_inventory(state); return true; }
                if (state == Fo4InventoryState::HasItems) probe.stack = entry.get<uintptr_t, 8>();
            }
        }
        return true;
    }
};

Fo4EntityRefresh::Fo4EntityRefresh() : state_(std::make_unique<State>()) {}
Fo4EntityRefresh::~Fo4EntityRefresh() = default;
const Fo4EntitySnapshot& Fo4EntityRefresh::snapshot() const { return state_->view; }
const Fo4EntityScanStats& Fo4EntityRefresh::stats() const { return state_->metrics; }

bool Fo4EntityRefresh::update(const Fo4EntityScanOptions& options, std::chrono::microseconds budget, Clock::time_point now) {
    auto& s = *state_;
    const auto start = Clock::now();
    const auto deadline = start + std::max(budget, std::chrono::microseconds(0));
    s.now = now; s.changed = false;
    Fo4EntitySnapshot root;
    const bool ready = s.context(root);
    if (!ready && s.view.status == "Ready") {
        // A failed bulk read is not evidence of a transition. Retain the
        // completed snapshot only while its live player/cell identity agrees.
        uintptr_t player = 0, cell = 0;
        uint32_t id = 0;
        if (read_value(s.view.image_address + kPlayerPointerRva, player) && player == s.view.player_address &&
            read_value(player + 0xB8, cell) && cell == s.view.cell_address &&
            read_value(cell + 0x14, id) && id == s.view.cell_form_id) {
            s.cancel();
            s.metrics.last_slice_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            return false;
        }
    }
    const bool new_context = root.player_address != s.view.player_address || root.cell_address != s.view.cell_address ||
        root.cell_form_id != s.view.cell_form_id || root.cell_is_interior != s.view.cell_is_interior ||
        root.worldspace_address != s.view.worldspace_address;
    const bool same_exterior = ready && s.view.status == "Ready" &&
        root.player_address == s.view.player_address && !root.cell_is_interior && !s.view.cell_is_interior &&
        root.worldspace_address && root.worldspace_address == s.view.worldspace_address;
    if (new_context && same_exterior) {
        // Crossing an outdoor cell boundary does not change the radar's
        // worldspace. Restart pending scans without blanking the published data.
        s.cancel();
        s.view.cell_address = root.cell_address;
        s.view.cell_form_id = root.cell_form_id;
        s.changed = true;
    }
    if (!ready || (new_context && !same_exterior)) {
        s.cancel(); s.names.clear(); s.forms.clear(); s.inventories.clear();
        s.view = std::move(root); s.changed = true;
    } else {
        std::copy(root.player_position, root.player_position + 3, s.view.player_position);
        s.view.player_heading_radians = root.player_heading_radians;
        s.view.player_heading_valid = root.player_heading_valid;
    }
    if (!(s.options == options)) { s.cancel(); s.options = options; }
    if (s.actors.active) {
        uintptr_t lists = 0, table = 0;
        if (!read_value(s.view.image_address + kProcessListsPointerRva, lists) || lists != s.actors.lists ||
            !read_value(s.view.image_address + kHandleTablePointerRva, table) || table != s.actors.table)
            s.actor_failed();
    }
    if (s.world.active && s.world.cursor < s.world.references.size()) {
        ReadBlock<0xBC> cell;
        if (!cell.read(s.world.cell + 0x14) || cell.get<uint32_t, 0>() != s.world.cell_id ||
            cell.get<uint8_t, 6>() != 63 ||
            (s.world.cell != s.view.cell_address &&
             ((cell.get<uint16_t, 0x2C>() & 1) || cell.get<uintptr_t, 0xB4>() != s.view.worldspace_address)))
            s.world = {};
    }
    if (!s.want_actors()) { s.view.actors.clear(); s.view.actor_status = "Ready"; }
    if (!s.want_world()) { s.view.entities.clear(); s.view.trap_candidates.clear(); }
    if (options.trap_range <= 0) s.view.trap_candidates.clear();
    if (ready) {
        unsigned idle = 0;
        while (Clock::now() < deadline && idle < 3) {
            bool work = false;
            switch (s.turn++ % 3) {
                case 0: work = s.actor_step(); break;
                case 1: work = s.world_step(); break;
                case 2: work = s.inventory_step(); break;
            }
            idle = work ? 0 : idle + 1;
        }
    }
    s.metrics.cached_names = s.names.size(); s.metrics.cached_forms = s.forms.size();
    s.metrics.last_slice_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return s.changed;
}

bool fo4hud_player_in_snapshot_cell(const Fo4EntitySnapshot& snapshot) {
    uintptr_t current_cell = 0;
    return snapshot.status == "Ready" && snapshot.player_address && snapshot.cell_address &&
           read_value(snapshot.player_address + 0xB8, current_cell) &&
           current_cell == snapshot.cell_address;
}

bool fo4hud_actor_in_player_space(const Fo4EntitySnapshot& snapshot, const Fo4Entity& actor) {
    if (!actor.cell_address || !snapshot.cell_address) return false;
    if (snapshot.cell_is_interior) return actor.cell_address == snapshot.cell_address;
    return !actor.cell_is_interior && snapshot.worldspace_address &&
           actor.worldspace_address == snapshot.worldspace_address;
}

bool fo4hud_read_actor_position(const Fo4Entity& actor, float position[3]) {
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    if (!actor.address || !read_value(actor.address + 0x14, form_id) ||
        !read_value(actor.address + 0x1A, form_type) || form_id != actor.form_id ||
        form_type != 65 || !read_memory(actor.address + 0xD0, position, 3 * sizeof(float))) return false;
    return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
}

bool fo4hud_world_reference_visible(const Fo4Entity& entity) {
    if (!entity.address || entity.form_type != 64 || !entity.radar_category) return false;
    uint32_t form_id = 0, flags = 0;
    uint8_t type = 0;
    uintptr_t cell = 0;
    return read_value(entity.address + 0x14, form_id) && form_id == entity.form_id &&
           read_value(entity.address + 0x1A, type) && type == entity.form_type &&
           read_value(entity.address + 0x10, flags) &&
           !(flags & ((1u << 5) | (1u << 11))) &&
           read_value(entity.address + 0xB8, cell) && cell &&
           (!entity.cell_address || cell == entity.cell_address);
}

bool fo4hud_is_cap_stash_base(uintptr_t base) {
    ReadBlock<7> data;
    return base && data.read(base + 0x14) &&
           fo4hud_is_cap_stash_form(data.template get<uint8_t, 6>(),
                                   data.template get<uint32_t, 0>());
}

uint32_t fo4hud_scripted_loot_category_for_base(uintptr_t base) {
    ReadBlock<7> data;
    if (!base || !data.read(base + 0x14)) return 0;
    return fo4hud_scripted_loot_category(data.get<uint8_t, 6>(), data.get<uint32_t, 0>());
}

// TESProduceForm::Activate (0x31C640) rejects reference flag 0x2000;
// successful harvest sets it through 0x312E40. Regrowth clears it again.
bool fo4hud_flora_harvestable(uintptr_t reference) {
    uint8_t type = 0;
    uint32_t flags = 0;
    uintptr_t base = 0, produce = 0;
    return reference && read_value(reference + 0x1A, type) && type == 64 &&
           read_value(reference + 0x10, flags) &&
           !(flags & ((1u << 5) | (1u << 11) | (1u << 13))) &&
           read_value(reference + 0xE0, base) && base &&
           read_value(base + 0x1A, type) && type == 0x29 &&
           read_value(base + 0x158, produce) && produce;
}

bool fo4hud_container_auto_loot_safe(uintptr_t base) {
    uint8_t type = 0;
    uint32_t form_id = 0;
    if (!base || !read_value(base + 0x1A, type) || type != 0x1F ||
        !read_value(base + 0x14, form_id)) return false;
    // Base-game and DLC settlement resource containers have no workshop
    // keyword. These local FormIDs come from the installed ESM records;
    // compare the low 24 bits because DLC load-order prefixes can change.
    switch (form_id & 0x00FFFFFF) {
        case 0x000C1AEB: // WorkshopWorkbench
        case 0x000C1AEC: // WorkshopWorkbenchWireOnly
        case 0x0012E2C4: // WorkshopWorkbenchInterior
        case 0x00246F86: // WorkshopWorkbenchNonSettlement
        case 0x0024A336: // VRWorkshopShared_VRWorkshopWorkbench
        case 0x00023A96: // DLC03WorkshopWorkbench
        case 0x00003390: // DLC03VRWorkshopWorkbench
        case 0x00047DFA: // DLC04WorkshopWorkbench
        case 0x0000BE55: // DLC04WorkshopWorkbenchNukaWorld
        case 0x00000819: // DLC06WorkshopWorkbench
        case 0x00010222: // DLC01WorkshopWorkbench
        case 0x0022BF75: // WorkshopResourceContainerSettlement
        case 0x0022BF74: // WorkshopResourceContainerFree
        case 0x001AB88E: // WorkshopResourceContainerAirport
        case 0x000D2941: // WorkshopResourceContainerDmndPlayerHouse
        case 0x000A471A: case 0x000A4719: case 0x000A4718:
        case 0x000A4717: case 0x000A4716: case 0x000A4715:
        case 0x000A4714: case 0x000A4713: case 0x000A4712:
        case 0x000A4711: case 0x000A470F: // Settlement resource containers
        case 0x0001939B: // SanctuaryWorkshopResourceContainerOLD
        case 0x000D3940: // SanctuaryWorkshopResourceContainer
        case 0x000B3657: // WorkshopResourceContainerCastle
        case 0x00026831: // WorkshopResourceContainerHostile
        case 0x00061C0F: // WorkshopWorldResourceContainer
        case 0x00049CB7: // DLC03 visitors center resource container
        case 0x00003B0E: // DLC06 vault workshop resource container
            return false;
        default: break;
    }
    // TESObjectCONT::BGSKeywordForm begins at +0xF8. Workshop workbenches
    // in Fallout4.esm and DLCs all carry WorkshopKeyword (00054BA7).
    uintptr_t keywords = 0;
    uint32_t count = 0;
    if (!read_value(base + 0x108, keywords) ||
        !read_value(base + 0x110, count) || count > 256 ||
        (count && !keywords)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t keyword = 0;
        uint32_t keyword_id = 0;
        if (!read_value(keywords + uintptr_t(i) * 8, keyword) || !keyword ||
            !read_value(keyword + 0x14, keyword_id)) return false;
        if (keyword_id == 0x00054BA7 || // WorkshopKeyword
            keyword_id == 0x00246F85 || // WorkshopWorkbenchTypeSettlement
            keyword_id == 0x0023D9BA)   // WorkshopWorkbenchTypeInteriorOnly
            return false;
    }
    return true;
}

bool fo4hud_extra_list_unowned(uintptr_t extra_list) {
    if (!extra_list) return true;
    uintptr_t node = 0;
    // ExtraDataList::extraData (BaseExtraList) begins at +0x08.
    if (!read_value(extra_list + 0x08, node)) return false;
    for (unsigned depth = 0; node && depth < 256; ++depth) {
        uint8_t type = 0;
        uintptr_t next = 0;
        // BSExtraData::type is at +0x12; next is at +0x08.
        if (!read_value(node + 0x12, type) ||
            !read_value(node + 0x08, next) || next == node) return false;
        if (type == 0x21) return false; // ExtraOwnership
        node = next;
    }
    return node == 0;
}

bool fo4hud_reference_unowned(uintptr_t reference, uintptr_t cell) {
    uintptr_t reference_extras = 0, cell_extras = 0;
    uint8_t cell_type = 0;
    return reference && cell &&
           read_value(cell + 0x1A, cell_type) && cell_type == 63 &&
           read_value(reference + 0x100, reference_extras) &&
           fo4hud_extra_list_unowned(reference_extras) &&
           read_value(cell + 0x48, cell_extras) &&
           fo4hud_extra_list_unowned(cell_extras);
}

bool fo4hud_base_container_unowned(uintptr_t base, uint8_t base_type) {
    const uintptr_t container_offset = base_type == 0x1F ? 0x68 :
                                       base_type == 45 ? 0xD0 : 0;
    if (!base || !container_offset) return false;
    uintptr_t objects = 0;
    uint32_t count = 0;
    if (!read_value(base + container_offset + 0x08, objects) ||
        !read_value(base + container_offset + 0x10, count) ||
        count > 10000 || (count && !objects)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t object = 0, item_extra = 0, owner = 0;
        if (!read_value(objects + uintptr_t(i) * 8, object) || !object ||
            !read_value(object + 0x10, item_extra)) return false;
        if (item_extra && (!read_value(item_extra, owner) || owner)) return false;
    }
    return true;
}

bool fo4hud_read_actor_life_state(const Fo4Entity& actor, uint8_t& life_state) {
    // ActorState starts at Actor+0x128. lifeState occupies bits 17..20 of
    // the first state word at +0x08 (CommonLibF4 ActorState layout).
    uint32_t state_word = 0;
    if (!actor.address || !read_value(actor.address + 0x130, state_word)) return false;
    life_state = static_cast<uint8_t>((state_word >> 17) & 0xF);
    return true;
}

bool fo4hud_read_actor_label_position(const Fo4Entity& actor, const float origin[3],
                                      float position[3]) {
    // Actor reference coordinates stay fixed while the animated skeleton sways.
    // The high process exposes the current head and torso NiAVObject nodes.
    uintptr_t process = 0, middle_high = 0, node = 0;
    if (!read_value(actor.address + 0x300, process) || !process ||
        !read_value(process + 0x08, middle_high) || !middle_high) return false;
    for (uintptr_t offset : {uintptr_t(0x1F8), uintptr_t(0x200)}) {
        if (!read_value(middle_high + offset, node) || !node ||
            !read_memory(node + 0xA0, position, 3 * sizeof(float))) continue;
        bool valid = true;
        for (int axis = 0; axis < 3; ++axis)
            valid &= std::isfinite(position[axis]);
        if (!valid || std::abs(position[0] - origin[0]) > 400.0f ||
            std::abs(position[1] - origin[1]) > 400.0f ||
            position[2] - origin[2] < -100.0f ||
            position[2] - origin[2] > 500.0f) continue;
        // The head node is the label anchor. If it is unavailable, place the
        // label just above the live torso node instead.
        if (offset == 0x200) position[2] += 25.0f;
        return true;
    }
    return false;
}

bool fo4hud_read_actor_head_position(const Fo4Entity& actor, const float origin[3],
                                     float position[3]) {
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    uintptr_t process = 0, middle_high = 0, head = 0;
    if (!actor.address || !read_value(actor.address + 0x14, form_id) ||
        !read_value(actor.address + 0x1A, form_type) ||
        form_id != actor.form_id || form_type != 65 ||
        !read_value(actor.address + 0x300, process) || !process ||
        !read_value(process + 0x08, middle_high) || !middle_high ||
        !read_value(middle_high + 0x1F8, head) || !head ||
        !read_memory(head + 0xA0, position, 3 * sizeof(float))) return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(position[axis])) return false;
        const float difference = position[axis] - origin[axis];
        if ((axis == 2 && (difference < -100.0f || difference > 500.0f)) ||
            (axis != 2 && std::abs(difference) > 400.0f)) return false;
    }
    return true;
}

bool fo4hud_read_actor_skeleton(const Fo4Entity& actor, const float origin[3],
                                std::vector<Fo4BoneSegment>& segments,
                                std::vector<Fo4AimPoint>* aim_points) {
    segments.clear();
    if (aim_points) aim_points->clear();
    uint32_t form_id = 0;
    uint8_t form_type = 0;
    uintptr_t loaded = 0, root = 0;
    if (!actor.address || !read_value(actor.address + 0x14, form_id) ||
        !read_value(actor.address + 0x1A, form_type) ||
        form_id != actor.form_id || form_type != 65 ||
        !read_value(actor.address + 0xF0, loaded) || !loaded ||
        !read_value(loaded + 8, root) || !root) return false;

    struct Bone { uintptr_t node; int parent; std::string name; };
    struct Cache {
        uintptr_t root = 0;
        std::chrono::steady_clock::time_point scanned{};
        std::vector<Bone> bones;
    };
    // Only the render thread calls this function. The model can be replaced
    // without changing the actor pointer, so validate its root every frame.
    static std::unordered_map<uintptr_t, Cache> caches;
    if (caches.size() > 256) caches.clear();
    Cache& cache = caches[actor.address];
    const auto now = std::chrono::steady_clock::now();
    if (cache.root != root || now - cache.scanned > std::chrono::seconds(2)) {
        cache.root = root;
        cache.scanned = now;
        cache.bones.clear();

        uintptr_t children_vtable = 0;
        if (!read_value(root + 0x120, children_vtable) || !children_vtable) return false;
        auto node_name = [](uintptr_t node) { return read_pooled_string(node + 0x10); };
        auto children = [&](uintptr_t node, std::vector<uintptr_t>& out) {
            uintptr_t vtable = 0, data = 0;
            uint16_t capacity = 0, size = 0;
            if (!read_value(node + 0x120, vtable) || vtable != children_vtable ||
                !read_value(node + 0x128, data) ||
                !read_value(node + 0x130, capacity) ||
                !read_value(node + 0x134, size) ||
                size > capacity || capacity > 128 || (size && !data)) return;
            std::vector<uintptr_t> entries(capacity);
            if (capacity && !read_memory(data, entries.data(), capacity * sizeof(uintptr_t))) return;
            for (uintptr_t child : entries) if (child) out.push_back(child);
        };

        std::vector<uintptr_t> root_children;
        children(root, root_children);
        uintptr_t skeleton_root = 0;
        for (uintptr_t child : root_children) {
            const std::string name = node_name(child);
            if (name == "Root" || name.rfind("Root [", 0) == 0 ||
                (name.size() > 5 && name.compare(name.size() - 5, 5, "_Root") == 0)) {
                skeleton_root = child;
                break;
            }
        }
        if (!skeleton_root) return false;

        struct Work { uintptr_t node; int parent; int depth; };
        std::vector<Work> work{{skeleton_root, -1, 0}};
        std::unordered_set<uintptr_t> visited;
        while (!work.empty() && cache.bones.size() < 192) {
            const Work item = work.back();
            work.pop_back();
            if (!item.node || item.depth > 16 || !visited.insert(item.node).second) continue;
            uintptr_t node_children_vtable = 0;
            if (!read_value(item.node + 0x120, node_children_vtable) ||
                node_children_vtable != children_vtable) continue;
            const std::string name = node_name(item.node);
            if (name.empty()) continue;
            const bool helper = name.size() >= 5 &&
                name.compare(name.size() - 5, 5, "_skin") == 0;
            const bool draw_bone = !helper && name != "PipboyBone" &&
                name.find("Attach") == std::string::npos;
            int parent = item.parent;
            if (draw_bone) {
                parent = static_cast<int>(cache.bones.size());
                cache.bones.push_back({item.node, item.parent, name});
            }
            std::vector<uintptr_t> next;
            children(item.node, next);
            for (uintptr_t child : next) work.push_back({child, parent, item.depth + 1});
        }
    }

    if (cache.bones.size() < 3) return false;
    std::vector<std::array<float, 3>> positions(cache.bones.size());
    std::vector<bool> valid(cache.bones.size(), false);
    for (size_t i = 0; i < cache.bones.size(); ++i) {
        auto& point = positions[i];
        if (!read_memory(cache.bones[i].node + 0xA0, point.data(), 3 * sizeof(float))) continue;
        valid[i] = true;
        for (int axis = 0; axis < 3; ++axis)
            valid[i] = valid[i] && std::isfinite(point[axis]) &&
                       std::abs(point[axis] - origin[axis]) < 2500.0f;
    }
    if (aim_points) {
        // Names are taken from Fallout4 - Meshes.ba2 skeleton.nif files.
        // Human, super mutant, power armor, and ghoul rigs differ slightly.
        auto find_bone = [&](const char* primary, const char* alternate = nullptr) -> int {
            for (size_t i = 0; i < cache.bones.size(); ++i)
                if (valid[i] && (cache.bones[i].name == primary ||
                    (alternate && cache.bones[i].name == alternate)))
                    return static_cast<int>(i);
            return -1;
        };
        auto add = [&](Fo4BodyPart part, const char* primary, const char* alternate,
                       const char* secondary = nullptr, const char* secondary_alt = nullptr) {
            int first = find_bone(primary, alternate);
            if (first < 0) return;
            int second = secondary ? find_bone(secondary, secondary_alt) : -1;
            Fo4AimPoint point;
            point.part = part;
            for (int axis = 0; axis < 3; ++axis)
                point.position[axis] = second >= 0 ?
                    (positions[first][axis] + positions[second][axis]) * 0.5f :
                    positions[first][axis];
            aim_points->push_back(point);
        };
        add(Fo4BodyPart::Head, "HEAD", "Head");
        add(Fo4BodyPart::Chest, "Chest", "SPINE2");
        add(Fo4BodyPart::LeftArm, "LArm_UpperArm", "LUPPERARM",
            "LArm_ForeArm2", "LForeArm2");
        add(Fo4BodyPart::RightArm, "RArm_UpperArm", "RUPPERARM",
            "RArm_ForeArm2", "RForeArm2");
        add(Fo4BodyPart::LeftLeg, "LLeg_Thigh", "LTHIGH",
            "LLeg_Calf", "LCALF");
        add(Fo4BodyPart::RightLeg, "RLeg_Thigh", "RTHIGH",
            "RLeg_Calf", "RCALF");
    }
    for (size_t i = 0; i < cache.bones.size(); ++i) {
        const int parent = cache.bones[i].parent;
        if (parent < 0 || !valid[i] || !valid[parent]) continue;
        float length_squared = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float delta = positions[i][axis] - positions[parent][axis];
            length_squared += delta * delta;
        }
        if (length_squared < 1.0f || length_squared > 800.0f * 800.0f) continue;
        Fo4BoneSegment segment;
        for (int axis = 0; axis < 3; ++axis) {
            segment.from[axis] = positions[parent][axis];
            segment.to[axis] = positions[i][axis];
        }
        segments.push_back(segment);
    }
    return !segments.empty();
}

bool fo4hud_read_player_position(const Fo4EntitySnapshot& snapshot, float position[3]) {
    if (!snapshot.player_address ||
        !read_memory(snapshot.player_address + 0xD0, position, 3 * sizeof(float))) return false;
    return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]);
}

bool fo4hud_read_player_heading(const Fo4EntitySnapshot& snapshot, float& heading_radians) {
    if (!snapshot.player_address ||
        !read_value(snapshot.player_address + 0xC8, heading_radians)) return false;
    return std::isfinite(heading_radians);
}

bool fo4hud_read_projection(const Fo4EntitySnapshot& snapshot, Fo4Projection& projection) {
    if (!snapshot.image_address || snapshot.status != "Ready") return false;
    uint8_t ready = 0;
    if (!read_value(snapshot.image_address + kProjectionReadyRva, ready) || !ready ||
        !read_memory(snapshot.image_address + kProjectionMatrixRva, projection.matrix, sizeof(projection.matrix)) ||
        !read_memory(snapshot.image_address + kProjectionPortRva, projection.port, sizeof(projection.port))) return false;
    for (const auto& row : projection.matrix)
        for (float value : row) if (!std::isfinite(value)) return false;
    for (float value : projection.port) if (!std::isfinite(value)) return false;
    return projection.port[1] > projection.port[0] && projection.port[2] > projection.port[3];
}

bool fo4hud_projection_camera_position(const Fo4Projection& projection, float position[3]) {
    // At the perspective camera origin clip X, Y and W are zero. Solve those
    // three planes from the same matrix used for world-to-screen projection.
    const auto& a = projection.matrix[0];
    const auto& b = projection.matrix[1];
    const auto& c = projection.matrix[3];
    const float bc[3]{b[1]*c[2]-b[2]*c[1], b[2]*c[0]-b[0]*c[2],
                      b[0]*c[1]-b[1]*c[0]};
    const float ca[3]{c[1]*a[2]-c[2]*a[1], c[2]*a[0]-c[0]*a[2],
                      c[0]*a[1]-c[1]*a[0]};
    const float ab[3]{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2],
                      a[0]*b[1]-a[1]*b[0]};
    const float determinant = a[0]*bc[0] + a[1]*bc[1] + a[2]*bc[2];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f) return false;
    for (int axis = 0; axis < 3; ++axis) {
        position[axis] = (-a[3]*bc[axis] - b[3]*ca[axis] - c[3]*ab[axis]) /
                         determinant;
        if (!std::isfinite(position[axis])) return false;
    }
    return true;
}

bool fo4hud_project_world(const Fo4Projection& projection, const float position[3],
                          float width, float height, float screen[2]) {
    if (width <= 0.0f || height <= 0.0f) return false;
    float clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = position[0] * projection.matrix[row][0] +
                    position[1] * projection.matrix[row][1] +
                    position[2] * projection.matrix[row][2] + projection.matrix[row][3];
    float reciprocal_w = 1.0f / std::fabs(clip[3]);
    if (!std::isfinite(reciprocal_w) || std::fabs(clip[3]) <= 0.01f) return false;
    float depth = clip[2] * reciprocal_w;
    float x = (clip[0] * reciprocal_w + 1.0f) * 0.5f;
    float y = (clip[1] * reciprocal_w + 1.0f) * 0.5f;
    x = projection.port[0] + x * (projection.port[1] - projection.port[0]);
    y = projection.port[3] + y * (projection.port[2] - projection.port[3]);
    // The game's matrix and viewport already account for ultrawide output.
    // A second horizontal aspect correction displaced ESP away from actors.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(depth) ||
        depth < 0.0f || x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return false;
    screen[0] = x * width;
    screen[1] = (1.0f - y) * height;
    return true;
}

std::string fo4hud_entity_log_path() {
    const char* state_home = std::getenv("XDG_STATE_HOME");
    if (state_home && state_home[0] == '/') return std::string(state_home) + "/fo4hud/entities.log";
    const char* home = std::getenv("HOME");
    if (home && home[0] == '/') return std::string(home) + "/.local/state/fo4hud/entities.log";
    return "/tmp/fo4hud-entities-" + std::to_string(getuid()) + ".log";
}

bool fo4hud_append_entity_log(const Fo4EntitySnapshot& snapshot, std::string& error) {
    const std::string path = fo4hud_entity_log_path();
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), code);
    if (code) { error = "Cannot create log directory: " + code.message(); return false; }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) { error = "Cannot open log: " + std::string(std::strerror(errno)); return false; }
    FILE* file = fdopen(fd, "a");
    if (!file) {
        error = "Cannot stream log: " + std::string(std::strerror(errno));
        close(fd);
        return false;
    }
    std::time_t now = std::time(nullptr);
    char time_text[32]{};
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(time_text, sizeof(time_text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(file, "snapshot time=%s pid=%ld status=%s\n", time_text, long(getpid()), snapshot.status.c_str());
    std::fprintf(file, "image=0x%lx player=0x%lx cell=0x%lx cell_form_id=%08X interior=%d worldspace=0x%lx entries=0x%lx capacity=%u count=%u readable=%zu\n",
                 (unsigned long)snapshot.image_address, (unsigned long)snapshot.player_address,
                 (unsigned long)snapshot.cell_address, snapshot.cell_form_id,
                 snapshot.cell_is_interior, (unsigned long)snapshot.worldspace_address,
                 (unsigned long)snapshot.entries_address, snapshot.capacity,
                 snapshot.total_references, snapshot.entities.size());
    if (snapshot.player_heading_valid)
        std::fprintf(file, "player_heading_radians=%.6f player_heading_degrees=%.3f\n",
                     snapshot.player_heading_radians,
                     snapshot.player_heading_radians * (180.0 / 3.14159265358979323846));
    std::fprintf(file, "actors status=%s process_lists=0x%lx handle_table=0x%lx levels=%u,%u,%u,%u readable=%zu\n",
                 snapshot.actor_status.c_str(), (unsigned long)snapshot.process_lists_address,
                 (unsigned long)snapshot.handle_table_address,
                 snapshot.actor_handle_counts[0], snapshot.actor_handle_counts[1],
                 snapshot.actor_handle_counts[2], snapshot.actor_handle_counts[3], snapshot.actors.size());
    for (const Fo4Entity& actor : snapshot.actors)
        std::fprintf(file, "actor address=0x%lx handle=%08X level=%u form_id=%08X name=%s position=%.3f,%.3f,%.3f cell=0x%lx interior=%d worldspace=0x%lx visible_space=%d\n",
                     (unsigned long)actor.address, actor.handle, actor.process_level,
                     actor.form_id, actor.name.empty() ? "<unnamed>" : actor.name.c_str(),
                     actor.position[0], actor.position[1], actor.position[2],
                     (unsigned long)actor.cell_address, actor.cell_is_interior,
                     (unsigned long)actor.worldspace_address,
                     fo4hud_actor_in_player_space(snapshot, actor));
    for (const Fo4Entity& entity : snapshot.entities)
        std::fprintf(file, "entity address=0x%lx form_id=%08X type=%u position=%.3f,%.3f,%.3f\n",
                     (unsigned long)entity.address, entity.form_id, entity.form_type,
                     entity.position[0], entity.position[1], entity.position[2]);
    std::fputs("end snapshot\n", file);
    bool okay = std::fflush(file) == 0;
    if (okay) okay = fsync(fd) == 0;
    if (std::fclose(file) != 0) okay = false;
    if (!okay) error = "Could not finish writing entity log";
    return okay;
}

// Verified in 1.11.240: TESWorldSpace::GetMapData (0x574440),
// GetMapTexture (0x5744C0), and Pip-Boy corner setup (0xB589D0).
// MNAM's signed cell coordinates map directly to world units (*4096),
// without adding a cell or using the unrelated worldspace bounding box.
bool fo4hud_read_world_map(const Fo4EntitySnapshot& snapshot, Fo4MapInfo& map) {
    map = {};
    if (snapshot.cell_is_interior || !snapshot.worldspace_address ||
        !fo4hud_player_in_snapshot_cell(snapshot)) return false;
    uintptr_t world = snapshot.worldspace_address;
    for (int depth = 0; depth < 16; ++depth) {
        uintptr_t parent = 0;
        uint8_t flags = 0;
        if (!read_value(world + 0x188, parent) || !read_value(world + 0x8A, flags)) return false;
        if (!parent || !(flags & 4)) {
            int16_t cells[4]{};
            if (!read_memory(world + 0x1B4, cells, sizeof(cells))) return false;
            // GetMapTexture calls BSFixedString::c_str (0x167C490). The field
            // points to a string-pool entry, whose text starts at +0x18 and
            // may be reached through alias entries with flag 0x4000.
            std::string texture = read_pooled_string(world + 0x38);
            // The Pip-Boy falls back to the Commonwealth image for empty ICONs
            // (0xADA300), provided that the world still supplies map bounds.
            if (texture.empty()) texture = "Interface/Pip-Boy/WorldMap_d.dds";
            // Worldspace ICON paths omit the Textures directory.
            std::string lower = texture;
            for (char& c : lower) {
                if (c == '\\') c = '/';
                else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            }
            if (lower.rfind("textures/", 0) != 0) lower = "textures/" + lower;
            map = {cells[0] * 4096.0f, cells[1] * 4096.0f,
                   cells[2] * 4096.0f, cells[3] * 4096.0f, lower};
            // Pipboy_MapPage.swf: WorldMapHolder.onPipboyChangeEvent and
            // MapHolder.normalizedToMapSpace apply these image gutters after
            // normalizing world coordinates against the MNAM corners.
            map.u_west = map.v_north = 0.02685546875f;
            map.u_east = map.v_south = 1.0f - 0.0283203125f;
            if (lower == "textures/dlc03/interface/map/mapfarharbor.dds") {
                map.u_west = 0.00920067f;
                map.v_north = 0.02515624f;
                map.u_east = 1.0f - 0.03374173f;
                map.v_south = 1.0f - 0.00958931f;
            } else if (lower == "textures/dlc04/interface/map/nukaworldmap.dds") {
                map.u_west = -0.045f;
                map.v_north = 0.0005f;
                map.u_east = 1.0f + 0.095f;
                map.v_south = 1.0f + 0.155f;
            }
            return map.valid();
        }
        if (parent == world) return false;
        world = parent;
    }
    return false;
}

namespace {
using TrapClock = std::chrono::steady_clock;
std::mutex trap_mutex;
Fo4EntitySnapshot trap_context;
std::vector<Fo4TrapMarker> trap_markers;
uint64_t trap_generation = 0;
TrapClock::time_point trap_marker_time{};

std::string trap_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}

Fo4TrapKind trap_script_kind(const std::string& name) {
    if (name == "traptripwire" || name == "dn070_traptripwire" || name == "traptrigtension") return Fo4TrapKind::Wire;
    if (name == "traptrigplate") return Fo4TrapKind::PressurePlate;
    if (name == "trapgun") return Fo4TrapKind::Gun;
    if (name == "trapflamethrower") return Fo4TrapKind::Flame;
    if (name == "trapelectricarc" || name == "cyclicelectricalhazard") return Fo4TrapKind::Electric;
    if (name == "trapradiationscript") return Fo4TrapKind::Radiation;
    if (name == "trapgrenadebouquet" || name == "trapied" || name == "trapexplgas" || name == "trapexploil") return Fo4TrapKind::Explosive;
    if (name == "trapcanchimes") return Fo4TrapKind::Alarm;
    if (name == "trapmonkeytriggerscript" || name == "dn160_trapmonkeytriggerscript" ||
        name == "trapmonkeyscript" || name == "dn160_encountermonkeyscript") return Fo4TrapKind::Monkey;
    if (name == "trapbase" || name == "traptrigbase" || name == "trapbreakablewalkway" || name == "trapbarnacle" || name == "trapthistlescript") return Fo4TrapKind::Other;
    return Fo4TrapKind::None;
}

// BSScript::Object: type +8, current state +0x10, 16-byte Variables +0x30
// (20E8AD0). ObjectTypeInfo has parent +0x18, name +0x10, linked metadata
// flags +0x40 and data +0x50. 20EF5C0 maps variable names to inherited slots.
// Called while VirtualMachine::ForEachBoundObject holds the binding lock.
Fo4TrapMarker inspect_trap_script(uintptr_t object) {
    Fo4TrapMarker result;
    uintptr_t type = 0;
    uint32_t object_flags = 0;
    if (!read_value(object, object_flags) || !(object_flags & 1) || !read_value(object + 8, type)) return result;
    struct Type { uintptr_t address; uint32_t flags, count; };
    std::vector<Type> types;
    uint32_t total = 0;
    bool breakable_walkway = false;
    for (unsigned depth = 0; type && depth < 16; ++depth) {
        uint32_t flags = 0;
        if (!read_value(type + 0x40, flags) || (flags & 3) != 3) return {};
        const uint32_t count = (flags >> 8) & 0x3FF;
        if (total + count > 4096) return {};
        total += count;
        types.push_back({type, flags, count});
        const auto script_name = trap_lower(read_pooled_string(type + 0x10));
        breakable_walkway |= script_name == "trapbreakablewalkway";
        const auto kind = trap_script_kind(script_name);
        if (result.kind == Fo4TrapKind::None || (result.kind == Fo4TrapKind::Other && kind != Fo4TrapKind::None)) result.kind = kind;
        uintptr_t parent = 0;
        if (!read_value(type + 0x18, parent) || parent == type) return {};
        type = parent;
    }
    if (type || result.kind == Fo4TrapKind::None) return {};
    std::optional<bool> armed;
    std::string arm_state;
    uint32_t index = 0;
    for (auto it = types.rbegin(); it != types.rend(); ++it) {
        uintptr_t data = 0;
        if (!read_value(it->address + 0x50, data) || (it->count && !data)) return result;
        const uintptr_t variables = data + 8 * ((it->flags >> 3) & 0x1F);
        for (uint32_t i = 0; i < it->count; ++i, ++index) {
            const std::string name = trap_lower(read_pooled_string(variables + size_t(i) * 24));
            if (name != "isarmed" && name != "armstate") continue;
            const uintptr_t slot = object + 0x30 + size_t(index) * 16;
            uint64_t value_type = 0, value = 0;
            if (!read_value(slot, value_type) || !read_value(slot + 8, value)) continue;
            if (name == "isarmed" && value_type == 5) armed = value != 0;
            if (name == "armstate" && value_type == 2) arm_state = trap_lower(read_pooled_string(slot + 8));
        }
    }
    const std::string state = trap_lower(read_pooled_string(object + 0x10));
    auto disarmed = [](const std::string& s) {
        return s == "disarm" || s == "disarmed" || s == "disabled" || s == "trapfired" || s == "spent" || s == "donothing";
    };
    // TrapBreakableWalkway enters Done when BoardCount <= 0, both on
    // attachment and after a board is destroyed. Reset rebuilds the boards
    // and returns to Active. Do not remember fired references permanently,
    // or apply this script-specific terminal state to unrelated traps.
    if ((breakable_walkway && state == "done") ||
        disarmed(state) || disarmed(arm_state) || (armed && !*armed)) result.activity = Fo4TrapActivity::Inactive;
    else if ((armed && *armed) || state == "idle" || state == "active" || state == "warn" ||
             (state == "waiting" && result.kind == Fo4TrapKind::Explosive) ||
             arm_state == "arm" || arm_state == "armed" || result.kind == Fo4TrapKind::PressurePlate)
        result.activity = Fo4TrapActivity::Armed;
    // Unverified script states stay visibly uncertain instead of being called armed.
    return result;
}

// Reviewed installed Fallout4 - Misc.ba2 lockExtraLootSCRIPT.pex:
// SHA-256 dff72a83f0a40d9659b9b78354d345a304c67a6597d3b06543ff45c877bfc4ad.
// OnLoad adds lock-level bonus loot, then enters an empty donothing state.
// There are no OnActivate/OnOpen/OnItemRemoved handlers. Wait for completion;
// never run OnLoad ourselves or allow a derived class through this exception.
// Reviewed installed containerOpenIfEmpty.pex:
// SHA-256 77d7efbb70dd1fa7bbe94db74707e24f5f1a3e681ec4972cfef371d512e446d3.
// OnLoad/OnClose only play Open/Close animations based on whether it is empty.
// It has no custom states, inventory mutations or quest effects.
bool loot_script_allows_auto_loot(uintptr_t object) {
    uint32_t flags = 0;
    uintptr_t type = 0;
    if (!object || !read_value(object, flags) || !(flags & 1) ||
        !read_value(object + 8, type) || !type) return false;
    struct ReviewedScript { const char* name; const char* state; };
    static constexpr ReviewedScript reviewed[] = {
        {"lockextralootscript", "donothing"},
        {"containeropenifempty", ""},
    };
    const auto name = trap_lower(read_pooled_string(type + 0x10));
    const auto state = read_checked_pooled_string(object + 0x10);
    if (!state) return false;
    bool allowed = false;
    for (const auto& script : reviewed)
        if (name == script.name && trap_lower(*state) == script.state) allowed = true;
    if (!allowed) return false;
    const char* hierarchy[] = {name.c_str(), "objectreference", "form", "scriptobject"};
    for (const char* expected : hierarchy) {
        uintptr_t parent = 0;
        if (!type || trap_lower(read_pooled_string(type + 0x10)) != expected ||
            !read_value(type + 0x18, parent) || parent == type) return false;
        type = parent;
    }
    return type == 0;
}

bool loot_alias_script_allows_auto_loot(uintptr_t object) {
    uint32_t flags = 0;
    uintptr_t type = 0;
    if (!object || !read_value(object, flags) || !(flags & 1) ||
        !read_value(object + 8, type) || !type) return false;
    const auto name = trap_lower(read_pooled_string(type + 0x10));
    const auto state = read_checked_pooled_string(object + 0x10);
    if (!state || !state->empty()) return false;
    if (name != "recollectionaliasscript" && name != "defaultcollectionaliasonload") return false;
    // DefaultCollectionAliasOnLoad inherits the reviewed stage-setting helper.
    const char* hierarchy[] = {name.c_str(), "defaultcollectionalias", "refcollectionalias", "alias", "scriptobject"};
    for (size_t i = 0; i < 5; ++i) {
        if (i == 1 && name == "recollectionaliasscript") continue;
        uintptr_t parent = 0;
        if (!type || trap_lower(read_pooled_string(type + 0x10)) != hierarchy[i] ||
            !read_value(type + 0x18, parent) || parent == type) return false;
        type = parent;
    }
    return type == 0;
}

struct LootScriptVisitor {
    const uintptr_t* vtable;
    bool clear = true;
    bool collection_alias = false;
    unsigned count = 0;
};
int32_t __attribute__((ms_abi)) visit_loot_script(LootScriptVisitor* visitor, uintptr_t* object) {
    ++visitor->count;
    if (!object || !(visitor->collection_alias ? loot_alias_script_allows_auto_loot(*object)
                                             : loot_script_allows_auto_loot(*object))) visitor->clear = false;
    return 1;
}

struct TrapVisitor { const uintptr_t* vtable; Fo4TrapMarker* result; };
int32_t __attribute__((ms_abi)) visit_trap_object(TrapVisitor* visitor, uintptr_t* object) {
    if (object && *object) {
        auto found = inspect_trap_script(*object);
        if (found.kind != Fo4TrapKind::None &&
            (visitor->result->kind == Fo4TrapKind::None || found.activity == Fo4TrapActivity::Armed)) *visitor->result = found;
    }
    return 1; // BSContainer::ForEachResult::Continue
}

Fo4TrapMarker sample_trap(uintptr_t image, const Fo4Entity& entity) {
    Fo4TrapMarker result;
    float position[3]{};
    if (!fo4hud_trap_reference_live(entity, position, true)) return result;
    uintptr_t base = 0;
    uint8_t base_type = 0;
    if (!read_value(entity.address + 0xE0, base) || !base || !read_value(base + 0x1A, base_type)) return result;
    uint32_t base_id = 0, reference_flags = 0;
    if (!read_value(base + 0x14, base_id) || !read_value(entity.address + 0x10, reference_flags)) return result;
    // Vanilla cymbal monkeys have no attached script. Their one destruction
    // stage replaces the intact mesh with TrapCymbalMonkeyHulk.nif (head gone).
    // ObjectReference.IsDestroyed (1.11.240 RVA 1181540) reads flag 0x800000.
    if (entity.form_type == 64 && base_type == 27 && (base_id == 0x142F88 || base_id == 0x14BED3)) {
        result.kind = Fo4TrapKind::Monkey;
        result.activity = (reference_flags & 0x800000) ? Fo4TrapActivity::Sprung : Fo4TrapActivity::Armed;
    } else if (entity.form_type == 64 && base_type == 27 && (base_id == 0x1796A6 || base_id == 0x21C928)) {
        // Invisible trigger volumes are separate from the visible monkeys.
        // Mark the physical monkey, whose destruction state is authoritative.
        return result;
    } else if (reference_flags & 0x800000) {
        return result;
    } else if (entity.form_type == 68 && base_type == 51) {
        // GrenadeProjectile::IsMine (E324B0), not an inventory WEAP record.
        uint32_t flags = 0;
        float proximity = 0;
        uintptr_t vtable = 0;
        if (read_value(entity.address, vtable) && vtable == image + 0x257DB78 &&
            read_value(base + 0xC0, flags) && (flags & 4) &&
            read_value(base + 0xE0, proximity) && std::isfinite(proximity) && proximity > 0) {
            result.kind = Fo4TrapKind::Mine;
            result.activity = Fo4TrapActivity::Armed;
        }
    } else if (entity.form_type == 64 && trap_script_candidate_base(base_type)) {
        // Same handle-policy + bound-object route used by GetVMScriptVariable
        // (59C830). No script functions are executed and no game data is edited.
        uintptr_t game_vm = 0, vm = 0, table = 0, policy = 0, handle_fn = 0;
        if (!read_value(image + 0x30EB308, game_vm) || !game_vm ||
            !read_value(game_vm + 0xB0, vm) || !vm || !read_value(vm, table) || table != image + 0x28FB520 ||
            !read_value(table + 0xF0, handle_fn) || handle_fn != image + 0x211A4A0 ||
            !read_value(vm + 0x90, policy) || !policy || !read_value(policy, table) ||
            !read_value(table + 0x38, handle_fn) || handle_fn < image + 0x1000 || handle_fn >= image + 0x2440000) return result;
        using Handle = uint64_t (__attribute__((ms_abi)) *)(void*, uint32_t, void*);
        using ForEach = void (__attribute__((ms_abi)) *)(void*, uint64_t, void*);
        const uint64_t handle = reinterpret_cast<Handle>(handle_fn)(reinterpret_cast<void*>(policy), 64, reinterpret_cast<void*>(entity.address));
        static const uintptr_t visitor_vtable[] = {0, 0, reinterpret_cast<uintptr_t>(visit_trap_object), 0, 0};
        TrapVisitor visitor{visitor_vtable, &result};
        struct Function { uintptr_t storage[7]{}; TrapVisitor* target; } function{{}, &visitor};
        // 211A4A0 invokes the target at std::function+0x38, vtable slot 2.
        reinterpret_cast<ForEach>(image + 0x211A4A0)(reinterpret_cast<void*>(vm), handle, &function);
    }
    result.entity = entity;
    std::copy(position, position + 3, result.entity.position);
    return result;
}
}

bool fo4hud_trap_reference_live(const Fo4Entity& entity, float position[3], bool allow_destroyed) {
    uint32_t id = 0, flags = 0;
    uint8_t type = 0;
    uintptr_t cell = 0;
    if (!entity.address || !read_value(entity.address + 0x14, id) || id != entity.form_id ||
        !read_value(entity.address + 0x1A, type) || type != entity.form_type || (type != 64 && type != 68) ||
        !read_value(entity.address + 0x10, flags) || (flags & (0x20 | 0x800)) || (!allow_destroyed && (flags & 0x800000)) ||
        !read_value(entity.address + 0xB8, cell) || !cell || cell != entity.cell_address ||
        !read_memory(entity.address + 0xD0, position, 3 * sizeof(float))) return false;
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(position[i])) return false;
    uint64_t projectile_flags = 0;
    // Live, placed mines can have both bit 2 and 0x8000 set. Neither is
    // sufficient to suppress a hazard. Disarming (E471F0) sets 0x200;
    // retirement (E43740) sets 0x2000000. Keep warning until either occurs.
    return type != 68 || (read_value(entity.address + 0x260, projectile_flags) &&
                         !(projectile_flags & (0x200 | 0x2000000)));
}

void fo4hud_submit_trap_candidates(const Fo4EntitySnapshot& snapshot, bool enabled) {
    std::lock_guard<std::mutex> lock(trap_mutex);
    if (!enabled || snapshot.status != "Ready") {
        if (trap_context.player_address) { trap_context = {}; trap_markers.clear(); ++trap_generation; }
        return;
    }
    if (trap_context.player_address != snapshot.player_address || trap_context.cell_address != snapshot.cell_address ||
        trap_context.worldspace_address != snapshot.worldspace_address) { trap_markers.clear(); ++trap_generation; }
    trap_context.player_address = snapshot.player_address;
    trap_context.cell_address = snapshot.cell_address;
    trap_context.worldspace_address = snapshot.worldspace_address;
    trap_context.cell_is_interior = snapshot.cell_is_interior;
    std::copy(snapshot.player_position, snapshot.player_position + 3, trap_context.player_position);
    if (trap_context.trap_candidates.size() == snapshot.trap_candidates.size() &&
        std::equal(trap_context.trap_candidates.begin(), trap_context.trap_candidates.end(), snapshot.trap_candidates.begin(),
            [](const auto& a, const auto& b) { return a.address == b.address && a.form_id == b.form_id; })) return;
    trap_context.trap_candidates = snapshot.trap_candidates;

}

void fo4hud_traps_tick(uintptr_t image, void* player) {
    struct Cached { uint32_t id = 0; Fo4TrapMarker marker; TrapClock::time_point checked{}, due{}; };
    static std::unordered_map<uintptr_t, Cached> cache;
    static uint64_t generation = 0;
    static size_t cursor = 0;
    Fo4EntitySnapshot context;
    uint64_t current = 0;
    { std::lock_guard<std::mutex> lock(trap_mutex);
      context.player_address = trap_context.player_address; context.cell_address = trap_context.cell_address;
      context.trap_candidates = trap_context.trap_candidates; current = trap_generation; }
    if (generation != current) { cache.clear(); cursor = 0; generation = current; }
    uintptr_t cell = 0;
    if (!image || reinterpret_cast<uintptr_t>(player) != context.player_address || !context.player_address ||
        !read_value(context.player_address + 0xB8, cell) || cell != context.cell_address) return;
    const auto now = TrapClock::now();
    const auto deadline = now + std::chrono::microseconds(1000);
    unsigned done = 0;
    auto inspect = [&](const Fo4Entity& e) {
        auto& entry = cache[e.address];
        if (entry.id == e.form_id && entry.due > now) return;
        entry.id = e.form_id; entry.marker = sample_trap(image, e); entry.checked = now;
        entry.due = now + (entry.marker.kind == Fo4TrapKind::None ? std::chrono::milliseconds(5000) : std::chrono::milliseconds(150));
        ++done;
    };
    // Keep recognized traps fresh even in cells with many unrelated activators.
    for (const auto& e : context.trap_candidates) {
        auto it = cache.find(e.address);
        if (it != cache.end() && it->second.marker.kind != Fo4TrapKind::None) inspect(e);
        if (done >= 8 || TrapClock::now() >= deadline) break;
    }
    for (size_t n = 0; n < context.trap_candidates.size() && done < 12 && TrapClock::now() < deadline; ++n) {
        cursor %= context.trap_candidates.size();
        inspect(context.trap_candidates[cursor++]);
    }
    std::vector<Fo4TrapMarker> markers;
    std::unordered_set<uintptr_t> retained;
    for (const auto& e : context.trap_candidates) {
        retained.insert(e.address);
        auto it = cache.find(e.address);
        if (it != cache.end() && it->second.id == e.form_id && it->second.marker.kind != Fo4TrapKind::None &&
            it->second.marker.activity != Fo4TrapActivity::Inactive && now - it->second.checked < std::chrono::seconds(2))
            markers.push_back(it->second.marker);
    }
    for (auto it = cache.begin(); it != cache.end();) {
        if (!retained.count(it->first)) it = cache.erase(it); else ++it;
    }
    std::lock_guard<std::mutex> lock(trap_mutex);
    if (trap_generation == current) { trap_markers = std::move(markers); trap_marker_time = now; }
}

std::vector<Fo4TrapMarker> fo4hud_read_trap_markers() {
    std::lock_guard<std::mutex> lock(trap_mutex);
    return TrapClock::now() - trap_marker_time < std::chrono::seconds(2) ? trap_markers : std::vector<Fo4TrapMarker>{};
}

// Same checked VM path as sample_trap. Every binding must pass the narrow
// reviewed-script allowlist; one unknown or unsafe binding rejects loot.
static bool loot_bound_scripts_allow_auto_loot(uintptr_t image, uintptr_t reference, bool collection_alias) {
    uintptr_t game_vm = 0, vm = 0, table = 0, policy = 0, handle_fn = 0, invalid_fn = 0;
    uint8_t type = 0;
    // Verified 1.11.240 BGSRefCollectionAlias vtable and QType implementation.
    // Do not pass a different alias class to the collection-alias handle policy.
    uintptr_t alias_table = 0, qtype = 0;
    if (!image || !reference) return false;
    if (collection_alias) {
        if (!read_value(reference, alias_table) || alias_table != image + 0x2487E30 ||
            !read_value(alias_table + 0x18, qtype) || qtype != image + 0x404370) return false;
    } else if (!read_value(reference + 0x1A, type) || type != 64) return false;
    if (
        !read_value(image + 0x30EB308, game_vm) || !game_vm ||
        !read_value(game_vm + 0xB0, vm) || !vm || !read_value(vm, table) || table != image + 0x28FB520 ||
        !read_value(table + 0xF0, handle_fn) || handle_fn != image + 0x211A4A0 ||
        !read_value(vm + 0x90, policy) || !policy || !read_value(policy, table) ||
        !read_value(table + 0x30, invalid_fn) || invalid_fn < image + 0x1000 || invalid_fn >= image + 0x2440000 ||
        !read_value(table + 0x38, handle_fn) || handle_fn < image + 0x1000 || handle_fn >= image + 0x2440000)
        return false;
    using Handle = uint64_t (__attribute__((ms_abi)) *)(void*, uint32_t, void*);
    using Invalid = uint64_t (__attribute__((ms_abi)) *)(void*);
    using ForEach = void (__attribute__((ms_abi)) *)(void*, uint64_t, void*);
    const auto handle = reinterpret_cast<Handle>(handle_fn)(reinterpret_cast<void*>(policy), collection_alias ? 165 : 64,
                                                         reinterpret_cast<void*>(reference));
    if (!handle || handle == UINT64_MAX ||
        handle == reinterpret_cast<Invalid>(invalid_fn)(reinterpret_cast<void*>(policy))) return false;
    static const uintptr_t visitor_vtable[] = {0, 0, reinterpret_cast<uintptr_t>(visit_loot_script), 0, 0};
    LootScriptVisitor visitor{visitor_vtable};
    visitor.collection_alias = collection_alias;
    struct Function { uintptr_t storage[7]{}; LootScriptVisitor* target; } function{{}, &visitor};
    reinterpret_cast<ForEach>(image + 0x211A4A0)(reinterpret_cast<void*>(vm), handle, &function);
    return visitor.clear && (!collection_alias || visitor.count != 0);
}

bool fo4hud_reference_scripts_allow_auto_loot(uintptr_t image, uintptr_t reference) {
    return loot_bound_scripts_allow_auto_loot(image, reference, false);
}

bool fo4hud_collection_alias_scripts_allow_auto_loot(uintptr_t image, uintptr_t alias) {
    return loot_bound_scripts_allow_auto_loot(image, alias, true);
}
