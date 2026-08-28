// Cosmic Desk — Bridge UI overlay implementation (docs/UI_MIGRATION.md U2-U4).
//
// Draws the fullscreen ImGui window that sits above the parallax scene and
// below the classic control window: the monitor boot sequence (once per
// launch) and the hosting beacon. The window background is fully transparent
// so the scene shows through; all geometry derives from the monitor screen
// rect (scene::screen_rect) and is specified in design px, multiplied by
// ui::scale() at draw time like the rest of the Bridge UI.

#include "ui/bridge/bridge.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/bridge/design.h"
#include "ui/bridge/scene.h"
#include "ui/bridge/text.h"
#include "ui/fonts.h"
#include "ui/scale.h"

namespace cosmic::ui::bridge {

namespace {

// Boot timing (docs/UI_MIGRATION.md §4): the five mono lines appear at
// 0.3/1.0/1.7/2.4/3.1 s after boot start; the overlay clears at 4.4 s.
constexpr double kBootDurationS = 4.4;
constexpr double kBootLineTimesS[] = {0.3, 1.0, 1.7, 2.4, 3.1};

// Screen-logo fade window after the boot overlay clears (U2).
constexpr double kLogoFadeS = 1.4;

// Beacon pulse period (prototype `beacon` keyframes).
constexpr float kBeaconPeriodS = 2.6f;

// The monitor screen rect is 28.56% of the art-box width (scene.h
// screen_rect); the beacon dot is 0.55% of the art-box width. The desk group
// is drawn at es 1.03, so screen_rect's width includes that factor.
constexpr float kScreenRectW = 0.2856f;
constexpr float kDotArtW = 0.0055f;
constexpr float kDeskEs = 1.03f;

}  // namespace

float draw_bridge(const BridgeInput& in, BridgeState* state) {
    // The first call starts the boot sequence (once per launch).
    if (state->boot_start_s < 0.0) {
        state->boot_start_s = in.time_s;
    }
    const double t = in.time_s - state->boot_start_s;

    const float scale = cosmic::ui::scale();
    const ImVec2 vp_size = ImGui::GetMainViewport()->Size;
    const int out_w = static_cast<int>(vp_size.x);
    const int out_h = static_cast<int>(vp_size.y);
    const SDL_FRect scr = cosmic::ui::scene::screen_rect(out_w, out_h);

    // Fullscreen borderless window with no background: the scene shows
    // through, and the classic control window is drawn after this one so it
    // stays on top. No interactive widgets (U3 adds the cards).
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size, ImGuiCond_Always);
    ImGui::Begin("##Bridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoFocusOnAppearing);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Boot sequence: dark overlay over the monitor screen + the five mono
    // lines, each appearing at its own time after boot start.
    if (t < kBootDurationS) {
        // rgba(16,18,38,.9) — the design's deep indigo at 90% opacity.
        const ImU32 boot_bg = ImGui::GetColorU32(
            ImVec4(16.0f / 255.0f, 18.0f / 255.0f, 38.0f / 255.0f, 0.9f));
        draw_list->AddRectFilled(ImVec2(scr.x, scr.y),
                                 ImVec2(scr.x + scr.w, scr.y + scr.h), boot_bg);

        // The LISTENING line reflects real host state: the configured port
        // when hosting started, FAIL otherwise.
        char listening[64];
        if (in.hosting_ok) {
            std::snprintf(listening, sizeof(listening),
                          "LISTENING ON :%d ........ OK", in.port_base);
        } else {
            std::snprintf(listening, sizeof(listening),
                          "LISTENING .............. FAIL");
        }
        const char* boot_lines[5] = {
            "COSMIC DESK BIOS v2.6",
            "GAMESTREAM CORE ............ OK",
            "ENCODER .................... OK",
            listening,
            "BRIDGE ONLINE - WELCOME",
        };

        // First line at 6% of the screen height, lines spaced 22% apart,
        // left-aligned at 5% of the screen width.
        const float line_x = scr.x + 0.05f * scr.w;
        const float line_y0 = scr.y + 0.06f * scr.h;
        const float line_dy = 0.22f * scr.h;

        ImGui::PushFont(cosmic::ui::FontMonoMedium());
        // Boot font: clamp(8, 0.008*vw, 13) design px. The vw term must be in
        // CSS px (out_w is device px, so divide by the UI scale), and the base
        // font is 13px * scale, so the window font scale is design_px / 13.
        const float boot_px = std::clamp(0.008f * out_w / scale, 8.0f, 13.0f);
        ImGui::SetWindowFontScale(boot_px / 13.0f);
        // BIOS-style diagnostics in the status green (design #8ac49c).
        ImGui::PushStyleColor(ImGuiCol_Text, cosmic::ui::Rgba(cosmic::ui::kGreen));
        for (int i = 0; i < 4; ++i) {
            if (t >= kBootLineTimesS[i]) {
                ImGui::SetCursorScreenPos(ImVec2(line_x, line_y0 + line_dy * i));
                cosmic::ui::TextSpaced(boot_lines[i], 0.12f);
            }
        }
        ImGui::PopStyleColor();
        // The final line is always present from 3.1 s, in the data cyan.
        if (t >= kBootLineTimesS[4]) {
            ImGui::PushStyleColor(ImGuiCol_Text, cosmic::ui::Rgba(cosmic::ui::kCyan));
            ImGui::SetCursorScreenPos(ImVec2(line_x, line_y0 + line_dy * 4));
            cosmic::ui::TextSpaced(boot_lines[4], 0.12f);
            ImGui::PopStyleColor();
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
    }

    // Hosting beacon (always): green dot + "HOSTING :port - n PAIRED" pill at
    // 52.9%/55.4% of the monitor screen rect.
    const ImVec2 beacon(scr.x + 0.529f * scr.w, scr.y + 0.554f * scr.h);

    // Dot: 0.55% of the ART-BOX width (the screen rect is 28.56% of the art
    // box, and the desk layer is drawn at es 1.03 — undo that to recover the
    // true art-box width). screen_rect() returns device px, so divide by the
    // UI scale to get design px before applying the design ratio; the size is
    // then scaled back to device px once (min 2 design-px diameter).
    const float art_w = scr.w / (kScreenRectW * kDeskEs * scale);
    const float dot_px = kDotArtW * art_w;
    const float dot_radius = std::max(dot_px * 0.5f, 1.0f) * scale;

    // 2.6s pulse, eased 0..1 (prototype `beacon` keyframes).
    const float pulse =
        0.5f - 0.5f * std::cosf(2.0f * 3.14159265f * (in.time_s / kBeaconPeriodS));

    const ImVec4 green = cosmic::ui::Rgba(cosmic::ui::kGreen);
    // Soft outer glow: kGreen at ~0.35 * pulse alpha, 1.6x the dot radius.
    draw_list->AddCircleFilled(
        beacon, dot_radius * 1.6f,
        ImGui::GetColorU32(ImVec4(green.x, green.y, green.z, 0.35f * pulse)));
    // Solid dot.
    draw_list->AddCircleFilled(beacon, dot_radius, ImGui::GetColorU32(green));

    // Pill: mono text on a dark rounded rect, sized to the text via
    // TextSpacedSize plus 6x2 design px padding.
    char pill_text[64];
    std::snprintf(pill_text, sizeof(pill_text), "HOSTING :%d - %d PAIRED",
                  in.port_base, in.paired_count);
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.0f / 13.0f);
    const ImVec2 pill_text_size = cosmic::ui::TextSpacedSize(pill_text, 0.18f);
    const float pad_x = 6.0f * scale;
    const float pad_y = 2.0f * scale;
    const ImVec2 pill_size(pill_text_size.x + 2.0f * pad_x,
                           pill_text_size.y + 2.0f * pad_y);
    const ImVec2 pill_min(beacon.x + dot_radius + 8.0f * scale,
                          beacon.y - pill_size.y * 0.5f);
    const ImVec2 pill_max(pill_min.x + pill_size.x, pill_min.y + pill_size.y);
    // rgba(16,18,38,.85) — the design's deep indigo at 85% opacity.
    const ImU32 pill_bg = ImGui::GetColorU32(
        ImVec4(16.0f / 255.0f, 18.0f / 255.0f, 38.0f / 255.0f, 0.85f));
    draw_list->AddRectFilled(pill_min, pill_max, pill_bg, 3.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(green));
    ImGui::SetCursorScreenPos(ImVec2(pill_min.x + pad_x, pill_min.y + pad_y));
    cosmic::ui::TextSpaced(pill_text, 0.18f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleColor();

    // Screen-logo opacity for the scene: full during boot, then fading to 0
    // over the 1.4s window after the overlay clears at 4.4s.
    float screen_logo_alpha = 1.0f;
    if (t >= kBootDurationS) {
        screen_logo_alpha = static_cast<float>(
            std::clamp((kBootDurationS + kLogoFadeS - t) / kLogoFadeS, 0.0, 1.0));
    }
    return screen_logo_alpha;
}

}  // namespace cosmic::ui::bridge