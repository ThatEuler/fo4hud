#pragma once
#include "dialogue_affinity.h"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>

struct Fo4DialogueLabelRect { ImVec2 min, max; };

inline Fo4DialogueLabelRect fo4hud_dialogue_label_rect(unsigned option,
        const Fo4DialogueChoiceBounds& bounds, ImVec2 text, ImVec2 display, float gap, ImVec2 padding) {
    const float width=text.x+2*padding.x, height=text.y+2*padding.y;
    const float left=bounds.left*display.x, right=bounds.right*display.x;
    const float top=bounds.top*display.y, bottom=bounds.bottom*display.y;
    float x=(left+right-width)*.5f, y=(top+bottom-height)*.5f;
    if (option==2) x=left-gap-width;
    else if (option==1) x=right+gap;
    else if (option==3) y=top-gap-height;
    else y=bottom+gap;
    x=std::clamp(x,4.0f,std::max(4.0f,display.x-width-4));
    y=std::clamp(y,4.0f,std::max(4.0f,display.y-height-4));
    return {{x,y},{x+width,y+height}};
}

inline void fo4hud_draw_dialogue_affinity(const Fo4DialogueAffinity& state, ImVec2 display, ImFont* font = nullptr) {
    if (!state.visible || !state.status.empty() || display.x<640 || display.y<300) return;
    const float size=std::clamp(display.y*.018f,16.0f,26.0f);
    const float gap=std::max(6.0f,display.y*.007f);
    const ImVec2 padding(7,4);
    ImGui::PushFont(font,size);
    auto* draw=ImGui::GetForegroundDrawList();
    for (unsigned i=0;i<4;++i) {
        const auto& choice=state.choices[i];
        const auto& bounds=state.bounds[i];
        if (!choice.present || !bounds.valid) continue;
        float available=std::min(360.0f,display.x*.4f);
        if (i==2) available=std::min(available,bounds.left*display.x-gap-4);
        if (i==1) available=std::min(available,display.x*(1-bounds.right)-gap-4);
        const float wrap=available-2*padding.x;
        if (wrap<60) continue;
        struct Line { std::string text; ImVec2 size; bool positive; };
        std::vector<Line> lines;
        ImVec2 text_size(0,0);
        for (const auto& reaction:choice.reactions) {
            if (std::find(state.nearby_companions.begin(),state.nearby_companions.end(),reaction.companion)==state.nearby_companions.end()) continue;
            std::string text=reaction.companion+" "+reaction.reaction+(reaction.conditional ? " (conditional)" : "");
            auto measured=ImGui::CalcTextSize(text.c_str(),nullptr,false,wrap);
            text_size.x=std::max(text_size.x,measured.x);
            if (!lines.empty()) text_size.y+=3;
            text_size.y+=measured.y;
            lines.push_back({std::move(text),measured,reaction.reaction=="likes" || reaction.reaction=="loves"});
        }
        if (lines.empty()) continue;
        auto rect=fo4hud_dialogue_label_rect(i,bounds,text_size,display,gap,padding);
        draw->AddRectFilled(rect.min,rect.max,IM_COL32(12,18,14,225),4);
        ImVec2 cursor(rect.min.x+padding.x,rect.min.y+padding.y);
        for (const auto& line:lines) {
            draw->AddText(ImGui::GetFont(),size,cursor,line.positive ? IM_COL32(102,255,115,255) : IM_COL32(255,115,102,255),line.text.c_str(),nullptr,wrap);
            cursor.y+=line.size.y+3;
        }
    }
    ImGui::PopFont();
}
