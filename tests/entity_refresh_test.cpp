#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <numeric>

static size_t syscall_count = 0, name_payload_reads = 0;
static uintptr_t payload_address = 0, failed_read_address = 0;
static ssize_t counted_refresh_read(pid_t pid, const iovec* local, unsigned long local_count,
                                    const iovec* remote, unsigned long remote_count, unsigned long flags) {
    ++syscall_count;
    if (remote_count && reinterpret_cast<uintptr_t>(remote[0].iov_base) == failed_read_address) return -1;
    for (unsigned long i = 0; i < remote_count; ++i)
        if (reinterpret_cast<uintptr_t>(remote[i].iov_base) == payload_address) ++name_payload_reads;
    return process_vm_readv(pid, local, local_count, remote, remote_count, flags);
}
#define process_vm_readv counted_refresh_read
#include "../game_entities.cpp"
#undef process_vm_readv

template<class T> void put(void* p, size_t off, T value) { std::memcpy(static_cast<char*>(p) + off, &value, sizeof(value)); }
template<class T> uintptr_t address(T& p) { return reinterpret_cast<uintptr_t>(p.data()); }

struct Fixture {
    int fd;
    unsigned char* image;
    std::array<unsigned char, 0xD0> cell{}, other_cell{};
    std::array<unsigned char, 0xA0> lists{};
    std::array<unsigned char, 0x2A0> item{}, npc{};
    std::array<unsigned char, 0x30> text{}, new_text{};
    std::vector<std::array<unsigned char, 0x140>> refs, actors;
    std::vector<uintptr_t> ref_ptrs;
    std::vector<uint32_t> handles;
    std::vector<std::array<unsigned char, 16>> slots;
    Fixture(size_t nr = 5886, size_t na = 1647) : refs(nr), actors(na), slots(na + 1) {
        fd = memfd_create("Fallout4.exe", 0); assert(fd >= 0);
        assert(ftruncate(fd, kImageSize) == 0);
        image = static_cast<unsigned char*>(mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        assert(image != MAP_FAILED);
        put(image, 0, uint16_t(0x5A4D)); put(image, 0x3C, uint32_t(0x100));
        put(image, 0x100, uint32_t(0x4550)); put(image, 0x108, kTimestamp);
        put(image, 0x130, uint64_t(kImageBase)); put(image, 0x150, kImageSize);
        put(text.data(), 0x10, uint32_t(4)); std::memcpy(text.data() + 0x18, "Test", 4);
        put(new_text.data(), 0x10, uint32_t(3)); std::memcpy(new_text.data() + 0x18, "New", 3);
        payload_address = address(text) + 0x18;
        put(item.data(), 0x14, uint32_t(0x1234)); put(item.data(), 0x1A, uint8_t(0x23));
        put(item.data(), 0x70, address(text));
        put(npc.data(), 0x14, uint32_t(0x5678)); put(npc.data(), 0x1A, uint8_t(45));
        put(npc.data(), 0x128, address(text));
        for (auto* c : {&cell, &other_cell}) {
            put(c->data(), 0x1A, uint8_t(63)); put(c->data(), 0x40, uint16_t(1));
        }
        put(cell.data(), 0x14, uint32_t(0x10)); put(other_cell.data(), 0x14, uint32_t(0x20));
        for (size_t i = 0; i < nr; ++i) {
            auto& r = refs[i]; put(r.data(), 0x14, uint32_t(i + 1)); put(r.data(), 0x1A, uint8_t(64));
            put(r.data(), 0xB8, address(cell)); put(r.data(), 0xE0, address(item));
            put(r.data(), 0xD0, float(i < 32 ? 100 : 1000000)); ref_ptrs.push_back(address(r));
        }
        put(cell.data(), 0x70, address(ref_ptrs)); put(cell.data(), 0x78, uint32_t(nr)); put(cell.data(), 0x80, uint32_t(nr));
        for (size_t i = 0; i < na; ++i) {
            auto& a = actors[i]; const uint32_t h = i + 1; handles.push_back(h);
            put(a.data(), 0x14, uint32_t(0x10000 + i)); put(a.data(), 0x1A, uint8_t(65));
            put(a.data(), 40, h << 11); put(a.data(), 0xB8, address(cell)); put(a.data(), 0xE0, address(npc));
            put(a.data(), 0xD0, float(i < 16 ? 100 : 1000000));
            put(slots[h].data(), 0, uint32_t(0x4000000)); put(slots[h].data(), 8, address(a) + 32);
        }
        put(lists.data(), 0, reinterpret_cast<uintptr_t>(image) + kProcessListsVtableRva);
        put(lists.data(), 0x40, address(handles)); put(lists.data(), 0x48, uint32_t(na)); put(lists.data(), 0x50, uint32_t(na));
        put(image, kPlayerPointerRva, address(actors[0])); put(image, kProcessListsPointerRva, address(lists));
        put(image, kHandleTablePointerRva, address(slots));
    }
    ~Fixture() {
        verified_image.store(0); munmap(image, kImageSize); close(fd);
    }
};

int main() {
    Fixture f;
    using Clock = Fo4EntityRefresh::Clock;
    auto now = Clock::now();
    Fo4EntityScanOptions options;
    options.actor_range = 1000; options.actor_names = true;
    Fo4EntityRefresh refresh;
    auto run_until = [&](auto condition, int max_frames = 10000) {
        for (int frame = 0; frame < max_frames && !condition(); ++frame) {
            refresh.update(options, std::chrono::microseconds(50), now);
            now += std::chrono::milliseconds(1);
        }
        assert(condition());
    };
    run_until([&] { return refresh.stats().actor_passes == 1; });
    // Position and worldspace filtering precede all name/inventory reads.
    assert(refresh.snapshot().actors.size() == 16);
    assert(refresh.stats().world_passes == 0 && refresh.snapshot().entities.empty());
    assert(refresh.stats().inventory_steps == 0);
    assert(name_payload_reads == 1 && refresh.stats().cached_names == 1);
    for (auto& a : refresh.snapshot().actors) assert(a.name == "Test");

    // Changed pooled-string pointers invalidate names even before the TTL.
    put(f.npc.data(), 0x128, address(f.new_text));
    now += std::chrono::seconds(1);
    run_until([&] { return refresh.stats().actor_passes == 2; });
    for (auto& a : refresh.snapshot().actors) assert(a.name == "New");

    options = {}; options.loot_items = true; options.loot_range = 1000;
    refresh.update(options, std::chrono::microseconds(50), now);
    assert(refresh.snapshot().actors.empty());
    assert(refresh.stats().world_passes == 0); // A full scan cannot run in this tiny slice.
    run_until([&] { return refresh.stats().world_passes == 1; });
    assert(refresh.snapshot().entities.size() == 32);
    assert(refresh.stats().distant_skipped > 5000);
    const auto retained = refresh.snapshot().entities.size();
    for (auto& r : f.refs) put(r.data(), 0xD0, float(1000000));
    now += std::chrono::seconds(1);
    refresh.update(options, std::chrono::microseconds(50), now);
    assert(refresh.snapshot().entities.size() == retained); // Keep the completed list until replacement is ready.
    run_until([&] { return refresh.stats().world_passes == 2; });
    assert(refresh.snapshot().entities.empty());

    // Work spanning frames is discarded immediately on a cell transition.
    now += std::chrono::seconds(1);
    refresh.update(options, std::chrono::microseconds(50), now);
    put(f.actors[0].data(), 0xB8, address(f.other_cell));
    refresh.update(options, std::chrono::microseconds(0), now);
    assert(refresh.snapshot().cell_address == address(f.other_cell));
    assert(refresh.snapshot().entities.empty() && refresh.snapshot().actors.empty());
    assert(refresh.stats().cached_names == 0 && refresh.stats().cached_forms == 0);
    put(f.actors[0].data(), 0xB8, address(f.cell));

    // Reused base addresses must not reuse an old classification.
    put(f.refs[0].data(), 0xD0, float(100));
    put(f.item.data(), 0x1A, uint8_t(0x29)); // FLOR with no produce: excluded.
    now += std::chrono::seconds(1);
    const auto world_pass = refresh.stats().world_passes;
    run_until([&] { return refresh.stats().world_passes > world_pass; });
    assert(refresh.snapshot().entities.empty());
    put(f.item.data(), 0x1A, uint8_t(0x23));
    now += std::chrono::seconds(1);
    run_until([&] { return refresh.stats().world_passes > world_pass + 1; });
    assert(refresh.snapshot().entities.size() == 1);

    // Both scripted activators survive the incremental world scan and cached
    // form classification, without enabling container looting or harvesting.
    for (uint32_t id : {0x0003000Cu, 0x001715CDu, 0x0016579Bu}) {
        put(f.item.data(), 0x1A, uint8_t(0x1B));
        put(f.item.data(), 0x14, id);
        now += std::chrono::seconds(3);
        const auto before = refresh.stats().world_passes;
        run_until([&] { return refresh.stats().world_passes > before; });
        if (id == 0x0016579B) assert(refresh.snapshot().entities.empty());
        else {
            assert(refresh.snapshot().entities.size() == 1);
            const auto& item = refresh.snapshot().entities[0];
            assert(item.radar_category == 3);
            assert(item.name.empty()); // Loot-only scans skip presentation names.
        }
    }
    put(f.item.data(), 0x1A, uint8_t(0x23));
    put(f.item.data(), 0x14, uint32_t(0x1234));

    // Inventory traversal yields between stack nodes and rejects a header
    // replaced while the traversal is in progress.
    std::array<unsigned char, 0x70> inventory{};
    std::vector<std::array<unsigned char, 0x28>> stacks(5000);
    std::array<unsigned char, 0x2B0> inventory_item{};
    put(inventory_item.data(), 0x1A, uint8_t(0x23));
    uintptr_t entries[] = {address(inventory_item), address(stacks[0])};
    uintptr_t replacement_entries[] = {address(inventory_item), 0};
    for (size_t i = 0; i + 1 < stacks.size(); ++i) put(stacks[i].data(), 0x10, address(stacks[i + 1]));
    put(stacks.back().data(), 0x20, uint32_t(1));
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(entries));
    put(inventory.data(), 0x60, uint32_t(1)); put(inventory.data(), 0x68, uint32_t(1));
    put(f.refs[0].data(), 0xF8, address(inventory));
    put(f.item.data(), 0x14, uint32_t(0xAABB)); put(f.item.data(), 0x1A, uint8_t(0x1F));
    put(f.item.data(), 0x88, address(f.text));
    options = {}; options.loot_inventories = true; options.loot_range = 1000;
    now += std::chrono::seconds(1);
    const auto inventory_pass = refresh.stats().inventory_probes;
    run_until([&] { return refresh.stats().inventory_steps > 50; });
    assert(refresh.stats().inventory_probes == inventory_pass);
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(replacement_entries));
    run_until([&] { return refresh.stats().inventory_probes > inventory_pass; });
    assert(refresh.snapshot().entities[0].inventory_state == Fo4InventoryState::Unknown);
    // A subsequent unchanged empty inventory eventually updates the marker.
    now += std::chrono::seconds(2);
    run_until([&] { return refresh.snapshot().entities[0].inventory_state == Fo4InventoryState::Empty; });

    // Corpse colors retain the last confirmed result across cache expiry,
    // multi-frame probes, and publication of a replacement actor list.
    put(f.lists.data(), 0x50, uint32_t(2));
    put(f.actors[1].data(), 0x130, uint32_t(1 << 17));
    put(f.actors[1].data(), 0xF8, address(inventory));
    Fo4EntityRefresh corpses;
    Fo4EntityScanOptions corpse_options;
    corpse_options.actor_range = 1000;
    corpse_options.radar_range = 1000;
    corpse_options.radar_categories = 1 << 4;
    auto corpse = [&]() -> const Fo4Entity* {
        for (const auto& e : corpses.snapshot().actors)
            if (e.address == address(f.actors[1])) return &e;
        return nullptr;
    };
    bool keep_empty_color = false;
    auto corpse_until = [&](auto condition) {
        for (int frame = 0; frame < 10000 && !condition(); ++frame) {
            corpses.update(corpse_options, std::chrono::microseconds(50), now);
            now += std::chrono::milliseconds(1);
            if (keep_empty_color && corpse())
                assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
        }
        assert(condition());
    };
    corpse_until([&] { return corpse() && corpse()->inventory_state == Fo4InventoryState::Empty; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    keep_empty_color = true;
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(entries)); // Slow new probe.
    now += std::chrono::seconds(2);
    const auto actor_pass = corpses.stats().actor_passes;
    corpse_until([&] { return corpses.stats().actor_passes > actor_pass; });
    assert(corpse()->inventory_state == Fo4InventoryState::Unknown);
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    corpse_until([&] { return corpses.stats().inventory_steps > 20; });
    const auto corpse_probe = corpses.stats().inventory_probes;
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(replacement_entries));
    corpse_until([&] { return corpses.stats().inventory_probes > corpse_probe; });
    assert(corpse()->inventory_state == Fo4InventoryState::Unknown);
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    // Confirmed newly added loot still changes the color to bright.
    keep_empty_color = false;
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(entries));
    put(stacks[0].data(), 0x20, uint32_t(1));
    now += std::chrono::seconds(2);
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::HasItems; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::HasItems);
    // Manual looting must update both published corpse copies promptly,
    // without advancing beyond the old one-second cache lifetime.
    const auto looted_at = now;
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(replacement_entries));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::Empty; });
    assert(now - looted_at < std::chrono::milliseconds(250));
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    // World labels alone also request corpse inventories, independently of
    // radar visibility and auto-looting.
    corpse_options.radar_categories = 0;
    corpse_options.corpse_inventories = true;
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(entries));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::HasItems; });
    const auto labels_looted_at = now;
    put(stacks[0].data(), 0x20, uint32_t(0));
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(replacement_entries));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::Empty; });
    assert(now - labels_looted_at < std::chrono::milliseconds(250));
    // Built-in robot equipment must not keep an otherwise looted corpse red.
    put(inventory.data(), 0x58, reinterpret_cast<uintptr_t>(entries));
    put(stacks[0].data(), 0x20, uint32_t(1));
    put(inventory_item.data(), 0x1A, uint8_t(0x2B));
    put(inventory_item.data(), 0x2A8, uint32_t(0x1A0100));
    const auto robot_check = corpses.stats().inventory_probes;
    corpse_until([&] { return corpses.stats().inventory_probes > robot_check; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    // Adding an ordinary playable weapon still makes the corpse bright.
    put(inventory_item.data(), 0x2A8, uint32_t(0xA0100));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::HasItems; });
    put(inventory_item.data(), 0x1A, uint8_t(0x1D));
    put(inventory_item.data(), 0x10, uint32_t(0xD));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::Empty; });
    // A playable but unnamed ghoul skin must also stay dim after refresh.
    put(inventory_item.data(), 0x10, uint32_t(0x9));
    put(inventory_item.data(), 0x70, uintptr_t(0));
    const auto skin_check = corpses.stats().inventory_probes;
    corpse_until([&] { return corpses.stats().inventory_probes > skin_check; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    // Match the live skin's non-null shared empty string, after named loot.
    put(inventory_item.data(), 0x70, address(f.new_text));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::HasItems; });
    std::array<unsigned char, 0x20> empty_name{};
    put(empty_name.data(), 8, uint16_t(0x3FFF));
    put(inventory_item.data(), 0x70, address(empty_name));
    corpse_until([&] { return corpse()->inventory_state == Fo4InventoryState::Empty; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Empty);
    // Reusing the address for a different reference cannot inherit its hint.
    put(f.actors[1].data(), 0x14, uint32_t(0xABCDEF));
    put(f.actors[1].data(), 0xF8, uintptr_t(1));
    now += std::chrono::seconds(2);
    corpse_until([&] { return corpse()->form_id == 0xABCDEF; });
    assert(corpse()->display_inventory_state() == Fo4InventoryState::Unknown);
    put(f.actors[1].data(), 0x14, uint32_t(0x10001));
    put(f.actors[1].data(), 0xF8, uintptr_t(0));
    put(f.actors[1].data(), 0x130, uint32_t(0));
    put(f.lists.data(), 0x50, uint32_t(f.actors.size()));

    // Disabling features cancels all pending work and clears published lists.
    options = {};
    const auto old_world_pass = refresh.stats().world_passes;
    refresh.update(options, std::chrono::microseconds(1500), now);
    assert(refresh.snapshot().actors.empty() && refresh.snapshot().entities.empty());
    assert(refresh.stats().world_passes == old_world_pass);

    // Exterior cells are accepted only in the current worldspace. A damaged
    // grid must leave current-cell scanning usable.
    std::array<unsigned char, 0x20> tes{}, grid{};
    uintptr_t cells[] = {address(f.other_cell)};
    uintptr_t current_refs[] = {address(f.refs[0])}, nearby_refs[] = {address(f.refs[1])};
    put(f.cell.data(), 0x40, uint16_t(0)); put(f.other_cell.data(), 0x40, uint16_t(0));
    put(f.cell.data(), 0xC8, uintptr_t(0x123400)); put(f.other_cell.data(), 0xC8, uintptr_t(0x123400));
    put(f.cell.data(), 0x70, reinterpret_cast<uintptr_t>(current_refs)); put(f.cell.data(), 0x80, uint32_t(1));
    put(f.other_cell.data(), 0x70, reinterpret_cast<uintptr_t>(nearby_refs));
    put(f.other_cell.data(), 0x78, uint32_t(1)); put(f.other_cell.data(), 0x80, uint32_t(1));
    put(f.refs[1].data(), 0xB8, address(f.other_cell)); put(f.refs[1].data(), 0xD0, float(100));
    put(tes.data(), 0, reinterpret_cast<uintptr_t>(f.image) + kTesVtableRva); put(tes.data(), 0x18, address(grid));
    put(grid.data(), 0, reinterpret_cast<uintptr_t>(f.image) + kGridCellVtableRva);
    put(grid.data(), 0x10, uint32_t(1)); put(grid.data(), 0x18, reinterpret_cast<uintptr_t>(cells));
    put(f.image, kTesPointerRva, address(tes));
    put(f.item.data(), 0x1A, uint8_t(0x23));
    options.loot_items = true; options.loot_range = 1000;
    auto exterior_pass = refresh.stats().world_passes;
    run_until([&] { return refresh.stats().world_passes > exterior_pass; });
    assert(refresh.snapshot().entities.size() == 2);
    // Exterior boundaries retain published markers even with no scan budget.
    put(f.actors[0].data(), 0xB8, address(f.other_cell));
    refresh.update(options, std::chrono::microseconds(0), now);
    assert(refresh.snapshot().cell_address == address(f.other_cell));
    assert(refresh.snapshot().entities.size() == 2);
    put(f.actors[0].data(), 0xB8, address(f.cell));
    refresh.update(options, std::chrono::microseconds(0), now);
    assert(refresh.snapshot().entities.size() == 2);

    // A transient failed context read retains only a confirmed live identity.
    failed_read_address = address(f.actors[0]) + 0x14;
    refresh.update(options, std::chrono::microseconds(1500), now);
    assert(refresh.snapshot().status == "Ready" && refresh.snapshot().entities.size() == 2);
    failed_read_address = 0;

    auto no_partial_pass = [&] {
        const auto passes = refresh.stats().world_passes;
        for (int frame = 0; frame < 30; ++frame) {
            now += std::chrono::milliseconds(100);
            refresh.update(options, std::chrono::microseconds(1500), now);
            assert(refresh.snapshot().entities.size() == 2);
        }
        assert(refresh.stats().world_passes == passes);
    };
    // Missing grid, unreadable reference array and inconsistent cell headers
    // must never publish a partial replacement of the previous complete scan.
    put(grid.data(), 0x10, uint32_t(1000));
    no_partial_pass();
    put(grid.data(), 0x10, uint32_t(1));
    failed_read_address = reinterpret_cast<uintptr_t>(nearby_refs);
    no_partial_pass();
    failed_read_address = 0;
    put(f.other_cell.data(), 0x78, uint32_t(0));
    no_partial_pass();
    put(f.other_cell.data(), 0x78, uint32_t(1));
    exterior_pass = refresh.stats().world_passes;
    run_until([&] { return refresh.stats().world_passes > exterior_pass; });
    assert(refresh.snapshot().entities.size() == 2);

    // A genuine change in a neighboring cell's worldspace removes its markers.
    put(f.other_cell.data(), 0xC8, uintptr_t(0x999900));
    now += std::chrono::seconds(2);
    exterior_pass = refresh.stats().world_passes;
    run_until([&] { return refresh.stats().world_passes > exterior_pass; });
    assert(refresh.snapshot().entities.size() == 1);

    // Startup with a damaged grid can still bootstrap the current cell.
    put(grid.data(), 0x10, uint32_t(1000));
    Fo4EntityRefresh bootstrap;
    for (int frame = 0; frame < 100 && !bootstrap.stats().world_passes; ++frame) {
        bootstrap.update(options, std::chrono::microseconds(1500), now);
        now += std::chrono::milliseconds(16);
    }
    assert(bootstrap.stats().world_passes && bootstrap.snapshot().entities.size() == 1);

    put(f.cell.data(), 0x40, uint16_t(1));
    put(f.cell.data(), 0x70, address(f.ref_ptrs)); put(f.cell.data(), 0x80, uint32_t(f.refs.size()));
    put(f.refs[1].data(), 0xB8, address(f.cell));

    // Measure a complete incremental pass at the production budget, with the
    // same large object counts as the original blocking refresh fixture.
    Fo4EntityRefresh measured;
    options.actor_range = 1000; options.actor_names = true;
    options.loot_items = true; options.loot_range = 1000;
    options.radar_categories = 1 << 3; options.radar_range = 1000;
    put(f.item.data(), 0x1A, uint8_t(0x23));
    for (size_t i = 0; i < 32; ++i) put(f.refs[i].data(), 0xD0, float(100));
    const auto blocking_start = Clock::now();
    syscall_count = 0;
    auto blocking = fo4hud_read_current_cell();
    const auto blocking_ms = std::chrono::duration<double, std::milli>(Clock::now() - blocking_start).count();
    assert(blocking.entities.size() == f.refs.size() && blocking.actors.size() == f.actors.size());
    std::cout << "Blocking pass: ms=" << blocking_ms << " syscalls=" << syscall_count << '\n';
    std::vector<double> slices;
    syscall_count = 0;
    for (int frame = 0; frame < 1000 && (!measured.stats().world_passes || !measured.stats().actor_passes); ++frame) {
        measured.update(options, std::chrono::microseconds(1500), now);
        slices.push_back(measured.stats().last_slice_ms);
        now += std::chrono::milliseconds(16);
    }
    assert(measured.stats().world_passes && measured.stats().actor_passes);
    assert(measured.snapshot().entities.size() == 32 && measured.snapshot().actors.size() == 16);
    const double max_slice = *std::max_element(slices.begin(), slices.end());
    std::cout << "Incremental pass: frames=" << slices.size() << " total_ms="
              << std::accumulate(slices.begin(), slices.end(), 0.0) << " max_slice_ms=" << max_slice
              << " syscalls=" << syscall_count << " retained_refs=" << measured.snapshot().entities.size()
              << " retained_actors=" << measured.snapshot().actors.size() << '\n';

    // Dense areas must also yield when every object is relevant, and caches
    // stay bounded even with more distinct forms than the cache capacity.
    std::vector<std::array<unsigned char, 0x90>> bases(f.refs.size());
    for (size_t i = 0; i < f.refs.size(); ++i) {
        put(f.refs[i].data(), 0xD0, float(100));
        put(f.refs[i].data(), 0xE0, address(bases[i]));
        put(bases[i].data(), 0x14, uint32_t(0x70000 + i));
        put(bases[i].data(), 0x1A, uint8_t(0x23));
        put(bases[i].data(), 0x70, address(f.text));
    }
    for (auto& a : f.actors) put(a.data(), 0xD0, float(100));
    Fo4EntityRefresh dense;
    slices.clear();
    for (int frame = 0; frame < 10000 && (!dense.stats().world_passes || !dense.stats().actor_passes); ++frame) {
        dense.update(options, std::chrono::microseconds(1500), now);
        slices.push_back(dense.stats().last_slice_ms);
        now += std::chrono::milliseconds(16);
    }
    assert(dense.snapshot().entities.size() == f.refs.size());
    assert(dense.snapshot().actors.size() == f.actors.size());
    assert(dense.stats().cached_names <= 2048 && dense.stats().cached_forms <= 2048);
    std::cout << "Dense pass: frames=" << slices.size() << " max_slice_ms="
              << *std::max_element(slices.begin(), slices.end()) << " cached_names="
              << dense.stats().cached_names << " cached_forms=" << dense.stats().cached_forms << '\n';
    // Trap-only scans include activators and deployed grenade projectiles,
    // without depending on radar/loot. Loose mine WEAPs remain ordinary loot.
    for (size_t i = 0; i < 7; ++i) {
        put(f.refs[i].data(), 0x1A, uint8_t(64)); put(f.refs[i].data(), 0x10, uint32_t(0));
        put(bases[i].data(), 0x1A, uint8_t(27));
    }
    put(f.refs[3].data(), 0x1A, uint8_t(68)); put(bases[3].data(), 0x1A, uint8_t(51));
    put(bases[4].data(), 0x1A, uint8_t(43));
    put(f.refs[5].data(), 0x10, uint32_t(0x800));
    put(f.refs[6].data(), 0xD0, 1000000.0f);
    for (size_t i = 7; i < 10; ++i) {
        put(bases[i].data(), 0x1A, uint8_t(i == 7 ? 31 : i == 8 ? 32 : 36));
    }
    options = {}; options.trap_range = 1000;
    Fo4EntityRefresh traps;
    for (int frame = 0; frame < 1000 && !traps.stats().world_passes; ++frame) {
        traps.update(options, std::chrono::microseconds(1500), now);
        now += std::chrono::milliseconds(16);
    }
    assert(traps.stats().world_passes && traps.snapshot().trap_candidates.size() == 7);
    assert(traps.snapshot().entities.empty());
    options.trap_range = 0; traps.update(options, std::chrono::microseconds(0), now);
    assert(traps.snapshot().trap_candidates.empty());

}
