// Cosmic Desk — default theme implementation. See theme.h for the contract.

#include "ui/theme.h"

#include <imgui.h>

namespace cosmic::ui {
namespace {

ImVec4 Rgb(int packed) {
    return ImVec4(static_cast<float>((packed >> 16) & 0xff) / 255.0f,
                  static_cast<float>((packed >> 8) & 0xff) / 255.0f,
                  static_cast<float>(packed & 0xff) / 255.0f, 1.0f);
}

ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Space background.
const ImVec4 kBg        = Rgb(0x1a1c37);  // deep indigo (hero background)
const ImVec4 kBgSoft    = Rgb(0x222641);  // popup/menu/alt-row background
const ImVec4 kBgDeep    = Rgb(0x14172e);  // logo disc (darkest)
const ImVec4 kBezel     = Rgb(0x1c2043);  // hero bezel shadow

// Text.
const ImVec4 kFg        = Rgb(0xedf2fb);  // moon white ("Cosmic" text)
const ImVec4 kDisabled  = Rgb(0x565e86);  // muted blue

// Nebula accents.
const ImVec4 kPurple     = Rgb(0xb897d3);  // hover / focus highlight
const ImVec4 kPurpleDk   = Rgb(0x8e6db8);  // active purple
const ImVec4 kPeriwinkle = Rgb(0x6a74bb);  // interactive base (buttons, title, grab)
const ImVec4 kGreen      = Rgb(0x8ac49c);  // positive / on
const ImVec4 kGreenDk    = Rgb(0x609e75);  // selected / active green
const ImVec4 kGreenDeep  = Rgb(0x438a70);  // positive button fill
const ImVec4 kBlue       = Rgb(0x8ac7e5);  // info / links
const ImVec4 kRoseDeep   = Rgb(0xb0556b);  // destructive button fill (rose)
const ImVec4 kAmber      = Rgb(0xffce54);  // sun / pressed / grab
const ImVec4 kOrange     = Rgb(0xff8f3c);  // sun gradient end

// Panels / frames.
const ImVec4 kFrame      = Rgb(0x23284a);
const ImVec4 kFrameHover = Rgb(0x2f3560);
const ImVec4 kFrameActive = Rgb(0x3d4470);
const ImVec4 kDivider    = Rgb(0x3a3f6b);

}  // namespace

void StyleColorsDefault() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.Colors[ImGuiCol_Text] = kFg;
    style.Colors[ImGuiCol_TextDisabled] = kDisabled;
    style.Colors[ImGuiCol_WindowBg] = kBg;
    style.Colors[ImGuiCol_ChildBg] = kBg;
    style.Colors[ImGuiCol_PopupBg] = kBgSoft;
    style.Colors[ImGuiCol_Border] = kDivider;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = kFrame;
    style.Colors[ImGuiCol_FrameBgHovered] = kFrameHover;
    style.Colors[ImGuiCol_FrameBgActive] = kFrameActive;
    style.Colors[ImGuiCol_TitleBg] = kBezel;
    style.Colors[ImGuiCol_TitleBgActive] = kPeriwinkle;
    style.Colors[ImGuiCol_TitleBgCollapsed] = kBezel;
    style.Colors[ImGuiCol_MenuBarBg] = kBgDeep;
    style.Colors[ImGuiCol_ScrollbarBg] = kBgDeep;
    style.Colors[ImGuiCol_ScrollbarGrab] = kPeriwinkle;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = kPurple;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = kAmber;
    style.Colors[ImGuiCol_CheckMark] = kGreen;
    style.Colors[ImGuiCol_SliderGrab] = kPurpleDk;
    style.Colors[ImGuiCol_SliderGrabActive] = kAmber;
    style.Colors[ImGuiCol_Button] = kPeriwinkle;
    style.Colors[ImGuiCol_ButtonHovered] = kPurple;
    style.Colors[ImGuiCol_ButtonActive] = kAmber;
    style.Colors[ImGuiCol_Header] = kGreenDk;
    style.Colors[ImGuiCol_HeaderHovered] = kPurpleDk;
    style.Colors[ImGuiCol_HeaderActive] = kPurple;
    style.Colors[ImGuiCol_Separator] = kDivider;
    style.Colors[ImGuiCol_SeparatorHovered] = kPurple;
    style.Colors[ImGuiCol_SeparatorActive] = kAmber;
    style.Colors[ImGuiCol_ResizeGrip] = kPeriwinkle;
    style.Colors[ImGuiCol_ResizeGripHovered] = kPurple;
    style.Colors[ImGuiCol_ResizeGripActive] = kAmber;
    style.Colors[ImGuiCol_TabHovered] = kPurple;
    style.Colors[ImGuiCol_Tab] = kFrame;
    style.Colors[ImGuiCol_TabSelected] = kGreenDk;
    style.Colors[ImGuiCol_TabSelectedOverline] = kAmber;
    style.Colors[ImGuiCol_TabDimmed] = kFrame;
    style.Colors[ImGuiCol_TabDimmedSelected] = kGreenDk;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = kAmber;
    style.Colors[ImGuiCol_PlotLines] = kBlue;
    style.Colors[ImGuiCol_PlotLinesHovered] = kAmber;
    style.Colors[ImGuiCol_PlotHistogram] = kGreen;
    style.Colors[ImGuiCol_PlotHistogramHovered] = kAmber;
    style.Colors[ImGuiCol_TableHeaderBg] = kFrame;
    style.Colors[ImGuiCol_TableBorderStrong] = kDivider;
    style.Colors[ImGuiCol_TableBorderLight] = kDivider;
    style.Colors[ImGuiCol_TableRowBg] = kBg;
    style.Colors[ImGuiCol_TableRowBgAlt] = kBgSoft;
    style.Colors[ImGuiCol_TextLink] = kBlue;
    style.Colors[ImGuiCol_TextSelectedBg] = kPurple;
    style.Colors[ImGuiCol_DragDropTarget] = kGreen;
    style.Colors[ImGuiCol_NavCursor] = kAmber;
    style.Colors[ImGuiCol_NavWindowingHighlight] = kPurple;
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);

    // Net-box rose (#e07b8f) and the sun's gradient end (#ff8f3c) are part of
    // the palette for later graph/meter rendering; see kRoseDeep/kOrange above.
    (void)kOrange;
}

ImVec4 AccentColor(Accent accent) {
    switch (accent) {
    case Accent::Positive:
        return kGreenDeep;
    case Accent::Destructive:
        return kRoseDeep;
    }
    return kPeriwinkle;
}

bool AccentButton(const char* label, Accent accent) {
    const ImVec4 base = AccentColor(accent);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          Lerp(base, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          Lerp(base, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.20f));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return clicked;
}

}  // namespace cosmic::ui
