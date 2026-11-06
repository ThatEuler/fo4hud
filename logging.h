#pragma once

// Appends a timestamped diagnostic line to the named fo4hud log. If the log
// cannot be opened, writes the message to stderr instead.
void fo4hud_log(const char* log_name, const char* format, ...);

// Opt-in aim diagnostics, enabled with FO4HUD_AIM_TRACE=1 at game launch.
bool fo4hud_aim_trace_enabled();
