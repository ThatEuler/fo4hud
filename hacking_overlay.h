#pragma once

#include "game_entities.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cstdio>

inline void fo4hud_draw_hacking_overlay(const Fo4HackingState& state, ImVec2 display) {
    if (display.x < 320 || display.y < 200 || state.password.empty() ||
        state.password_position >= state.board.size() ||
        state.password.size() > state.board.size() - state.password_position) return;
    auto* font = ImGui::GetFont();
    const float size = std::min(ImGui::GetFontSize(), (display.y - 156.0f) / 16.0f);
    const float cell = font->CalcTextSizeA(size, 1000, 0, "W").x + 1;
    const float row_height = size + 3;
    const float width = 28 * cell + 24;
    const float height = 16 * row_height + 92;
    const ImVec2 top(std::max(8.0f, display.x - width - 16), std::max(8.0f, (display.y - height) * 0.5f));
    auto* draw = ImGui::GetForegroundDrawList();
    const ImU32 green = IM_COL32(115, 245, 155, 255);
    const ImU32 yellow = IM_COL32(255, 225, 90, 255);
    draw->AddRectFilled(top, ImVec2(top.x + width, top.y + height), IM_COL32(12, 19, 22, 235), 8);
    draw->AddRect(top, ImVec2(top.x + width, top.y + height), green, 8);
    draw->AddText(font, size, ImVec2(top.x + 12, top.y + 10), green, "HACKING GUIDE");
    const std::string label = "Password: " + state.password;
    draw->AddText(font, size, ImVec2(top.x + 12, top.y + 13 + row_height), yellow, label.c_str());
    char location[80];
    std::snprintf(location, sizeof(location), "%s column, row %u, character %u",
        state.password_position < 192 ? "Left" : "Right",
        (state.password_position % 192) / 12 + 1, state.password_position % 12 + 1);
    draw->AddText(font, size, ImVec2(top.x + 12, top.y + 16 + 2 * row_height), green, location);
    for (unsigned i = 0; i < state.board.size(); ++i) {
        const ImVec2 pos(top.x + 12 + (i / 192 * 16 + i % 12) * cell,
                         top.y + 76 + (i % 192 / 12) * row_height);
        const bool correct = i >= state.password_position && i - state.password_position < state.password.size();
        if (correct)
            draw->AddRectFilled(ImVec2(pos.x - 1, pos.y - 1), ImVec2(pos.x + cell, pos.y + size + 2), yellow);
        draw->AddText(font, size, pos, correct ? IM_COL32(12, 19, 22, 255) : green,
                      &state.board[i], &state.board[i] + 1);
    }
}
