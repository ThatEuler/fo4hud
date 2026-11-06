#pragma once

#include "game_entities.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>

// Positive mouse X decreases the game's pick angle (LockpickingMenu's
// mouse handler). Positive game angles therefore occupy the guide's left.
inline float fo4hud_lockpick_screen_angle(float degrees) {
    return (-degrees - 90.0f) * (3.14159265358979323846f / 180.0f);
}

inline bool fo4hud_lockpick_aligned(const Fo4LockpickState& state) {
    return std::abs(state.pick_degrees - state.sweet_spot_degrees) <= state.sweet_spot_width * 0.5f;
}

inline void fo4hud_draw_lockpick_overlay(const Fo4LockpickState& state, ImVec2 display) {
    if (display.x < 320 || display.y < 200) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.65f, 1.5f);
    const float width = std::min(380.0f * scale, display.x - 24.0f);
    const ImVec2 top((display.x - width) * 0.5f, display.y * 0.10f);
    const ImVec2 bottom(top.x + width, top.y + 205.0f * scale);
    const ImVec2 center(display.x * 0.5f, top.y + 158.0f * scale);
    const float radius = std::min(112.0f * scale, width * 0.38f);
    const ImU32 green = IM_COL32(115, 245, 155, 255);
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(top, bottom, IM_COL32(12, 19, 22, 220), 10.0f * scale);
    draw->AddRect(top, bottom, IM_COL32(90, 125, 115, 170), 10.0f * scale);
    auto centered_text = [&](float y, const char* text, ImU32 color) {
        draw->AddText(ImVec2(center.x - ImGui::CalcTextSize(text).x * 0.5f, y), color, text);
    };
    centered_text(top.y + 12 * scale, "LOCKPICK GUIDE", green);
    draw->PathArcTo(center, radius, -3.14159265358979323846f, 0, 64);
    draw->PathStroke(IM_COL32(105, 125, 130, 200), 0, 3 * scale);
    auto point = [&](float degrees, float distance) {
        const float angle = fo4hud_lockpick_screen_angle(degrees);
        return ImVec2(center.x + std::cos(angle) * distance, center.y + std::sin(angle) * distance);
    };
    for (int angle : {-90, -45, 0, 45, 90})
        draw->AddLine(point(float(angle), radius + 5 * scale), point(float(angle), radius + 11 * scale),
                      IM_COL32(150, 170, 175, 180));
    const float half_width = state.sweet_spot_width * 0.5f;
    const float start = fo4hud_lockpick_screen_angle(std::min(90.0f, state.sweet_spot_degrees + half_width));
    const float end = fo4hud_lockpick_screen_angle(std::max(-90.0f, state.sweet_spot_degrees - half_width));
    if (end > start) {
        draw->PathArcTo(center, radius, start, end, 32);
        draw->PathStroke(green, 0, 9 * scale);
    }
    draw->AddLine(point(state.sweet_spot_degrees, radius - 14 * scale),
                  point(state.sweet_spot_degrees, radius + 14 * scale), green, 3 * scale);
    const bool aligned = fo4hud_lockpick_aligned(state);
    const ImU32 pick_color = aligned ? green : IM_COL32(245, 245, 235, 255);
    draw->AddLine(center, point(state.pick_degrees, radius - 5 * scale), pick_color, 3 * scale);
    draw->AddCircleFilled(center, 5 * scale, pick_color);
    const char* hint = aligned ? "Aligned - turn the lock" :
        state.sweet_spot_degrees > state.pick_degrees ? "Move pick left toward green" : "Move pick right toward green";
    centered_text(top.y + 179 * scale, hint, pick_color);
}
