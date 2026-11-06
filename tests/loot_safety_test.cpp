#include "../loot_safety.h"
#include "../game_entities.h"
#include <array>
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <vector>

namespace {
template<class T, size_t N> void put(std::array<unsigned char, N>& p, size_t off, T value) {
    assert(off + sizeof(T) <= N);
    std::memcpy(p.data() + off, &value, sizeof(value));
}
template<size_t N> uintptr_t address(std::array<unsigned char, N>& p) {
    return reinterpret_cast<uintptr_t>(p.data());
}
void test_corpse_alias_exceptions() {
    std::array<unsigned char, 0x140> corpse{};
    std::array<unsigned char, 0x20> extras{}, blocking{};
    std::array<unsigned char, 0x30> node{}, quest{}, alias{};
    std::array<unsigned char, 0x40> name{};
    std::array<unsigned char, 0x90> file{};
    std::array<uintptr_t, 2> sources{};
    std::array<uintptr_t, 1> files{};
    std::array<uintptr_t, 6> entries{};
    std::vector<std::pair<uintptr_t, size_t>> regions;
    auto region = [&](auto& data) {
        regions.push_back({reinterpret_cast<uintptr_t>(data.data()), sizeof(data)});
    };
    region(corpse); region(extras); region(blocking); region(node); region(quest);
    region(alias); region(name); region(file); region(sources); region(files); region(entries);
    auto read = [&](uintptr_t p, auto& value) {
        for (auto r : regions) if (p >= r.first && p - r.first + sizeof(value) <= r.second) {
            std::memcpy(&value, reinterpret_cast<void*>(p), sizeof(value)); return true;
        }
        return false;
    };
    auto ptr = [](auto& data) { return reinterpret_cast<uintptr_t>(data.data()); };
    sources = {ptr(files), 1}; files[0] = address(file);
    std::memcpy(file.data() + 0x70, "DLCCoast.esm", 12);
    put(quest, 8, ptr(sources)); put(quest, 0x1A, uint8_t(0x50));
    put(alias, 8, address(name)); put(alias, 0x10, address(quest));
    entries = {address(quest), address(alias), 0, address(quest), address(alias), 0};
    put(node, 0x12, uint8_t(0x88)); put(node, 0x18, ptr(entries));
    put(node, 0x20, uint32_t(2)); put(node, 0x28, uint32_t(1));
    put(extras, 8, address(node)); put(corpse, 0x100, address(extras));
    put(corpse, 0x1A, uint8_t(65)); put(corpse, 0x130, uint32_t(2 << 17));
    auto reason = [&] { return fo4hud_loot_reference_reason(address(corpse), read); };
    struct AliasCase { const char* label; uint32_t id, local; const char* plugin; };
    const AliasCase cases[] = {
        {"Trappers", 13, 0x001B40, "DLCCoast.esm"},
        {"Enemies_Wave2", 9, 0x043822, "DLCCoast.esm"},
        {"MirelurkCollection", 3, 0x032D34, "DLCCoast.esm"},
    };
    for (const auto& example : cases) {
        const char* label = example.label;
        const uint32_t id = example.id, local = example.local;
        file.fill(0);
        std::memcpy(file.data() + 0x70, example.plugin, std::strlen(example.plugin) + 1);
        put(quest, 0x14, uint32_t(0x07000000 | local)); // Load order is not fixed.
        put(alias, 0x18, id); name.fill(0);
        put(name, 0x10, uint32_t(std::strlen(label)));
        std::memcpy(name.data() + 0x18, label, std::strlen(label) + 1);
        assert(!reason());
        assert(fo4hud_loot_extra_reason(address(extras), read)); // Stacks stay protected.
        put(corpse, 0x130, uint32_t(1 << 17)); assert(!reason());
        put(corpse, 0x130, uint32_t(0)); assert(reason()); // Living actor.
        put(corpse, 0x130, uint32_t(2 << 17));
        put(corpse, 0x1A, uint8_t(64)); assert(reason()); // Loose reference/container.
        put(corpse, 0x1A, uint8_t(65));
        put(alias, 0x18, uint32_t(id+1)); assert(reason()); put(alias, 0x18, id);
        put(quest, 0x14, uint32_t(local+1)); assert(reason());
        put(quest, 0x14, uint32_t(0x03000000 | local));
        name[0x18] = 'X'; assert(reason()); name[0x18] = label[0];
        file[0x70] = 'X'; assert(reason()); file[0x70] = example.plugin[0];
        const char* wrong_plugin = example.local == 0x1125E2 ? "DLCCoast.esm" : "Fallout4.esm";
        std::strcpy(reinterpret_cast<char*>(file.data() + 0x70), wrong_plugin);
        assert(reason()); // Matching local IDs in the wrong plugin stay protected.
        std::strcpy(reinterpret_cast<char*>(file.data() + 0x70), example.plugin);
        put(alias, 0x10, uintptr_t(1)); assert(reason()); put(alias, 0x10, address(quest));
        // One allowed attachment never hides another protected/unreadable one.
        put(node, 0x28, uint32_t(2)); assert(!reason());
        entries[4] = 1; assert(reason()); entries[4] = address(alias);
        put(node, 0x28, uint32_t(0)); assert(reason());
        put(node, 0x28, uint32_t(3)); assert(reason());
        put(node, 0x28, uint32_t(1));
        for (uint8_t type : {0x95, 0x2A, 0x52, 0xA0}) {
            put(node, 8, address(blocking)); put(blocking, 0x12, type);
            put(blocking, 0x18, uint32_t(1)); assert(reason());
        }
        put(node, 8, uintptr_t(0)); assert(!reason());
    }
    // Script approval replaces the quest-ID-specific settlement attack rule.
    // It works for both attack quests and other quests/plugins/alias names.
    static uintptr_t approved_alias;
    approved_alias = address(alias);
    auto script_policy = +[](uintptr_t a) { return a == approved_alias; };
    auto with_policy = [&] { return fo4hud_loot_reference_reason(address(corpse), read, script_policy); };
    for (uint32_t id : {0x00097CC7u, 0x001125E2u, 0xFE123456u}) {
        put(quest, 0x14, id);
        assert(reason()); // No script verdict: still manual.
        assert(!with_policy());
    }
    approved_alias = 0; assert(with_policy()); approved_alias = address(alias);
    put(alias, 0x10, uintptr_t(1)); assert(with_policy());
    put(alias, 0x10, address(quest));
    put(corpse, 0x130, uint32_t(0)); assert(with_policy());
    put(corpse, 0x130, uint32_t(2 << 17));
    put(corpse, 0x1A, uint8_t(64)); assert(with_policy());
    put(corpse, 0x1A, uint8_t(65));
    assert(fo4hud_loot_extra_reason(address(extras), read, false, script_policy));
    put(node, 0x28, uint32_t(2)); entries[4] = 1; assert(with_policy());
    entries[4] = address(alias); assert(!with_policy());
    put(node, 0x12, uint8_t(0x95)); assert(with_policy());
    put(node, 0x12, uint8_t(0x88));
    put(node, 8, address(blocking)); put(blocking, 0x12, uint8_t(0x2A)); assert(with_policy());
}

bool bound_script = false;
std::vector<uintptr_t> bound_objects{0x123};
uint64_t script_handle = 123;
uint32_t expected_vm_type = 64;
uint64_t __attribute__((ms_abi)) get_handle(void*, uint32_t type, void*) {
    assert(type == expected_vm_type); return script_handle;
}
uint64_t __attribute__((ms_abi)) invalid_handle(void*) { return 0xFFFF00000000ull; }
void __attribute__((ms_abi)) visit_scripts(void*, uint64_t handle, uintptr_t* function) {
    assert(handle == script_handle);
    if (!bound_script) return;
    auto* visitor = reinterpret_cast<uintptr_t*>(function[7]);
    auto* vtable = reinterpret_cast<uintptr_t*>(visitor[0]);
    using Visit = int32_t (__attribute__((ms_abi)) *)(void*, uintptr_t*);
    for (auto object : bound_objects)
        assert(reinterpret_cast<Visit>(vtable[2])(visitor, &object) == 1);
}
}

int main() {
    test_corpse_alias_exceptions();
    std::array<unsigned char, 0x108> reference{};
    std::array<unsigned char, 0x20> extras{}, node{};
    std::vector<std::pair<uintptr_t, size_t>> readable = {
        {address(reference), reference.size()}, {address(extras), extras.size()}, {address(node), node.size()}};
    auto read = [&](uintptr_t p, auto& value) {
        for (auto region : readable) {
            if (p >= region.first && p - region.first + sizeof(value) <= region.second) {
                std::memcpy(&value, reinterpret_cast<void*>(p), sizeof(value)); return true;
            }
        }
        return false;
    };
    auto reason = [&] { return fo4hud_loot_reference_reason(address(reference), read); };
    assert(!reason());
    put(reference, 0x100, address(extras)); put(extras, 8, address(node));
    for (uint8_t type : {0x88, 0x95, 0x2A, 0xA3, 0x52, 0x53, 0x68}) {
        put(node, 0x12, type); assert(reason());
    }
    put(node, 0x12, uint8_t(0xA0));
    for (uint32_t flags : {1u, 8u, 9u}) { put(node, 0x18, flags); assert(reason()); }
    put(node, 0x18, uint32_t(0x20)); assert(!reason()); // PlayerHasTaken is harmless.
    put(node, 8, address(node)); assert(reason()); // Cycle.
    put(node, 8, uintptr_t(1)); assert(reason()); // Unreadable link.
    put(node, 8, uintptr_t(0)); put(extras, 8, uintptr_t(1)); assert(reason());
    assert(fo4hud_loot_reference_reason(1, read));
    assert(fo4hud_loot_extra_reason(1, read));

    // Exercise the real VM bridge with native ABI stubs, including unknown
    // VM state, zero scripts, one script, invalid handles and validation failure.
    void* memory = mmap(nullptr, 0x425A000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    const auto image = reinterpret_cast<uintptr_t>(memory);
    auto store = [&](uintptr_t p, uintptr_t value) { std::memcpy(reinterpret_cast<void*>(p), &value, 8); };
    auto jump = [&](uintptr_t p, uintptr_t target) {
        unsigned char code[] = {0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0};
        std::memcpy(code + 2, &target, 8); std::memcpy(reinterpret_cast<void*>(p), code, sizeof(code));
        assert(mprotect(reinterpret_cast<void*>(p & ~uintptr_t(4095)), 4096, PROT_READ | PROT_EXEC) == 0);
    };
    put(reference, 0x1A, uint8_t(64));
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    const auto game_vm = image + 0x4000, vm = image + 0x5000;
    const auto policy = image + 0x6000, policy_table = image + 0x7000;
    store(image + 0x30EB308, game_vm); store(game_vm + 0xB0, vm);
    store(vm, image + 0x28FB520); store(image + 0x28FB520 + 0xF0, image + 0x211A4A0);
    store(vm + 0x90, policy); store(policy, policy_table); store(policy_table + 0x38, image + 0x8000);
    store(policy_table + 0x30, image + 0x9000);
    jump(image + 0x9000, reinterpret_cast<uintptr_t>(invalid_handle));
    jump(image + 0x8000, reinterpret_cast<uintptr_t>(get_handle));
    jump(image + 0x211A4A0, reinterpret_cast<uintptr_t>(visit_scripts));
    assert(fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_script = true; assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));

    // A completed vanilla bonus-loot script is a reviewed exception.
    std::array<unsigned char, 0x30> object{};
    std::array<std::array<unsigned char, 0x20>, 5> types{};
    std::array<std::array<unsigned char, 0x60>, 8> names{};
    auto pooled = [&](size_t i, const char* name) {
        const auto size = std::strlen(name);
        assert(size + 0x18 < names[i].size());
        names[i].fill(0);
        put(names[i], 0x10, uint32_t(size));
        std::memcpy(names[i].data() + 0x18, name, size);
        return address(names[i]);
    };
    const char* hierarchy[] = {"lockExtraLootSCRIPT", "ObjectReference", "Form", "ScriptObject"};
    for (size_t i = 0; i < 4; ++i) {
        put(types[i], 0x10, pooled(i, hierarchy[i]));
        put(types[i], 0x18, i < 3 ? address(types[i+1]) : uintptr_t(0));
    }
    put(object, 0, uint32_t(7)); put(object, 8, address(types[0]));
    put(object, 0x10, pooled(4, "donothing"));
    bound_objects = {address(object)};
    assert(fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    // Animation-only containerOpenIfEmpty is allowed in its default state.
    // Both reviewed bindings can coexist, as they do on vanilla wooden crates.
    std::array<unsigned char, 0x30> animation_object{};
    std::array<unsigned char, 0x20> animation_type{};
    put(animation_type, 0x10, pooled(6, "containerOpenIfEmpty"));
    put(animation_type, 0x18, address(types[1]));
    put(animation_object, 0, uint32_t(7));
    put(animation_object, 8, address(animation_type));
    put(animation_object, 0x10, pooled(7, ""));
    bound_objects = {address(animation_object), address(object)};
    assert(fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(animation_object, 0x10, uintptr_t(1));
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(animation_object, 0x10, address(names[4]));
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(animation_object, 0x10, uintptr_t(0)); // Null is also a valid empty state.
    assert(fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_objects.push_back(0x123);
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    // A subclass with added behavior cannot inherit the animation exception.
    put(types[4], 0x18, address(animation_type));
    put(types[4], 0x10, pooled(5, "CustomAnimationSubclass"));
    put(animation_object, 8, address(types[4]));
    bound_objects = {address(animation_object)};
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_objects = {address(object)};
    put(object, 0x10, pooled(4, "startState")); // Do not race OnLoad.
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(object, 0x10, uintptr_t(1)); // Unreadable state.
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(object, 0x10, pooled(4, "donothing"));
    put(types[4], 0x10, pooled(5, "CustomQuestSubclass"));
    put(types[4], 0x18, address(types[0])); put(object, 8, address(types[4]));
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(object, 8, address(types[0]));
    put(types[1], 0x10, pooled(1, "CustomReferenceParent"));
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(types[1], 0x10, pooled(1, "ObjectReference"));
    put(types[3], 0x18, address(types[0])); // Cyclic/extended hierarchy.
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(types[3], 0x18, uintptr_t(0));
    bound_objects = {address(object), 0x123}; // Unknown sibling cannot be hidden by safe binding.
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_objects = {0x123, address(object)};
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_objects = {0};
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    bound_objects = {address(object)};
    assert(fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    put(object, 0, uint32_t(0)); // Uninitialized object.
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));

    // Collection aliases use their own VM handle type and exact reviewed class
    // chains. Any unreviewed sibling or subclass still protects the corpse.
    std::array<unsigned char, 0x50> collection{};
    std::array<std::array<unsigned char, 0x20>, 6> alias_types{};
    std::array<std::array<unsigned char, 0x60>, 6> alias_names{};
    std::array<unsigned char, 0x30> re_object{}, load_object{};
    const char* alias_hierarchy[] = {"RECollectionAliasScript", "DefaultCollectionAliasOnLoad",
                                   "DefaultCollectionAlias", "RefCollectionAlias", "Alias", "ScriptObject"};
    for (size_t i = 0; i < 6; ++i) {
        const auto n = std::strlen(alias_hierarchy[i]);
        put(alias_names[i], 0x10, uint32_t(n));
        std::memcpy(alias_names[i].data() + 0x18, alias_hierarchy[i], n + 1);
        put(alias_types[i], 0x10, address(alias_names[i]));
        put(alias_types[i], 0x18, i < 5 ? address(alias_types[i+1]) : uintptr_t(0));
    }
    put(alias_types[0], 0x18, address(alias_types[3]));
    put(re_object, 0, uint32_t(7)); put(re_object, 8, address(alias_types[0]));
    put(load_object, 0, uint32_t(7)); put(load_object, 8, address(alias_types[1]));
    put(collection, 0, image + 0x2487E30);
    store(image + 0x2487E30 + 0x18, image + 0x404370);
    expected_vm_type = 165;
    auto alias_allowed = [&] { return fo4hud_collection_alias_scripts_allow_auto_loot(image, address(collection)); };
    bound_script = false; assert(!alias_allowed()); // Empty binding set is not an exception.
    bound_script = true;
    bound_objects = {address(re_object)}; assert(alias_allowed());
    bound_objects = {address(load_object)}; assert(alias_allowed());
    bound_objects = {address(re_object), address(load_object)}; assert(alias_allowed());
    for (auto unknown : {uintptr_t(0), uintptr_t(0x123), address(object)}) {
        bound_objects = {address(re_object), unknown}; assert(!alias_allowed());
        bound_objects = {unknown, address(load_object)}; assert(!alias_allowed());
    }
    bound_objects = {address(re_object), address(load_object)};
    put(re_object, 0, uint32_t(0)); assert(!alias_allowed()); put(re_object, 0, uint32_t(7));
    put(load_object, 0x10, address(names[4])); assert(!alias_allowed());
    put(load_object, 0x10, uintptr_t(1)); assert(!alias_allowed());
    put(load_object, 0x10, uintptr_t(0)); assert(alias_allowed());
    alias_names[0][0x18] = 'X'; assert(!alias_allowed()); alias_names[0][0x18] = 'R';
    put(alias_types[2], 0x18, address(alias_types[5])); assert(!alias_allowed());
    put(alias_types[2], 0x18, address(alias_types[3]));
    put(alias_types[5], 0x18, address(alias_types[3])); assert(!alias_allowed());
    put(alias_types[5], 0x18, uintptr_t(0)); assert(alias_allowed());
    put(collection, 0, uintptr_t(1)); assert(!alias_allowed());
    put(collection, 0, image + 0x2487E30);
    store(image + 0x2487E30 + 0x18, 1); assert(!alias_allowed());
    store(image + 0x2487E30 + 0x18, image + 0x404370);
    script_handle = 0; assert(!alias_allowed()); script_handle = 123;
    expected_vm_type = 64;
    bound_script = false;
    script_handle = 0; assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    script_handle = 0xFFFF00000000ull; assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    script_handle = UINT64_MAX; assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    script_handle = 123; store(vm, 1);
    assert(!fo4hud_reference_scripts_allow_auto_loot(image, address(reference)));
    assert(munmap(memory, 0x425A000) == 0);
}
