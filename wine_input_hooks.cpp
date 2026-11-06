#include "wine_input_hooks.h"
#include "logging.h"
#include "protocols/relative-pointer-client-protocol.h"
#include <wayland-client.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <initializer_list>
#include <vector>
#include <linux/input-event-codes.h>

namespace {
using KeyFn = void (*)(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t);
using ButtonFn = void (*)(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
using MotionFn = void (*)(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t);
using FrameFn = void (*)(void*, wl_pointer*);
using AxisFn = void (*)(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t);
using RelativeFn = void (*)(void*, zwp_relative_pointer_v1*, uint32_t, uint32_t,
                            wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t);

std::mutex hook_mutex;
std::atomic<bool> capture_mouse{false}, capture_keyboard{false}, installed{false};
std::atomic<bool> aim_enabled{false}, aim_button_down{false};
std::atomic<float> aim_strength{0.0f};
std::atomic<uint64_t> aim_error{UINT64_MAX};
std::atomic<int64_t> aim_updated_ns{0};
std::atomic<int64_t> last_real_relative_ns{0};
std::atomic<wl_display*> aim_display{nullptr};
std::atomic<wl_event_queue*> aim_queue{nullptr};
std::atomic<bool> aim_pulse_pending{false};
thread_local bool synthetic_relative = false;
using EnterFn = void (*)(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
using LeaveFn = void (*)(void*, wl_keyboard*, uint32_t, wl_surface*);
EnterFn original_enter = nullptr;
LeaveFn original_leave = nullptr;
KeyFn original_key = nullptr;
ButtonFn original_button = nullptr;
MotionFn original_motion = nullptr;
FrameFn original_frame = nullptr;
AxisFn original_axis = nullptr;
RelativeFn original_relative = nullptr;
bool consumed_keys[256]{};
bool consumed_buttons[3]{};
struct Patch { void** slot; void* original; };
std::vector<Patch> patches;

std::mutex control_mutex;
std::atomic<void (*)()> control_cancel_callback{nullptr};
wl_keyboard* control_keyboard = nullptr;
void* control_keyboard_data = nullptr;
wl_pointer* control_pointer = nullptr;
void* control_pointer_data = nullptr;
wl_display* control_display = nullptr;
wl_event_queue* control_queue = nullptr;
bool physical_keys[512]{}, control_sent[512]{}, control_desired[512]{};
std::chrono::steady_clock::time_point control_until{};
float control_look_x = 0, control_look_y = 0;
bool control_pending = false, control_start_pending = false, control_focused = false;
int control_duration_ms = 0;
std::chrono::steady_clock::time_point control_dispatch_deadline{};
uintptr_t control_epoch = 1;
uint32_t control_serial = 0;

bool control_any_keys() {
    for (size_t i=0;i<512;++i) if (control_sent[i] || control_desired[i]) return true;
    return false;
}
void control_real_key(void* data, wl_keyboard* keyboard, uint32_t serial,
                      uint32_t key, uint32_t state) {
    bool override = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        if (keyboard) {
            control_focused=true;
            control_keyboard=keyboard;control_keyboard_data=data;control_serial=serial;
            control_display=wl_proxy_get_display(reinterpret_cast<wl_proxy*>(keyboard));
            control_queue=wl_proxy_get_queue(reinterpret_cast<wl_proxy*>(keyboard));
        }
        if (key<512) physical_keys[key]=state==WL_KEYBOARD_KEY_STATE_PRESSED;
        override=state==WL_KEYBOARD_KEY_STATE_PRESSED && control_any_keys();
    }
    if (override) {
        if (auto callback=control_cancel_callback.load()) callback();
        else fo4hud_wine_control_cancel();
    }
}

void keyboard_enter(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface, wl_array* keys) {
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        control_focused=true;control_keyboard=keyboard;control_keyboard_data=data;control_serial=serial;
        control_display=wl_proxy_get_display(reinterpret_cast<wl_proxy*>(keyboard));
        control_queue=wl_proxy_get_queue(reinterpret_cast<wl_proxy*>(keyboard));
        std::fill(std::begin(physical_keys),std::end(physical_keys),false);
        if(keys)for(size_t i=0;i<keys->size/sizeof(uint32_t);++i){auto key=static_cast<uint32_t*>(keys->data)[i];if(key<512)physical_keys[key]=true;}
    }
    if(original_enter)original_enter(data,keyboard,serial,surface,keys);
}
void keyboard_leave(void* data, wl_keyboard* keyboard, uint32_t serial, wl_surface* surface) {
    if(auto callback=control_cancel_callback.load())callback();else fo4hud_wine_control_cancel();
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        control_focused=false;
        // Wine releases its physical/game key state on leave. Do not replay
        // obsolete synthetic releases if the window is focused again later.
        std::fill(std::begin(control_sent),std::end(control_sent),false);
        std::fill(std::begin(physical_keys),std::end(physical_keys),false);
    }
    if(original_leave)original_leave(data,keyboard,serial,surface);
}

void keyboard_key(void* data, wl_keyboard* keyboard, uint32_t serial, uint32_t time,
                  uint32_t key, uint32_t state) {
    control_real_key(data, keyboard, serial, key, state);
    bool consume = capture_keyboard.load(std::memory_order_relaxed);
    if (key < 256) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) consumed_keys[key] = consume;
        else { consume = consumed_keys[key]; consumed_keys[key] = false; }
    }
    if (!consume && original_key) original_key(data, keyboard, serial, time, key, state);
}
void pointer_button(void* data, wl_pointer* pointer, uint32_t serial, uint32_t time,
                    uint32_t button, uint32_t state) {
    bool override = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        control_pointer=pointer;control_pointer_data=data;
        if (button<512) physical_keys[button]=state==WL_POINTER_BUTTON_STATE_PRESSED;
        override=state==WL_POINTER_BUTTON_STATE_PRESSED && control_any_keys();
    }
    if (override) {
        if (auto callback=control_cancel_callback.load()) callback();
        else fo4hud_wine_control_cancel();
    }
    int index = button == BTN_LEFT ? 0 : button == BTN_RIGHT ? 1 : button == BTN_MIDDLE ? 2 : -1;
    bool consume = capture_mouse.load(std::memory_order_relaxed);
    if (index >= 0) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) consumed_buttons[index] = consume;
        else { consume = consumed_buttons[index]; consumed_buttons[index] = false; }
    }
    if (button == BTN_RIGHT) {
        aim_button_down.store(!consume && state == WL_POINTER_BUTTON_STATE_PRESSED,
                              std::memory_order_relaxed);
        if (fo4hud_aim_trace_enabled())
            fo4hud_log("aim", "button right state=%u consumed=%d capture_mouse=%d "
                       "aim_enabled=%d effective_down=%d", state, int(consume),
                       int(capture_mouse.load(std::memory_order_relaxed)),
                       int(aim_enabled.load(std::memory_order_relaxed)),
                       int(aim_button_down.load(std::memory_order_relaxed)));
    }
    if (!consume && original_button) original_button(data, pointer, serial, time, button, state);
}
void pointer_motion(void* data, wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    if (!capture_mouse.load(std::memory_order_relaxed) && original_motion)
        original_motion(data, pointer, time, x, y);
}
void pointer_axis(void* data, wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    if (!capture_mouse.load(std::memory_order_relaxed) && original_axis)
        original_axis(data, pointer, time, axis, value);
}
void relative_motion(void* data, zwp_relative_pointer_v1* pointer, uint32_t hi, uint32_t lo,
                     wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dxu, wl_fixed_t dyu) {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!synthetic_relative) {
        last_real_relative_ns.store(now_ns, std::memory_order_release);
        if (pointer) {
            auto* proxy = reinterpret_cast<wl_proxy*>(pointer);
            aim_display.store(wl_proxy_get_display(proxy), std::memory_order_release);
            aim_queue.store(wl_proxy_get_queue(proxy), std::memory_order_release);
        }
    }
    static thread_local int64_t last_trace_ns = 0;
    static thread_local uint32_t received_since_trace = 0, applied_since_trace = 0;
    static thread_local uint32_t synthetic_since_trace = 0;
    ++received_since_trace;
    if (synthetic_relative) ++synthetic_since_trace;
    const bool captured = capture_mouse.load(std::memory_order_relaxed);
    const bool enabled = aim_enabled.load(std::memory_order_relaxed);
    const bool button = aim_button_down.load(std::memory_order_relaxed);
    const bool hooked = installed.load(std::memory_order_relaxed);
    const uint64_t packed = aim_error.load(std::memory_order_acquire);
    const int64_t updated_ns = aim_updated_ns.load(std::memory_order_acquire);
    const int64_t age_ns = updated_ns > 0 ? now_ns - updated_ns : -1;
    const bool trace_now = fo4hud_aim_trace_enabled() && (enabled || button) &&
        now_ns - last_trace_ns >= 250000000;
    const wl_fixed_t input_dx = dx, input_dy = dy, input_dxu = dxu, input_dyu = dyu;
    if (captured || !original_relative) {
        if (trace_now) {
            fo4hud_log("aim", "relative skipped capture=%d callback=%d enabled=%d rmb=%d "
                       "events=%u synthetic=%u", int(captured), int(original_relative != nullptr),
                       int(enabled), int(button), received_since_trace, synthetic_since_trace);
            last_trace_ns = now_ns;
            received_since_trace = applied_since_trace = 0;
            synthetic_since_trace = 0;
        }
        return;
    }
    const bool fresh = packed != UINT64_MAX && age_ns >= 0 && age_ns < 100000000;
    float assist_x = 0.0f, assist_y = 0.0f;
    if (hooked && enabled && button && fresh) {
        const float strength = aim_strength.load(std::memory_order_relaxed);
        const int32_t error_x = static_cast<int32_t>(packed);
        const int32_t error_y = static_cast<int32_t>(packed >> 32);
        // A small bounded bias lets the player steer away and prevents a
        // single mouse event from jumping across the target.
        const float limit = 6.0f * strength;
        assist_x = std::abs(error_x) < 4 ? 0.0f :
            std::clamp(error_x * 0.075f * strength, -limit, limit);
        assist_y = std::abs(error_y) < 4 ? 0.0f :
            std::clamp(error_y * 0.075f * strength, -limit, limit);
        const wl_fixed_t add_x = wl_fixed_from_double(assist_x);
        const wl_fixed_t add_y = wl_fixed_from_double(assist_y);
        auto add_fixed = [](wl_fixed_t value, wl_fixed_t addition) {
            return static_cast<wl_fixed_t>(std::clamp<int64_t>(
                int64_t(value) + addition, INT32_MIN, INT32_MAX));
        };
        dx = add_fixed(dx, add_x);
        dy = add_fixed(dy, add_y);
        dxu = add_fixed(dxu, add_x);
        dyu = add_fixed(dyu, add_y);
        ++applied_since_trace;
    }
    if (trace_now) {
        const int32_t error_x = packed == UINT64_MAX ? 0 : static_cast<int32_t>(packed);
        const int32_t error_y = packed == UINT64_MAX ? 0 : static_cast<int32_t>(packed >> 32);
        fo4hud_log("aim", "relative hook=%d enabled=%d rmb=%d fresh=%d age_ms=%.1f "
                   "events=%u synthetic=%u applied=%u error_px=(%d,%d) strength=%.2f "
                   "input=(%.2f,%.2f) input_unaccel=(%.2f,%.2f) "
                   "bias=(%.2f,%.2f) output=(%.2f,%.2f) output_unaccel=(%.2f,%.2f)",
                   int(hooked), int(enabled), int(button), int(fresh),
                   age_ns < 0 ? -1.0 : double(age_ns) / 1000000.0,
                   received_since_trace, synthetic_since_trace, applied_since_trace, error_x, error_y,
                   aim_strength.load(std::memory_order_relaxed),
                   wl_fixed_to_double(input_dx), wl_fixed_to_double(input_dy),
                   wl_fixed_to_double(input_dxu), wl_fixed_to_double(input_dyu),
                   assist_x, assist_y, wl_fixed_to_double(dx), wl_fixed_to_double(dy),
                   wl_fixed_to_double(dxu), wl_fixed_to_double(dyu));
        last_trace_ns = now_ns;
        received_since_trace = applied_since_trace = 0;
        synthetic_since_trace = 0;
    }
    original_relative(data, pointer, hi, lo, dx, dy, dxu, dyu);
}

void aim_pulse_done(void*, wl_callback* callback, uint32_t) {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last_real = last_real_relative_ns.load(std::memory_order_acquire);
    const int64_t updated = aim_updated_ns.load(std::memory_order_acquire);
    if (installed.load(std::memory_order_acquire) && original_relative && original_frame &&
        aim_enabled.load(std::memory_order_acquire) &&
        aim_button_down.load(std::memory_order_acquire) &&
        !capture_mouse.load(std::memory_order_acquire) &&
        aim_error.load(std::memory_order_acquire) != UINT64_MAX &&
        now_ns - updated < 100000000 && now_ns - last_real >= 25000000) {
        // This callback runs on Wine's Wayland event queue. Its native
        // relative-motion handler accumulates delta; frame delivers it to
        // Fallout through the same path as a physical mouse movement.
        synthetic_relative = true;
        relative_motion(nullptr, nullptr, 0, 0, 0, 0, 0, 0);
        synthetic_relative = false;
        original_frame(nullptr, nullptr);
    }
    wl_callback_destroy(callback);
    aim_pulse_pending.store(false, std::memory_order_release);
}
const wl_callback_listener aim_pulse_listener{aim_pulse_done};

// Requires control_mutex. Kept separate from the Wayland callback so timed
// presses, cancellations, and physical-key ownership can be tested directly.
void dispatch_control_input(std::chrono::steady_clock::time_point now) {
    if(!installed.load(std::memory_order_acquire)||!control_focused)return;
    if(control_start_pending){
        if(now>control_dispatch_deadline){std::fill(std::begin(control_desired),std::end(control_desired),false);control_look_x=control_look_y=0;}
        else control_until=now+std::chrono::milliseconds(control_duration_ms);
        control_start_pending=false;
    }
    if(now>=control_until)std::fill(std::begin(control_desired),std::end(control_desired),false);
    const uint32_t time=uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    for(uint32_t key=0;key<512;++key){
        if(control_sent[key]==control_desired[key])continue;
        const bool pressed=control_desired[key];
        if(!physical_keys[key]){
            if(key<256&&original_key)original_key(control_keyboard_data,control_keyboard,control_serial,time,key,pressed);
            else if(key>=256&&original_button)original_button(control_pointer_data,control_pointer,control_serial,time,key,pressed);
        }
        control_sent[key]=pressed;
    }
    if((control_look_x!=0||control_look_y!=0)&&original_relative)
        original_relative(nullptr,nullptr,0,0,wl_fixed_from_double(control_look_x),wl_fixed_from_double(control_look_y),
                          wl_fixed_from_double(control_look_x),wl_fixed_from_double(control_look_y));
    control_look_x=control_look_y=0;
    if(original_frame)original_frame(control_pointer_data,control_pointer);
}
void control_pulse_done(void* data, wl_callback* callback, uint32_t) {
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        if(reinterpret_cast<uintptr_t>(data)==control_epoch){
            dispatch_control_input(std::chrono::steady_clock::now());control_pending=false;
        }
    }
    wl_callback_destroy(callback);
}
const wl_callback_listener control_listener{control_pulse_done};

struct Module { const char* path=nullptr; uintptr_t base=0; };
int find_module(dl_phdr_info* info, size_t, void* userdata) {
    if (info->dlpi_name && std::strstr(info->dlpi_name, "/winewayland.so")) {
        auto* module = static_cast<Module*>(userdata);
        module->path = info->dlpi_name;
        module->base = info->dlpi_addr;
        return 1;
    }
    return 0;
}
struct Symbols { void** keyboard=nullptr; void** pointer=nullptr; void** relative=nullptr; };
bool find_symbols(const Module& module, Symbols& out) {
    FILE* file = std::fopen(module.path, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::rewind(file);
    if (size < long(sizeof(Elf64_Ehdr))) { std::fclose(file); return false; }
    std::vector<unsigned char> bytes(size);
    bool read_ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!read_ok) return false;
    const auto* hdr = reinterpret_cast<const Elf64_Ehdr*>(bytes.data());
    if (std::memcmp(hdr->e_ident, ELFMAG, SELFMAG) || hdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        hdr->e_shentsize != sizeof(Elf64_Shdr) ||
        hdr->e_shoff > bytes.size() ||
        uint64_t(hdr->e_shnum) * sizeof(Elf64_Shdr) > bytes.size() - hdr->e_shoff) return false;
    const auto* sections = reinterpret_cast<const Elf64_Shdr*>(bytes.data()+hdr->e_shoff);
    for (uint16_t i=0; i<hdr->e_shnum; ++i) {
        const auto& section = sections[i];
        if (section.sh_type != SHT_SYMTAB || section.sh_link >= hdr->e_shnum ||
            section.sh_entsize != sizeof(Elf64_Sym) || section.sh_offset > bytes.size() ||
            section.sh_size > bytes.size()-section.sh_offset) continue;
        const auto& names = sections[section.sh_link];
        if (names.sh_offset > bytes.size() || names.sh_size > bytes.size()-names.sh_offset) continue;
        const char* strings = reinterpret_cast<const char*>(bytes.data()+names.sh_offset);
        const auto* syms = reinterpret_cast<const Elf64_Sym*>(bytes.data()+section.sh_offset);
        for (size_t j=0; j<section.sh_size/sizeof(Elf64_Sym); ++j) {
            const auto& sym = syms[j];
            if (sym.st_name >= names.sh_size || ELF64_ST_TYPE(sym.st_info) != STT_OBJECT) continue;
            const char* name = strings+sym.st_name;
            if (!std::strcmp(name, "keyboard_listener") && sym.st_size == 6*sizeof(void*))
                out.keyboard = reinterpret_cast<void**>(module.base+sym.st_value);
            else if (!std::strcmp(name, "pointer_listener") && sym.st_size >= 9*sizeof(void*))
                out.pointer = reinterpret_cast<void**>(module.base+sym.st_value);
            else if (!std::strcmp(name, "relative_pointer_v1_listener") && sym.st_size == sizeof(void*))
                out.relative = reinterpret_cast<void**>(module.base+sym.st_value);
        }
    }
    return out.keyboard && out.pointer && out.relative;
}
int page_protection(void* address) {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], perms[5];
    uintptr_t start=0, end=0, target=reinterpret_cast<uintptr_t>(address);
    int result=0;
    while (std::fgets(line, sizeof(line), maps)) {
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (target >= start && target < end) {
            if (perms[0]=='r') result |= PROT_READ;
            if (perms[1]=='w') result |= PROT_WRITE;
            if (perms[2]=='x') result |= PROT_EXEC;
            break;
        }
    }
    std::fclose(maps);
    return result;
}
bool patch(void** slot, void* replacement, void** original) {
    if (!slot || !replacement || !original) return false;
    int protection=page_protection(slot);
    if (!(protection & PROT_READ)) return false;
    long page_size=sysconf(_SC_PAGESIZE);
    auto page=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~(uintptr_t(page_size)-1));
    if (mprotect(page, page_size, protection | PROT_WRITE) != 0) return false;
    *original = __atomic_exchange_n(slot, replacement, __ATOMIC_SEQ_CST);
    mprotect(page, page_size, protection);
    patches.push_back({slot,*original});
    return true;
}
void restore_patches() {
    for (auto it=patches.rbegin(); it!=patches.rend(); ++it) {
        int protection=page_protection(it->slot);
        long page_size=sysconf(_SC_PAGESIZE);
        auto page=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(it->slot) & ~(uintptr_t(page_size)-1));
        if (mprotect(page, page_size, protection | PROT_WRITE) == 0) {
            __atomic_store_n(it->slot, it->original, __ATOMIC_SEQ_CST);
            mprotect(page, page_size, protection);
        }
    }
    patches.clear();
}
}

bool fo4hud_wine_hooks_install() {
    std::lock_guard<std::mutex> lock(hook_mutex);
    if (installed.load()) return true;
    Module module;
    dl_iterate_phdr(find_module, &module);
    if (!module.path) {
        if (fo4hud_aim_trace_enabled()) fo4hud_log("aim", "hook_install winewayland_not_loaded");
        return false;
    }
    Symbols symbols;
    if (!find_symbols(module, symbols)) {
        fo4hud_log("wine-input", "fo4hud: Wine Wayland listener symbols unavailable; input capture disabled");
        return false;
    }
    for (void** slot : {symbols.keyboard+1, symbols.keyboard+2, symbols.keyboard+3, symbols.pointer+2,
                        symbols.pointer+3, symbols.pointer+4, symbols.pointer+5,
                        symbols.relative}) {
        Dl_info callback{};
        if (!*slot || !dladdr(*slot, &callback) || !callback.dli_fname ||
            !std::strstr(callback.dli_fname, "/winewayland.so")) {
            fo4hud_log("wine-input", "fo4hud: Wine Wayland listener layout unexpected; input capture disabled");
            return false;
        }
    }
    original_frame = reinterpret_cast<FrameFn>(symbols.pointer[5]);
    void* original=nullptr;
    bool okay=true;
    okay &= patch(symbols.keyboard+1, reinterpret_cast<void*>(keyboard_enter), &original);
    original_enter=reinterpret_cast<EnterFn>(original);
    if (okay) { okay &= patch(symbols.keyboard+2, reinterpret_cast<void*>(keyboard_leave), &original); original_leave=reinterpret_cast<LeaveFn>(original); }
    if (okay) okay &= patch(symbols.keyboard+3, reinterpret_cast<void*>(keyboard_key), &original);
    original_key=reinterpret_cast<KeyFn>(original);
    if (okay) { okay &= patch(symbols.pointer+3, reinterpret_cast<void*>(pointer_button), &original); original_button=reinterpret_cast<ButtonFn>(original); }
    if (okay) { okay &= patch(symbols.pointer+2, reinterpret_cast<void*>(pointer_motion), &original); original_motion=reinterpret_cast<MotionFn>(original); }
    if (okay) { okay &= patch(symbols.pointer+4, reinterpret_cast<void*>(pointer_axis), &original); original_axis=reinterpret_cast<AxisFn>(original); }
    if (okay) { okay &= patch(symbols.relative, reinterpret_cast<void*>(relative_motion), &original); original_relative=reinterpret_cast<RelativeFn>(original); }
    if (!okay) { restore_patches(); return false; }
    installed=true;
    if (fo4hud_aim_trace_enabled()) fo4hud_log("aim", "hook_install ready module=%s", module.path);
    fo4hud_log("wine-input", "fo4hud: Wine Wayland input capture initialized");
    return true;
}
void fo4hud_wine_hooks_uninstall() {
    std::lock_guard<std::mutex> lock(hook_mutex);
    installed=false;
    {
        std::lock_guard<std::mutex> control_lock(control_mutex);
        ++control_epoch;control_pending=false;control_start_pending=false;control_focused=false;
        control_keyboard=nullptr;control_pointer=nullptr;control_display=nullptr;control_queue=nullptr;
        std::fill(std::begin(control_sent),std::end(control_sent),false);
        std::fill(std::begin(control_desired),std::end(control_desired),false);
        std::fill(std::begin(physical_keys),std::end(physical_keys),false);
        control_look_x=control_look_y=0;
    }
    capture_mouse=false; capture_keyboard=false;
    aim_enabled=false; aim_button_down=false; aim_error=UINT64_MAX;
    aim_display.store(nullptr, std::memory_order_release);
    aim_queue.store(nullptr, std::memory_order_release);
    aim_pulse_pending.store(false, std::memory_order_release);
    restore_patches();
}
void fo4hud_wine_hooks_set_capture(bool mouse, bool keyboard) {
    bool old_mouse=capture_mouse.exchange(mouse, std::memory_order_relaxed);
    bool old_keyboard=capture_keyboard.exchange(keyboard, std::memory_order_relaxed);
    if (mouse) aim_button_down.store(false, std::memory_order_relaxed);
    if (installed.load() && std::getenv("FO4HUD_TRACE") &&
        (old_mouse != mouse || old_keyboard != keyboard))
        fo4hud_log("wine-input", "fo4hud: Wine capture mouse=%d keyboard=%d", int(mouse), int(keyboard));
}
bool fo4hud_wine_hooks_active() { return installed.load(); }
void fo4hud_wine_hooks_set_aim_assist(bool enabled, float strength) {
    const float clamped = std::clamp(strength, 0.0f, 1.0f);
    const float previous_strength = aim_strength.exchange(clamped, std::memory_order_relaxed);
    const bool previous_enabled = aim_enabled.exchange(enabled, std::memory_order_release);
    if (!enabled) aim_error.store(UINT64_MAX, std::memory_order_release);
    if (fo4hud_aim_trace_enabled() &&
        (enabled != previous_enabled || std::abs(clamped - previous_strength) >= 0.01f))
        fo4hud_log("aim", "configuration enabled=%d strength=%.2f", int(enabled), clamped);
}
void fo4hud_wine_hooks_set_aim_target(float error_x, float error_y, bool valid) {
    if (!valid || !std::isfinite(error_x) || !std::isfinite(error_y)) {
        const uint64_t previous = aim_error.exchange(UINT64_MAX, std::memory_order_release);
        if (fo4hud_aim_trace_enabled() && previous != UINT64_MAX)
            fo4hud_log("aim", "target cleared valid=%d finite=%d", int(valid),
                       int(std::isfinite(error_x) && std::isfinite(error_y)));
        return;
    }
    const int32_t x = static_cast<int32_t>(std::lround(std::clamp(error_x, -10000.0f, 10000.0f)));
    const int32_t y = static_cast<int32_t>(std::lround(std::clamp(error_y, -10000.0f, 10000.0f)));
    const uint64_t previous = aim_error.exchange(
        (uint64_t(uint32_t(y)) << 32) | uint32_t(x), std::memory_order_release);
    aim_updated_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
    if (fo4hud_aim_trace_enabled() && previous == UINT64_MAX)
        fo4hud_log("aim", "target acquired error_px=(%d,%d)", x, y);
}
void fo4hud_wine_hooks_aim_pulse() {
    if (!installed.load(std::memory_order_acquire) ||
        !aim_enabled.load(std::memory_order_acquire) ||
        !aim_button_down.load(std::memory_order_acquire) ||
        capture_mouse.load(std::memory_order_acquire)) return;
    const uint64_t packed = aim_error.load(std::memory_order_acquire);
    if (packed == UINT64_MAX) return;
    const int32_t x = static_cast<int32_t>(packed);
    const int32_t y = static_cast<int32_t>(packed >> 32);
    if (std::abs(x) < 4 && std::abs(y) < 4) return;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last_real = last_real_relative_ns.load(std::memory_order_acquire);
    const int64_t updated = aim_updated_ns.load(std::memory_order_acquire);
    if (updated <= 0 || now_ns - updated >= 100000000 ||
        now_ns - last_real < 25000000 ||
        aim_pulse_pending.exchange(true, std::memory_order_acq_rel)) return;

    std::lock_guard<std::mutex> lock(hook_mutex);
    wl_display* display = aim_display.load(std::memory_order_acquire);
    wl_event_queue* queue = aim_queue.load(std::memory_order_acquire);
    if (!installed.load(std::memory_order_acquire) || !display || !queue) {
        aim_pulse_pending.store(false, std::memory_order_release);
        return;
    }
    void* wrapper = wl_proxy_create_wrapper(display);
    if (!wrapper) {
        aim_pulse_pending.store(false, std::memory_order_release);
        return;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
    wl_callback* callback = wl_display_sync(reinterpret_cast<wl_display*>(wrapper));
    wl_proxy_wrapper_destroy(wrapper);
    if (!callback) {
        aim_pulse_pending.store(false, std::memory_order_release);
        return;
    }
    if (wl_callback_add_listener(callback, &aim_pulse_listener, nullptr) < 0) {
        wl_callback_destroy(callback);
        aim_pulse_pending.store(false, std::memory_order_release);
        return;
    }
    (void)wl_display_flush(display);
}
bool fo4hud_wine_hooks_aim_button_down() {
    return aim_button_down.load(std::memory_order_acquire);
}

bool fo4hud_wine_control_ready() {
    std::lock_guard<std::mutex> lock(control_mutex);
    return installed.load(std::memory_order_acquire) && control_focused && control_keyboard && control_display;
}
bool fo4hud_wine_control_busy() {
    std::lock_guard<std::mutex> lock(control_mutex);
    return control_any_keys() || control_look_x!=0 || control_look_y!=0;
}
void fo4hud_wine_control_set_cancel_callback(void (*callback)()) { control_cancel_callback.store(callback); }
void fo4hud_wine_control_cancel() {
    std::lock_guard<std::mutex> lock(control_mutex);
    std::fill(std::begin(control_desired),std::end(control_desired),false);
    control_look_x=control_look_y=0;
    control_until={};control_start_pending=false;
}
bool fo4hud_wine_control_input(const std::vector<uint32_t>& keys,int duration_ms,
                               float look_x,float look_y,std::string& error) {
    if(duration_ms<1 || duration_ms>5000 || !std::isfinite(look_x)||!std::isfinite(look_y)||
       std::abs(look_x)>2000 || std::abs(look_y)>2000 || keys.size()>8) {
        error="invalid input duration, key count, or look delta";return false;
    }
    std::lock_guard<std::mutex> lock(control_mutex);
    if(!installed.load() || !control_focused || !control_keyboard || !control_display) {error="Wine input is not ready; focus the game and press a key once";return false;}
    if(control_any_keys() || control_look_x!=0 || control_look_y!=0) {error="another input action is active; stop it first";return false;}
    if(capture_keyboard.load() || capture_mouse.load()) {error="the overlay is capturing input; move the cursor outside it or hide it";return false;}
    for(auto key:keys) {
        if(key==0 || key>=512 || (key>=256 && key!=BTN_LEFT && key!=BTN_RIGHT && key!=BTN_MIDDLE)) {error="unsupported key code";return false;}
        if(physical_keys[key]) {error="the requested key is physically held";return false;}
        if(key>=256 && !control_pointer) {error="click in the game once to initialize mouse input";return false;}
    }
    for(auto key:keys)control_desired[key]=true;
    control_start_pending=true;control_duration_ms=duration_ms;
    control_dispatch_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    control_look_x=look_x;control_look_y=look_y;
    return true;
}
void fo4hud_wine_control_pulse() {
    std::lock_guard<std::mutex> hook_lock(hook_mutex);
    std::lock_guard<std::mutex> lock(control_mutex);
    if(!installed.load() || !control_display || control_pending ||
       (!control_any_keys() && control_look_x==0 && control_look_y==0))return;
    void* wrapper=wl_proxy_create_wrapper(control_display);
    if(!wrapper)return;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper),control_queue);
    wl_callback* callback=wl_display_sync(reinterpret_cast<wl_display*>(wrapper));
    wl_proxy_wrapper_destroy(wrapper);
    if(!callback)return;
    if(wl_callback_add_listener(callback,&control_listener,reinterpret_cast<void*>(control_epoch))<0){wl_callback_destroy(callback);return;}
    control_pending=true;
    wl_display_flush(control_display);
}
