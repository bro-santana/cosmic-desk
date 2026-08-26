// Cosmic Desk — viewer input forwarding (plan M2.6). SDL events -> LiSend*
// while streaming, plus the Ctrl+Alt+Shift+Q/Enter/Z escape combos. Pattern
// follows moonlight-qt app/streaming/input/{keyboard,mouse,input}.cpp (see
// VENDOR.md).

#pragma once

#include <SDL.h>

namespace cosmic::viewer {

namespace input {

// Escape-combo actions detected while streaming (plan M4.3). The caller
// applies them after the ImGui frame: input.cpp must not touch the window or
// the session mid-frame.
struct InputActions {
    bool quit = false;         // Ctrl+Alt+Shift+Q: end the session.
    bool fullscreen = false;   // Ctrl+Alt+Shift+Enter: toggle fullscreen.
    bool toggle_grab = false;  // Ctrl+Alt+Shift+Z: toggle the keyboard grab.
};

// Stores the negotiated stream dimensions (from SessionStatus) used to map
// window mouse coordinates into stream coordinates. Call once when the stream
// geometry is known (main.cpp, on entering Viewing mode).
void init(int stream_width, int stream_height);

// Forwards one SDL event to the host while streaming. Returns true if the
// event was consumed and must not reach ImGui. imgui_wants_kb / imgui_wants_mouse
// gate keyboard/mouse forwarding while the overlay is being interacted with;
// the Ctrl+Alt+Shift+Q (end session), Ctrl+Alt+Shift+Enter (fullscreen) and
// Ctrl+Alt+Shift+Z (toggle grab) escape combos stay active regardless. actions
// is set when a combo fires; the caller applies the change after the frame.
bool handle_event(const SDL_Event& event, SDL_Window* window,
                  bool imgui_wants_kb, bool imgui_wants_mouse,
                  InputActions* actions);

// Sends KEY_ACTION_UP for every key and BUTTON_ACTION_RELEASE for every mouse
// button currently held down in the remote session, then clears the tracked
// state. Called on grab-toggle-off and on window focus loss so the host does
// not keep stuck keys/buttons (moonlight-qt raiseAllKeys pattern). Main thread
// only, like handle_event().
void flush_input_state();

}  // namespace input
}  // namespace cosmic::viewer