#pragma once
#include <cstdint>
struct wl_display;
struct wl_surface;
struct ImGuiIO;

// Register separate Wayland seat resources on the display used by the game.
bool fo4hud_wayland_start(wl_display* display, wl_surface* surface);
void fo4hud_wayland_stop();
void fo4hud_wayland_new_frame(ImGuiIO& io, uint32_t width, uint32_t height);
bool fo4hud_wayland_active();
bool fo4hud_overlay_visible();
void fo4hud_overlay_set_visible(bool shown);
uint64_t fo4hud_wayland_absolute_count();
uint64_t fo4hud_wayland_relative_count();
