#pragma once
#include "game_entities.h"
#include <array>
#include <string>
#include <vector>

struct Fo4DialogueReaction {
    std::string companion, reaction;
    bool conditional = false;
};
struct Fo4DialogueChoiceAffinity {
    bool present = false;
    std::vector<Fo4DialogueReaction> reactions;
};
struct Fo4DialogueChoiceBounds {
    bool valid = false;
    // Normalized screen coordinates, including the choice text and arrow.
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct Fo4DialogueAffinity {
    bool visible = false;
    std::string status;
    std::vector<std::string> nearby_companions;
    // Native choice order: down, right, left, up. Never sorted by text.
    std::array<Fo4DialogueChoiceAffinity, 4> choices;
    std::array<Fo4DialogueChoiceBounds, 4> bounds;
};

bool fo4hud_dialogue_affinity_install(uintptr_t image);
void fo4hud_dialogue_affinity_uninstall();
Fo4DialogueAffinity fo4hud_read_dialogue_affinity(const Fo4EntitySnapshot& snapshot);
