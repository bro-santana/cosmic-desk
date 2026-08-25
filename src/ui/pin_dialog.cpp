// Cosmic Desk — PIN dialog implementation. ASCII-only strings: the default
// ImGui font has no glyphs beyond Basic Latin, so anything else renders as '?'.

#include "ui/pin_dialog.h"

#include "hostglue/pin_bridge.h"

#include <imgui.h>

#include <cstring>

namespace cosmic::ui {
namespace {

// 4 digits + NUL terminator (ImGui InputText needs room for the terminator).
constexpr int kPinBufferSize = 5;

}  // namespace

void draw_pin_dialog(const std::string &client_name, bool &open, bool &result_ok) {
  static bool was_open = false;
  static char pin_buffer[kPinBufferSize] = {};
  static bool pin_error = false;

  if (open && !was_open) {
    // A fresh pairing request: start with a clean dialog.
    pin_buffer[0] = '\0';
    pin_error = false;
  }
  was_open = open;

  if (!open) {
    return;
  }

  ImGui::OpenPopup("Pairing request");

  if (!ImGui::BeginPopupModal(
        "Pairing request", &open,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }

  ImGui::TextUnformatted("A new client wants to connect.");
  ImGui::Text("Client: %s", client_name.c_str());
  ImGui::TextUnformatted("Enter the 4-digit PIN shown on the client:");

  // Center the PIN field under the text.
  const float field_width = 120.0f;
  ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - field_width) * 0.5f);
  ImGui::SetNextItemWidth(field_width);
  ImGui::InputText("##pin", pin_buffer, kPinBufferSize, ImGuiInputTextFlags_CharsDecimal);

  const bool has_four_digits = std::strlen(pin_buffer) == 4;

  ImGui::Separator();

  // Center the OK/Cancel buttons.
  const float button_width = 80.0f;
  const float button_spacing = 8.0f;
  const float buttons_width = button_width * 2.0f + button_spacing;
  ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttons_width) * 0.5f);

  ImGui::BeginDisabled(!has_four_digits);
  if (ImGui::Button("OK", ImVec2(button_width, 0.0f))) {
    if (cosmic::pin_bridge::submit_pin(pin_buffer)) {
      result_ok = true;
      open = false;
    } else {
      pin_error = true;
    }
  }
  ImGui::EndDisabled();

  ImGui::SameLine(0.0f, button_spacing);

  if (ImGui::Button("Cancel", ImVec2(button_width, 0.0f))) {
    // The pairing request stays parked server-side; the client will time out
    // and can retry. There is nothing to clean up here.
    open = false;
  }

  if (pin_error) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
    ImGui::TextWrapped("Pairing failed. The request may have expired; ask the client to retry.");
    ImGui::PopStyleColor();
  }

  ImGui::EndPopup();
}

}  // namespace cosmic::ui