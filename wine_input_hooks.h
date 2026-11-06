#pragma once

// Intercepts the Wayland callbacks in Wine's loaded native driver. These
// functions never load the overlay through LD_PRELOAD.
bool fo4hud_wine_hooks_install();
void fo4hud_wine_hooks_uninstall();
void fo4hud_wine_hooks_set_capture(bool mouse, bool keyboard);
bool fo4hud_wine_hooks_active();
void fo4hud_wine_hooks_set_aim_assist(bool enabled, float strength);
void fo4hud_wine_hooks_set_aim_target(float error_x, float error_y, bool valid);
// Schedule one correction on Wine's Wayland queue when the mouse is idle.
void fo4hud_wine_hooks_aim_pulse();
bool fo4hud_wine_hooks_aim_button_down();

#include <cstdint>
#include <string>
#include <vector>
// Timed game-only input. Dispatches on Wine's Wayland event queue, never a
// socket or render thread. Physical key/button presses cancel automation.
bool fo4hud_wine_control_input(const std::vector<uint32_t>& keys, int duration_ms,
                               float look_x, float look_y, std::string& error);
void fo4hud_wine_control_cancel();
void fo4hud_wine_control_pulse();
bool fo4hud_wine_control_ready();
bool fo4hud_wine_control_busy();
void fo4hud_wine_control_set_cancel_callback(void (*callback)());
