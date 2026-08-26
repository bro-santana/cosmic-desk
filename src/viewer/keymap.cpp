// COSMIC MODIFICATION: lifted from moonlight-qt v6.1.0 (GPL-3.0)
// app/streaming/input/keyboard.cpp — SDL scancode -> Windows VK table.
// The upstream file carries no separate license header; this comment is the
// attribution required by docs/VENDOR.md. The mapping values, range rules and
// case list are verbatim; the only changes are the value-returning function
// shape (upstream assigns to a local and sends via LiSendKeyboardEvent) and
// the dropped isSystemKeyCaptureActive() gates on the LGUI/RGUI cases (that
// Qt-specific behavior belongs to M4 keyboard grab, not to the table).

#include "viewer/keymap.h"

#define VK_0 0x30
#define VK_A 0x41

// These are real Windows VK_* codes
#ifndef VK_F1
#define VK_F1 0x70
#define VK_F13 0x7C
#define VK_NUMPAD0 0x60
#endif

namespace cosmic::viewer {

std::uint16_t sdl_scancode_to_vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
        return static_cast<std::uint16_t>((sc - SDL_SCANCODE_1) + VK_0 + 1);
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return static_cast<std::uint16_t>((sc - SDL_SCANCODE_A) + VK_A);
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<std::uint16_t>((sc - SDL_SCANCODE_F1) + VK_F1);
    }
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) {
        return static_cast<std::uint16_t>((sc - SDL_SCANCODE_F13) + VK_F13);
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
        return static_cast<std::uint16_t>((sc - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1);
    }
    switch (sc) {
        case SDL_SCANCODE_BACKSPACE:
            return 0x08;
        case SDL_SCANCODE_TAB:
            return 0x09;
        case SDL_SCANCODE_CLEAR:
            return 0x0C;
        case SDL_SCANCODE_KP_ENTER: // FIXME: Is this correct?
        case SDL_SCANCODE_RETURN:
            return 0x0D;
        case SDL_SCANCODE_PAUSE:
            return 0x13;
        case SDL_SCANCODE_CAPSLOCK:
            return 0x14;
        case SDL_SCANCODE_ESCAPE:
            return 0x1B;
        case SDL_SCANCODE_SPACE:
            return 0x20;
        case SDL_SCANCODE_PAGEUP:
            return 0x21;
        case SDL_SCANCODE_PAGEDOWN:
            return 0x22;
        case SDL_SCANCODE_END:
            return 0x23;
        case SDL_SCANCODE_HOME:
            return 0x24;
        case SDL_SCANCODE_LEFT:
            return 0x25;
        case SDL_SCANCODE_UP:
            return 0x26;
        case SDL_SCANCODE_RIGHT:
            return 0x27;
        case SDL_SCANCODE_DOWN:
            return 0x28;
        case SDL_SCANCODE_SELECT:
            return 0x29;
        case SDL_SCANCODE_EXECUTE:
            return 0x2B;
        case SDL_SCANCODE_PRINTSCREEN:
            return 0x2C;
        case SDL_SCANCODE_INSERT:
            return 0x2D;
        case SDL_SCANCODE_DELETE:
            return 0x2E;
        case SDL_SCANCODE_HELP:
            return 0x2F;
        case SDL_SCANCODE_KP_0:
            // See comment above about why we only handle SDL_SCANCODE_KP_0 here
            return VK_NUMPAD0;
        case SDL_SCANCODE_0:
            // See comment above about why we only handle SDL_SCANCODE_0 here
            return VK_0;
        case SDL_SCANCODE_KP_MULTIPLY:
            return 0x6A;
        case SDL_SCANCODE_KP_PLUS:
            return 0x6B;
        case SDL_SCANCODE_KP_COMMA:
            return 0x6C;
        case SDL_SCANCODE_KP_MINUS:
            return 0x6D;
        case SDL_SCANCODE_KP_PERIOD:
            return 0x6E;
        case SDL_SCANCODE_KP_DIVIDE:
            return 0x6F;
        case SDL_SCANCODE_NUMLOCKCLEAR:
            return 0x90;
        case SDL_SCANCODE_SCROLLLOCK:
            return 0x91;
        case SDL_SCANCODE_LSHIFT:
            return 0xA0;
        case SDL_SCANCODE_RSHIFT:
            return 0xA1;
        case SDL_SCANCODE_LCTRL:
            return 0xA2;
        case SDL_SCANCODE_RCTRL:
            return 0xA3;
        case SDL_SCANCODE_LALT:
            return 0xA4;
        case SDL_SCANCODE_RALT:
            return 0xA5;
        case SDL_SCANCODE_LGUI:
            // Upstream gates this on isSystemKeyCaptureActive(); that
            // Qt-specific behavior is dropped (M4 owns system-key capture).
            return 0x5B;
        case SDL_SCANCODE_RGUI:
            // Upstream gates this on isSystemKeyCaptureActive(); that
            // Qt-specific behavior is dropped (M4 owns system-key capture).
            return 0x5C;
        case SDL_SCANCODE_APPLICATION:
            return 0x5D;
        case SDL_SCANCODE_AC_BACK:
            return 0xA6;
        case SDL_SCANCODE_AC_FORWARD:
            return 0xA7;
        case SDL_SCANCODE_AC_REFRESH:
            return 0xA8;
        case SDL_SCANCODE_AC_STOP:
            return 0xA9;
        case SDL_SCANCODE_AC_SEARCH:
            return 0xAA;
        case SDL_SCANCODE_AC_BOOKMARKS:
            return 0xAB;
        case SDL_SCANCODE_AC_HOME:
            return 0xAC;
        case SDL_SCANCODE_SEMICOLON:
            return 0xBA;
        case SDL_SCANCODE_EQUALS:
            return 0xBB;
        case SDL_SCANCODE_COMMA:
            return 0xBC;
        case SDL_SCANCODE_MINUS:
            return 0xBD;
        case SDL_SCANCODE_PERIOD:
            return 0xBE;
        case SDL_SCANCODE_SLASH:
            return 0xBF;
        case SDL_SCANCODE_GRAVE:
            return 0xC0;
        case SDL_SCANCODE_LEFTBRACKET:
            return 0xDB;
        case SDL_SCANCODE_BACKSLASH:
            return 0xDC;
        case SDL_SCANCODE_RIGHTBRACKET:
            return 0xDD;
        case SDL_SCANCODE_APOSTROPHE:
            return 0xDE;
        case SDL_SCANCODE_NONUSBACKSLASH:
            return 0xE2;
        default:
            // Upstream logs and skips unmapped scancodes; 0 means "skip".
            return 0;
    }
}

}  // namespace cosmic::viewer