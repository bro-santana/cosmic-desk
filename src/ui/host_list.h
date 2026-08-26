// Cosmic Desk — managed host list UI (plan M3.x). Replaces the old
// "Connect to host" IP field + "Recent:" list with a list the user maintains:
// pair a machine once, pick it by name, connect, nickname it, set a per-machine
// port override, or remove it. ASCII-only strings: the default ImGui font has
// no glyphs beyond Basic Latin, so anything else renders as '?'.

#pragma once

#include "app/settings.h"

#include <string>
#include <vector>

namespace cosmic::ui {

// Action the caller applies after the ImGui frame (same no-side-effects pattern
// as TopBarAction): structural changes and session calls must not run mid-frame.
struct HostListAction {
  enum Kind {
    None,
    Connect,     // address + port
    StartPair,   // address + port (0 = follow port_base) + optional nickname
                 // (empty = leave any existing nickname alone)
    CancelPair,  // stop an in-flight handshake; the modal stays open
    ClosePair,   // dismiss the Pair modal and clear its sticky error
    Remove,      // address
    Edit,        // address + nickname (may be empty = clear) + port
  } kind = None;
  std::string address;
  std::string nickname;
  int port = 0;
};

// Live pair-worker feedback for the Pair modal. main.cpp fills this from
// viewer::SessionStatus: the ui layer must not include viewer/session.h (same
// bridging as MonitorInfo / to_monitor_info in main.cpp).
struct PairProgress {
  bool active = false;      // a pair worker is running
  bool show_pin = false;    // state is PairingNeedPin/PairingInProgress
  std::string pin;          // 4 digits, shown large
  std::string message;
  std::string error;        // sticky; non-empty = the last attempt failed
};

// Caller-owned state, persists across frames.
struct HostListState {
  std::string selected;     // address of the selected row ("" = none)
  bool pair_modal_open = false;
  bool edit_modal_open = false;
  bool remove_modal_open = false;
  std::string modal_target; // address the edit/remove modal acts on
  char address_input[64] = {};
  char nickname_input[48] = {};
  int  port_input = 0;
  bool use_default_port = true;
};

// Draws the host list and its modals. Call once per frame, between the caller's
// ImGui::Begin()/End() — the modals open against the caller's window and its ID
// stack. Takes a const snapshot (not Settings&) so iterating it cannot be
// invalidated; Remove/Edit come back as actions. Returns the action to apply
// after ImGui::Render().
//   hosts         — settings.hosts_snapshot() for this frame.
//   pairing       — pair-worker feedback from the main-loop latch.
//   default_port  — settings.port_base (the "use default port" target).
//   session_busy  — g_session->busy(): disables Pair/Connect while a worker
//                    runs, so a click cannot silently no-op.
//   state         — persistent selection + modal state.
HostListAction draw_host_list(const std::vector<SavedHost>& hosts,
                              const PairProgress& pairing, int default_port,
                              bool session_busy, HostListState* state);

}  // namespace cosmic::ui
