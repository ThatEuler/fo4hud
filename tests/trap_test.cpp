#include "../game_entities.cpp"
#include "../trap_overlay.h"
#include <sys/mman.h>
#include <cassert>

namespace {
template<class T> void put(void* p, size_t off, T v) { std::memcpy(static_cast<char*>(p) + off, &v, sizeof(v)); }
uintptr_t addr(void* p) { return reinterpret_cast<uintptr_t>(p); }
struct Text {
    char bytes[160]{};
    explicit Text(const char* s) {
        put(bytes, 0x10, uint32_t(std::strlen(s))); std::strcpy(bytes + 0x18, s);
    }
    uintptr_t pointer() { return addr(bytes); }
};
uintptr_t script_object = 0;
unsigned visits = 0;
uint64_t __attribute__((ms_abi)) handle_for(void*, uint32_t type, void*) { assert(type == 64); return 0x1234; }
void __attribute__((ms_abi)) for_each(void*, uint64_t handle, void* function) {
    assert(handle == 0x1234); ++visits;
    auto* visitor = *reinterpret_cast<TrapVisitor**>(static_cast<char*>(function) + 56);
    using Invoke = int32_t (__attribute__((ms_abi)) *)(TrapVisitor*, uintptr_t*);
    assert(reinterpret_cast<Invoke>(visitor->vtable[2])(visitor, &script_object) == 1);
}
void trampoline(unsigned char* code, uintptr_t fn) {
    code[0] = 0x48; code[1] = 0xB8; put(code, 2, fn); code[10] = 0xFF; code[11] = 0xE0;
}
}
int main() {
    auto* image = static_cast<unsigned char*>(mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED); const uintptr_t image_base = addr(image);
    alignas(8) char reference[0x300]{}, base[0x180]{}, cell[0x100]{}, player[0x200]{};
    Fo4Entity entity; entity.address = addr(reference); entity.form_id = 123; entity.form_type = 64;
    entity.cell_address = addr(cell); entity.cell_is_interior = true;
    put(reference, 0x14, entity.form_id); put(reference, 0x1A, entity.form_type);
    put(reference, 0xB8, addr(cell)); put(reference, 0xE0, addr(base)); put(base, 0x1A, uint8_t(27));
    put(player, 0xB8, addr(cell)); put(player, 0x14, uint32_t(0x14)); put(player, 0x1A, uint8_t(65));
    put(cell, 0x1A, uint8_t(63)); put(cell, 0x40, uint16_t(1));
    // Real bound-object visitor ABI; inherited variables precede child slots.
    alignas(8) char object[0x80]{}, child[0x60]{}, parent[0x60]{}, child_vars[24]{}, parent_vars[24]{};
    Text spent("DoNothing"), waiting("Waiting"), oil_name("TrapExplOil"),
         child_name("TrapTripwire"), parent_name("TrapBase"), armed_name("isArmed"), arm_name("ArmState"),
         active("Active"), idle("Idle"), disarmed("Disarmed"), empty(""), unrelated("WorkshopObjectScript");
    put(object, 0, uint32_t(1)); put(object, 8, addr(child)); put(object, 0x10, active.pointer());
    put(child, 0x10, child_name.pointer()); put(child, 0x18, addr(parent)); put(child, 0x40, uint32_t(3 | (1 << 8)));
    put(child, 0x50, addr(child_vars)); put(child_vars, 0, armed_name.pointer());
    put(parent, 0x10, parent_name.pointer()); put(parent, 0x40, uint32_t(3 | (1 << 8)));
    put(parent, 0x50, addr(parent_vars)); put(parent_vars, 0, arm_name.pointer());
    put(object, 0x30, uint64_t(2)); put(object, 0x38, idle.pointer());
    put(object, 0x40, uint64_t(5)); put(object, 0x48, uint64_t(1));
    auto found = inspect_trap_script(addr(object));
    assert(found.kind == Fo4TrapKind::Wire && found.activity == Fo4TrapActivity::Armed);
    put(object, 0x48, uint64_t(0)); assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Inactive);
    put(object, 0x48, uint64_t(1)); put(object, 0x10, disarmed.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Inactive);
    put(object, 0x10, empty.pointer()); put(object, 0x38, empty.pointer()); put(object, 0x40, uint64_t(4));
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Unknown);
    put(child, 0x10, oil_name.pointer()); put(object, 0x10, waiting.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Armed);
    put(object, 0x10, spent.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Inactive);
    put(child, 0x10, child_name.pointer());
    put(object, 0x10, active.pointer()); put(object, 0x40, uint64_t(5));
    put(parent, 0x18, addr(child)); assert(inspect_trap_script(addr(object)).kind == Fo4TrapKind::None);
    put(parent, 0x18, uintptr_t(0));
    put(child, 0x10, unrelated.pointer()); put(child, 0x18, uintptr_t(0));
    assert(inspect_trap_script(addr(object)).kind == Fo4TrapKind::None);
    put(child, 0x10, child_name.pointer()); put(child, 0x18, addr(parent));

    // Vanilla TrapBreakableWalkway.pex calls GoToState("Done") once
    // BoardCount <= 0. It inherits ObjectReference, not TrapBase, and has
    // no isArmed or ArmState variables. A reset returns it to Active.
    Text walkway_name("TrapBreakableWalkway"), done("Done"), custom_name("CustomWalkway");
    put(child, 0x10, walkway_name.pointer()); put(child, 0x18, uintptr_t(0));
    put(child, 0x40, uint32_t(3));
    put(object, 0x10, active.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Armed);
    put(object, 0x10, done.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Inactive);
    // Custom subclasses inherit the same terminal-state semantics.
    put(parent, 0x10, walkway_name.pointer()); put(parent, 0x40, uint32_t(3));
    put(child, 0x10, custom_name.pointer()); put(child, 0x18, addr(parent));
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Inactive);
    put(object, 0x10, active.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Armed);
    // Unverified states on other traps remain visible as uncertain hazards.
    put(child, 0x10, child_name.pointer()); put(child, 0x18, uintptr_t(0));
    put(object, 0x10, done.pointer());
    assert(inspect_trap_script(addr(object)).activity == Fo4TrapActivity::Unknown);
    put(object, 0x10, active.pointer());
    put(child, 0x18, addr(parent)); put(child, 0x40, uint32_t(3 | (1 << 8)));
    put(parent, 0x10, parent_name.pointer()); put(parent, 0x40, uint32_t(3 | (1 << 8)));

    alignas(8) char game_vm[0xB8]{}, vm[0x100]{}, policy[8]{};
    uintptr_t policy_vtable[8]{}; policy_vtable[7] = image_base + 0x1000;
    put(image, 0x30EB308, addr(game_vm)); put(game_vm, 0xB0, addr(vm)); put(vm, 0, image_base + 0x28FB520);
    put(image, 0x28FB520 + 0xF0, image_base + 0x211A4A0); put(vm, 0x90, addr(policy)); put(policy, 0, addr(policy_vtable));
    trampoline(image + 0x1000, reinterpret_cast<uintptr_t>(handle_for));
    trampoline(image + 0x211A4A0, reinterpret_cast<uintptr_t>(for_each)); script_object = addr(object);
    found = sample_trap(image_base, entity);
    assert(found.kind == Fo4TrapKind::Wire && found.activity == Fo4TrapActivity::Armed && visits == 1);
    for (uint32_t flag : {0x20u, 0x800u, 0x800000u}) {
        put(reference, 0x10, flag); assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None); assert(visits == 1);
    }
    put(reference, 0x10, uint32_t(0));
    put(reference, 0x14, uint32_t(999)); assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    put(reference, 0x14, entity.form_id);
    put(vm, 0, uintptr_t(1)); assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    put(vm, 0, image_base + 0x28FB520);

    // A deployed mine is a GrenadeProjectile with a live proximity trigger.
    entity.form_type = 68; put(reference, 0x1A, uint8_t(68)); put(reference, 0, image_base + 0x257DB78);
    put(base, 0x1A, uint8_t(51)); put(base, 0xC0, uint32_t(4)); put(base, 0xE0, 120.0f);
    assert(sample_trap(image_base, entity).kind == Fo4TrapKind::Mine);
    put(base, 0x1A, uint8_t(50)); // NOTE is not a projectile record.
    assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    put(base, 0x1A, uint8_t(51)); // PROJ in Fallout 4 1.11.240.
    // Captured from the live bridge mine (PROJ 0xE56C3): bit 2 is set
    // while the mine is still armed, along with other resting-projectile bits.
    put(reference, 0x260, uint64_t(0x41C104));
    assert(sample_trap(image_base, entity).kind == Fo4TrapKind::Mine);
    for (uint64_t flag : {0x200u, 0x2000000u}) {
        put(reference, 0x260, uint64_t(0x41C104 | flag));
        assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    }
    put(reference, 0x260, uint64_t(0));
    put(base, 0xC0, uint32_t(2)); assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None); // Ordinary grenade.
    put(base, 0xC0, uint32_t(4)); put(base, 0xE0, 0.0f); assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    put(base, 0xE0, 120.0f);
    entity.form_type = 64; put(reference, 0x1A, uint8_t(64)); put(base, 0x1A, uint8_t(43));
    assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None); // Loose mine weapon is loot.
    put(base, 0x1A, uint8_t(27));
    // Physical vanilla monkeys have no scripts; the invisible trigger is a
    // different reference. Destroyed monkeys stay visible as sprung, then
    // return to active if the game resets their destruction state.
    for (uint32_t monkey_id : {0x142F88u, 0x14BED3u}) {
        put(base, 0x14, monkey_id);
        put(reference, 0x10, uint32_t(0));
        found = sample_trap(image_base, entity);
        assert(found.kind == Fo4TrapKind::Monkey && found.activity == Fo4TrapActivity::Armed);
        put(reference, 0x10, uint32_t(0x800000));
        found = sample_trap(image_base, entity);
        assert(found.kind == Fo4TrapKind::Monkey && found.activity == Fo4TrapActivity::Sprung);
        float position[3]{};
        assert(!fo4hud_trap_reference_live(entity, position));
        assert(fo4hud_trap_reference_live(entity, position, true));
        for (uint32_t flag : {0x20u, 0x800u}) {
            put(reference, 0x10, flag | 0x800000);
            assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
        }
        put(reference, 0x10, uint32_t(0));
        assert(sample_trap(image_base, entity).activity == Fo4TrapActivity::Armed);
    }
    for (uint32_t trigger_id : {0x1796A6u, 0x21C928u}) {
        put(base, 0x14, trigger_id);
        assert(sample_trap(image_base, entity).kind == Fo4TrapKind::None);
    }
    assert(trap_script_kind("trapcanchimes") == Fo4TrapKind::Alarm);
    assert(trap_script_kind("trapmonkeytriggerscript") == Fo4TrapKind::Monkey);
    assert(trap_script_kind("dn160_trapmonkeytriggerscript") == Fo4TrapKind::Monkey);
    assert(trap_script_kind("trapmonkeyscript") == Fo4TrapKind::Monkey);
    assert(trap_script_kind("dn160_encountermonkeyscript") == Fo4TrapKind::Monkey);
    assert(std::strcmp(fo4hud_trap_label(Fo4TrapKind::Monkey), "MONKEY TRAP") == 0);
    assert(std::strcmp(fo4hud_trap_status(Fo4TrapActivity::Sprung), " - SPRUNG") == 0);
    put(base, 0x14, uint32_t(0));
    Fo4EntitySnapshot snapshot; snapshot.status = "Ready"; snapshot.player_address = addr(player);
    snapshot.cell_address = addr(cell); snapshot.cell_is_interior = true; snapshot.trap_candidates = {entity};
    fo4hud_submit_trap_candidates(snapshot, true); fo4hud_traps_tick(image_base, player);
    assert(fo4hud_read_trap_markers().size() == 1);
    fo4hud_submit_trap_candidates(snapshot, false); assert(fo4hud_read_trap_markers().empty());

    // Render a live warning and ensure it does not capture dialogue input.
    put(image, kProjectionReadyRva, uint8_t(1));
    float matrix[4][4]{}; matrix[0][0] = 0.001f; matrix[1][1] = 0.001f; matrix[2][2] = 0.001f; matrix[3][3] = 1;
    std::memcpy(image + kProjectionMatrixRva, matrix, sizeof(matrix));
    const float port[] = {0, 1, 1, 0}; std::memcpy(image + kProjectionPortRva, port, sizeof(port));
    snapshot.image_address = image_base;
    Fo4TrapMarker marker{entity, Fo4TrapKind::Wire, Fo4TrapActivity::Armed};
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    unsigned char* pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    for (ImVec2 display : {ImVec2(640, 480), ImVec2(1920, 1080)}) {
        io.DisplaySize = display; io.DeltaTime = 1.0f / 60;
        ImGui::NewFrame(); fo4hud_draw_trap_overlay(snapshot, {marker}, display, 2000); ImGui::Render();
        assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        assert(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    marker.kind = Fo4TrapKind::Monkey; marker.activity = Fo4TrapActivity::Sprung;
    put(reference, 0x10, uint32_t(0x800000));
    ImGui::NewFrame(); fo4hud_draw_trap_overlay(snapshot, {marker}, io.DisplaySize, 2000); ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount > 0);
    assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
    // A disabled sprung monkey must disappear rather than linger as a marker.
    put(reference, 0x10, uint32_t(0x800800));
    ImGui::NewFrame(); fo4hud_draw_trap_overlay(snapshot, {marker}, io.DisplaySize, 2000); ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount == 0);
    ImGui::DestroyContext(); munmap(image, kImageSize);
}
