// Cosmic Desk â€” PIN dialog implementation. ASCII-only strings: the default
// ImGui font has no glyphs beyond Basic Latin, so anything else renders as '?'.
//
// U4 restyle: the host-side PIN entry now matches the Bridge glass-panel look
// (docs/UI_MIGRATION.md) â€” kPanel fill, kPurple border, a border-tab title
// straddling the top border, and the Bridge fonts/colors for the copy, the PIN
// field and the OK/Deny buttons. The modal semantics and the OK/Deny return
// path via `result_ok` are unchanged.

#include "ui/pin_dialog.h"

#include "hostglue/pin_bridge.h"
#include "ui/bridge/design.h"
#include "ui/bridge/text.h"
#include "ui/fonts.h"
#include "ui/scale.h"

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
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    const float scale = cosmic::ui::scale();

    // Glass-panel frame: kPanel fill at 97% opacity, kPurple border at 55%,
    // square corners, generous padding (the Bridge panel look).
    const ImVec4 panel_bg = cosmic::ui::Rgba(kPanel);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(panel_bg.x, panel_bg.y, panel_bg.z, 0.97f));
    const ImVec4 purple = cosmic::ui::Rgba(kPurple);
    ImGui::PushStyleColor(ImGuiCol_Border,
                          ImVec4(purple.x, purple.y, purple.z, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(24.0f * scale, 26.0f * scale));

    // Intro copy (sans 12.5, kTextDim).
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    ImGui::TextUnformatted("A machine wants to pair with this host:");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // The requesting client, highlighted (mono 13, kCyan).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::TextUnformatted(client_name.c_str());
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Section label (mono 9.5, .26em, kCyan).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    cosmic::ui::TextSpaced("ENTER PIN SHOWN ON THE CLIENT:", 0.26f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Center the PIN field under the label.
    const float field_width = 120.0f * scale;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - field_width) * 0.5f);
    ImGui::SetNextItemWidth(field_width);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgba(kBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Rgba(kPurple));
    ImGui::InputText("##pin", pin_buffer, kPinBufferSize,
                     ImGuiInputTextFlags_CharsDecimal);
    const bool pin_focused = ImGui::IsItemFocused();
    ImGui::PopStyleColor(6);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    // Focus ring: kPurple border while the field is focused.
    if (pin_focused) {
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(item_min, item_max,
                                            ImGui::GetColorU32(Rgba(kPurple)),
                                            0.0f, 0, 1.0f * scale);
    }

    const bool has_four_digits = std::strlen(pin_buffer) == 4;

    ImGui::Separator();

    // Center the OK/Deny buttons.
    const float button_width = 80.0f * scale;
    const float button_spacing = 8.0f * scale;
    const float buttons_width = button_width * 2.0f + button_spacing;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttons_width) * 0.5f);

    ImGui::BeginDisabled(!has_four_digits);
    ImGui::PushStyleColor(ImGuiCol_Button, Rgba(kGreenBtn));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(kGreenHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(kGreenHover));
    if (ImGui::Button("OK", ImVec2(button_width, 0.0f))) {
        if (cosmic::pin_bridge::submit_pin(pin_buffer)) {
            result_ok = true;
            open = false;
        } else {
            pin_error = true;
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, button_spacing);

    ImGui::PushStyleColor(ImGuiCol_Button, Rgba(kRed));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(kRedHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(kRedHover));
    if (ImGui::Button("Deny", ImVec2(button_width, 0.0f))) {
        // The pairing request stays parked server-side; the client will time out
        // and can retry. There is nothing to clean up here.
        open = false;
    }
    ImGui::PopStyleColor(3);

    if (pin_error) {
        // Sticky error (sans 12.5, kRed â€” the design's error color).
        ImGui::PushFont(cosmic::ui::FontSansRegular());
        ImGui::SetWindowFontScale(12.5f / 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kRed));
        ImGui::TextWrapped(
            "Pairing failed. The request may have expired; ask the client to retry.");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
    }

    // Border-tab title "PAIRING REQUEST" (mono bold 12, .24em, kPurple) on a
    // kBg tab straddling the top border, like the Bridge panels. The window's
    // content clip rect ends at the window edge, so push a clip rect covering
    // the tab band above it (intersect=false replaces the clip entirely).
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(12.0f / 13.0f);
    const ImVec2 title_size = cosmic::ui::TextSpacedSize("PAIRING REQUEST", 0.24f);
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float tab_top = win_pos.y - 9.0f * scale;
    const float tab_h = 14.0f * scale;
    const float tab_cy = tab_top + tab_h * 0.5f;
    const float tab_x = win_pos.x + 8.0f * scale;
    const float tab_w = title_size.x + 24.0f * scale;
    ImGui::PushClipRect(ImVec2(tab_x, tab_top),
                        ImVec2(tab_x + tab_w, tab_top + tab_h), false);
    draw_list->AddRectFilled(ImVec2(tab_x, tab_top),
                             ImVec2(tab_x + tab_w, tab_top + tab_h),
                             ImGui::GetColorU32(Rgba(kBg)));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kPurple));
    ImGui::SetCursorScreenPos(ImVec2(tab_x + 12.0f * scale,
                                     tab_cy - title_size.y * 0.5f));
    cosmic::ui::TextSpaced("PAIRING REQUEST", 0.24f);
    ImGui::PopStyleColor();
    ImGui::PopClipRect();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);

    ImGui::EndPopup();
}

}  // namespace cosmic::ui
