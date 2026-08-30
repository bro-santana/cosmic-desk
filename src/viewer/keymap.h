// Cosmic Desk — SDL scancode -> Windows VK translation (plan M2.6).
// COSMIC MODIFICATION: lifted from moonlight-qt v6.1.0 (GPL-3.0)
// app/streaming/input/keyboard.cpp — SDL scancode -> Windows VK table.

#pragma once

#include <SDL3/SDL_scancode.h>

#include <cstdint>

namespace cosmic::viewer {

// Maps an SDL scancode to the Windows Virtual-Key code moonlight-qt sends for
// it. Returns 0 for scancodes with no VK mapping; callers skip those.
std::uint16_t sdl_scancode_to_vk(SDL_Scancode sc);

}  // namespace cosmic::viewer