#pragma once
#include "game_entities.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

inline const char* fo4hud_trap_label(Fo4TrapKind kind) {
    switch (kind) {
        case Fo4TrapKind::Mine: return "MINE";
        case Fo4TrapKind::Wire: return "TRIPWIRE";
        case Fo4TrapKind::PressurePlate: return "PRESSURE PLATE";
        case Fo4TrapKind::Gun: return "GUN TRAP";
        case Fo4TrapKind::Flame: return "FLAME TRAP";
        case Fo4TrapKind::Electric: return "ELECTRICAL TRAP";
        case Fo4TrapKind::Radiation: return "RADIATION TRAP";
        case Fo4TrapKind::Explosive: return "EXPLOSIVE TRAP";
        case Fo4TrapKind::Alarm: return "ALARM TRAP";
        case Fo4TrapKind::Monkey: return "MONKEY TRAP";
        default: return "TRAP";
    }
}

inline const char* fo4hud_trap_status(Fo4TrapActivity activity) {
    if (activity == Fo4TrapActivity::Sprung) return " - SPRUNG";
    if (activity == Fo4TrapActivity::Unknown) return " (status unknown)";
    return " - ACTIVE";
}

inline ImU32 fo4hud_trap_color(const Fo4TrapMarker& marker) {
    if (marker.activity == Fo4TrapActivity::Sprung) return IM_COL32(150, 165, 150, 255);
    if (marker.activity == Fo4TrapActivity::Unknown) return IM_COL32(255, 180, 90, 255);
    if (marker.kind == Fo4TrapKind::Mine || marker.kind == Fo4TrapKind::Explosive) return IM_COL32(255, 70, 70, 255);
    if (marker.kind == Fo4TrapKind::Wire || marker.kind == Fo4TrapKind::PressurePlate) return IM_COL32(255, 225, 65, 255);
    return IM_COL32(255, 115, 220, 255);
}

inline void fo4hud_draw_trap_overlay(const Fo4EntitySnapshot& snapshot,
                                     const std::vector<Fo4TrapMarker>& markers, ImVec2 display, float range) {
    if (display.x < 200 || display.y < 200 || range <= 0 || !fo4hud_player_in_snapshot_cell(snapshot)) return;
    Fo4Projection projection;
    float player[3]{};
    if (!fo4hud_read_player_position(snapshot, player) || !fo4hud_read_projection(snapshot, projection)) return;
    struct Visible { Fo4TrapMarker marker; float position[3]; float distance; };
    std::vector<Visible> visible;
    for (const auto& marker : markers) {
        if (marker.kind == Fo4TrapKind::None || marker.activity == Fo4TrapActivity::Inactive ||
            !fo4hud_actor_in_player_space(snapshot, marker.entity)) continue;
        Visible v{marker, {}, 0};
        if (!fo4hud_trap_reference_live(marker.entity, v.position, marker.kind == Fo4TrapKind::Monkey)) continue;
        double squared = 0;
        for (unsigned i = 0; i < 3; ++i) { double d = double(v.position[i]) - player[i]; squared += d * d; }
        if (!std::isfinite(squared) || squared > double(range) * range) continue;
        v.distance = float(std::sqrt(squared)); visible.push_back(std::move(v));
    }
    std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    auto* draw = ImGui::GetForegroundDrawList();
    const float pulse = 7.0f + 2.0f * (1.0f + std::sin(float(ImGui::GetTime()) * 6));
    unsigned drawn = 0;
    for (auto& v : visible) {
        if (drawn >= 32) break;
        float screen[2]{};
        v.position[2] += 12;
        if (!fo4hud_project_world(projection, v.position, display.x, display.y, screen) ||
            screen[0] < 0 || screen[0] > display.x || screen[1] < 0 || screen[1] > display.y) continue;
        const ImVec2 p(screen[0], screen[1]);
        const ImU32 color = fo4hud_trap_color(v.marker);
        const bool sprung = v.marker.activity == Fo4TrapActivity::Sprung;
        draw->AddCircle(p, pulse + 4, IM_COL32(0, 0, 0, 200), 16, 4);
        draw->AddQuadFilled(ImVec2(p.x, p.y - pulse), ImVec2(p.x + pulse, p.y),
                            ImVec2(p.x, p.y + pulse), ImVec2(p.x - pulse, p.y), IM_COL32(20, 15, 15, 230));
        draw->AddQuad(ImVec2(p.x, p.y - pulse), ImVec2(p.x + pulse, p.y),
                      ImVec2(p.x, p.y + pulse), ImVec2(p.x - pulse, p.y), color, 2);
        draw->AddText(ImVec2(p.x - 3, p.y - ImGui::GetFontSize() * 0.5f), color, sprung ? "-" : "!");
        char text[120];
        std::snprintf(text, sizeof(text), "%s%s | %.0f units", fo4hud_trap_label(v.marker.kind),
            fo4hud_trap_status(v.marker.activity), v.distance);
        const auto size = ImGui::CalcTextSize(text);
        const ImVec2 label(std::clamp(p.x - size.x * 0.5f, 4.0f, std::max(4.0f, display.x - size.x - 4)),
                           std::clamp(p.y + pulse + 5, 4.0f, display.y - size.y - 4));
        draw->AddRectFilled(ImVec2(label.x - 3, label.y - 2), ImVec2(label.x + size.x + 3, label.y + size.y + 2), IM_COL32(10, 10, 10, 220), 3);
        draw->AddText(label, color, text);
        ++drawn;
    }
    // Sprung traps keep a world label but do not produce a nearby hazard alert.
    auto hazard = std::find_if(visible.begin(), visible.end(), [](const auto& v) {
        return v.marker.activity != Fo4TrapActivity::Sprung;
    });
    if (hazard != visible.end() && hazard->distance <= 400) {
        const auto& v = *hazard;
        char text[120];
        std::snprintf(text, sizeof(text), "NEARBY %s%s | %.0f units", fo4hud_trap_label(v.marker.kind),
            v.marker.activity == Fo4TrapActivity::Unknown ? " (status unknown)" : "", v.distance);
        auto size = ImGui::CalcTextSize(text);
        const ImVec2 p(std::max(4.0f, (display.x - size.x) * 0.5f), 44);
        draw->AddRectFilled(ImVec2(p.x - 6, p.y - 4), ImVec2(p.x + size.x + 6, p.y + size.y + 4), IM_COL32(20, 8, 8, 235), 4);
        draw->AddText(p, fo4hud_trap_color(v.marker), text);
    }
}
