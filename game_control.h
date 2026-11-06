#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Local, same-user MCP bridge. Native commands run from PlayerCharacter::Update.
bool fo4hud_control_start(uintptr_t image);
void fo4hud_control_stop();
void fo4hud_control_tick(uintptr_t image, void* player);
// Main/pause menu updates continue while the player simulation is paused.
void fo4hud_control_ui_tick(uintptr_t image, uintptr_t menu);
void fo4hud_control_cancel();
std::string fo4hud_control_socket_path();

// Render-thread screenshot handoff; false means there is no pending request.
bool fo4hud_control_begin_capture();
void fo4hud_control_finish_capture(int width, int height, bool bgra,
                                   const unsigned char* pixels, const std::string& error = {});
