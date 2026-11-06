#pragma once

#include "game_entities.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cstdio>

// A passive dialogue-side window: it must never consume a dialogue click/key.
inline void fo4hud_draw_companion_overlay(const Fo4CompanionState& state, ImVec2 display) {
    if (!state.address || display.x < 320 || display.y < 200) return;
    const float width = std::min(300.0f, display.x - 24);
    const float top = std::max(12.0f, display.y * 0.16f);
    ImGui::SetNextWindowPos(ImVec2(12, top), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, display.y - top - 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));
    ImGui::SetNextWindowBgAlpha(0.90f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("Actor information", nullptr, flags)) {
        ImGui::TextWrapped("%s", state.name.empty() ? "Actor" : state.name.c_str());
        ImGui::TextWrapped("Race: %s", state.race.c_str());
        ImGui::TextUnformatted(state.synth_status == Fo4SynthStatus::Confirmed ?
                               "Synth: Confirmed" : "Synth: No synth markers detected");
        ImGui::Separator();
        if (state.affinity) {
            ImGui::Text("Affinity: %.0f", *state.affinity);
            ImGui::ProgressBar(std::clamp(*state.affinity / 1000.0f, 0.0f, 1.0f), ImVec2(-1, 0),
                               "toward 1000 affinity");
        } else if (state.is_companion) ImGui::TextDisabled("Affinity: unavailable / not applicable");
        auto pool = [](const char* label, std::optional<float> current, std::optional<float> maximum) {
            if (current && maximum) ImGui::Text("%s: %.0f / %.0f", label, *current, *maximum);
            else if (current) ImGui::Text("%s: %.0f", label, *current);
            else ImGui::TextDisabled("%s: unavailable", label);
        };
        pool("Health", state.health, state.max_health);
        pool("AP", state.action_points, state.max_action_points);
        ImGui::SeparatorText("SPECIAL");
        constexpr const char* names[] = {"Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck"};
        if (ImGui::BeginTable("##companion-special", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthStretch, 3);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1);
            for (unsigned i = 0; i < 7; ++i) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(names[i]);
                ImGui::TableNextColumn();
                if (state.special[i]) ImGui::Text("%.1f", *state.special[i]);
                else ImGui::TextDisabled("--");
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Equipment stats");
        if (state.inventory_weight && state.carry_weight)
            ImGui::Text("Carry weight: %.1f / %.0f", *state.inventory_weight, *state.carry_weight);
        else if (state.carry_weight)
            ImGui::Text("Carry weight: -- / %.0f", *state.carry_weight);
        else if (state.inventory_weight)
            ImGui::Text("Carry weight: %.1f / --", *state.inventory_weight);
        else ImGui::TextDisabled("Carry weight: unavailable");
        pool("Damage resistance", state.damage_resistance, {});
        pool("Energy resistance", state.energy_resistance, {});
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
