#include "wayland_input.h"
#include "game_control.h"
#include "logging.h"
#include "imgui/imgui.h"
#include <wayland-client.h>
#include "protocols/relative-pointer-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {
enum class Kind { Position, Leave, Button, Wheel, Key, Text, Modifiers, Focus };
struct Event { Kind kind; float x=0, y=0; uint32_t code=0; bool pressed=false; bool ctrl=false, shift=false, alt=false, super=false; };
std::mutex event_mutex;
std::deque<Event> events;
std::atomic<bool> visible{false};
std::atomic<bool> active{false};
std::atomic<bool> running{false};
std::atomic<uint64_t> absolute_count{0}, relative_count{0};
std::thread worker;
wl_display* display = nullptr;
wl_event_queue* queue = nullptr;
wl_registry* registry = nullptr;
wl_seat* seat = nullptr;
wl_pointer* pointer = nullptr;
uint32_t pointer_enter_serial = 0;
std::chrono::steady_clock::time_point last_cursor_update{};
zwp_relative_pointer_manager_v1* relative_manager = nullptr;
zwp_relative_pointer_v1* relative_pointer = nullptr;
wl_keyboard* keyboard = nullptr;
wl_surface* target = nullptr;
xkb_context* xkb = nullptr;
xkb_keymap* keymap = nullptr;
xkb_state* xkb_state_ptr = nullptr;
bool pointer_focused = false;
bool keyboard_focused = false;
double cursor_x = 0, cursor_y = 0;
std::chrono::steady_clock::time_point last_absolute{};
std::chrono::steady_clock::time_point last_relative{};
double last_absolute_x = 0, last_absolute_y = 0;
bool last_absolute_moved = false;
std::atomic<uint32_t> render_width{0}, render_height{0};
std::mutex state_mutex;

void update_hardware_cursor() {
    if (!pointer || !pointer_focused || !pointer_enter_serial) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_cursor_update < std::chrono::milliseconds(100)) return;
    last_cursor_update = now;
    wl_pointer_set_cursor(pointer, pointer_enter_serial, nullptr, 0, 0);
    wl_display_flush(display);
}

void push(Event e) {
    std::lock_guard<std::mutex> lock(event_mutex);
    if (events.size() >= 2048) events.pop_front();
    events.push_back(e);
}
ImGuiKey key_from_sym(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return ImGuiKey(ImGuiKey_A + sym - XKB_KEY_a);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return ImGuiKey(ImGuiKey_A + sym - XKB_KEY_A);
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return ImGuiKey(ImGuiKey_0 + sym - XKB_KEY_0);
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) return ImGuiKey(ImGuiKey_F1 + sym - XKB_KEY_F1);
    switch (sym) {
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return ImGuiKey_Tab;
    case XKB_KEY_Left: return ImGuiKey_LeftArrow;
    case XKB_KEY_Right: return ImGuiKey_RightArrow;
    case XKB_KEY_Up: return ImGuiKey_UpArrow;
    case XKB_KEY_Down: return ImGuiKey_DownArrow;
    case XKB_KEY_Page_Up: return ImGuiKey_PageUp;
    case XKB_KEY_Page_Down: return ImGuiKey_PageDown;
    case XKB_KEY_Home: return ImGuiKey_Home;
    case XKB_KEY_End: return ImGuiKey_End;
    case XKB_KEY_Insert: return ImGuiKey_Insert;
    case XKB_KEY_Delete: return ImGuiKey_Delete;
    case XKB_KEY_BackSpace: return ImGuiKey_Backspace;
    case XKB_KEY_space: return ImGuiKey_Space;
    case XKB_KEY_Return: return ImGuiKey_Enter;
    case XKB_KEY_Escape: return ImGuiKey_Escape;
    case XKB_KEY_Control_L: return ImGuiKey_LeftCtrl;
    case XKB_KEY_Control_R: return ImGuiKey_RightCtrl;
    case XKB_KEY_Shift_L: return ImGuiKey_LeftShift;
    case XKB_KEY_Shift_R: return ImGuiKey_RightShift;
    case XKB_KEY_Alt_L: return ImGuiKey_LeftAlt;
    case XKB_KEY_Alt_R: return ImGuiKey_RightAlt;
    case XKB_KEY_Super_L: return ImGuiKey_LeftSuper;
    case XKB_KEY_Super_R: return ImGuiKey_RightSuper;
    case XKB_KEY_grave: case XKB_KEY_asciitilde: return ImGuiKey_GraveAccent;
    case XKB_KEY_comma: return ImGuiKey_Comma;
    case XKB_KEY_period: return ImGuiKey_Period;
    case XKB_KEY_slash: return ImGuiKey_Slash;
    case XKB_KEY_minus: return ImGuiKey_Minus;
    case XKB_KEY_equal: return ImGuiKey_Equal;
    default: return ImGuiKey_None;
    }
}
bool mod(const char* name) { return xkb_state_ptr && xkb_state_mod_name_is_active(xkb_state_ptr, name, XKB_STATE_MODS_EFFECTIVE) > 0; }
void pointer_enter(void*, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
    // Wine gives its Vulkan client subsurface an empty input region. The
    // pointer therefore enters its parent window, not the swapchain surface.
    pointer_focused = true;
    pointer_enter_serial = serial;
    last_cursor_update = {};
    update_hardware_cursor();
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("wayland-input", "fo4hud: pointer surface=%p render surface=%p", (void*)surface, (void*)target);
    cursor_x = wl_fixed_to_double(x); cursor_y = wl_fixed_to_double(y);
    last_absolute_x = cursor_x; last_absolute_y = cursor_y;
    last_absolute_moved = false;
    last_absolute = std::chrono::steady_clock::now();
    absolute_count.fetch_add(1, std::memory_order_relaxed);
    push({Kind::Position, float(cursor_x), float(cursor_y)});
}
void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    pointer_focused = false;
    pointer_enter_serial = 0;
    push({Kind::Leave});
}
void pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    if (pointer_focused) {
        double next_x = wl_fixed_to_double(x), next_y = wl_fixed_to_double(y);
        last_absolute_moved = std::abs(next_x-last_absolute_x) > 0.5 ||
                              std::abs(next_y-last_absolute_y) > 0.5;
        last_absolute_x = next_x; last_absolute_y = next_y;
        last_absolute = std::chrono::steady_clock::now();
        absolute_count.fetch_add(1, std::memory_order_relaxed);
        if (last_absolute - last_relative > std::chrono::milliseconds(100)) {
            cursor_x = next_x; cursor_y = next_y;
            push({Kind::Position, float(cursor_x), float(cursor_y)});
        }
    }
}
void pointer_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    if (!pointer_focused) return;
    int index = button == BTN_LEFT ? 0 : button == BTN_RIGHT ? 1 : button == BTN_MIDDLE ? 2 : -1;
    if (index >= 0) { Event e{Kind::Button}; e.code=index; e.pressed=state == WL_POINTER_BUTTON_STATE_PRESSED; push(e); }
}
void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (!pointer_focused) return;
    Event e{Kind::Wheel};
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) e.y = -float(wl_fixed_to_double(value))/30.0f;
    else e.x = float(wl_fixed_to_double(value))/30.0f;
    push(e);
}
void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
void pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
void pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
void pointer_warp(void*, wl_pointer*, wl_fixed_t x, wl_fixed_t y) {
    pointer_motion(nullptr, nullptr, 0, x, y);
}
#endif
void relative_motion(void*, zwp_relative_pointer_v1*, uint32_t, uint32_t,
                     wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
    relative_count.fetch_add(1, std::memory_order_relaxed);
    if (!pointer_focused) return;
    // Compositors can send absolute and relative events together. Use the
    // latter only when a pointer lock has stopped absolute motion.
    auto now = std::chrono::steady_clock::now();
    if (last_absolute_moved && now - last_absolute < std::chrono::milliseconds(30) &&
        now - last_relative > std::chrono::milliseconds(100)) return;
    last_relative = now;
    cursor_x += wl_fixed_to_double(dx);
    cursor_y += wl_fixed_to_double(dy);
    cursor_x = std::clamp(cursor_x, 0.0, double(render_width.load()));
    cursor_y = std::clamp(cursor_y, 0.0, double(render_height.load()));
    push({Kind::Position, float(cursor_x), float(cursor_y)});
}
const zwp_relative_pointer_v1_listener relative_listener = {relative_motion};
void ensure_relative_pointer() {
    if (relative_manager && pointer && !relative_pointer) {
        relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
        if (relative_pointer) {
            zwp_relative_pointer_v1_add_listener(relative_pointer, &relative_listener, nullptr);
            fo4hud_log("wayland-input", "fo4hud: Wayland relative pointer initialized");
        }
    }
}
const wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
    pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete,
    pointer_axis_value120
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    , pointer_axis_relative_direction
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
    , pointer_warp
#endif
};
void keyboard_keymap(void*, wl_keyboard*, uint32_t format, int fd, uint32_t size) {
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size && xkb) {
        void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            xkb_keymap* next = xkb_keymap_new_from_string(xkb, static_cast<char*>(map), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(map, size);
            if (next) {
                if (xkb_state_ptr) xkb_state_unref(xkb_state_ptr);
                if (keymap) xkb_keymap_unref(keymap);
                keymap = next;
                xkb_state_ptr = xkb_state_new(keymap);
            }
        }
    }
    close(fd);
}
void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*) {
    keyboard_focused = true;
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("wayland-input", "fo4hud: keyboard surface=%p render surface=%p", (void*)surface, (void*)target);
    Event e{Kind::Focus}; e.pressed = true; push(e);
}
void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { keyboard_focused = false; push({Kind::Focus}); }
void keyboard_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (!xkb_state_ptr || !keyboard_focused) return;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    auto sym = xkb_state_key_get_one_sym(xkb_state_ptr, key + 8);
    bool ctrl=mod(XKB_MOD_NAME_CTRL), shift=mod(XKB_MOD_NAME_SHIFT), alt=mod(XKB_MOD_NAME_ALT), super=mod(XKB_MOD_NAME_LOGO);
    if (std::getenv("FO4HUD_TRACE"))
        fo4hud_log("wayland-input", "fo4hud: key sym=%u pressed=%d", sym, int(pressed));
    if (shift && sym == XKB_KEY_F9) {
        if (pressed) {
            fo4hud_control_cancel();
            fo4hud_log("control", "MCP actions stopped with Shift+F9");
        }
        return;
    }
    if (shift && sym == XKB_KEY_F8) {
        if (pressed) {
            visible.store(!visible.load());
            last_cursor_update = {};
            update_hardware_cursor();
            fo4hud_log("wayland-input", "fo4hud: overlay %s (Shift+F8)", visible.load() ? "shown" : "hidden");
        }
        return;
    }
    Event e{Kind::Key}; e.code=sym; e.pressed=pressed; push(e);
    if (pressed && !ctrl && !alt && !super) {
        uint32_t ch = xkb_state_key_get_utf32(xkb_state_ptr, key + 8);
        if (ch) { Event t{Kind::Text}; t.code=ch; push(t); }
    }
}
void keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (xkb_state_ptr) xkb_state_update_mask(xkb_state_ptr, depressed, latched, locked, 0, 0, group);
    Event e{Kind::Modifiers}; e.ctrl=mod(XKB_MOD_NAME_CTRL); e.shift=mod(XKB_MOD_NAME_SHIFT); e.alt=mod(XKB_MOD_NAME_ALT); e.super=mod(XKB_MOD_NAME_LOGO); push(e);
}
void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info};
void seat_capabilities(void*, wl_seat* seat_obj, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat_obj);
        wl_pointer_add_listener(pointer, &pointer_listener, nullptr);
        ensure_relative_pointer();
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
        if (relative_pointer) { zwp_relative_pointer_v1_destroy(relative_pointer); relative_pointer=nullptr; }
        wl_pointer_release(pointer); pointer=nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(seat_obj);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, nullptr);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) { wl_keyboard_release(keyboard); keyboard=nullptr; }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {seat_capabilities, seat_name};
void registry_global(void*, wl_registry* r, uint32_t name, const char* interface, uint32_t version) {
    if (!seat && std::strcmp(interface, wl_seat_interface.name) == 0) {
        seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, std::min(version, 7u)));
        wl_seat_add_listener(seat, &seat_listener, nullptr);
    } else if (!relative_manager && std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        relative_manager = static_cast<zwp_relative_pointer_manager_v1*>(
            wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1));
        ensure_relative_pointer();
    }
}
void registry_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {registry_global, registry_remove};
void run() {
    while (running.load()) {
        int pending = wl_display_dispatch_queue_pending(display, queue);
        if (pending < 0) break;
        while (wl_display_prepare_read_queue(display, queue) != 0) {
            if (wl_display_dispatch_queue_pending(display, queue) < 0) { running=false; break; }
        }
        if (!running.load()) { wl_display_cancel_read(display); break; }
        wl_display_flush(display);
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        int n = poll(&fd, 1, 25);
        if (n > 0 && (fd.revents & POLLIN)) wl_display_read_events(display);
        else wl_display_cancel_read(display);
        update_hardware_cursor();
    }
    active=false;
}
}

bool fo4hud_wayland_start(wl_display* d, wl_surface* s) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (running.load()) return display == d;
    if (!d || !s) return false;
    display=d; target=s;
    queue=wl_display_create_queue(display);
    if (!queue) return false;
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    if (!wrapper) return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
    registry=wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (!registry) return false;
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    xkb=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (wl_display_roundtrip_queue(display, queue) < 0 || wl_display_roundtrip_queue(display, queue) < 0 || !seat) return false;
    running=true; active=true;
    worker=std::thread(run);
    fo4hud_log("wayland-input", "fo4hud: native Wayland input initialized");
    return true;
}
void fo4hud_wayland_stop() {
    std::lock_guard<std::mutex> lock(state_mutex);
    running=false;
    if (worker.joinable()) worker.join();
    if (relative_pointer) { zwp_relative_pointer_v1_destroy(relative_pointer); relative_pointer=nullptr; }
    if (pointer) { wl_pointer_release(pointer); pointer=nullptr; }
    if (keyboard) { wl_keyboard_release(keyboard); keyboard=nullptr; }
    if (relative_manager) { zwp_relative_pointer_manager_v1_destroy(relative_manager); relative_manager=nullptr; }
    if (seat) { wl_seat_release(seat); seat=nullptr; }
    if (registry) { wl_registry_destroy(registry); registry=nullptr; }
    if (queue) { wl_event_queue_destroy(queue); queue=nullptr; }
    if (xkb_state_ptr) { xkb_state_unref(xkb_state_ptr); xkb_state_ptr=nullptr; }
    if (keymap) { xkb_keymap_unref(keymap); keymap=nullptr; }
    if (xkb) { xkb_context_unref(xkb); xkb=nullptr; }
    display=nullptr; target=nullptr; active=false;
}
bool fo4hud_wayland_active() { return active.load(); }
bool fo4hud_overlay_visible() { return visible.load(); }
void fo4hud_overlay_set_visible(bool shown) { visible.store(shown); }
uint64_t fo4hud_wayland_absolute_count() { return absolute_count.load(std::memory_order_relaxed); }
uint64_t fo4hud_wayland_relative_count() { return relative_count.load(std::memory_order_relaxed); }
void fo4hud_wayland_new_frame(ImGuiIO& io, uint32_t width, uint32_t height) {
    render_width.store(width); render_height.store(height);
    io.BackendPlatformName="fo4hud_wayland";
    io.DisplaySize=ImVec2(float(width), float(height));
    std::deque<Event> frame;
    { std::lock_guard<std::mutex> lock(event_mutex); frame.swap(events); }
    for (auto& e:frame) {
        switch (e.kind) {
        case Kind::Position: io.AddMousePosEvent(e.x,e.y); break;
        case Kind::Leave: io.AddMousePosEvent(-3.4e38f,-3.4e38f); break;
        case Kind::Button: io.AddMouseButtonEvent(int(e.code),e.pressed); break;
        case Kind::Wheel: io.AddMouseWheelEvent(e.x,e.y); break;
        case Kind::Key: { auto key=key_from_sym(e.code); if (key != ImGuiKey_None) io.AddKeyEvent(key,e.pressed); break; }
        case Kind::Text: io.AddInputCharacter(e.code); break;
        case Kind::Modifiers:
            io.AddKeyEvent(ImGuiMod_Ctrl,e.ctrl); io.AddKeyEvent(ImGuiMod_Shift,e.shift);
            io.AddKeyEvent(ImGuiMod_Alt,e.alt); io.AddKeyEvent(ImGuiMod_Super,e.super); break;
        case Kind::Focus: io.AddFocusEvent(e.pressed); break;
        }
    }
}
