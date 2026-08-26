// Cosmic Desk — viewer input forwarding (plan M2.6). SDL events -> LiSend*
// while streaming, plus the Ctrl+Alt+Shift+Q escape combo. Pattern follows
// moonlight-qt app/streaming/input/{keyboard,mouse,input}.cpp (see VENDOR.md).

#pragma once

#include <SDL.h>

namespace cosmic::viewer {

class Session;

namespace input {

// Stores the negotiated stream dimensions (from SessionStatus) used to map
// window mouse coordinates into stream coordinates. Call once when the stream
// geometry is known (main.cpp, on entering Viewing mode).
void init(int stream_width, int stream_height);

// Forwards one SDL event to the host while streaming. Returns true if the
// event was consumed and must not reach ImGui. imgui_wants_kb / imgui_wants_mouse
// gate keyboard/mouse forwarding while the overlay is being interacted with;
// the Ctrl+Alt+Shift+Q escape combo stays active regardless.
bool handle_event(const SDL_Event& event, Session& session, SDL_Window* window,
                  bool imgui_wants_kb, bool imgui_wants_mouse);

}  // namespace input
}  // namespace cosmic::viewer