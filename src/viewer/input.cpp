// Cosmic Desk — viewer input forwarding (plan M2.6). SDL events -> LiSend*
// while streaming, plus the Ctrl+Alt+Shift+Q escape combo. Pattern follows
// moonlight-qt app/streaming/input/{keyboard,mouse,input}.cpp: absolute mouse
// position (mapped into stream coordinates), button/scroll forwarding, and
// the escape-combo check on keydown.

#include "viewer/input.h"

#include "viewer/keymap.h"
#include "viewer/session.h"

#include <Limelight.h>

#include <algorithm>
#include <cstdint>

namespace cosmic::viewer {
namespace input {
namespace {

int g_stream_width = 0;
int g_stream_height = 0;

// Ctrl+Alt+Shift+Q ends the session (moonlight-qt KeyComboQuit pattern). The
// Z (ungrab) and Enter (fullscreen) combos are M4 and deliberately not handled
// here. Detection matches moonlight-qt's handleKeyEvent(): the combo is
// checked on keydown via keysym.mod, before any ImGui capture gate.
bool handle_escape_combo(const SDL_KeyboardEvent& key) {
    return key.state == SDL_PRESSED && !key.repeat &&
           (key.keysym.mod & KMOD_CTRL) != 0 &&
           (key.keysym.mod & KMOD_ALT) != 0 &&
           (key.keysym.mod & KMOD_SHIFT) != 0 &&
           key.keysym.scancode == SDL_SCANCODE_Q;
}

bool handle_key_event(const SDL_KeyboardEvent& key, Session& session,
                      bool imgui_wants_kb) {
    if (handle_escape_combo(key)) {
        session.end_session();
        return true;
    }
    if (imgui_wants_kb) {
        return false;  // The overlay has keyboard focus; let ImGui see it.
    }
    if (key.repeat) {
        // Ignore repeat key down events (moonlight-qt behavior).
        return true;
    }
    const std::uint16_t keycode = sdl_scancode_to_vk(key.keysym.scancode);
    if (keycode != 0) {
        // Modifier flags and the 0x8000 extended-key bit match moonlight-qt
        // keyboard.cpp:433 (v6.1.0). MODIFIER_META is skipped: system-key
        // capture is M4 scope (see keymap.cpp).
        char modifiers = 0;
        if (key.keysym.mod & KMOD_CTRL) {
            modifiers |= MODIFIER_CTRL;
        }
        if (key.keysym.mod & KMOD_ALT) {
            modifiers |= MODIFIER_ALT;
        }
        if (key.keysym.mod & KMOD_SHIFT) {
            modifiers |= MODIFIER_SHIFT;
        }
        LiSendKeyboardEvent(static_cast<short>(0x8000 | keycode),
                            key.state == SDL_PRESSED ? KEY_ACTION_DOWN
                                                     : KEY_ACTION_UP,
                            modifiers);
    }
    return true;
}

bool handle_mouse_motion(const SDL_MouseMotionEvent& motion, SDL_Window* window) {
    if (g_stream_width <= 0 || g_stream_height <= 0) {
        return true;  // No stream geometry yet; nothing to map to.
    }
    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    if (window_w <= 0 || window_h <= 0) {
        return true;
    }
    // M2 renders stretch-fill, so map window coordinates proportionally into
    // stream coordinates and clamp to the stream bounds.
    int x = motion.x * g_stream_width / window_w;
    int y = motion.y * g_stream_height / window_h;
    x = std::clamp(x, 0, g_stream_width - 1);
    y = std::clamp(y, 0, g_stream_height - 1);
    LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y),
                             static_cast<short>(g_stream_width),
                             static_cast<short>(g_stream_height));
    return true;
}

bool handle_mouse_button(const SDL_MouseButtonEvent& button) {
    int remote_button = 0;
    switch (button.button) {
        case SDL_BUTTON_LEFT:
            remote_button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            remote_button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            remote_button = BUTTON_RIGHT;
            break;
        default:
            // X1/X2 are not forwarded (M2 scope: three buttons only).
            return true;
    }
    LiSendMouseButtonEvent(
        button.state == SDL_PRESSED ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
        remote_button);
    return true;
}

bool handle_mouse_wheel(const SDL_MouseWheelEvent& wheel) {
    // LiSendHighResScrollEvent exists in our moonlight-common-c; one wheel
    // notch is WHEEL_DELTA (120) on the host.
    LiSendHighResScrollEvent(static_cast<short>(wheel.y * 120));
    return true;
}

}  // namespace

void init(int stream_width, int stream_height) {
    g_stream_width = stream_width;
    g_stream_height = stream_height;
}

bool handle_event(const SDL_Event& event, Session& session, SDL_Window* window,
                  bool imgui_wants_kb, bool imgui_wants_mouse) {
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return handle_key_event(event.key, session, imgui_wants_kb);
    case SDL_MOUSEMOTION:
        if (imgui_wants_mouse) {
            return false;  // Overlay interaction (e.g. hovering the top bar).
        }
        return handle_mouse_motion(event.motion, window);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (imgui_wants_mouse) {
            return false;
        }
        return handle_mouse_button(event.button);
    case SDL_MOUSEWHEEL:
        if (imgui_wants_mouse) {
            return false;
        }
        return handle_mouse_wheel(event.wheel);
    default:
        return false;  // Not an input event; let ImGui and main.cpp see it.
    }
}

}  // namespace input
}  // namespace cosmic::viewer