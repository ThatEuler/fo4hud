#include "../layer.cpp"
#include <cassert>
#include <filesystem>

int main() {
    char directory[] = "/tmp/fo4hud-visibility-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    settings_loaded = true;
    for (bool shown : {true, false, true}) {
        fo4hud_overlay_set_visible(shown);
        settings_dirty = false;
        flush_settings(); // Also exercises saving a final hotkey change at shutdown.
        OverlaySettings restored;
        std::string message;
        assert(fo4hud_load_settings(restored, message));
        assert(restored.overlay_visible == shown && !settings_dirty);
        fo4hud_overlay_set_visible(false);
        fo4hud_overlay_set_visible(restored.overlay_visible);
        assert(fo4hud_overlay_visible() == shown);
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    RadarWindowFade fade;
    // Theft is distinguished by geometry as well as color. The badge is
    // included in the same persistent vertex range as the category marker.
    {
        ImDrawList badge(ImGui::GetDrawListSharedData());
        badge._ResetForNewFrame();
        badge.PushClipRectFullScreen();
        draw_radar_theft_badge(&badge, {100, 100});
        assert(badge.VtxBuffer.Size > 0 && badge.IdxBuffer.Size > 0);
        bool magenta = false;
        for (const auto& v : badge.VtxBuffer)
            magenta |= (v.col & ~IM_COL32_A_MASK) == (radar_theft_color & ~IM_COL32_A_MASK);
        assert(magenta);
        badge.PopClipRect();
    }

    auto frame = [&](bool radar_selected, bool mouse_inside) {
        io.AddMousePosEvent(mouse_inside ? 150.0f : 900.0f, mouse_inside ? 150.0f : 700.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(50, 50));
        ImGui::SetNextWindowSize(ImVec2(500, 500));
        assert(ImGui::Begin("Fade test", nullptr, ImGuiWindowFlags_NoSavedSettings));
        ImGui::TextUnformatted("Tab bar and range controls");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        fade.opaque_ranges.clear();
        int start = draw->VtxBuffer.Size;
        draw->AddCircleFilled(ImVec2(300, 300), 180, IM_COL32(16, 24, 35, 235));
        draw->AddCircle(ImVec2(300, 300), 180, IM_COL32_WHITE);
        draw_radar_compass(draw, {300, 300}, 180, 0.7f);
        fade.keep(draw, start);
        // Controls and count fade, but compass letters stay with the map.
        draw->AddText(ImVec2(300, 100), IM_COL32_WHITE, "Range");
        start = draw->VtxBuffer.Size;
        draw->AddTriangleFilled(ImVec2(300, 290), ImVec2(306, 306),
                                ImVec2(294, 306), IM_COL32(255, 225, 95, 255));
        draw_radar_theft_badge(draw, {350, 300});
        fade.keep(draw, start);
        draw->AddText(ImVec2(80, 100), IM_COL32_WHITE, "2 shown");
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
        assert(hovered == mouse_inside);
        std::vector<ImU32> colors;
        for (const auto& vertex : draw->VtxBuffer) colors.push_back(vertex.col);
        ImGui::End();
        fade.apply(draw, radar_selected, hovered, io.DeltaTime);
        for (int i = 0; i < draw->VtxBuffer.Size; ++i) {
            bool retained = false;
            for (const auto& range : fade.opaque_ranges)
                if (i >= range.first && i < range.second) retained = true;
            if (retained || !radar_selected) assert(draw->VtxBuffer[i].col == colors[i]);
            else if (fade.opacity == 0) assert((draw->VtxBuffer[i].col & IM_COL32_A_MASK) == 0);
        }
        ImGui::Render();
    };
    frame(true, true);
    assert(fade.opacity == 1);
    frame(true, false);
    assert(fade.opacity > 0 && fade.opacity < 1);
    for (int i = 0; i < 30; ++i) frame(true, false);
    assert(fade.opacity == 0);
    // A fully faded window still submits its geometry and responds to hover.
    frame(true, true);
    assert(fade.opacity > 0 && fade.opacity < 1);
    for (int i = 0; i < 15; ++i) frame(true, true);
    assert(fade.opacity == 1);
    for (int i = 0; i < 30; ++i) frame(true, false);
    frame(false, false);
    assert(fade.opacity == 1); // Other tabs stay fully visible.
    ImGui::DestroyContext();
    settings_loaded = false;
    std::filesystem::remove_all(directory);
}
