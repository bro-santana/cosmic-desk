// Cosmic Desk — managed host list UI implementation. ASCII-only strings: the
// default ImGui font has no glyphs beyond Basic Latin, so anything else renders
// as '?'. No side effects: every user action is returned as a HostListAction
// that main.cpp applies after ImGui::Render().

#include "ui/host_list.h"

#include "ui/scale.h"
#include "ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace cosmic::ui {
namespace {

constexpr int kPortMin = 1024;
constexpr int kPortMax = 65400;

// ASCII filter: the default ProggyClean atlas has only Basic Latin, so a pasted
// non-ASCII character would render as '?' forever.
int ascii_filter(ImGuiInputTextCallbackData* data) {
  if (data->EventChar < 0x20 || data->EventChar > 0x7E) {
    return 1;  // Reject the character.
  }
  return 0;
}

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// Visible row label: "Nickname  (address)" or "address"; append ":port" when a
// port override is set and "  [not paired]" when !host.paired.
std::string host_label(const SavedHost& host) {
  std::string label;
  if (!host.nickname.empty()) {
    label = host.nickname + "  (" + host.address + ")";
  } else {
    label = host.address;
  }
  if (host.port > 0) {
    label += ":" + std::to_string(host.port);
  }
  if (!host.paired) {
    label += "  [not paired]";
  }
  return label;
}

void port_label(char* buf, int size, int default_port) {
  std::snprintf(buf, size, "Use default port (%d)", default_port);
}

}  // namespace

HostListAction draw_host_list(const std::vector<SavedHost>& hosts,
                              const PairProgress& pairing, int default_port,
                              bool session_busy, HostListState* state) {
  HostListAction action;

  // Self-healing selection: if this frame's snapshot no longer contains the
  // selected address (a removal or an external config change), clear it so a
  // stale selection can never drive a Connect.
  if (!state->selected.empty()) {
    bool found = false;
    for (const SavedHost& h : hosts) {
      if (h.address == state->selected) {
        found = true;
        break;
      }
    }
    if (!found) {
      state->selected.clear();
    }
  }

  ImGui::TextUnformatted("Machines");
  ImGui::BeginChild("##hostlist", ImVec2(0.0f, 160.0f * scale()),
                    ImGuiChildFlags_Borders);
  if (hosts.empty()) {
    ImGui::TextDisabled("No machines yet. Click Pair to add one.");
  }
  for (const SavedHost& host : hosts) {
    // "###<address>" pins the widget ID to the address, so renaming a machine
    // does not change its ID mid-click and lose the active/held state.
    const std::string id = "###" + host.address;
    const std::string label = host_label(host) + id;
    const bool is_selected = (host.address == state->selected);
    if (ImGui::Selectable(label.c_str(), is_selected,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
      state->selected = host.address;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !session_busy) {
        action.kind = HostListAction::Connect;
        action.address = host.address;
      }
    }
  }
  ImGui::EndChild();  // Unconditional, unlike EndPopup which is conditional.

  const bool has_selection = !state->selected.empty();

  ImGui::BeginDisabled(session_busy);
  if (ImGui::Button("Pair...")) {
    state->pair_modal_open = true;
    // nickname_input/port_input/use_default_port are shared with the Edit
    // modal, so reset them here or the Pair dialog inherits whatever the last
    // Edit left behind. (Only one modal can be open at a time -- ImGui blocks
    // the parent window -- so one set of buffers is enough.)
    state->nickname_input[0] = '\0';
    state->port_input = 0;
    state->use_default_port = true;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::BeginDisabled(!has_selection || session_busy);
  if (AccentButton("Connect", Accent::Positive)) {
    action.kind = HostListAction::Connect;
    action.address = state->selected;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::BeginDisabled(!has_selection);
  if (ImGui::Button("Edit...")) {
    // Prefill once, in the button handler: prefilling every frame would swallow
    // typing. Load the target's nickname and port override from this snapshot.
    state->edit_modal_open = true;
    state->modal_target = state->selected;
    state->nickname_input[0] = '\0';
    state->port_input = 0;
    state->use_default_port = true;
    for (const SavedHost& h : hosts) {
      if (h.address == state->selected) {
        std::snprintf(state->nickname_input, sizeof(state->nickname_input),
                      "%s", h.nickname.c_str());
        if (h.port > 0) {
          state->port_input = h.port;
          state->use_default_port = false;
        }
        break;
      }
    }
  }
  ImGui::SameLine();
  if (AccentButton("Remove", Accent::Destructive)) {
    state->remove_modal_open = true;
    state->modal_target = state->selected;
  }
  ImGui::EndDisabled();

  // The three popups are opened against the caller's window and its ID stack.
  // OpenPopup and BeginPopupModal both hash the name against the stack of the
  // current window at call time, so all six calls sit here at the top level of
  // draw_host_list (after EndChild, outside any PushID), and the row loop and
  // button handlers above only ever set the state->*_modal_open /
  // modal_target fields. BeginPopupModal must be called every frame while open,
  // so OpenPopup is driven unconditionally from the open bool (re-opening is a
  // no-op). Closing, however, is NOT symmetric: OpenPopup is a one-shot trigger
  // that pushes the popup onto ImGui's stack, and clearing the bool merely
  // stops re-triggering it -- only CloseCurrentPopup() pops it. Every dismissal
  // path below therefore calls it, and each modal also calls it on entry when
  // its bool was cleared from outside.

  // Pair a machine. p_open is nullptr so Esc/X cannot dismiss it mid-handshake;
  // dismissal is driven by the Close button through the ClosePair action, which
  // also clears the sticky error. The address/port fields are always present
  // (merely disabled while the handshake runs) rather than swapped out for the
  // progress view: hiding them behind a feedback-only branch left the dialog
  // with no way back to the input state once an attempt had failed.
  if (state->pair_modal_open) {
    ImGui::OpenPopup("Pair a machine");
  }
  if (ImGui::BeginPopupModal(
          "Pair a machine", nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    // OpenPopup is a one-shot trigger: it pushes the popup onto ImGui's stack,
    // and simply not calling it again does NOT pop it. Only CloseCurrentPopup()
    // does. This handles a close driven from outside draw_host_list (main.cpp
    // clears the flag when the handshake succeeds); the buttons below close
    // themselves on the same frame.
    if (!state->pair_modal_open) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextUnformatted("Address of the machine to pair with:");
    ImGui::BeginDisabled(pairing.active);
    ImGui::SetNextItemWidth(200.0f * scale());
    ImGui::InputText("Address", state->address_input,
                     sizeof(state->address_input),
                     ImGuiInputTextFlags_CallbackCharFilter, &ascii_filter);
    // Naming the machine here saves a trip through Edit... straight after
    // pairing. Optional: left empty, the list falls back to the address, and a
    // re-pair of a machine that already has a nickname keeps it (main.cpp only
    // applies a non-empty one).
    ImGui::SetNextItemWidth(200.0f * scale());
    ImGui::InputText("Nickname (optional)", state->nickname_input,
                     sizeof(state->nickname_input),
                     ImGuiInputTextFlags_CallbackCharFilter, &ascii_filter);
    char port_label_buf[64];
    port_label(port_label_buf, sizeof(port_label_buf), default_port);
    ImGui::Checkbox(port_label_buf, &state->use_default_port);
    if (!state->use_default_port) {
      ImGui::SetNextItemWidth(200.0f * scale());
      if (ImGui::InputInt("Port", &state->port_input)) {
        state->port_input = std::clamp(state->port_input, kPortMin, kPortMax);
      }
    }
    ImGui::EndDisabled();

    if (pairing.active) {
      if (pairing.show_pin && !pairing.pin.empty()) {
        ImGui::TextUnformatted("Enter this PIN on the host:");
        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextUnformatted(pairing.pin.c_str());
        ImGui::SetWindowFontScale(1.0f);
      } else if (!pairing.message.empty()) {
        ImGui::TextWrapped("%s", pairing.message.c_str());
      }
    }
    if (!pairing.error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
      ImGui::TextWrapped("%s", pairing.error.c_str());
      ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (pairing.active) {
      if (ImGui::Button("Cancel")) {
        action.kind = HostListAction::CancelPair;
      }
      ImGui::TextWrapped(
          "Cancelling may take a moment: the request is parked on the host.");
    } else {
      // session_busy as well as the empty address: a cancelled handshake can
      // still be parked in gs_pair for minutes, and start_pair() would silently
      // no-op for as long as that worker lives.
      ImGui::BeginDisabled(state->address_input[0] == '\0' || session_busy);
      if (ImGui::Button("Pair")) {
        action.kind = HostListAction::StartPair;
        action.address = trim(state->address_input);
        action.nickname = trim(state->nickname_input);
        action.port = state->use_default_port ? 0 : state->port_input;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Close")) {
        action.kind = HostListAction::ClosePair;
        state->pair_modal_open = false;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }

  // Edit machine: nickname + port override. OK is not disabled on an empty
  // nickname — empty is the "clear it" gesture.
  if (state->edit_modal_open) {
    ImGui::OpenPopup("Edit machine");
  }
  if (ImGui::BeginPopupModal(
          "Edit machine", nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!state->edit_modal_open) {
      ImGui::CloseCurrentPopup();  // See the note in the Pair modal above.
    }
    ImGui::TextUnformatted("Edit machine");
    ImGui::SetNextItemWidth(200.0f * scale());
    ImGui::InputText("Nickname", state->nickname_input,
                     sizeof(state->nickname_input),
                     ImGuiInputTextFlags_CallbackCharFilter, &ascii_filter);
    char port_label_buf[64];
    port_label(port_label_buf, sizeof(port_label_buf), default_port);
    ImGui::Checkbox(port_label_buf, &state->use_default_port);
    if (!state->use_default_port) {
      ImGui::SetNextItemWidth(200.0f * scale());
      if (ImGui::InputInt("Port", &state->port_input)) {
        state->port_input = std::clamp(state->port_input, kPortMin, kPortMax);
      }
    }
    ImGui::Separator();
    if (ImGui::Button("OK")) {
      action.kind = HostListAction::Edit;
      action.address = state->modal_target;
      action.nickname = trim(state->nickname_input);
      action.port = state->use_default_port ? 0 : state->port_input;
      state->edit_modal_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      state->edit_modal_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // Remove machine: local forget only. The host registers no /unpair route, so
  // it keeps listing this client as paired until removed there.
  if (state->remove_modal_open) {
    ImGui::OpenPopup("Remove machine");
  }
  if (ImGui::BeginPopupModal(
          "Remove machine", nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!state->remove_modal_open) {
      ImGui::CloseCurrentPopup();  // See the note in the Pair modal above.
    }
    std::string target_name = state->modal_target;
    for (const SavedHost& h : hosts) {
      if (h.address == state->modal_target) {
        if (!h.nickname.empty()) {
          target_name = h.nickname;
        }
        break;
      }
    }
    ImGui::Text("Remove %s?", target_name.c_str());
    ImGui::TextWrapped(
        "This only forgets it on this machine. The host keeps this client "
        "paired until it is removed there; re-Pair if needed.");
    ImGui::Separator();
    if (AccentButton("Remove", Accent::Destructive)) {
      action.kind = HostListAction::Remove;
      action.address = state->modal_target;
      state->remove_modal_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      state->remove_modal_open = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  return action;
}

}  // namespace cosmic::ui
