// Cosmic Desk — viewer input forwarding (plan M2.6). SDL events -> LiSend*
// while streaming, plus the Ctrl+Alt+Shift+Q/Enter/Z escape combos. Pattern
// follows moonlight-qt app/streaming/input/{keyboard,mouse,input}.cpp: absolute
// mouse position (mapped into stream coordinates), button/scroll forwarding,
// and the escape-combo check on keydown.

#include "viewer/input.h"

#include "viewer/keymap.h"

#include <Limelight.h>

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace cosmic::viewer {
namespace input {
namespace {

int g_stream_width = 0;
int g_stream_height = 0;
float g_topbar_height = 0.0f;

// Keys and mouse buttons currently held down in the remote session (plan
// M4.3). Main-thread only, like the rest of this file: handle_event() updates
// them and flush_input_state() reads and clears them, both from the SDL event
// loop.
std::unordered_set<std::uint16_t> g_keys_down;
std::unordered_set<int> g_mouse_buttons_down;

// Ctrl+Alt+Shift+Q ends the session, +Enter toggles fullscreen, +Z toggles the
// keyboard grab (moonlight-qt KeyComboQuit / KeyComboToggleFullScreen /
// KeyComboUngrabInput pattern). Detection matches moonlight-qt's
// handleKeyEvent(): the combo is checked on keydown via keysym.mod, before any
// ImGui capture gate. Returns true and sets the matching action when a combo
// fires; the caller applies the action after the frame.
bool handle_escape_combo(const SDL_KeyboardEvent& key, InputActions* actions) {
    if (key.state != SDL_PRESSED || key.repeat ||
        (key.keysym.mod & KMOD_CTRL) == 0 ||
        (key.keysym.mod & KMOD_ALT) == 0 ||
        (key.keysym.mod & KMOD_SHIFT) == 0) {
        return false;
    }
    switch (key.keysym.scancode) {
        case SDL_SCANCODE_Q:
            actions->quit = true;
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            actions->fullscreen = true;
            return true;
        case SDL_SCANCODE_Z:
            actions->toggle_grab = true;
            return true;
        default:
            return false;
    }
}

bool handle_key_event(const SDL_KeyboardEvent& key, bool imgui_wants_kb,
                      InputActions* actions) {
    if (handle_escape_combo(key, actions)) {
        return true;
    }
    // Swallow the key-up that follows a combo (the modifiers are still held at
    // that point) so no stray VK_RETURN / VK_Z / VK_Q reaches the host.
    if ((key.keysym.scancode == SDL_SCANCODE_RETURN ||
         key.keysym.scancode == SDL_SCANCODE_KP_ENTER ||
         key.keysym.scancode == SDL_SCANCODE_Z ||
         key.keysym.scancode == SDL_SCANCODE_Q) &&
        (key.keysym.mod & KMOD_CTRL) != 0 && (key.keysym.mod & KMOD_ALT) != 0 &&
        (key.keysym.mod & KMOD_SHIFT) != 0) {
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
        // Track the key state so flush_input_state() can release anything
        // still held when the grab is dropped or focus is lost (moonlight-qt
        // m_KeysDown pattern).
        if (key.state == SDL_PRESSED) {
            g_keys_down.insert(keycode);
        } else {
            g_keys_down.erase(keycode);
        }
        // Modifier flags and the 0x8000 extended-key bit match moonlight-qt
        // keyboard.cpp:433 (v6.1.0). MODIFIER_META is skipped: the Win key
        // reaches the host as VK_LWIN/VK_RWIN via keymap.cpp.
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
    // The stream is letterboxed into the window below the reserved top bar
    // strip (mirrors vrenderer). Map the pointer within that visible rect into
    // stream coordinates; clicks on the letterbox bars or the strip clamp to
    // the nearest stream edge.
    const int top = static_cast<int>(g_topbar_height);
    const int area_w = window_w;
    const int area_h = window_h - top;
    int x = 0;
    int y = 0;
    if (area_h <= 0) {
        return true;
    }
    const float scale = std::min(static_cast<float>(area_w) / g_stream_width,
                                 static_cast<float>(area_h) / g_stream_height);
    const int vid_w = static_cast<int>(g_stream_width * scale + 0.5f);
    const int vid_h = static_cast<int>(g_stream_height * scale + 0.5f);
    const int vid_x = (area_w - vid_w) / 2;
    const int vid_y = top + (area_h - vid_h) / 2;
    x = (motion.x - vid_x) * g_stream_width / vid_w;
    y = (motion.y - vid_y) * g_stream_height / vid_h;
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
    // Track held buttons so flush_input_state() can release them when the
    // grab is dropped or focus is lost (moonlight-qt raiseAllKeys pattern).
    if (button.state == SDL_PRESSED) {
        g_mouse_buttons_down.insert(remote_button);
    } else {
        g_mouse_buttons_down.erase(remote_button);
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

void set_topbar_height(float height) {
    g_topbar_height = height;
}

bool handle_event(const SDL_Event& event, SDL_Window* window,
                  bool imgui_wants_kb, bool imgui_wants_mouse,
                  InputActions* actions) {
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return handle_key_event(event.key, imgui_wants_kb, actions);
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

void flush_input_state() {
    for (std::uint16_t vk : g_keys_down) {
        // Match the key-down code (0x8000 extended bit) so the host releases
        // the exact key that was pressed.
        LiSendKeyboardEvent(static_cast<short>(0x8000 | vk), KEY_ACTION_UP, 0);
    }
    g_keys_down.clear();
    for (int button : g_mouse_buttons_down) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    }
    g_mouse_buttons_down.clear();
}

// COSMIC MODIFICATION (M5): monitor-switch hotkey synthesis (docs/PROTOCOL.md).
// The host's input.cpp::apply_shortcut() fires switch_display on the F-key
// keydown while Ctrl+Alt+Shift are held, so we send the three modifiers down
// (VKEY_LSHIFT 0xA0, VKEY_LCONTROL 0xA2, VKEY_LMENU 0xA4 — the VKs
// update_shortcutFlags() tracks), then the F key, then release everything in
// reverse order. The trailing redundant modifier ups are the D3b/M5
// no-stuck-state mitigation: Sunshine's "Already released" path tolerates them.
void send_monitor_switch(int index) {
    if (index < 0 || index > 12) {
        return;  // F1..F13 only (docs/PROTOCOL.md).
    }
    const short modifiers[3] = {0xA2, 0xA4, 0xA0};  // LCONTROL, LMENU, LSHIFT
    const short f_key = static_cast<short>(0x70 + index);  // VK_F1 = 0x70
    for (short vk : modifiers) {
        LiSendKeyboardEvent(static_cast<short>(0x8000 | vk), KEY_ACTION_DOWN, 0);
    }
    LiSendKeyboardEvent(static_cast<short>(0x8000 | f_key), KEY_ACTION_DOWN, 0);
    LiSendKeyboardEvent(static_cast<short>(0x8000 | f_key), KEY_ACTION_UP, 0);
    for (int i = 2; i >= 0; --i) {
        LiSendKeyboardEvent(static_cast<short>(0x8000 | modifiers[i]),
                            KEY_ACTION_UP, 0);
    }
    // Explicit key-ups for all three modifiers once more, so no stuck
    // Ctrl/Alt/Shift state can leak into the host.
    for (short vk : modifiers) {
        LiSendKeyboardEvent(static_cast<short>(0x8000 | vk), KEY_ACTION_UP, 0);
    }
}

}  // namespace input
}  // namespace cosmic::viewer
