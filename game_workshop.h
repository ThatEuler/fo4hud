#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Fo4ScrapReference {
    uintptr_t address = 0;
    uintptr_t cell = 0;
    uint32_t form_id = 0;
    uint32_t base_form_id = 0;
    float position[3]{};
    std::string name;
};

struct Fo4WorkshopScrapPreview {
    uint64_t generation = 0;
    uintptr_t workshop = 0;
    uint32_t workshop_form_id = 0;
    uintptr_t player_cell = 0;
    std::string status = "Preview not requested";
    bool ready = false;
    bool running = false;
    size_t processed = 0;
    size_t skipped = 0;
    size_t selected_count = 0;
    std::vector<Fo4ScrapReference> references;
};

void fo4hud_workshop_request_preview();
Fo4WorkshopScrapPreview fo4hud_workshop_scrap_preview();
bool fo4hud_workshop_begin_scrap(uint64_t generation,
                                const std::vector<uint8_t>& selected);
void fo4hud_workshop_cancel_scrap();
void fo4hud_workshop_tick(uintptr_t image, void* player);
void fo4hud_workshop_reset();
