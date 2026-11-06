#include "../game_entities.cpp"
#include "../companion_overlay.h"
#include "../imgui/imgui_internal.h"
#include <sys/mman.h>
#include <cassert>

namespace {
template <class T> void put(void* bytes, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(bytes) + offset, &value, sizeof(value));
}
int value_calls = 0;
uintptr_t affinity_info = 0;
float computed_weight = 42.5f;
void* __attribute__((ms_abi)) lookup_form(uint32_t id) {
    assert(id == kAffinityForm);
    return reinterpret_cast<void*>(affinity_info);
}
float __attribute__((ms_abi)) get_weight(void* actor) {
    uintptr_t inventory = 0;
    std::memcpy(&inventory, static_cast<unsigned char*>(actor) + 0xF8, 8);
    if (inventory) std::memcpy(reinterpret_cast<void*>(inventory + 0x70), &computed_weight, 4);
    return std::min(computed_weight, 12.0f); // NPC return is capped; cache retains the real load.
}
float __attribute__((ms_abi)) get_value(void*, void* info) {
    ++value_calls;
    float value;
    std::memcpy(&value, static_cast<unsigned char*>(info) + 0x20, 4);
    return value;
}
float __attribute__((ms_abi)) get_modifier(void*, int32_t kind, void* info) {
    assert(kind == 2);
    float damage;
    std::memcpy(&damage, static_cast<unsigned char*>(info) + 0x24, 4);
    return damage;
}
void trampoline(unsigned char* code, uintptr_t destination) {
    code[0] = 0x48; code[1] = 0xB8;
    put(code, 2, destination);
    code[10] = 0xFF; code[11] = 0xE0;
}
}

int main() {
    auto* image = static_cast<unsigned char*>(mmap(nullptr, kImageSize,
        PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    alignas(8) unsigned char ui[0x260]{}, menu[0x180]{}, manager[0x80]{}, actor[0x500]{},
        npc[0x300]{}, faction[0x60]{}, extras[0x20]{}, changes[0x40]{}, records[16]{};
    uintptr_t stack[] = {reinterpret_cast<uintptr_t>(menu)};
    std::array<unsigned char, 64> handles{};
    auto addr = [](auto& bytes) { return reinterpret_cast<uintptr_t>(&bytes); };
    put(image, kUiPointerRva, addr(ui)); put(ui, 0, base + kUiVtableRva);
    put(ui, 0x190, addr(stack)); put(ui, 0x198, uint32_t(1)); put(ui, 0x1A0, uint32_t(1));
    put(menu, 0, base + kDialogueVtableRva); put(menu, 0x58, uint32_t(0x40)); put(menu, 0x164, uint8_t(1));
    put(image, kTopicManagerRva, addr(manager)); put(manager, 0x4C, uint8_t(1));
    constexpr uint32_t handle = 0x200001;
    put(manager, 0x14, handle);
    put(image, kHandleTablePointerRva, addr(handles));
    put(handles.data(), 16, uint32_t(0x4000000 | 0x200000)); put(handles.data(), 24, addr(actor) + 32);
    put(actor, 40, uint32_t(1 << 11)); put(actor, 0x14, uint32_t(0x102030)); put(actor, 0x1A, uint8_t(65));
    put(actor, 0xE0, addr(npc)); put(npc, 0x1A, uint8_t(45)); put(npc, 0x14, uint32_t(0x2F1E));
    put(npc, 0xB8, addr(records)); put(npc, 0xC8, uint32_t(1));
    put(records, 0, addr(faction)); put(faction, 0x14, uint32_t(0x1EC1B9)); put(faction, 0x1A, uint8_t(14));
    put(actor, 0x58, base + kActorValueOwnerVtableRva);
    put(image, kActorValueOwnerVtableRva + 8, base + 0xCC93D0);
    put(image, kActorValueOwnerVtableRva + 7 * 8, base + 0xCC9E00);
    trampoline(image + 0xCC93D0, reinterpret_cast<uintptr_t>(get_value));
    trampoline(image + 0xCC9E00, reinterpret_cast<uintptr_t>(get_modifier));
    trampoline(image + 0x311B80, reinterpret_cast<uintptr_t>(lookup_form));
    trampoline(image + 0x507DA0, reinterpret_cast<uintptr_t>(get_weight));
    alignas(8) unsigned char inventory[0x80]{};
    put(actor, 0xF8, addr(inventory));
    put(inventory, 0x70, -1.0f);
    constexpr uintptr_t offsets[] = {0xD8, 0x08, 0x248, 0x170, 0xC0, 0x88, 0x100, 0x18, 0x140, 0x80, 0xA8, 0x318};
    alignas(8) unsigned char avs[13][0x40]{};
    uintptr_t values[13]{};
    for (unsigned i = 0; i < 13; ++i) {
        values[i] = reinterpret_cast<uintptr_t>(avs[i]);
        put(avs[i], 0x1A, uint8_t(98)); put(avs[i], 0x20, float(i + 3));
        if (i < 12) put(image, kActorValuesRva + offsets[i], values[i]);
    }
    put(avs[0], 0x20, 100.0f); put(avs[0], 0x24, -50.0f);
    put(avs[1], 0x20, 60.0f); put(avs[1], 0x24, -20.0f);
    put(avs[12], 0x14, kAffinityForm); put(avs[12], 0x20, 750.0f);
    put(image, kActorValuesRva + 0x420, addr(values));
    put(image, kActorValuesRva + 0x428, uint32_t(12)); put(image, kActorValuesRva + 0x430, uint32_t(12));
    // Real game layout: CA_Affinity is outside the built-in actor-value array.
    affinity_info = values[12];
    Fo4CompanionState state;
    assert(sample_companion(base, state));
    assert(state.affinity == 750 && state.health == 100 && state.max_health == 150);
    assert(state.inventory_weight == 42.5f); // Preserve fractional/over-capacity inventory weight.
    affinity_info = 0; assert(sample_companion(base, state)); assert(!state.affinity);
    affinity_info = values[0]; assert(sample_companion(base, state)); assert(!state.affinity);
    affinity_info = values[12];
    assert(sample_companion(base, state));
    assert(state.action_points == 60 && state.max_action_points == 80);
    for (unsigned i = 0; i < 7; ++i) assert(state.special[i] == float(i + 5));
    assert(state.carry_weight == 12 && state.damage_resistance == 13 && state.energy_resistance == 14);
    assert(state.synth_status == Fo4SynthStatus::Unknown);
    alignas(8) unsigned char race[0x100]{}, keyword[0x30]{}, death_item[0x30]{};
    uintptr_t keyword_array[] = {addr(keyword)};
    put(race, 0x1A, uint8_t(17));
    put(actor, 0x418, addr(race));
    for (uint32_t id : {0xE8D09u, 0x10BD65u, 0x2261A4u}) {
        put(race, 0x14, id);
        assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Confirmed);
    }
    put(race, 0x14, uint32_t(0x13746)); // A human race is insufficient to rule out a synth.
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(keyword, 0x14, uint32_t(0x10C3CE)); put(keyword, 0x1A, uint8_t(4));
    put(race, 0x88, addr(keyword_array)); put(race, 0x90, uint32_t(1));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Confirmed); // Custom synth race keyword.
    put(race, 0x90, uint32_t(0));
    put(npc, 0x168, addr(keyword_array)); put(npc, 0x170, uint32_t(1));
    assert(sample_companion(base, state)); assert(state.synth_status == Fo4SynthStatus::Confirmed);
    put(keyword, 0x1A, uint8_t(31));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown); // ID alone is not a keyword.
    put(keyword, 0x1A, uint8_t(4));
    put(npc, 0x170, uint32_t(1025));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(npc, 0x170, uint32_t(1)); put(npc, 0x168, uintptr_t(1));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(npc, 0x170, uint32_t(0)); put(npc, 0x168, uintptr_t(0));
    put(npc, 0x88, addr(death_item)); put(death_item, 0x1A, uint8_t(56));
    for (uint32_t id : {0x1357E6u, 0xD10ACu}) {
        put(death_item, 0x14, id);
        assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Confirmed); // Human-race synth death loot.
    }
    put(death_item, 0x1A, uint8_t(31));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(death_item, 0x1A, uint8_t(56));
    put(death_item, 0x14, uint32_t(0x1234));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(npc, 0x88, uintptr_t(1));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Unknown);
    put(npc, 0x88, uintptr_t(0));
    put(actor, 0x418, uintptr_t(0)); put(npc, 0x1B8, addr(race)); put(race, 0x14, uint32_t(0x2261A4));
    assert(companion_synth_status(addr(actor)) == Fo4SynthStatus::Confirmed); // Base race fallback.
    put(npc, 0x1B8, uintptr_t(0));
    assert(sample_companion(base, state)); assert(state.synth_status == Fo4SynthStatus::Unknown);
    auto rejected = [&] {
        const int calls = value_calls;
        assert(!sample_companion(base, state)); assert(!state.address && !state.affinity);
        assert(calls == value_calls); // No native calls after a failed identity/layout check.
    };
    put(manager, 0x4C, uint8_t(0)); rejected(); put(manager, 0x4C, uint8_t(1));
    put(menu, 0x164, uint8_t(0)); rejected(); put(menu, 0x164, uint8_t(1));
    put(menu, 0x58, uint32_t(0)); rejected(); put(menu, 0x58, uint32_t(0x40));
    stack[0] = 1; rejected(); stack[0] = addr(menu);
    put(ui, 0x1A0, uint32_t(65)); rejected(); put(ui, 0x1A0, uint32_t(1));
    put(handles.data(), 16, uint32_t(0x4000000)); rejected();
    put(handles.data(), 16, uint32_t(0x4000000 | 0x200000));
    put(actor, 0x10, uint32_t(0x20)); rejected(); put(actor, 0x10, uint32_t(0));
    put(actor, 0x58, uintptr_t(1)); rejected(); put(actor, 0x58, base + kActorValueOwnerVtableRva);
    put(faction, 0x14, uint32_t(0x999)); assert(!companion_factions(addr(actor)));
    assert(sample_companion(base, state) && !state.is_companion && !state.affinity);
    put(faction, 0x14, uint32_t(0x1EC1B9)); put(records, 8, int8_t(-1)); assert(!companion_factions(addr(actor)));
    // Current/previous companions added through ExtraFactionChanges.
    put(actor, 0x100, addr(extras)); put(extras, 8, addr(changes)); put(changes, 0x12, uint8_t(91));
    unsigned char runtime_record[16]{};
    put(runtime_record, 0, addr(faction)); put(changes, 0x20, addr(runtime_record)); put(changes, 0x30, uint32_t(1));
    put(faction, 0x14, uint32_t(0x23C01)); assert(sample_companion(base, state));
    put(runtime_record, 8, int8_t(-1)); assert(!companion_factions(addr(actor)));
    // Dogmeat uses different factions and has no affinity system.
    put(npc, 0x14, uint32_t(0x1D15C)); assert(sample_companion(base, state)); assert(!state.affinity);
    put(npc, 0x14, uint32_t(0x2F1E)); put(records, 8, int8_t(0));
    // Missing and nonfinite values are unavailable; zero is a valid affinity.
    put(avs[12], 0x20, 0.0f); put(avs[2], 0x20, std::numeric_limits<float>::quiet_NaN());
    put(image, kActorValuesRva + 0x80, uintptr_t(0));
    assert(sample_companion(base, state)); assert(state.affinity == 0 && !state.special[0] && !state.carry_weight);
    put(avs[0], 0x24, std::numeric_limits<float>::infinity());
    assert(sample_companion(base, state)); assert(state.health && !state.max_health);

    computed_weight = 0; assert(sample_companion(base, state)); assert(state.inventory_weight == 0);
    computed_weight = std::numeric_limits<float>::quiet_NaN();
    assert(sample_companion(base, state)); assert(!state.inventory_weight);
    computed_weight = -1; assert(sample_companion(base, state)); assert(!state.inventory_weight);
    computed_weight = 42.5f; assert(sample_companion(base, state));
    // Cached samples cannot survive a menu close, target change, or stopped updates.
    companion_state = state;
    const auto now = std::chrono::steady_clock::now();
    companion_sample_time = now;
    assert(read_cached_companion(base, state, now));
    assert(!read_cached_companion(base, state, now + std::chrono::seconds(2)));
    assert(!state.address);
    put(manager, 0x4C, uint8_t(0)); assert(!read_cached_companion(base, state, now));
    put(manager, 0x4C, uint8_t(1));
    ++companion_state.form_id; assert(!read_cached_companion(base, state, now)); --companion_state.form_id;
    ++companion_state.handle; assert(!read_cached_companion(base, state, now)); --companion_state.handle;
    assert(read_cached_companion(base, state, now));

    put(actor, 0x418, addr(race));
    for (const auto& entry : {std::pair<uint32_t, const char*>{0x13746, "Human"},
                            {0x1A009, "Super mutant"}, {0xBB7D9, "Super mutant behemoth"},
                            {0x6B4EC, "Feral ghoul"}, {0xA96BF, "Feral ghoul (glowing)"},
                            {0xEAFB6, "Ghoul"}}) {
        put(race, 0x14, entry.first);
        assert(information_actor_race(addr(actor), Fo4SynthStatus::Unknown) == entry.second);
    }
    put(race, 0x14, uint32_t(0x13746));
    assert(information_actor_race(addr(actor), Fo4SynthStatus::Confirmed) == "Synth");
    put(race, 0x14, uint32_t(0xFF123456));
    unsigned char race_name[64]{};
    put(race_name, 0x10, uint32_t(11)); std::memcpy(race_name + 0x18, "Custom race", 11);
    put(race, 0x28, addr(race_name));
    assert(information_actor_race(addr(actor), Fo4SynthStatus::Unknown) == "Custom race");
    put(race, 0x28, uintptr_t(1));
    assert(information_actor_race(addr(actor), Fo4SynthStatus::Unknown) == "Unknown");
    put(race, 0x14, uint32_t(0x13746));

    // The activation target also works outside dialogue, for ordinary actors.
    alignas(8) unsigned char hud[0x600]{}, cursor[0x58]{}, player[0x200]{}, cell[0x100]{}, other_cell[0x100]{};
    put(image, kHudDataPointerRva, addr(hud)); put(hud, 0, base + kHudDataVtableRva);
    put(hud, 0x18, base + kRolloverVtableRva); put(hud, 0x598, base + kActivateSourceVtableRva);
    put(hud, 0x5A0, handle); put(hud, 0x5A4, uint8_t(1));
    put(image, kMenuCursorPointerRva, addr(cursor)); put(cursor, 0, base + kMenuCursorVtableRva);
    put(image, kPlayerPointerRva, addr(player)); put(player, 0xB8, addr(cell)); put(actor, 0xB8, addr(cell));
    put(cell, 0x1A, uint8_t(63)); put(cell, 0x40, uint16_t(1));
    put(other_cell, 0x1A, uint8_t(63)); put(other_cell, 0x40, uint16_t(1));
    put(image, kActivatePickLengthRva, 180.0f);
    put(actor, 0xD0, 100.0f); put(manager, 0x4C, uint8_t(0));
    put(npc, 0xC8, uint32_t(0)); put(actor, 0x100, uintptr_t(0));
    assert(sample_companion(base, state));
    assert(state.race == "Human" && !state.is_companion && !state.affinity);
    companion_state = state; companion_sample_time = now;
    assert(read_cached_companion(base, state, now));
    put(hud, 0x5A4, uint8_t(0)); rejected();
    assert(!read_cached_companion(base, state, now) && !state.address);
    put(hud, 0x5A4, uint8_t(1));
    put(hud, 0x5A0, uint32_t(0)); rejected(); put(hud, 0x5A0, handle);
    put(hud, 0x5A0, uint32_t(handle ^ 0x200000)); rejected(); put(hud, 0x5A0, handle);
    put(cursor, 0x50, uint32_t(1)); rejected(); put(cursor, 0x50, uint32_t(0));
    put(actor, 0xD0, 181.0f); rejected(); put(actor, 0xD0, 100.0f);
    put(actor, 0xD0, std::numeric_limits<float>::quiet_NaN()); rejected(); put(actor, 0xD0, 100.0f);
    put(image, kActivatePickLengthRva, 0.0f); rejected(); put(image, kActivatePickLengthRva, 180.0f);
    put(actor, 0xB8, addr(other_cell)); rejected(); put(actor, 0xB8, addr(cell));
    put(actor, 0x130, uint32_t(1 << 17)); rejected(); put(actor, 0x130, uint32_t(0));
    // A creature can be picked directly without offering a talk/activate prompt.
    put(hud, 0x5A4, uint8_t(0)); put(hud, 0x3C8, base + kViewCastSinkVtableRva);
    put(hud, 0x408, uint8_t(1)); put(hud, 0x3D0, handle);
    put(race, 0x14, uint32_t(0x6B4EC));
    assert(sample_companion(base, state) && state.race == "Feral ghoul");
    put(hud, 0x3D0, uint32_t(0)); rejected();
    put(hud, 0x5A4, uint8_t(1)); put(race, 0x14, uint32_t(0x13746));
    // Dialogue takes priority and survives cursor registration or normal pick range.
    put(manager, 0x4C, uint8_t(1)); put(cursor, 0x50, uint32_t(1)); put(actor, 0xD0, 500.0f);
    assert(sample_companion(base, state));
    state.is_companion = true; state.affinity = 750.0f; // Keep all rows present for the layout check.
    state.carry_weight = 350.0f; // Exercise the current / maximum display.
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (const ImVec2 display : {ImVec2(640, 480), ImVec2(1920, 1080), ImVec2(3440, 1440)}) {
        io.DisplaySize = display; io.DeltaTime = 1.0f / 60;
        for (unsigned frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame(); fo4hud_draw_companion_overlay(state, display); ImGui::Render();
        }
        assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        auto* window = ImGui::FindWindowByName("Actor information");
        assert(window && window->Pos.x >= 0 && window->Pos.y >= 0);
        assert(window->ScrollMax.y == 0); // Every stat fits without an unusable passive scrollbar.
        assert(window->Pos.x + window->Size.x <= display.x && window->Pos.y + window->Size.y <= display.y);
    }
    ImGui::DestroyContext();
    munmap(image, kImageSize);
}
