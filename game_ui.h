#pragma once
#include "control_json.h"
#include <cstdint>

bool fo4hud_ui_install(uintptr_t image);
void fo4hud_ui_uninstall();
bool fo4hud_ui_active();
fo4control::Json fo4hud_ui_state(uintptr_t image);
fo4control::Json fo4hud_ui_saves();
fo4control::Json fo4hud_ui_action(uintptr_t image, uintptr_t menu, json_object* args);
