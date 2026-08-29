// Cosmic Desk — Bridge UI panels implementation (docs/UI_MIGRATION.md U4).
//
// The Settings panel: a floating glass child window pinned to the bridge
// window at right 4% / top 10% of the viewport, 348 design px wide. The frame
// (bg + border + border-tab title) is drawn manually on the parent draw list,
// exactly like the machine cards, so the tab can straddle the top border and
// the perimeter is not clipped by the child's padded clip rect; the widgets
// live inside a transparent child pinned "##settingspanel".
//
// Values are emitted as BridgeAction Set* actions ONLY when the user changes
// them (ImGui widget return values); main.cpp applies them to Settings
// immediately and marks settings dirty — save() fires on the panel-close
// transition (plus the shutdown save), so steppers/sliders never hit disk
// per tick.

#include "ui/bridge/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ui/bridge/bridge.h"
#include "ui/bridge/design.h"
#include "ui/bridge/scene.h"
#include "ui/bridge/text.h"
#include "ui/fonts.h"
#include "ui/scale.h"

namespace cosmic::ui::bridge {
namespace {

// Panel geometry (design px; handoff README "Settings").
constexpr float kPanelW = 348.0f;
constexpr float kPanelPadX = 24.0f;
constexpr float kPanelPadY = 22.0f;
constexpr float kSectionGap = 16.0f;   // gap between stacked sections
constexpr float kLabelGap = 8.0f;      // gap between a section label and its control
constexpr float kSegH = 30.0f;         // segmented resolution buttons
constexpr float kStepperBtnW = 30.0f;  // -/+ buttons
constexpr float kStepperH = 32.0f;
constexpr float kCloseSize = 26.0f;    // X close button
constexpr float kToggleW = 40.0f;      // toggle rows (autostart, share wallpaper)
constexpr float kToggleH = 20.0f;
constexpr float kToggleRadius = 12.0f;  // pill corner radius
constexpr float kKnobR = 7.0f;          // toggle knob radius (14 px diameter)

// Resolution segments, in ResolutionMode enum order (Custom stays settable via
// cosmic.json, matching the old window).
const char* kResolutionLabels[] = {"NATIVE", "1080P", "1440P", "4K"};

// Footnote, wrapped at the design's break point: the full string is ~415
// design px at mono 9/.1em tracking, wider than the 300 px content column.
const char* kFootnoteLines[] = {
    "ALL SIX PORTS DERIVE FROM PORT BASE ·",
    "RESTART TO APPLY TO HOSTING",
};

constexpr int kFpsMin = 10;
constexpr int kFpsMax = 240;
constexpr int kFpsStep = 10;
constexpr int kPortMin = 1024;
constexpr int kPortMax = 65400;
constexpr int kBitrateMbpsMin = 5;
constexpr int kBitrateMbpsMax = 150;

// --- U4: Pair modal + PIN panel (design px; handoff README "Pair modal") ---

constexpr float kPairModalW = 372.0f;
constexpr float kPairModalPadX = 24.0f;
constexpr float kPairModalPadY = 26.0f;
constexpr float kPairModalGap = 14.0f;   // column gap between stacked rows
constexpr float kInputLabelGap = 6.0f;   // gap between an input's label and the input
constexpr float kCheckboxSize = 15.0f;   // default-port checkbox square
constexpr float kCheckboxMark = 5.0f;    // kBg check square inside the checkbox

// Slide/scrim timings (design: modal slides over .8s, scrim fades over ~.6s,
// PIN panel slides up over .7s).
constexpr float kModalSlideS = 0.8f;
constexpr float kScrimFadeS = 0.6f;
constexpr float kPinSlideS = 0.7f;
constexpr float kSweepPeriodS = 2.4f;  // PIN panel scanline sweep period

// ASCII filter: the default ImGui font has no glyphs beyond Basic Latin, so a
// pasted non-ASCII character would render as '?' forever.
int ascii_filter(ImGuiInputTextCallbackData* data) {
    if (data->EventChar < 0x20 || data->EventChar > 0x7E) {
        return 1;  // Reject the character.
    }
    return 0;
}

// Trims whitespace from both ends.
std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// easeOutCubic: 1-(1-p)^3, the design's approximation for the modal slide, the
// scrim fade and the PIN panel slide-up.
float EaseOutCubic(float p) {
    return 1.0f - std::pow(1.0f - p, 3.0f);
}

// Draws a button as a filled/bordered rect + centered TextSpaced label with an
// InvisibleButton hit test. Colors are ImU32 (GetColorU32(Rgba(token)) results,
// so callers can alpha-mod a token); a zero alpha skips the fill/border.
// Mirrors bridge.cpp's file-local DrawButton — panels.cpp keeps its own copy
// rather than promoting the helper. `hovered_out` (when given) receives the
// InvisibleButton's hover state so callers can draw glyph primitives in the
// right color.
bool DrawButton(const char* id, const char* label, float tracking_em,
                const ImVec2& pos, const ImVec2& size, ImU32 bg,
                ImU32 bg_hover, ImU32 border, ImU32 border_hover,
                ImU32 text, ImU32 text_hover, float scale,
                bool* hovered_out = nullptr) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    if (hovered_out != nullptr) {
        *hovered_out = hovered;
    }
    const ImU32 bg_col = hovered ? bg_hover : bg;
    const ImU32 border_col = hovered ? border_hover : border;
    const ImU32 text_col = hovered ? text_hover : text;
    if ((bg_col & IM_COL32_A_MASK) != 0) {
        draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg_col);
    }
    if ((border_col & IM_COL32_A_MASK) != 0) {
        draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border_col,
                           0.0f, 0, 1.0f * scale);
    }
    const ImVec2 label_size = cosmic::ui::TextSpacedSize(label, tracking_em);
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + (size.x - label_size.x) * 0.5f,
                                     pos.y + (size.y - label_size.y) * 0.5f));
    cosmic::ui::TextSpaced(label, tracking_em);
    ImGui::PopStyleColor();
    return clicked;
}

// Draws one -/+ stepper: 30 px buttons around a centered mono 13 value.
// `value` is the current value; a click steps by `step` within [min,max] and
// emits `kind` with the new value (only when the value actually changes).
// The value itself is click-to-edit: clicking it swaps in an InputText
// (digits only), Enter or focus loss commits (clamped to [min,max] but NOT
// snapped to `step` — the point of typing is arbitrary values like 72 fps),
// Esc cancels. Edit state lives in BridgeState so it survives frames.
void DrawStepper(const char* id, int value, int step, int min, int max,
                 BridgeAction::Kind kind, const ImVec2& pos, float col_w,
                 float scale, BridgeState* state, BridgeAction* out_action) {
    const ImVec2 btn_size(kStepperBtnW * scale, kStepperH * scale);
    const ImU32 border = ImGui::GetColorU32(Rgba(kBorder));
    const ImU32 border_hover = ImGui::GetColorU32(Rgba(kPurple));
    const ImU32 text = ImGui::GetColorU32(Rgba(kTextDim));
    const ImU32 text_hover = ImGui::GetColorU32(Rgba(kText));
    char minus_id[16];
    char plus_id[16];
    std::snprintf(minus_id, sizeof(minus_id), "##%s_minus", id);
    std::snprintf(plus_id, sizeof(plus_id), "##%s_plus", id);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    if (DrawButton(minus_id, "-", 0.0f, pos, btn_size, 0, 0, border,
                   border_hover, text, text_hover, scale)) {
        const int next = std::max(min, value - step);
        if (next != value) {
            out_action->kind = kind;
            out_action->value = next;
        }
    }
    if (DrawButton(plus_id, "+", 0.0f,
                   ImVec2(pos.x + col_w - btn_size.x, pos.y), btn_size, 0, 0,
                   border, border_hover, text, text_hover, scale)) {
        const int next = std::min(max, value + step);
        if (next != value) {
            out_action->kind = kind;
            out_action->value = next;
        }
    }
    // Value, mono 13, centered between the buttons — plain text normally, an
    // InputText while this stepper is being edited.
    const float value_w = col_w - 2.0f * btn_size.x;
    const ImVec2 value_pos(pos.x + btn_size.x, pos.y);
    // The hotspot and the InputText need DISTINCT ImGui IDs: sharing one makes
    // the hotspot's press-active state read as the InputText deactivating on
    // its first frame, which insta-commits and exits edit mode.
    char edit_id[24];
    char hot_id[24];
    std::snprintf(edit_id, sizeof(edit_id), "##%s_edit", id);
    std::snprintf(hot_id, sizeof(hot_id), "##%s_hot", id);
    if (state->editing_stepper == id) {
        // Frame padding sized so the input's frame height equals the stepper
        // row; styled like the Pair modal inputs (kPanelInput bg, kPurple
        // focus border drawn manually after the item).
        const float pad_y =
            std::max(0.0f, (kStepperH * scale - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(6.0f * scale, pad_y));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
        ImGui::SetCursorScreenPos(value_pos);
        ImGui::SetNextItemWidth(value_w);
        // Focus the input once, on the frame after the value was clicked
        // (same ImGuiStorage pattern as the card rename in bridge.cpp).
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID focus_key = ImGui::GetID(edit_id);
        if (storage->GetInt(focus_key, 0) == 0) {
            ImGui::SetKeyboardFocusHere();
            storage->SetInt(focus_key, 1);
        }
        const bool entered = ImGui::InputText(
            edit_id, state->stepper_edit_buf, sizeof(state->stepper_edit_buf),
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll |
                ImGuiInputTextFlags_EnterReturnsTrue);
        const bool escaped = ImGui::IsKeyPressed(ImGuiKey_Escape);
        const bool deactivated = ImGui::IsItemDeactivated();
        // kPurple focus rect over the frame, like the Pair modal inputs.
        ImGui::GetWindowDrawList()->AddRect(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            ImGui::GetColorU32(Rgba(kPurple)), 0.0f, 0, 1.0f * scale);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        if (escaped) {
            state->editing_stepper.clear();
            storage->SetInt(focus_key, 0);
        } else if (entered || deactivated) {
            const long parsed = std::strtol(state->stepper_edit_buf, nullptr, 10);
            if (state->stepper_edit_buf[0] != '\0') {
                const int next = static_cast<int>(
                    std::clamp(parsed, static_cast<long>(min), static_cast<long>(max)));
                if (next != value) {
                    out_action->kind = kind;
                    out_action->value = next;
                }
            }
            state->editing_stepper.clear();
            storage->SetInt(focus_key, 0);
        }
    } else {
        const std::string value_text = std::to_string(value);
        const ImVec2 value_size = cosmic::ui::TextSpacedSize(value_text.c_str(), 0.0f);
        // Click-to-edit hotspot over the whole value area.
        ImGui::SetCursorScreenPos(value_pos);
        ImGui::InvisibleButton(hot_id, ImVec2(value_w, kStepperH * scale));
        const bool value_hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            std::snprintf(state->stepper_edit_buf, sizeof(state->stepper_edit_buf),
                          "%d", value);
            state->editing_stepper = id;
        }
        if (value_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        ImGui::PushStyleColor(ImGuiCol_Text,
                              value_hovered ? Rgba(kPurple) : Rgba(kText));
        ImGui::SetCursorScreenPos(
            ImVec2(value_pos.x + (value_w - value_size.x) * 0.5f,
                   pos.y + (kStepperH * scale - value_size.y) * 0.5f));
        cosmic::ui::TextSpaced(value_text.c_str(), 0.0f);
        ImGui::PopStyleColor();
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
}

}  // namespace

void draw_settings_panel(const BridgeInput& in, BridgeState* state, BridgeAction* out_action) {
    if (!state->settings_open) {
        // Drop any in-progress inline stepper edit so reopening the panel
        // starts clean instead of resuming a stale, unfocused input.
        state->editing_stepper.clear();
        return;
    }
    const float scale = cosmic::ui::scale();
    const ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    const float w = kPanelW * scale;
    const float pad_x = kPanelPadX * scale;
    const float pad_y = kPanelPadY * scale;
    const float content_w = w - 2.0f * pad_x;

    // Panel position: right 4% / top 10% of the viewport. Fixed — the depth-70
    // parallax translation is decorative and deliberately not applied.
    const ImVec2 pos(vp_size.x - 0.04f * vp_size.x - w, 0.10f * vp_size.y);
    const float content_x = pos.x + pad_x;
    const float content_right = pos.x + w - pad_x;

    // Section label height (mono 9.5 design px), measured at the label font.
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    const float label_h = cosmic::ui::TextSpacedSize("RESOLUTION", 0.26f).y;
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Slider height: the only widget whose height is not fixed. Measured with
    // the same font/padding the slider will use.
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 7.0f * scale));
    const float slider_h = ImGui::GetFrameHeight();
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Toggle row height (autostart + share wallpaper): the toggle (20 px) is
    // taller than the copy (12.5 px).
    const float copy_h = 12.5f * scale;
    const float toggle_row_h = std::max(kToggleH * scale, copy_h);

    // Footnote height: two lines at 9 px text + 3 px leading.
    const float footnote_h = IM_ARRAYSIZE(kFootnoteLines) * 12.0f * scale;

    // Content height: top padding + sections + bottom padding.
    const float h =
        pad_y + label_h + kLabelGap * scale + kSegH * scale + kSectionGap * scale +
        label_h + kLabelGap * scale + kStepperH * scale + kSectionGap * scale +
        label_h + kLabelGap * scale + slider_h + kSectionGap * scale +
        toggle_row_h + kSectionGap * scale + toggle_row_h + kSectionGap * scale +
        footnote_h + pad_y;

    // Panel background (parent draw list, before the child): rgba(20,23,46,.97)
    // — kPanel at 97% opacity.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec4 panel_bg = cosmic::ui::Rgba(kPanel);
    draw_list->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                             ImGui::GetColorU32(ImVec4(panel_bg.x, panel_bg.y, panel_bg.z, 0.97f)));

    // Transparent child pinned "##settingspanel"; widgets are positioned with
    // SetCursorScreenPos. Zero window padding so the clip rect covers the full
    // panel (the X close button sits in the top padding band).
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##settingspanel", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    float y = pos.y + pad_y;

    // --- RESOLUTION ---
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("RESOLUTION", 0.26f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += label_h + kLabelGap * scale;

    // Segmented NATIVE/1080P/1440P/4K buttons (flush, equal widths). Selected:
    // green fill/border; unselected: input fill, purple border on hover.
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    const float seg_w = content_w / IM_ARRAYSIZE(kResolutionLabels);
    const ImU32 seg_selected_bg = ImGui::GetColorU32(Rgba(kGreenSel));
    const ImVec4 green = cosmic::ui::Rgba(kGreen);
    const ImU32 seg_selected_border =
        ImGui::GetColorU32(ImVec4(green.x, green.y, green.z, 0.8f));
    const ImU32 seg_bg = ImGui::GetColorU32(Rgba(kPanelInput));
    const ImU32 seg_border = ImGui::GetColorU32(Rgba(kBorder));
    const ImU32 seg_text = ImGui::GetColorU32(Rgba(kText));
    const ImU32 seg_text_dim = ImGui::GetColorU32(Rgba(kTextDim));
    const ImU32 hover_border = ImGui::GetColorU32(Rgba(kPurple));
    for (int i = 0; i < IM_ARRAYSIZE(kResolutionLabels); ++i) {
        const ResolutionMode mode = static_cast<ResolutionMode>(i);
        const bool selected = in.resolution_mode == mode;
        char id[16];
        std::snprintf(id, sizeof(id), "##res%d", i);
        if (DrawButton(id, kResolutionLabels[i], 0.06f,
                       ImVec2(content_x + i * seg_w, y), ImVec2(seg_w, kSegH * scale),
                       selected ? seg_selected_bg : seg_bg,
                       selected ? seg_selected_bg : seg_bg,
                       selected ? seg_selected_border : seg_border,
                       selected ? seg_selected_border : hover_border,
                       selected ? seg_text : seg_text_dim,
                       seg_text, scale)) {
            out_action->kind = BridgeAction::SetResolution;
            out_action->resolution = mode;
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += kSegH * scale + kSectionGap * scale;

    // --- FPS / PORT BASE (two steppers side by side) ---
    const float col_gap = kSectionGap * scale;
    const float col_w = (content_w - col_gap) * 0.5f;
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("FPS", 0.26f);
    ImGui::SetCursorScreenPos(ImVec2(content_x + col_w + col_gap, y));
    cosmic::ui::TextSpaced("PORT BASE", 0.26f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += label_h + kLabelGap * scale;

    DrawStepper("fps", in.fps, kFpsStep, kFpsMin, kFpsMax, BridgeAction::SetFps,
                ImVec2(content_x, y), col_w, scale, state, out_action);
    DrawStepper("port", in.port_base, 1, kPortMin, kPortMax, BridgeAction::SetPortBase,
                ImVec2(content_x + col_w + col_gap, y), col_w, scale, state, out_action);
    y += kStepperH * scale + kSectionGap * scale;

    // --- BITRATE ---
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("BITRATE", 0.26f);
    ImGui::PopStyleColor();
    // Value, right-aligned, kAmber ("20 MBPS").
    const std::string bitrate_text = std::to_string(in.bitrate_kbps / 1000) + " MBPS";
    const ImVec2 bitrate_size = cosmic::ui::TextSpacedSize(bitrate_text.c_str(), 0.1f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kAmber));
    ImGui::SetCursorScreenPos(ImVec2(content_right - bitrate_size.x, y));
    cosmic::ui::TextSpaced(bitrate_text.c_str(), 0.1f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += label_h + kLabelGap * scale;

    // Slider: edits Mbps, the stored value is kbps (×1000).
    int mbps = std::clamp(in.bitrate_kbps / 1000, kBitrateMbpsMin, kBitrateMbpsMax);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 7.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Rgba(kPurple));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Rgba(kLavenderDeep));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    ImGui::SetNextItemWidth(content_w);
    if (ImGui::SliderInt("##bitrate", &mbps, kBitrateMbpsMin, kBitrateMbpsMax, "")) {
        out_action->kind = BridgeAction::SetBitrate;
        out_action->value = mbps * 1000;
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += slider_h + kSectionGap * scale;

    // --- AUTOSTART ---
    // Locked in service mode: cosmicsvc already starts the host at boot, and
    // the HKCU Run key autostart.cpp writes would land in the SYSTEM profile's
    // hive (we run as SYSTEM there), never the logged-on user's. No hit test
    // is registered so the row cannot emit SetAutostart at all.
    const bool locked = in.service_mode;
    const bool toggle_on = in.autostart && !locked;
    const float toggle_x = content_x;
    const float toggle_y = y + (toggle_row_h - kToggleH * scale) * 0.5f;
    if (!locked) {
        ImGui::SetCursorScreenPos(ImVec2(toggle_x, toggle_y));
        ImGui::InvisibleButton("##autostart", ImVec2(kToggleW * scale, kToggleH * scale));
        if (ImGui::IsItemClicked()) {
            out_action->kind = BridgeAction::SetAutostart;
            out_action->on = !in.autostart;
        }
    }
    // Pill: kGreenBtn when on, kPanelInput when off; 14 px knob, rounded 12 px.
    // Locked reads as off and inert: kPanelInput fill with a kMuted knob.
    ImDrawList* child_list = ImGui::GetWindowDrawList();
    const ImU32 toggle_bg = toggle_on ? ImGui::GetColorU32(Rgba(kGreenBtn))
                                      : ImGui::GetColorU32(Rgba(kPanelInput));
    child_list->AddRectFilled(ImVec2(toggle_x, toggle_y),
                              ImVec2(toggle_x + kToggleW * scale, toggle_y + kToggleH * scale),
                              toggle_bg, kToggleRadius * scale);
    const float knob_cx = toggle_on ? toggle_x + (kToggleW - 10.0f) * scale
                                    : toggle_x + 10.0f * scale;
    child_list->AddCircleFilled(ImVec2(knob_cx, toggle_y + kToggleH * scale * 0.5f),
                                kKnobR * scale,
                                ImGui::GetColorU32(Rgba(locked ? kMuted : kText)));

    // Copy, sans 12.5, kTextDim (kMuted when locked), vertically centered on
    // the toggle. Both strings are one line at the 248 px copy column.
    const char* copy = locked ? "Autostart - handled by the service"
                              : "Autostart - launch to tray on login";
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    const ImVec2 copy_size = cosmic::ui::TextSpacedSize(copy, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(locked ? kMuted : kTextDim));
    ImGui::SetCursorScreenPos(ImVec2(toggle_x + kToggleW * scale + 12.0f * scale,
                                     y + (toggle_row_h - copy_size.y) * 0.5f));
    cosmic::ui::TextSpaced(copy, 0.0f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += toggle_row_h + kSectionGap * scale;

    // --- SHARE WALLPAPER ---
    // No service-mode lock: the wallpaper share opt-out is independent of who
    // started the host, so the row is always interactive (PLAN.md D10).
    {
        const bool toggle_on = in.share_wallpaper;
        const float toggle_x = content_x;
        const float toggle_y = y + (toggle_row_h - kToggleH * scale) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(toggle_x, toggle_y));
        ImGui::InvisibleButton("##share_wallpaper", ImVec2(kToggleW * scale, kToggleH * scale));
        if (ImGui::IsItemClicked()) {
            out_action->kind = BridgeAction::SetShareWallpaper;
            out_action->on = !in.share_wallpaper;
        }
        // Pill: kGreenBtn when on, kPanelInput when off; 14 px knob, rounded 12 px.
        ImDrawList* child_list = ImGui::GetWindowDrawList();
        const ImU32 toggle_bg = toggle_on ? ImGui::GetColorU32(Rgba(kGreenBtn))
                                          : ImGui::GetColorU32(Rgba(kPanelInput));
        child_list->AddRectFilled(ImVec2(toggle_x, toggle_y),
                                  ImVec2(toggle_x + kToggleW * scale, toggle_y + kToggleH * scale),
                                  toggle_bg, kToggleRadius * scale);
        const float knob_cx = toggle_on ? toggle_x + (kToggleW - 10.0f) * scale
                                        : toggle_x + 10.0f * scale;
        child_list->AddCircleFilled(ImVec2(knob_cx, toggle_y + kToggleH * scale * 0.5f),
                                    kKnobR * scale,
                                    ImGui::GetColorU32(Rgba(kText)));

        // Copy, sans 12.5, kTextDim, vertically centered on the toggle.
        const char* copy = "Share wallpaper - let clients show my background";
        ImGui::PushFont(cosmic::ui::FontSansRegular());
        ImGui::SetWindowFontScale(12.5f / 13.0f);
        const ImVec2 copy_size = cosmic::ui::TextSpacedSize(copy, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
        ImGui::SetCursorScreenPos(ImVec2(toggle_x + kToggleW * scale + 12.0f * scale,
                                         y + (toggle_row_h - copy_size.y) * 0.5f));
        cosmic::ui::TextSpaced(copy, 0.0f);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
    }
    y += toggle_row_h + kSectionGap * scale;

    // --- Footnote ---
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.0f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kMuted));
    for (int i = 0; i < IM_ARRAYSIZE(kFootnoteLines); ++i) {
        ImGui::SetCursorScreenPos(ImVec2(content_x, y + i * 12.0f * scale));
        cosmic::ui::TextSpaced(kFootnoteLines[i], 0.1f);
    }
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // X close (26 px square ghost), top-right in the padding band. The ✕ glyph
    // is not in the atlas, so the label is empty and two crossed lines are
    // drawn over the button rect instead.
    const float close_size = kCloseSize * scale;
    const ImVec2 close_pos(pos.x + w - 12.0f * scale - close_size, pos.y + 4.0f * scale);
    bool close_hovered = false;
    if (DrawButton("##settings_close", "", 0.0f, close_pos,
                   ImVec2(close_size, close_size), 0, 0,
                   ImGui::GetColorU32(Rgba(kBorder)), ImGui::GetColorU32(Rgba(kPurple)),
                   ImGui::GetColorU32(Rgba(kMuted)), ImGui::GetColorU32(Rgba(kText)), scale,
                   &close_hovered)) {
        out_action->kind = BridgeAction::CloseSettings;
    }
    // Cross: the diagonals of the button's inner rect (6 px inset), kMuted →
    // kText on hover like the old "X" label.
    const float cross_inset = 6.0f * scale;
    const ImU32 cross_col = ImGui::GetColorU32(Rgba(close_hovered ? kText : kMuted));
    child_list->AddLine(ImVec2(close_pos.x + cross_inset, close_pos.y + cross_inset),
                        ImVec2(close_pos.x + close_size - cross_inset,
                               close_pos.y + close_size - cross_inset),
                        cross_col, 1.5f * scale);
    child_list->AddLine(ImVec2(close_pos.x + close_size - cross_inset,
                               close_pos.y + cross_inset),
                        ImVec2(close_pos.x + cross_inset,
                               close_pos.y + close_size - cross_inset),
                        cross_col, 1.5f * scale);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Border, on the parent draw list so the perimeter is not clipped by the
    // child's clip rect. rgba(106,116,187,.55) — kLavender at 55%.
    const ImVec4 lavender = cosmic::ui::Rgba(kLavender);
    draw_list->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                       ImGui::GetColorU32(ImVec4(lavender.x, lavender.y, lavender.z, 0.55f)),
                       0.0f, 0, 1.0f * scale);

    // Border-tab title "SETTINGS" (mono bold 12, .24em, kPurple) on the top
    // border, drawn after the child so the tab covers the border.
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(12.0f / 13.0f);
    const ImVec2 title_size = cosmic::ui::TextSpacedSize("SETTINGS", 0.24f);
    const float tab_top = pos.y - 9.0f * scale;
    const float tab_h = 14.0f * scale;
    const float tab_cy = tab_top + tab_h * 0.5f;
    const float tab_x = pos.x + 8.0f * scale;
    const float tab_w = title_size.x + 24.0f * scale;
    draw_list->AddRectFilled(ImVec2(tab_x, tab_top),
                             ImVec2(tab_x + tab_w, tab_top + tab_h),
                             ImGui::GetColorU32(ImVec4(panel_bg.x, panel_bg.y, panel_bg.z, 0.97f)));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kPurple));
    ImGui::SetCursorScreenPos(ImVec2(tab_x + 12.0f * scale, tab_cy - title_size.y * 0.5f));
    cosmic::ui::TextSpaced("SETTINGS", 0.24f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
}

void draw_pair_modal(const BridgeInput& in, BridgeState* state, BridgeAction* out_action) {
    if (!state->pair_modal_open) {
        return;
    }
    const float scale = cosmic::ui::scale();
    const ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    const float w = kPairModalW * scale;
    const float pad_x = kPairModalPadX * scale;
    const float pad_y = kPairModalPadY * scale;
    const float gap = kPairModalGap * scale;
    const float content_w = w - 2.0f * pad_x;

    const bool pairing = in.pairing_active;
    // Anchor the modal's slide/scrim fade at the moment pairing became
    // visible. The PIN panel owns a separate anchor so its entry animation
    // plays from the PIN's first frame (pairing can start seconds before the
    // PIN arrives).
    if (pairing && state->pair_slide_at_s < 0.0) {
        state->pair_slide_at_s = in.time_s;
    }
    if (!pairing) {
        state->pair_slide_at_s = -1.0;  // Idle again: next pairing re-animates.
    }

    // --- measure the variable-height rows at their fonts ---

    // Copy line (sans 12.5).
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    const float copy_h = cosmic::ui::TextSpacedSize("Address of the machine to link with:", 0.0f).y;
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Input label (mono 9.5) and input frame height (mono 13 + frame padding).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    const float label_h = cosmic::ui::TextSpacedSize("Nickname (optional)", 0.0f).y;
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 6.0f * scale));
    const float input_h = ImGui::GetFrameHeight();
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Checkbox row: the 15 px square is taller than the 12.5 px label.
    const float checkbox_row_h = std::max(kCheckboxSize * scale, 12.5f * scale);

    // Handshake line (mono 11).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(11.0f / 13.0f);
    const float handshake_h =
        cosmic::ui::TextSpacedSize("HANDSHAKE IN TRANSIT - PIN IS ON THE MONITOR", 0.14f).y;
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Sticky error (mono 10.5, wrapped to the content width).
    float error_h = 0.0f;
    if (!in.pairing_error.empty()) {
        ImGui::PushFont(cosmic::ui::FontMonoRegular());
        ImGui::SetWindowFontScale(10.5f / 13.0f);
        error_h = ImGui::CalcTextSize(in.pairing_error.c_str(), nullptr, false, content_w).y;
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
    }

    // Button labels (mono bold 10).
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    const ImVec2 pair_label = cosmic::ui::TextSpacedSize("PAIR", 0.22f);
    const ImVec2 close_label = cosmic::ui::TextSpacedSize("CLOSE", 0.22f);
    const ImVec2 cancel_label = cosmic::ui::TextSpacedSize("CANCEL", 0.22f);
    const float btn_h = pair_label.y + 18.0f * scale;
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Panel height: top padding + rows + bottom padding. The divider band adds
    // a gap above and below the 1 px line.
    const bool show_feedback = pairing || !in.pairing_error.empty();
    float h = pad_y;
    h += copy_h + gap;
    h += label_h + kInputLabelGap * scale + input_h + gap;  // address
    h += label_h + kInputLabelGap * scale + input_h + gap;  // nickname
    h += checkbox_row_h + gap;
    if (!state->pair_use_default_port) {
        h += label_h + kInputLabelGap * scale + input_h + gap;  // port
    }
    if (show_feedback) {
        h += gap;  // divider band
        if (pairing) {
            h += handshake_h + gap;
        }
        if (!in.pairing_error.empty()) {
            h += error_h + gap;
        }
    }
    h += btn_h + gap;
    h += pad_y;

    // Idle: centered (viewport 50%/44%). Pairing: lower-left (left
    // max(16%,220px), top 72%), eased over .8s.
    const ImVec2 center_pos((vp_size.x - w) * 0.5f, 0.44f * vp_size.y - h * 0.5f);
    const ImVec2 lower_left_pos(std::max(0.16f * vp_size.x, 220.0f * scale),
                                0.72f * vp_size.y);
    ImVec2 pos = center_pos;
    if (pairing) {
        const float p = std::clamp(static_cast<float>(
                                       (in.time_s - state->pair_slide_at_s) / kModalSlideS),
                                   0.0f, 1.0f);
        const float ease = EaseOutCubic(p);
        pos = ImVec2(center_pos.x + (lower_left_pos.x - center_pos.x) * ease,
                     center_pos.y + (lower_left_pos.y - center_pos.y) * ease);
    }
    const float content_x = pos.x + pad_x;
    const float content_right = pos.x + w - pad_x;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec4 panel_bg = cosmic::ui::Rgba(kPanel);
    const ImVec4 purple = cosmic::ui::Rgba(kPurple);

    // Scrim: full-viewport rgba(6,8,20,.5), fading to 0 while pairing so the
    // monitor PIN stays prominent.
    float scrim_alpha = 0.5f;
    if (pairing) {
        const float p = std::clamp(static_cast<float>(
                                       (in.time_s - state->pair_slide_at_s) / kScrimFadeS),
                                   0.0f, 1.0f);
        scrim_alpha = 0.5f * (1.0f - EaseOutCubic(p));
    }
    if (scrim_alpha > 0.0f) {
        const ImVec4 scrim = cosmic::ui::Rgba(kScrim);
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), vp_size,
                                 ImGui::GetColorU32(ImVec4(scrim.x, scrim.y, scrim.z, scrim_alpha)));
    }
    // Click-to-close: full-viewport InvisibleButton under the panel. Only
    // active when idle; while pairing it is drawn but ignores clicks (the
    // panel child drawn later gets hit priority over it).
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##pairscrim", vp_size);
    if (ImGui::IsItemClicked() && !pairing) {
        state->pair_modal_open = false;
        out_action->kind = BridgeAction::ClosePair;
    }

    // Panel background (parent draw list, before the child): rgba(20,23,46,.98)
    // — kPanel at 98%.
    draw_list->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                             ImGui::GetColorU32(ImVec4(panel_bg.x, panel_bg.y, panel_bg.z, 0.98f)));

    // Transparent child pinned "##pairmodal"; widgets are positioned with
    // SetCursorScreenPos. Zero window padding so the clip rect covers the full
    // panel.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##pairmodal", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* child_list = ImGui::GetWindowDrawList();

    float y = pos.y + pad_y;

    // Copy line.
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("Address of the machine to link with:", 0.0f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += copy_h + gap;

    // Address input (mono 13, kPanelInput bg, kBorder frame; kPurple border
    // while focused). Disabled while pairing.
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("Address", 0.0f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 6.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgba(kBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Rgba(kPurple));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y + label_h + kInputLabelGap * scale));
    ImGui::SetNextItemWidth(content_w);
    ImGui::BeginDisabled(pairing);
    ImGui::InputText("##pair_address", state->pair_address_buf,
                     sizeof(state->pair_address_buf),
                     ImGuiInputTextFlags_CallbackCharFilter, &ascii_filter);
    const bool address_focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    if (address_focused) {
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        child_list->AddRect(item_min, item_max, ImGui::GetColorU32(Rgba(kPurple)),
                            0.0f, 0, 1.0f * scale);
    }
    y += label_h + kInputLabelGap * scale + input_h + gap;

    // Nickname input (same style; optional).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y));
    cosmic::ui::TextSpaced("Nickname (optional)", 0.0f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(13.0f / 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 6.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgba(kBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Rgba(kPurple));
    ImGui::SetCursorScreenPos(ImVec2(content_x, y + label_h + kInputLabelGap * scale));
    ImGui::SetNextItemWidth(content_w);
    ImGui::BeginDisabled(pairing);
    ImGui::InputText("##pair_nickname", state->pair_nickname_buf,
                     sizeof(state->pair_nickname_buf),
                     ImGuiInputTextFlags_CallbackCharFilter, &ascii_filter);
    const bool nickname_focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    if (nickname_focused) {
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        child_list->AddRect(item_min, item_max, ImGui::GetColorU32(Rgba(kPurple)),
                            0.0f, 0, 1.0f * scale);
    }
    y += label_h + kInputLabelGap * scale + input_h + gap;

    // Checkbox row: 15 px square + "Use default port (47989)" (sans 12.5,
    // kTextDim). Checked = kGreen fill + kBg check square; unchecked =
    // kPanelInput fill; border kLavender at .8.
    const float checkbox_size = kCheckboxSize * scale;
    const ImVec2 checkbox_pos(content_x, y + (checkbox_row_h - checkbox_size) * 0.5f);
    ImGui::BeginDisabled(pairing);
    ImGui::SetCursorScreenPos(checkbox_pos);
    ImGui::InvisibleButton("##pair_default_port", ImVec2(checkbox_size, checkbox_size));
    if (ImGui::IsItemClicked()) {
        state->pair_use_default_port = !state->pair_use_default_port;
    }
    ImGui::EndDisabled();
    child_list->AddRectFilled(checkbox_pos,
                              ImVec2(checkbox_pos.x + checkbox_size,
                                     checkbox_pos.y + checkbox_size),
                              state->pair_use_default_port
                                  ? ImGui::GetColorU32(Rgba(kGreen))
                                  : ImGui::GetColorU32(Rgba(kPanelInput)));
    const ImVec4 lavender = cosmic::ui::Rgba(kLavender);
    child_list->AddRect(checkbox_pos,
                        ImVec2(checkbox_pos.x + checkbox_size,
                               checkbox_pos.y + checkbox_size),
                        ImGui::GetColorU32(ImVec4(lavender.x, lavender.y, lavender.z, 0.8f)),
                        0.0f, 0, 1.0f * scale);
    if (state->pair_use_default_port) {
        const float mark = kCheckboxMark * scale;
        child_list->AddRectFilled(
            ImVec2(checkbox_pos.x + (checkbox_size - mark) * 0.5f,
                   checkbox_pos.y + (checkbox_size - mark) * 0.5f),
            ImVec2(checkbox_pos.x + (checkbox_size + mark) * 0.5f,
                   checkbox_pos.y + (checkbox_size + mark) * 0.5f),
            ImGui::GetColorU32(Rgba(kBg)));
    }
    char port_label[64];
    std::snprintf(port_label, sizeof(port_label), "Use default port (%d)", in.port_base);
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    ImGui::SetCursorScreenPos(ImVec2(checkbox_pos.x + checkbox_size + 10.0f * scale,
                                     y + (checkbox_row_h - copy_h) * 0.5f));
    cosmic::ui::TextSpaced(port_label, 0.0f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    y += checkbox_row_h + gap;

    // Port input (same style), only when the default-port checkbox is off.
    if (!state->pair_use_default_port) {
        ImGui::PushFont(cosmic::ui::FontMonoRegular());
        ImGui::SetWindowFontScale(9.5f / 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
        ImGui::SetCursorScreenPos(ImVec2(content_x, y));
        cosmic::ui::TextSpaced("Port", 0.0f);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(13.0f / 13.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 6.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_Border, Rgba(kBorder));
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Rgba(kPurple));
        ImGui::PushStyleColor(ImGuiCol_Button, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(kPanelInput));
        ImGui::SetCursorScreenPos(ImVec2(content_x, y + label_h + kInputLabelGap * scale));
        ImGui::SetNextItemWidth(content_w);
        ImGui::BeginDisabled(pairing);
        if (ImGui::InputInt("##pair_port", &state->pair_port_input)) {
            state->pair_port_input = std::clamp(state->pair_port_input, kPortMin, kPortMax);
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(9);
        ImGui::PopStyleVar();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        y += label_h + kInputLabelGap * scale + input_h + gap;
    }

    // Divider + pairing feedback / sticky error.
    if (show_feedback) {
        child_list->AddLine(ImVec2(content_x, y), ImVec2(content_right, y),
                            ImGui::GetColorU32(Rgba(kBorderDim)), 1.0f * scale);
        y += gap;
        if (pairing) {
            // Handshake line (mono 11, .14em, kGreen). The ◈ glyph is not in
            // the atlas, so a small drawn diamond precedes the text.
            ImGui::PushFont(cosmic::ui::FontMonoRegular());
            ImGui::SetWindowFontScale(11.0f / 13.0f);
            const ImVec2 handshake_size = cosmic::ui::TextSpacedSize(
                "HANDSHAKE IN TRANSIT - PIN IS ON THE MONITOR", 0.14f);
            // Diamond: ~8x8 design px, at the line's start, vertically
            // centered on the text line.
            const float diamond_half = 4.0f * scale;
            const ImVec2 diamond_c(content_x + diamond_half,
                                   y + handshake_size.y * 0.5f);
            child_list->AddQuadFilled(
                ImVec2(diamond_c.x, diamond_c.y - diamond_half),
                ImVec2(diamond_c.x + diamond_half, diamond_c.y),
                ImVec2(diamond_c.x, diamond_c.y + diamond_half),
                ImVec2(diamond_c.x - diamond_half, diamond_c.y),
                ImGui::GetColorU32(Rgba(kGreen)));
            ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kGreen));
            ImGui::SetCursorScreenPos(ImVec2(content_x + 14.0f * scale, y));
            cosmic::ui::TextSpaced("HANDSHAKE IN TRANSIT - PIN IS ON THE MONITOR", 0.14f);
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
            y += handshake_h + gap;
        }
        if (!in.pairing_error.empty()) {
            // Sticky error (mono 10.5, kRed at .9), wrapped.
            ImGui::PushFont(cosmic::ui::FontMonoRegular());
            ImGui::SetWindowFontScale(10.5f / 13.0f);
            const ImVec4 red = cosmic::ui::Rgba(kRed);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetColorU32(ImVec4(red.x, red.y, red.z, 0.9f)));
            ImGui::SetCursorScreenPos(ImVec2(content_x, y));
            ImGui::PushTextWrapPos(content_right);
            ImGui::TextUnformatted(in.pairing_error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
            y += error_h + gap;
        }
    }

    // Button row: PAIR (green) + CLOSE (ghost) when idle; CANCEL (red) while
    // pairing.
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    const ImVec2 pair_size(pair_label.x + 30.0f * scale, btn_h);
    const ImVec2 close_size(close_label.x + 24.0f * scale, btn_h);
    const ImVec2 cancel_size(cancel_label.x + 26.0f * scale, btn_h);
    if (pairing) {
        if (DrawButton("##pair_cancel", "CANCEL", 0.22f, ImVec2(content_x, y), cancel_size,
                       ImGui::GetColorU32(Rgba(kRed)), ImGui::GetColorU32(Rgba(kRedHover)),
                       0, 0, ImGui::GetColorU32(Rgba(kText)), ImGui::GetColorU32(Rgba(kText)),
                       scale)) {
            out_action->kind = BridgeAction::CancelPair;
        }
    } else {
        // PAIR (green); disabled (dim fill, no click) while the address is
        // empty or a session is busy.
        const bool pair_disabled = state->pair_address_buf[0] == '\0' || in.session_busy;
        const ImU32 pair_bg = pair_disabled ? ImGui::GetColorU32(Rgba(0x2C3152FF))
                                            : ImGui::GetColorU32(Rgba(kGreenBtn));
        const ImU32 pair_bg_hover = pair_disabled ? ImGui::GetColorU32(Rgba(0x2C3152FF))
                                                  : ImGui::GetColorU32(Rgba(kGreenHover));
        const ImU32 pair_text = pair_disabled ? ImGui::GetColorU32(Rgba(kMuted))
                                              : ImGui::GetColorU32(Rgba(kText));
        if (DrawButton("##pair_go", "PAIR", 0.22f, ImVec2(content_x, y), pair_size,
                       pair_bg, pair_bg_hover, 0, 0, pair_text, pair_text, scale) &&
            !pair_disabled) {
            out_action->kind = BridgeAction::StartPair;
            out_action->address = trim(state->pair_address_buf);
            out_action->nickname = trim(state->pair_nickname_buf);
            // port 0 = follow the global port_base; an unset override (<= 0)
            // must NOT fall through to the clamp's 1024 minimum.
            out_action->port =
                (state->pair_use_default_port || state->pair_port_input <= 0)
                    ? 0
                    : std::clamp(state->pair_port_input, kPortMin, kPortMax);
        }
        // CLOSE (ghost): closes locally this frame AND emits ClosePair so
        // main.cpp clears the sticky error.
        if (DrawButton("##pair_close", "CLOSE", 0.22f,
                       ImVec2(content_x + pair_size.x + 10.0f * scale, y), close_size,
                       0, 0, ImGui::GetColorU32(ImVec4(purple.x, purple.y, purple.z, 0.5f)),
                       ImGui::GetColorU32(Rgba(kPurple)),
                       ImGui::GetColorU32(Rgba(kPurple)), ImGui::GetColorU32(Rgba(kText)),
                       scale)) {
            out_action->kind = BridgeAction::ClosePair;
            state->pair_modal_open = false;
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Border, on the parent draw list so the perimeter is not clipped by the
    // child's clip rect. rgba(184,151,211,.55) — kPurple at 55%.
    draw_list->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                       ImGui::GetColorU32(ImVec4(purple.x, purple.y, purple.z, 0.55f)),
                       0.0f, 0, 1.0f * scale);

    // Border-tab title "PAIR A MACHINE" (mono bold 12, .24em, kPurple) on the
    // top border, drawn after the child so the tab covers the border.
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(12.0f / 13.0f);
    const ImVec2 title_size = cosmic::ui::TextSpacedSize("PAIR A MACHINE", 0.24f);
    const float tab_top = pos.y - 9.0f * scale;
    const float tab_h = 14.0f * scale;
    const float tab_cy = tab_top + tab_h * 0.5f;
    const float tab_x = pos.x + 8.0f * scale;
    const float tab_w = title_size.x + 24.0f * scale;
    draw_list->AddRectFilled(ImVec2(tab_x, tab_top),
                             ImVec2(tab_x + tab_w, tab_top + tab_h),
                             ImGui::GetColorU32(ImVec4(panel_bg.x, panel_bg.y, panel_bg.z, 0.98f)));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kPurple));
    ImGui::SetCursorScreenPos(ImVec2(tab_x + 12.0f * scale, tab_cy - title_size.y * 0.5f));
    cosmic::ui::TextSpaced("PAIR A MACHINE", 0.24f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
}

void draw_pin_panel(const BridgeInput& in, BridgeState* state) {
    // Reset the animation anchor when nothing is pairing, so the next pairing
    // re-animates from scratch.
    if (!in.pairing_active && !in.pairing_show_pin) {
        state->pin_shown_at_s = -1.0;
        return;
    }
    if (!in.pairing_show_pin || in.pairing_pin.empty()) {
        return;
    }
    if (state->pin_shown_at_s < 0.0) {
        state->pin_shown_at_s = in.time_s;
    }

    const float scale = cosmic::ui::scale();
    const ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    const int out_w = static_cast<int>(vp_size.x);
    const int out_h = static_cast<int>(vp_size.y);
    const SDL_FRect scr = cosmic::ui::scene::screen_rect(out_w, out_h);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Slide-up entry: translateY from 45% of the screen height + fade, eased
    // over .7s (the design's cubic-bezier(.2,1.2,.4,1) approximated with
    // easeOutCubic).
    const float p = std::clamp(static_cast<float>(
                                   (in.time_s - state->pin_shown_at_s) / kPinSlideS),
                               0.0f, 1.0f);
    const float ease = EaseOutCubic(p);
    const float offset = (1.0f - ease) * 0.45f * scr.h;

    // Everything is clipped to the monitor screen rect.
    ImGui::PushClipRect(ImVec2(scr.x, scr.y), ImVec2(scr.x + scr.w, scr.y + scr.h), true);

    // Panel bg: rgba(16,18,38,.94) over the screen rect, shifted by the slide.
    const ImVec4 pin_bg(16.0f / 255.0f, 18.0f / 255.0f, 38.0f / 255.0f, 0.94f * ease);
    draw_list->AddRectFilled(ImVec2(scr.x, scr.y + offset),
                             ImVec2(scr.x + scr.w, scr.y + scr.h + offset),
                             ImGui::GetColorU32(pin_bg));

    // Scanlines: repeating 2 px lines every 5 px.
    const ImVec4 cyan = cosmic::ui::Rgba(kCyan);
    const ImU32 scanline_col = ImGui::GetColorU32(
        ImVec4(cyan.x, cyan.y, cyan.z, (13.0f / 255.0f) * ease));
    for (float yy = scr.y; yy < scr.y + scr.h; yy += 5.0f * scale) {
        draw_list->AddRectFilled(ImVec2(scr.x, yy), ImVec2(scr.x + scr.w, yy + 2.0f * scale),
                                 scanline_col);
    }

    // Bright 3 px scanline sweeping bottom→top every 2.4 s.
    const float sweep_t = static_cast<float>(
        std::fmod(in.time_s - state->pin_shown_at_s, kSweepPeriodS));
    const float sweep_p = sweep_t / kSweepPeriodS;  // 0..1
    const float sweep_y = scr.y + scr.h + 6.0f * scale - sweep_p * (scr.h + 12.0f * scale);
    draw_list->AddRectFilled(ImVec2(scr.x, sweep_y), ImVec2(scr.x + scr.w, sweep_y + 3.0f * scale),
                             ImGui::GetColorU32(ImVec4(cyan.x, cyan.y, cyan.z, 0.45f * ease)));

    // Label: "ENTER THIS PIN ON THE HOST" (mono, .35em, kCyan), size
    // clamp(9, .85% of viewport width, 14) design px.
    const float label_px = std::clamp(0.0085f * out_w / scale, 9.0f, 14.0f);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(label_px / 13.0f);
    const ImVec2 label_size = cosmic::ui::TextSpacedSize("ENTER THIS PIN ON THE HOST", 0.35f);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetColorU32(ImVec4(cyan.x, cyan.y, cyan.z, ease)));
    ImGui::SetCursorScreenPos(ImVec2(scr.x + (scr.w - label_size.x) * 0.5f,
                                     scr.y + 0.28f * scr.h - label_size.y * 0.5f));
    cosmic::ui::TextSpaced("ENTER THIS PIN ON THE HOST", 0.35f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // PIN: mono bold, .28em, size clamp(24, 3.4% of viewport width, 64) design
    // px, with a pulsing kCyan glow circle behind.
    const float pin_px = std::clamp(0.034f * out_w / scale, 24.0f, 64.0f);
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(pin_px / 13.0f);
    const ImVec2 pin_size = cosmic::ui::TextSpacedSize(in.pairing_pin.c_str(), 0.28f);
    const ImVec2 pin_center(scr.x + scr.w * 0.5f, scr.y + scr.h * 0.5f);
    const float pulse = 0.5f + 0.5f * std::cos(2.0f * 3.14159265f *
                                                static_cast<float>(in.time_s) / kSweepPeriodS);
    const float glow_r = std::min(scr.w * 0.25f, 120.0f * scale);
    draw_list->AddCircleFilled(pin_center, glow_r,
                               ImGui::GetColorU32(ImVec4(cyan.x, cyan.y, cyan.z,
                                                         (0.10f + 0.10f * pulse) * ease)));
    const ImVec4 text_col = cosmic::ui::Rgba(kText);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetColorU32(ImVec4(text_col.x, text_col.y, text_col.z, ease)));
    ImGui::SetCursorScreenPos(ImVec2(pin_center.x - pin_size.x * 0.5f,
                                     pin_center.y - pin_size.y * 0.5f));
    cosmic::ui::TextSpaced(in.pairing_pin.c_str(), 0.28f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // "AWAITING CONFIRMATION..." (ASCII dots; mono, .25em, kMuted), size
    // clamp(8, .7% of viewport width, 12) design px.
    const float await_px = std::clamp(0.007f * out_w / scale, 8.0f, 12.0f);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(await_px / 13.0f);
    const ImVec2 await_size = cosmic::ui::TextSpacedSize("AWAITING CONFIRMATION...", 0.25f);
    const ImVec4 muted = cosmic::ui::Rgba(kMuted);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetColorU32(ImVec4(muted.x, muted.y, muted.z, ease)));
    ImGui::SetCursorScreenPos(ImVec2(scr.x + (scr.w - await_size.x) * 0.5f,
                                     scr.y + 0.72f * scr.h - await_size.y * 0.5f));
    cosmic::ui::TextSpaced("AWAITING CONFIRMATION...", 0.25f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::PopClipRect();
}

}  // namespace cosmic::ui::bridge