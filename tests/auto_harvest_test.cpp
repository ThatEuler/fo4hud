#include "../game_combat.cpp"
#include <array>
#include <cassert>

namespace {
int activation_calls = 0;
bool activation_succeeds = false;
void* expected_player = reinterpret_cast<void*>(0x123400);
bool __attribute__((ms_abi)) activate_flora(void*, void* reference, void* player,
                                           uint8_t, void*, int32_t) {
    assert(player == expected_player);
    ++activation_calls;
    if (activation_succeeds) {
        uint32_t flags = 0;
        std::memcpy(&flags, static_cast<unsigned char*>(reference) + 0x10, sizeof(flags));
        flags |= 0x2000; // The native produce activation sets this on success.
        std::memcpy(static_cast<unsigned char*>(reference) + 0x10, &flags, sizeof(flags));
    }
    return activation_succeeds;
}

template<class T, size_t N>
void put(std::array<unsigned char, N>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
}

int main() {
    char directory[] = "/tmp/fo4hud-harvest-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    unsigned char jump[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const uintptr_t target = reinterpret_cast<uintptr_t>(activate_flora);
    std::memcpy(jump + 2, &target, sizeof(target));
    std::memcpy(code, jump, sizeof(jump));
    assert(mprotect(code, 4096, PROT_READ | PROT_EXEC) == 0);
    const uintptr_t image = reinterpret_cast<uintptr_t>(code);
    std::array<uintptr_t, 0x41> base_vtable{};
    base_vtable[0x40] = image;
    std::array<unsigned char, 0x160> base{};
    std::array<unsigned char, 0xF0> plant{};
    put(base, 0x00, reinterpret_cast<uintptr_t>(base_vtable.data()));
    put(base, 0x1A, uint8_t(0x29));
    put(base, 0x158, uintptr_t(0x123500));
    put(plant, 0x1A, uint8_t(64));
    put(plant, 0xE0, reinterpret_cast<uintptr_t>(base.data()));
    AutoLootCandidate candidate{reinterpret_cast<uintptr_t>(plant.data()), 0x123600, 0x123700, 5};
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);
    auto attempt = [&](int seconds) {
        return harvest_flora_on_game_thread(image, candidate, reinterpret_cast<uintptr_t>(base.data()),
                                            expected_player, start + std::chrono::seconds(seconds));
    };
    assert(fo4hud_flora_harvestable(candidate.address));
    assert(!attempt(0) && activation_calls == 1); // Failed activation.
    activation_succeeds = true;
    assert(!attempt(1) && activation_calls == 1); // Brief backoff only.
    assert(attempt(2) && activation_calls == 2);  // Retry succeeds without a 10-minute wait.
    assert(!fo4hud_flora_harvestable(candidate.address));
    assert(!attempt(5) && activation_calls == 2); // Depleted plant is not activated again.
    put(plant, 0x10, uint32_t(0));               // Settlement crop regrew.
    assert(attempt(6) && activation_calls == 3); // Regrowth is eligible immediately.
    for (uint32_t flags : {1u << 5, 1u << 11, 1u << 13}) {
        put(plant, 0x10, flags);
        assert(!fo4hud_flora_harvestable(candidate.address));
        assert(!attempt(10) && activation_calls == 3);
    }
    put(plant, 0x10, uint32_t(0));
    put(base, 0x158, uintptr_t(0));
    assert(!fo4hud_flora_harvestable(candidate.address));
    assert(!attempt(10) && activation_calls == 3);
    put(base, 0x158, uintptr_t(0x123500));
    put(base, 0x1A, uint8_t(0x1F)); // A container is never treated as a crop.
    assert(!fo4hud_flora_harvestable(candidate.address));
    put(base, 0x1A, uint8_t(0x29));
    auto replacement = plant;
    candidate.address = reinterpret_cast<uintptr_t>(replacement.data());
    assert(attempt(7) && activation_calls == 4); // Reused form ID, different reference.
    munmap(code, 4096);
    std::filesystem::remove_all(directory);
}
