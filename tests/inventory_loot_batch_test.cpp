#include "../game_combat.cpp"
#include <array>
#include <cassert>

namespace {
template<class T, size_t N> void put(std::array<unsigned char, N>& p, size_t off, T value) {
    std::memcpy(p.data() + off, &value, sizeof(value));
}
template<size_t N> uintptr_t address(std::array<unsigned char, N>& p) {
    return reinterpret_cast<uintptr_t>(p.data());
}
std::array<unsigned char, 0x140> source{}, player{};
std::array<unsigned char, 0x70> inventory{};
std::array<std::array<unsigned char, 0x30>, 24> items{}, stacks{};
struct Entry { uintptr_t object, stack; };
std::vector<Entry> live;
uintptr_t refused = 0;
int calls = 0;
bool scripts_safe = true, change_scripts = false, revoke_vendor = false;
bool change_cell = false, revoke_stealing = false, slow_transfer = false, bind_quest = false;
std::array<unsigned char, 0x20> extras{};
std::array<unsigned char, 0x38> alias_node{};
void publish() {
    put(inventory, 0x58, reinterpret_cast<uintptr_t>(live.data()));
    put(inventory, 0x60, uint32_t(live.capacity()));
    put(inventory, 0x68, uint32_t(live.size()));
}
void reset() {
    scripts_safe = true; change_scripts = false; revoke_vendor = false;
    loot_vendor_chests = false;
    calls = 0; change_cell = revoke_stealing = slow_transfer = bind_quest = false;
    put(source, 0x100, uintptr_t(0));
    inventory_loot_cursors.clear();
    auto_loot = auto_loot_inventories = auto_loot_steal = true;
    put(source, 0xB8, uintptr_t(0x123400)); put(player, 0xB8, uintptr_t(0x123400));
    live.clear();
    for (size_t i = 0; i < items.size(); ++i) {
        put(items[i], 0x1A, uint8_t(0x30)); put(stacks[i], 0x20, uint32_t(1));
        put(stacks[i], 0x18, uintptr_t(0));
        live.push_back({address(items[i]), address(stacks[i])});
    }
    publish();
}
void __attribute__((ms_abi)) remove_item(void* actor, uint32_t*, void* raw) {
    assert(actor == source.data());
    uintptr_t object = 0, destination = 0; int32_t count = 0, reason = 0;
    auto* data = static_cast<unsigned char*>(raw);
    std::memcpy(&object, data + 0x20, sizeof(object));
    std::memcpy(&count, data + 0x28, sizeof(count));
    std::memcpy(&reason, data + 0x2C, sizeof(reason));
    std::memcpy(&destination, data + 0x30, sizeof(destination));
    assert(count == 1 && reason == 4 && destination == address(player));
    ++calls;
    if (object != refused) {
        // Native RemoveItem can compact and reallocate the inventory array.
        std::vector<Entry> replacement;
        for (const auto& entry : live)
            if (entry.object != object) replacement.push_back(entry);
        live.swap(replacement); publish();
    }
    if (change_cell) put(player, 0xB8, uintptr_t(0x999900));
    if (revoke_stealing) auto_loot_steal = false;
    if (slow_transfer) usleep(3000);
    if (bind_quest) put(source, 0x100, address(extras));
    if (change_scripts) scripts_safe = false;
    if (revoke_vendor) loot_vendor_chests = false;
}
}

int main() {
    char directory[] = "/tmp/fo4hud-loot-batch-XXXXXX";
    assert(mkdtemp(directory)); assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
    void* memory = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const auto image = reinterpret_cast<uintptr_t>(memory);
    std::array<unsigned char, 0x100> base{};
    const auto vtable = image + 0x3000, remove = image + 0x2000;
    unsigned char jump[] = {0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0};
    const auto target = reinterpret_cast<uintptr_t>(remove_item);
    std::memcpy(jump + 2, &target, sizeof(target));
    std::memcpy(reinterpret_cast<void*>(remove), jump, sizeof(jump));
    std::memcpy(reinterpret_cast<void*>(vtable + 0x6D * 8), &remove, sizeof(remove));
    assert(mprotect(reinterpret_cast<void*>(remove), 4096, PROT_READ | PROT_EXEC) == 0);
    put(source, 0, vtable); put(source, 0x14, uint32_t(0x123));
    put(source, 0xE0, address(base)); put(source, 0xF8, address(inventory));
    auto loot = [&] { loot_inventory_on_game_thread(image, address(source), address(base), 0x1F,
                                                   address(player), 0x123, true, [](uintptr_t, uintptr_t) { return scripts_safe; }); };
    reset(); refused = address(items[0]);
    loot();
    assert(calls > 1 && calls <= int(kInventoryLootBatchSize));
    for (int pass = 0; pass < 30 && live.size() > 1; ++pass) loot();
    assert(live.size() == 1 && live[0].object == refused); // Refused first item cannot block the chest.
    reset(); change_cell = true; loot(); assert(calls == 1);
    reset(); revoke_stealing = true; loot(); assert(calls == 1);
    reset(); slow_transfer = true; loot(); assert(calls == 1); // No more transfers after the time budget.
    // Quest sources stay untouched even with stealing enabled.
    refused = 0;
    put(extras, 8, address(alias_node));put(alias_node, 0x12, uint8_t(0x88));
    put(alias_node, 0x20, uint32_t(4));put(alias_node, 0x28, uint32_t(1));
    reset();put(source, 0x100, address(extras));loot();assert(calls == 0);
    Fo4Entity quest_container;quest_container.address=address(source);
    quest_container.cell_address=0x123400;quest_container.form_id=0x123;quest_container.radar_category=2;
    { std::lock_guard<std::mutex> lock(auto_loot_mutex);pending_auto_loot={}; }
    // Queueing is only scheduling; execution rejects the protected source.
    assert(fo4hud_combat_queue_auto_loot(quest_container));
    { std::lock_guard<std::mutex> lock(auto_loot_mutex);pending_auto_loot={}; }
    // Binding a quest during a transfer stops the batch before the next item.
    reset();bind_quest=true;loot();assert(calls == 1);
    // Quest-bound stacks are preserved while ordinary loot is transferred.
    reset();put(stacks[0], 0x18, address(extras));
    for (int pass=0;pass<30 && live.size()>1;++pass) loot();
    assert(live.size() == 1 && live[0].object == address(items[0]));
    // Locks and blocked activation also override the stealing toggle.
    for (uint8_t type : {0x2A, 0xA3, 0x52, 0x53, 0x68, 0x95, 0xA0}) {
        reset(); put(alias_node, 0x12, type); put(alias_node, 0x18, uint32_t(1));
        put(source, 0x100, address(extras)); loot(); assert(calls == 0);
    }
    // Scripted/unknown containers stay manual; a newly attached script stops a batch.
    reset(); scripts_safe = false; loot(); assert(calls == 0);
    reset(); change_scripts = true; loot(); assert(calls == 1);
    // Any quest-bound stack excludes the entire object, since RemoveItem may
    // choose a stack other than the first one.
    reset(); put(alias_node, 0x12, uint8_t(0x88));
    std::array<unsigned char, 0x30> second_stack{};
    put(second_stack, 0x20, uint32_t(1)); put(second_stack, 0x18, address(extras));
    put(stacks[0], 0x10, address(second_stack));
    for (int pass=0;pass<30 && live.size()>1;++pass) loot();
    assert(live.size() == 1 && live[0].object == address(items[0]));
    put(stacks[0], 0x10, uintptr_t(0));
    reset();
    uintptr_t object = 0; int32_t count = 0; size_t cursor = 0;
    assert(!first_inventory_stack(address(source), true, object, count, &cursor, LootClock::now() - std::chrono::seconds(1)));
    assert(calls == 0);
    // Category selection continues to apply to every item in a batch.
    auto_loot_categories = Fo4LootWeapons; loot(); assert(calls == 0);
    // Vendor-only looting works with every regular auto-loot option off and
    // category filters empty, and bypasses the chest lock without changing it.
    std::array<unsigned char, 0x380> plugin{};
    std::array<unsigned char, 0x10> source_files{}, plugin_files{};
    std::strcpy(reinterpret_cast<char*>(plugin.data()+0x70), "Fallout4.esm");
    put(source_files, 0, address(plugin_files)); put(source_files, 8, uint32_t(1));
    put(plugin_files, 0, address(plugin));
    put(source, 8, address(source_files)); put(base, 8, address(source_files));
    put(source, 0x14, uint32_t(0x1679C7)); put(source, 0x1A, uint8_t(64));
    put(base, 0x14, uint32_t(0x1679C6)); put(base, 0x1A, uint8_t(0x1F));
    auto vendor_loot = [&] { loot_inventory_on_game_thread(image, address(source), address(base), 0x1F,
        address(player), 0x1679C7, false, [](uintptr_t, uintptr_t) { return scripts_safe; }, true); };
    auto reset_vendor = [&] {
        reset(); loot_vendor_chests = true;
        auto_loot = auto_loot_inventories = auto_loot_steal = false;
        auto_loot_categories = 0;
        put(alias_node, 0x12, uint8_t(0x2A)); put(source, 0x100, address(extras));
    };
    reset_vendor(); vendor_loot(); assert(calls > 1);
    for (int pass=0; pass<30 && !live.empty(); ++pass) vendor_loot();
    assert(live.empty());
    uint8_t lock_type = 0; assert(read_value(address(alias_node)+0x12, lock_type) && lock_type == 0x2A);
    reset_vendor(); revoke_vendor = true; vendor_loot(); assert(calls == 1);
    reset_vendor(); change_cell = true; vendor_loot(); assert(calls == 1);
    reset_vendor(); change_scripts = true; vendor_loot(); assert(calls == 1);
    reset_vendor(); put(alias_node, 0x12, uint8_t(0x88)); vendor_loot(); assert(calls == 0);
    reset_vendor(); loot_vendor_chests = false; vendor_loot(); assert(calls == 0);
    reset_vendor(); put(base, 0x14, uint32_t(0x123)); vendor_loot(); assert(calls == 0);
    put(base, 0x14, uint32_t(0x1679C6));
    // Vendor transfers take whole quantities, rather than the normal 1000 cap.
    reset_vendor(); put(stacks[0], 0x20, uint32_t(5000));
    uintptr_t vendor_object = 0; int32_t vendor_count = 0;
    assert(first_inventory_stack(address(source), true, vendor_object, vendor_count, nullptr,
        LootClock::time_point::max(), nullptr, true));
    assert(vendor_count == 5000);
    munmap(memory, kImageSize);
    std::filesystem::remove_all(directory);
}
