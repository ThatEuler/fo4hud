#include "../game_combat.cpp"
#include <array>
#include <cassert>

namespace {
float current_health = 100.0f;
float health_at_death_check = 0.0f;
int death_checks = 0, combat_calls = 0;
uintptr_t __attribute__((ms_abi)) limb_only_hit(void*, void*) {
    ++combat_calls;
    return 0xCAFE;
}
float __attribute__((ms_abi)) get_health(void*, void*) { return current_health; }
void __attribute__((ms_abi)) mod_health(void*, int32_t modifier, void*, float delta) {
    assert(modifier == 2);
    current_health += delta;
}
void __attribute__((ms_abi)) death_check(void*, void*) {
    ++death_checks;
    health_at_death_check = current_health;
}

template<class T, size_t N>
void put(std::array<unsigned char, N>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

// The real hook requires native AV methods to reside inside the game image.
// Stand-in code in that image jumps to our MS-ABI getter and modifier.
void trampoline(uintptr_t address, uintptr_t target) {
    unsigned char code[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(code + 2, &target, sizeof(target));
    std::memcpy(reinterpret_cast<void*>(address), code, sizeof(code));
}
}

uintptr_t __attribute__((ms_abi)) ordinary_hit(void* actor, void*) {
    ++combat_calls;
    uintptr_t player = 0;
    assert(read_value(image_base + kPlayerPointerRva, player));
    health_hook(actor, reinterpret_cast<void*>(player));
    return 0xCAFE;
}

int main() {
    void* memory = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    image_base = reinterpret_cast<uintptr_t>(memory);
    trampoline(image_base, reinterpret_cast<uintptr_t>(get_health));
    trampoline(image_base + 16, reinterpret_cast<uintptr_t>(mod_health));
    assert(mprotect(memory, 4096, PROT_READ | PROT_EXEC) == 0);
    uintptr_t owner_vtable[7]{};
    owner_vtable[1] = image_base;
    owner_vtable[6] = image_base + 16;
    std::array<unsigned char, 0x440> actor{};
    std::array<unsigned char, 0x78> base{};
    put(actor, 0x1A, uint8_t(65));
    put(base, 0x1A, uint8_t(45));
    put(actor, 0xE0, reinterpret_cast<uintptr_t>(base.data()));
    put(actor, 0x58, reinterpret_cast<uintptr_t>(owner_vtable));
    const uintptr_t player = 0x123400;
    const uintptr_t health_info = 0x123500;
    std::memcpy(reinterpret_cast<void*>(image_base + kPlayerPointerRva), &player, sizeof(player));
    std::memcpy(reinterpret_cast<void*>(image_base + kActorValueListRva + kHealthActorValueOffset),
                &health_info, sizeof(health_info));
    original_health = death_check;
    installed = true;
    enabled = true;

    struct Case { uint32_t runtime, base; bool excluded; };
    const Case cases[] = {
        {0, 0, false},
        {0, 1u << 11, false}, // Reaver base Protected flag
        {1u << 19, 0, false}, // Runtime Protected flag
        {1u << 19, 1u << 11, false},
        {1u << 18, 0, true}, // Runtime Essential
        {0, 1u << 1, true},  // Base Essential
        {0, 1u << 31, true}, // Base Invulnerable
        {1u << 19, (1u << 11) | (1u << 1), true}
    };
    for (const auto& test : cases) {
        put(actor, 0x43C, test.runtime);
        put(base, 0x70, test.base);
        for (float starting_health : {100.0f, -20.0f}) {
            current_health = starting_health;
            const int prior_checks = death_checks;
            health_hook(actor.data(), reinterpret_cast<void*>(player));
            assert(death_checks == prior_checks + 1);
            assert(test.excluded ? health_at_death_check > 0.0f : health_at_death_check <= 0.0f);
        }
    }
    assert(instant_hits == 4 && protected_hits == 8);
    put(actor, 0x43C, uint32_t(0));
    put(base, 0x70, uint32_t(1u << 11));
    current_health = 100.0f;
    health_hook(actor.data(), reinterpret_cast<void*>(player + 8));
    assert(current_health == 100.0f); // NPC attacker still gets vanilla handling.
    enabled = false;
    health_hook(actor.data(), reinterpret_cast<void*>(player));
    assert(current_health == 100.0f);
    // A confirmed robot limb hit need not invoke HandleHealthDamage. The
    // queued-hit hook must kill on that first hit, after vanilla processing.
    std::array<unsigned char, 0x140> player_actor{};
    std::array<unsigned char, 0xE0> hit{};
    std::array<uintptr_t, 6> handles{};
    const uintptr_t live_player = reinterpret_cast<uintptr_t>(player_actor.data());
    put(player_actor, 0x28, uint32_t(1 << 11));
    handles[2] = 0x4000000;
    handles[3] = live_player + 0x20;
    const auto table = reinterpret_cast<uintptr_t>(handles.data());
    std::memcpy(reinterpret_cast<void*>(image_base + kPlayerPointerRva), &live_player, sizeof(live_player));
    std::memcpy(reinterpret_cast<void*>(image_base + kHandleTablePointerRva), &table, sizeof(table));
    put(hit, 0x40, uint32_t(1)); put(hit, 0x9C, 10.0f);
    original_combat_damage = limb_only_hit;
    enabled = true;
    put(actor, 0x130, uint32_t(0));
    for (const auto& test : cases) {
        put(actor, 0x43C, test.runtime); put(base, 0x70, test.base);
        current_health = 200.0f;
        const auto calls = combat_calls;
        assert(combat_damage_hook(actor.data(), hit.data()) == 0xCAFE);
        assert(combat_calls == calls + 1);
        assert(test.excluded ? current_health == 200.0f : current_health <= 0.0f);
    }
    put(actor, 0x43C, uint32_t(0)); put(base, 0x70, uint32_t(0));
    auto ignored_hit = [&] {
        current_health = 200.0f;
        const auto calls = combat_calls;
        assert(combat_damage_hook(actor.data(), hit.data()) == 0xCAFE);
        assert(combat_calls == calls + 1 && current_health == 200.0f);
    };
    put(hit, 0x9C, 0.0f); ignored_hit(); // No health or limb damage.
    put(hit, 0x9C, 10.0f);
    put(hit, 0x40, uint32_t(0)); ignored_hit(); // No attacker.
    put(hit, 0x40, uint32_t(0x200001)); ignored_hit(); // Stale handle generation.
    put(hit, 0x40, uint32_t(2)); ignored_hit(); // NPC handle, not player.
    put(hit, 0x40, uint32_t(1));
    enabled = false; ignored_hit();
    enabled = true;
    put(actor, 0x130, uint32_t(2 << 17)); ignored_hit(); // Already dead.
    put(actor, 0x130, uint32_t(0));
    put(hit, 0x9C, 0.0f); put(hit, 0x90, 10.0f);
    current_health = 200.0f;
    combat_damage_hook(actor.data(), hit.data());
    assert(current_health <= 0.0f); // Ordinary health hit also covered.

    original_combat_damage = ordinary_hit;
    current_health = 200.0f;
    const auto death_checks_before = death_checks;
    combat_damage_hook(actor.data(), hit.data());
    assert(current_health <= 0.0f && death_checks == death_checks_before + 1);

    // Verify the actual rel32 patch, forwarding island, and restoration.
    const auto call_site = image_base + kCombatDamageCallRva;
    const int32_t expected_delta = int32_t(kCombatDamageRva - kCombatDamageCallRva - 5);
    *reinterpret_cast<uint8_t*>(call_site) = 0xE8;
    std::memcpy(reinterpret_cast<void*>(call_site + 1), &expected_delta, sizeof(expected_delta));
    const auto page = call_site & ~(uintptr_t(4095));
    assert(mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_EXEC) == 0);
    uintptr_t patched = 0; int32_t original = 0; uint8_t* stub = nullptr;
    assert(install_relative_call(image_base, kCombatDamageCallRva, kCombatDamageRva,
                                reinterpret_cast<uintptr_t>(combat_damage_hook), patched, original, stub));
    int32_t delta = 0;
    std::memcpy(&delta, reinterpret_cast<void*>(call_site + 1), sizeof(delta));
    assert(patched == call_site && original == expected_delta);
    assert(call_site + 5 + delta == reinterpret_cast<uintptr_t>(stub));
    assert(patch_relative_call(call_site, original));
    std::memcpy(&delta, reinterpret_cast<void*>(call_site + 1), sizeof(delta));
    assert(delta == expected_delta);
    munmap(stub, 4096);
    munmap(memory, kImageSize);
}
