#pragma once

#include "game_entities.h"
#include <memory>

struct Fo4LocalMapFrame {
    uintptr_t cell = 0;
    uint32_t cell_form_id = 0;
    float position[3]{};
    Fo4MapInfo map;
    int width = 0, height = 0;
    uint64_t sequence = 0;
    std::vector<unsigned char> pixels;
};

// Installs a checked call-site hook in the supported game's render loop.
// Rendering and D3D readback run on that thread, never in Vulkan present.
bool fo4hud_local_map_install();
void fo4hud_local_map_uninstall();
// Requests are short-lived; stopping radar drawing stops local-map work.
std::shared_ptr<const Fo4LocalMapFrame> fo4hud_local_map_request(
    const Fo4EntitySnapshot& snapshot);
