// Cosmic Desk — Bridge UI overlay implementation (docs/UI_MIGRATION.md U2-U4).
//
// Draws the fullscreen ImGui window that sits above the parallax scene and
// below the classic control window: the monitor boot sequence (once per
// launch), the hosting beacon, and (U3) the machine cards orbiting the scene
// center, the bottom dock and the session status. The window background is
// fully transparent so the scene shows through; all geometry derives from the
// monitor screen rect (scene::screen_rect) and is specified in design px,
// multiplied by ui::scale() at draw time like the rest of the Bridge UI.

#include "ui/bridge/bridge.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "ui/bridge/design.h"
#include "ui/bridge/panels.h"
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

// --- U3: machine cards, dock, session status (design px) ---

// Card box and content layout (handoff README "Machine cards").
constexpr float kCardW = 256.0f;
constexpr float kCardH = 164.0f;
constexpr float kCardPadX = 17.0f;
constexpr float kCardPadTop = 15.0f;
constexpr float kCardGap = 9.0f;

// Card depth (parallax weight) and float-bob period cycle by index (i % 3).
constexpr float kCardDepths[] = {88.0f, 102.0f, 94.0f};
constexpr float kCardBobPeriods[] = {7.0f, 8.5f, 9.5f};

// Empty-state beacon card (8%/30% of the viewport, 270 wide).
constexpr float kEmptyW = 270.0f;
constexpr float kEmptyH = 170.0f;
constexpr float kEmptyBobPeriodS = 6.5f;  // prototype `holo 6.5s`

// Card colors (packed 0xRRGGBBAA, from the design tokens).
constexpr uint32_t kCardBg = 0x101226F7;             // rgba(16,18,38,.97)
constexpr uint32_t kCardBorderHover = 0x8AC7E5D9;    // rgba(138,199,229,.85)
constexpr uint32_t kCardBorderSelected = 0xB897D3BF; // rgba(184,151,211,.75)
constexpr uint32_t kEmptyBorder = 0x8AC49C8C;        // rgba(138,196,156,.55)
constexpr uint32_t kPurpleBorder = 0xB897D380;       // rgba(184,151,211,.5)
constexpr uint32_t kDockPairBg = 0x6A74BBE6;         // rgba(106,116,187,.9)
constexpr uint32_t kDockPairHover = 0x8E6DB8F2;      // rgba(142,109,184,.95)

// Truncates `text` to fit within `max_width` device px at the current font and
// tracking, appending ".." when truncated. Call with the target font pushed.
std::string Ellipsize(const std::string& text, float tracking_em, float max_width) {
    if (cosmic::ui::TextSpacedSize(text.c_str(), tracking_em).x <= max_width) {
        return text;
    }
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const std::string candidate = text.substr(0, i + 1) + "..";
        if (cosmic::ui::TextSpacedSize(candidate.c_str(), tracking_em).x > max_width) {
            break;
        }
        out = candidate;
    }
    return out.empty() ? ".." : out;
}

// Formats a host's last_connected (unix seconds) as the card's status line.
std::string LastLinkText(int64_t last_connected, int64_t now_unix) {
    if (last_connected <= 0) {
        return "NEVER LINKED";
    }
    const int64_t age = now_unix - last_connected;
    if (age < 60) {
        return "LAST LINK · JUST NOW";
    }
    if (age < 3600) {
        return "LAST LINK · " + std::to_string(age / 60) + "M AGO";
    }
    if (age < 86400) {
        return "LAST LINK · " + std::to_string(age / 3600) + "H AGO";
    }
    return "LAST LINK · " + std::to_string(age / 86400) + "D AGO";
}

// Trims whitespace and uppercases the inline-rename buffer (nicknames are
// stored uppercase, matching the design).
std::string NormalizeNickname(const char* text) {
    std::string out(text);
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = out.find_last_not_of(" \t\r\n");
    out = out.substr(first, last - first + 1);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// The card's address line: "address:port" (the per-machine override if set,
// else the global port base), matching the prototype's addr field.
std::string HostAddressLabel(const SavedHost& host, int port_base) {
    return host.address + ":" + std::to_string(host.port > 0 ? host.port : port_base);
}

// Draws a button as a filled/bordered rect + centered TextSpaced label with an
// InvisibleButton hit test. Colors are packed 0xRRGGBBAA tokens; the *_hover
// variants replace their idle counterparts while hovered. A zero alpha skips
// the fill/border. Returns true when clicked this frame; `hovered_out` (when
// given) receives the InvisibleButton's hover state so callers can tell
// interactive-widget hover apart from the non-interactive TextSpaced label.
bool DrawButton(const char* id, const char* label, float tracking_em,
                const ImVec2& pos, const ImVec2& size, uint32_t bg,
                uint32_t bg_hover, uint32_t border, uint32_t border_hover,
                uint32_t text, uint32_t text_hover, float scale,
                bool* hovered_out = nullptr) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    if (hovered_out != nullptr) {
        *hovered_out = hovered;
    }
    const uint32_t bg_col = hovered ? bg_hover : bg;
    const uint32_t border_col = hovered ? border_hover : border;
    const uint32_t text_col = hovered ? text_hover : text;
    if ((bg_col & 0xFF) != 0) {
        draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                 ImGui::GetColorU32(Rgba(bg_col)));
    }
    if ((border_col & 0xFF) != 0) {
        draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                           ImGui::GetColorU32(Rgba(border_col)), 0.0f, 0,
                           1.0f * scale);
    }
    const ImVec2 label_size = cosmic::ui::TextSpacedSize(label, tracking_em);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(text_col));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + (size.x - label_size.x) * 0.5f,
                                     pos.y + (size.y - label_size.y) * 0.5f));
    cosmic::ui::TextSpaced(label, tracking_em);
    ImGui::PopStyleColor();
    return clicked;
}

// Orbit position for card `index` (design: cards spread around the upper arc
// of the ring, gently swaying; see handoff README "Machine cards").
ImVec2 CardOrbitCenter(const BridgeInput& in, const cosmic::ui::scene::CursorSmooth& cursor,
                       int index, const ImVec2& vp_size, float scale) {
    // Prototype (Cosmic Desk.dc.html): 0-indexed i, base = -pi/2 + (i-1)*1.15,
    // sway = sin(now*0.09 + i*2.1)*0.07 + cx*0.05.
    const float base = -3.14159265f / 2.0f + (index - 1) * 1.15f;
    const float sway = 0.07f * std::sinf(0.09f * in.time_s + 2.1f * index) +
                       0.05f * cursor.x;
    const float ang = base + sway;
    const float rx = std::min(0.26f * vp_size.x, 400.0f * scale);
    const float ry = std::min(0.24f * vp_size.y, 235.0f * scale);
    const float tilt = -8.0f * 3.14159265f / 180.0f;
    const float depth = kCardDepths[index % 3];
    const float ox = rx * std::cosf(ang);
    const float oy = ry * std::sinf(ang);
    // Tilted ellipse + the card's own parallax (depth * 0.35, like the
    // prototype's data-depth transform).
    float x = ox * std::cosf(tilt) - oy * std::sinf(tilt) - cursor.x * depth * 0.35f;
    float y = ox * std::sinf(tilt) + oy * std::cosf(tilt) - cursor.y * depth * 0.35f;
    // Clamp so the card stays fully in viewport with an 8px margin — but only
    // while the scene is at rest. During the warp (U5) the clamp is skipped so
    // the cards exit through the viewport edges and stay out while w ~ 1.
    const float ax = 0.5f * vp_size.x;
    const float ay = 0.47f * vp_size.y;
    const float half_w = 128.0f * scale;
    const float half_h = 82.0f * scale;
    const float margin = 8.0f * scale;
    if (in.warp <= 0.02f) {
        x = std::clamp(x, -ax + half_w + margin, vp_size.x - ax - half_w - margin);
        y = std::clamp(y, -ay + half_h + margin, vp_size.y - ay - half_h - margin);
    }
    // Warp (U5): orbit offsets multiply by 1 + w^2*5, accelerating the cards
    // out through the viewport edges (prototype: x *= 1 + wt*wt*5).
    x *= 1.0f + in.warp * in.warp * 5.0f;
    y *= 1.0f + in.warp * in.warp * 5.0f;
    // Float bob: translateY 0 <-> -8px, cosine ease, period by index.
    const float period = kCardBobPeriods[index % 3];
    const float bob = -8.0f * (0.5f - 0.5f * std::cosf(2.0f * 3.14159265f * (in.time_s / period)));
    return ImVec2(ax + x, ay + y + bob * scale);
}

// Draws one machine card centered at `center` (device px). The card is a child
// window pinned to the host's address so a rename never changes its ID; the
// frame (bg + border + border-tab title) is drawn manually so the tab can
// straddle the top border. Returns any action the card produced.
BridgeAction DrawMachineCard(const BridgeInput& in, BridgeState* state,
                             const SavedHost& host, int index,
                             const ImVec2& center, float scale, int64_t now_unix) {
    BridgeAction action;
    // Warp (U5): cards scale up by 1 + 0.6*w around their center — the pos
    // below derives from the center, so the card stays centered while it grows.
    const float w = kCardW * scale * (1.0f + 0.6f * in.warp);
    const float h = kCardH * scale * (1.0f + 0.6f * in.warp);
    const ImVec2 pos(center.x - w * 0.5f, center.y - h * 0.5f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Card background (the child window itself is transparent).
    draw_list->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                             ImGui::GetColorU32(Rgba(kCardBg)));

    // Child window pinned to the address so a rename never changes its ID.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::SetCursorScreenPos(pos);
    const std::string child_id = "##card" + host.address;
    ImGui::BeginChild(child_id.c_str(), ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Border: cyan-ish on hover, purple when selected, dim otherwise. Computed
    // here (hover state), but DRAWN after the child below: inside the child
    // the draw-list clip is inset by the window padding, so the perimeter
    // would be clipped away.
    const bool hovered = ImGui::IsWindowHovered();
    const uint32_t border = hovered ? kCardBorderHover
                            : (state->selected == host.address ? kCardBorderSelected : kBorderDim);

    // Divider under the title row.
    const float content_x = pos.x + kCardPadX * scale;
    const float content_right = pos.x + w - kCardPadX * scale;
    const float divider_y = pos.y + kCardPadTop * scale;
    draw_list->AddLine(ImVec2(content_x, divider_y),
                       ImVec2(content_right, divider_y),
                       ImGui::GetColorU32(Rgba(kBorderDim)), 1.0f * scale);

    // Address line (mono 12px, data cyan).
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(12.0f / 13.0f);
    const std::string addr_text = HostAddressLabel(host, in.port_base);
    const float addr_h = cosmic::ui::TextSpacedSize(addr_text.c_str(), 0.06f).y;
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::SetCursorScreenPos(ImVec2(content_x, divider_y + 1.0f * scale + kCardGap * scale));
    cosmic::ui::TextSpaced(addr_text.c_str(), 0.06f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Action row.
    const float row_y = divider_y + 1.0f * scale + kCardGap * scale + addr_h + kCardGap * scale;

    // Interactive-widget hover, accumulated while drawing the card's buttons
    // and input: a click on one must not select the card. The TextSpaced
    // labels (address, last-connected, tab) are non-interactive and must NOT
    // block selection.
    bool widget_hovered = false;

    if (state->renaming == host.address) {
        // Inline rename: input + OK replaces the action row (Enter saves, Esc
        // cancels).
        ImGui::PushFont(cosmic::ui::FontMonoRegular());
        ImGui::SetWindowFontScale(11.0f / 13.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * scale, 5.0f * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Rgba(kPanelInput));
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
        // Measure the OK button so the input fills the remaining width.
        ImGui::PushFont(cosmic::ui::FontMonoBold());
        ImGui::SetWindowFontScale(10.0f / 13.0f);
        const ImVec2 ok_label = ImGui::CalcTextSize("OK");
        const ImVec2 ok_size(ok_label.x + 18.0f * scale, ok_label.y + 10.0f * scale);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(content_x, row_y));
        // Focus the input on the frame it appears so typing starts immediately.
        // The per-window flag is cleared when the rename ends, so the next
        // rename re-focuses.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID focus_key = ImGui::GetID("##rename_focus");
        if (storage->GetInt(focus_key, 0) == 0) {
            ImGui::SetKeyboardFocusHere();
            storage->SetInt(focus_key, 1);
        }
        ImGui::SetNextItemWidth(content_right - content_x - ok_size.x - 6.0f * scale);
        const bool enter = ImGui::InputText("##rename", state->rename_buf,
                                            sizeof(state->rename_buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        widget_hovered |= ImGui::IsItemHovered();  // the rename input
        const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        // OK button (green), right of the input.
        ImGui::PushFont(cosmic::ui::FontMonoBold());
        ImGui::SetWindowFontScale(10.0f / 13.0f);
        bool ok_hovered = false;
        const bool ok = DrawButton("##ok", "OK", 0.0f,
                                   ImVec2(content_right - ok_size.x, row_y), ok_size,
                                   kGreenBtn, kGreenHover, 0, 0, kText, kText, scale,
                                   &ok_hovered);
        widget_hovered |= ok_hovered;
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        if (enter || ok) {
            action.kind = BridgeAction::Edit;
            action.address = host.address;
            action.nickname = NormalizeNickname(state->rename_buf);
            state->renaming.clear();
            storage->SetInt(focus_key, 0);
        } else if (esc) {
            state->renaming.clear();
            storage->SetInt(focus_key, 0);
        }
    } else {
        // CONNECT (or PAIR for unpaired machines) + rename button, right
        // aligned; the last-connected line ellipsizes into the remaining width.
        const bool linking = in.connecting_address == host.address;
        const char* connect_label = linking ? "LINKING..." : "CONNECT";
        ImGui::PushFont(cosmic::ui::FontMonoBold());
        ImGui::SetWindowFontScale(9.5f / 13.0f);
        const ImVec2 connect_label_size = ImGui::CalcTextSize(connect_label);
        const ImVec2 connect_size(connect_label_size.x + 26.0f * scale,
                                  connect_label_size.y + 16.0f * scale);
        const ImVec2 pair_label_size = ImGui::CalcTextSize("PAIR");
        const ImVec2 pair_size(pair_label_size.x + 24.0f * scale,
                               pair_label_size.y + 14.0f * scale);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        ImGui::PushFont(cosmic::ui::FontMonoRegular());
        ImGui::SetWindowFontScale(10.0f / 13.0f);
        const ImVec2 edit_label_size = ImGui::CalcTextSize("EDIT");
        const ImVec2 edit_size(edit_label_size.x + 18.0f * scale,
                               edit_label_size.y + 14.0f * scale);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        const float btn_gap = 10.0f * scale;
        // Rightmost button: CONNECT (paired) or PAIR (unpaired).
        const ImVec2 cta_size = host.paired ? connect_size : pair_size;
        const float cta_x = content_right - cta_size.x;
        const float edit_x = cta_x - btn_gap - edit_size.x;
        const float last_right = host.paired ? edit_x : cta_x;
        const float last_max_w = last_right - btn_gap - content_x;

        // Last-connected (mono 9.5px, muted, ellipsized).
        ImGui::PushFont(cosmic::ui::FontMonoRegular());
        ImGui::SetWindowFontScale(9.5f / 13.0f);
        const std::string last_text =
            Ellipsize(LastLinkText(host.last_connected, now_unix), 0.12f, last_max_w);
        const float last_h = cosmic::ui::TextSpacedSize(last_text.c_str(), 0.12f).y;
        ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kMuted));
        ImGui::SetCursorScreenPos(ImVec2(content_x, row_y + (cta_size.y - last_h) * 0.5f));
        cosmic::ui::TextSpaced(last_text.c_str(), 0.12f);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        // Rename (ghost) button — only for paired cards.
        if (host.paired) {
            ImGui::PushFont(cosmic::ui::FontMonoRegular());
            ImGui::SetWindowFontScale(10.0f / 13.0f);
            bool edit_hovered = false;
            if (DrawButton("##edit", "EDIT", 0.0f, ImVec2(edit_x, row_y), edit_size,
                           0, 0, kBorder, kPurple, kMuted, kText, scale,
                           &edit_hovered)) {
                state->renaming = host.address;
                std::snprintf(state->rename_buf, sizeof(state->rename_buf), "%s",
                              host.nickname.c_str());
            }
            widget_hovered |= edit_hovered;
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        }

        // CONNECT (green) or PAIR (ghost).
        if (host.paired) {
            ImGui::PushFont(cosmic::ui::FontMonoBold());
            ImGui::SetWindowFontScale(9.5f / 13.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(13.0f * scale, 8.0f * scale));
            ImGui::PushStyleColor(ImGuiCol_Button, Rgba(kGreenBtn));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(kGreenHover));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(kGreenHover));
            ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
            ImGui::SetCursorScreenPos(ImVec2(cta_x, row_y));
            ImGui::BeginDisabled(linking || in.session_busy);
            if (ImGui::Button(connect_label)) {
                action.kind = BridgeAction::Connect;
                action.address = host.address;
            }
            ImGui::EndDisabled();
            widget_hovered |= ImGui::IsItemHovered();  // the CONNECT button
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        } else {
            ImGui::PushFont(cosmic::ui::FontMonoBold());
            ImGui::SetWindowFontScale(9.5f / 13.0f);
            bool pair_hovered = false;
            // Pairing while a session is busy would silently no-op, so disable.
            ImGui::BeginDisabled(in.session_busy);
            if (DrawButton("##pair", "PAIR", 0.2f, ImVec2(cta_x, row_y), pair_size,
                           0, 0, kPurpleBorder, kPurple, kPurple, kText, scale,
                           &pair_hovered)) {
                // Open the Bridge's own Pair modal, prefilled with this host's
                // address; the nickname/port fields start clean.
                state->pair_modal_open = true;
                std::snprintf(state->pair_address_buf, sizeof(state->pair_address_buf),
                              "%s", host.address.c_str());
                state->pair_nickname_buf[0] = '\0';
                state->pair_use_default_port = true;
                state->pair_port_input = 0;
            }
            ImGui::EndDisabled();
            widget_hovered |= pair_hovered;
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        }
    }

    // Card click: single click selects, double-click on a paired card connects.
    // A click on one of the card's interactive widgets never selects; a busy
    // session never connects (a second connect would silently no-op).
    if (hovered && !widget_hovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            state->selected = host.address;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && host.paired &&
            !in.session_busy) {
            action.kind = BridgeAction::Connect;
            action.address = host.address;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Border, on the parent draw list so the perimeter is not clipped by the
    // child's padded clip rect.
    ImGui::GetWindowDrawList()->AddRect(
        pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(Rgba(border)), 0.0f,
        0, 1.0f * scale);

    // Border-tab title: drawn after the child so the kBg tab covers the border
    // at the top edge (the tab sits ON the border per the design).
    // State mapping (docs/UI_MIGRATION.md U3 §3): online && paired -> LINK
    // READY (green); paired -> STANDBY (amber); !paired -> NOT PAIRED (muted).
    // `online` comes from the U6 presence poller.
    const bool online = host.paired && in.presence.count(host.address) > 0 &&
                        in.presence.at(host.address);
    const uint32_t state_color = online ? kGreen : (host.paired ? kAmber : kMuted);
    const char* state_label = online ? "LINK READY" : (host.paired ? "STANDBY" : "NOT PAIRED");
    const float tab_top = pos.y - 9.0f * scale;
    const float tab_h = 14.0f * scale;
    const float tab_cy = tab_top + tab_h * 0.5f;

    // Name, ellipsized to the tab's max width (62% of the card).
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(11.0f / 13.0f);
    const float name_max_w = 0.62f * w - 14.0f * scale - 7.0f * scale - 6.0f * scale;
    const std::string name = host.nickname.empty() ? host.address : host.nickname;
    const std::string name_ell = Ellipsize(name, 0.08f, name_max_w);
    const ImVec2 name_size = cosmic::ui::TextSpacedSize(name_ell.c_str(), 0.08f);
    const float tab_x = pos.x + 8.0f * scale;
    const float tab_w = 14.0f * scale + 7.0f * scale + 6.0f * scale + name_size.x;
    draw_list->AddRectFilled(ImVec2(tab_x, tab_top),
                             ImVec2(tab_x + tab_w, tab_top + tab_h),
                             ImGui::GetColorU32(Rgba(kBg)));
    // Status dot (7px) + soft glow.
    const ImVec2 dot_center(tab_x + 7.0f * scale + 3.5f * scale, tab_cy);
    draw_list->AddCircleFilled(dot_center, 3.5f * scale, ImGui::GetColorU32(Rgba(state_color)));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
    ImGui::SetCursorScreenPos(ImVec2(tab_x + 14.0f * scale + 7.0f * scale + 6.0f * scale,
                                     tab_cy - name_size.y * 0.5f));
    cosmic::ui::TextSpaced(name_ell.c_str(), 0.08f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // State label, right-aligned on its own kBg tab.
    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(9.0f / 13.0f);
    const ImVec2 state_size = cosmic::ui::TextSpacedSize(state_label, 0.14f);
    const float state_tab_w = state_size.x + 14.0f * scale;
    const float state_tab_x = pos.x + w - 8.0f * scale - state_tab_w;
    draw_list->AddRectFilled(ImVec2(state_tab_x, pos.y - 7.0f * scale),
                             ImVec2(state_tab_x + state_tab_w, pos.y - 7.0f * scale + tab_h),
                             ImGui::GetColorU32(Rgba(kBg)));
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(state_color));
    ImGui::SetCursorScreenPos(ImVec2(state_tab_x + 7.0f * scale,
                                     pos.y - 7.0f * scale + (tab_h - state_size.y) * 0.5f));
    cosmic::ui::TextSpaced(state_label, 0.14f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    return action;
}

// Empty state: a single beacon card at 8%/30% of the viewport when there are
// no hosts.
BridgeAction DrawEmptyCard(const BridgeInput& in, BridgeState* state,
                           const ImVec2& vp_size, float scale) {
    BridgeAction action;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float w = kEmptyW * scale;
    const float h = kEmptyH * scale;
    // Float bob: translateY 0 <-> -8px, cosine ease, 6.5s (prototype `holo`).
    const float bob = -8.0f * (0.5f - 0.5f * std::cosf(2.0f * 3.14159265f * (in.time_s / kEmptyBobPeriodS)));
    const ImVec2 pos(0.08f * vp_size.x, 0.30f * vp_size.y + bob * scale);

    // Card frame: bg + green-tinted border.
    draw_list->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                             ImGui::GetColorU32(Rgba(kCardBg)));
    draw_list->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                       ImGui::GetColorU32(Rgba(kEmptyBorder)), 0.0f, 0,
                       1.0f * scale);

    // Pulsing green dot (2s cosine pulse, like the hosting beacon).
    const float pulse = 0.5f - 0.5f * std::cosf(2.0f * 3.14159265f * (in.time_s / 2.0f));
    const ImVec2 dot_center(pos.x + 24.5f * scale, pos.y + 24.5f * scale);
    const ImVec4 green = cosmic::ui::Rgba(cosmic::ui::kGreen);
    draw_list->AddCircleFilled(dot_center, (4.5f + 6.0f * pulse) * scale,
                               ImGui::GetColorU32(ImVec4(green.x, green.y, green.z,
                                                         0.9f - 0.7f * pulse)));
    draw_list->AddCircleFilled(dot_center, 4.5f * scale, ImGui::GetColorU32(green));

    // Title.
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(11.0f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kText));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.0f * scale, pos.y + 41.0f * scale));
    cosmic::ui::TextSpaced("NO MACHINES IN RANGE", 0.18f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // Body copy (wrapped).
    ImGui::PushFont(cosmic::ui::FontSansRegular());
    ImGui::SetWindowFontScale(12.5f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kTextDim));
    const char* body = "Pair your first machine to open a link across the void.";
    const float body_w = (kEmptyW - 40.0f) * scale;
    const ImVec2 body_size = ImGui::CalcTextSize(body, nullptr, false, body_w);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.0f * scale, pos.y + 64.0f * scale));
    ImGui::PushTextWrapPos(pos.x + 20.0f * scale + body_w);
    ImGui::TextUnformatted(body);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();

    // PAIR A MACHINE button (green).
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    const ImVec2 btn_label = cosmic::ui::TextSpacedSize("PAIR A MACHINE", 0.22f);
    const ImVec2 btn_size(btn_label.x + 32.0f * scale, btn_label.y + 20.0f * scale);
    const ImVec2 btn_pos(pos.x + 20.0f * scale,
                         pos.y + 64.0f * scale + body_size.y + 12.0f * scale);
    if (DrawButton("##empty_pair", "PAIR A MACHINE", 0.22f, btn_pos, btn_size,
                   kGreenBtn, kGreenHover, 0, 0, kText, kText, scale)) {
        // Open the Bridge's own Pair modal with a clean address field.
        state->pair_modal_open = true;
        state->pair_address_buf[0] = '\0';
        state->pair_nickname_buf[0] = '\0';
        state->pair_use_default_port = true;
        state->pair_port_input = 0;
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    return action;
}

// Bottom dock: PAIR MACHINE + SETTINGS, fixed at bottom center (never orbits).
// The SETTINGS button flips state->settings_open directly — the panel is drawn
// after the dock, so it appears above it, and main.cpp never sees a
// ToggleSettings action.
BridgeAction DrawDock(BridgeState* state, const ImVec2& vp_size, float scale) {
    BridgeAction action;
    ImGui::PushFont(cosmic::ui::FontMonoBold());
    ImGui::SetWindowFontScale(11.0f / 13.0f);
    const ImVec2 pair_label = cosmic::ui::TextSpacedSize("PAIR MACHINE", 0.24f);
    const ImVec2 settings_label = cosmic::ui::TextSpacedSize("SETTINGS", 0.24f);
    const ImVec2 pair_size(pair_label.x + 52.0f * scale, pair_label.y + 26.0f * scale);
    const ImVec2 settings_size(settings_label.x + 52.0f * scale,
                               settings_label.y + 26.0f * scale);
    const float gap = 14.0f * scale;
    const float dock_w = pair_size.x + gap + settings_size.x;
    const float dock_x = (vp_size.x - dock_w) * 0.5f;
    const float dock_y = vp_size.y - 34.0f * scale - pair_size.y;

    if (DrawButton("##dock_pair", "PAIR MACHINE", 0.24f,
                   ImVec2(dock_x, dock_y), pair_size,
                   kDockPairBg, kDockPairHover, 0, 0, kText, kText, scale)) {
        // Open the Bridge's own Pair modal with a clean address field.
        state->pair_modal_open = true;
        state->pair_address_buf[0] = '\0';
        state->pair_nickname_buf[0] = '\0';
        state->pair_use_default_port = true;
        state->pair_port_input = 0;
    }
    if (DrawButton("##dock_settings", "SETTINGS", 0.24f,
                   ImVec2(dock_x + pair_size.x + gap, dock_y), settings_size,
                   0, 0, kBorderDim, kPurple, kPurple, kText, scale)) {
        state->settings_open = !state->settings_open;
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    return action;
}

// Session status (bottom-left): DISCONNECT above the SESSION line.
BridgeAction DrawSessionStatus(const BridgeInput& in, const ImVec2& vp_size, float scale) {
    BridgeAction action;
    const float status_x = 34.0f * scale;
    const float status_bottom = vp_size.y - 32.0f * scale;

    if (in.connected_or_connecting) {
        ImGui::PushFont(cosmic::ui::FontMonoBold());
        ImGui::SetWindowFontScale(9.0f / 13.0f);
        const ImVec2 disc_label = cosmic::ui::TextSpacedSize("DISCONNECT", 0.20f);
        const ImVec2 disc_size(disc_label.x + 24.0f * scale, disc_label.y + 14.0f * scale);
        const ImVec2 disc_pos(status_x, status_bottom - 10.0f * scale - disc_size.y);
        if (DrawButton("##disconnect", "DISCONNECT", 0.20f, disc_pos, disc_size,
                       kRed, kRedHover, 0, 0, kText, kText, scale)) {
            action.kind = BridgeAction::Disconnect;
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
    }

    ImGui::PushFont(cosmic::ui::FontMonoRegular());
    ImGui::SetWindowFontScale(10.0f / 13.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Rgba(kCyan));
    ImGui::SetCursorScreenPos(ImVec2(status_x, status_bottom));
    const std::string session_line = "SESSION · " + in.session_label;
    cosmic::ui::TextSpaced(session_line.c_str(), 0.24f);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    return action;
}

}  // namespace

BridgeDrawResult draw_bridge(const BridgeInput& in, BridgeState* state) {
    BridgeDrawResult result;

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
    // stays on top.
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

    // ---- U3: machine cards, bottom dock, session status ----
    // While pairing, the chrome fades to 25% so the monitor PIN stays
    // prominent (design: "scrim + other UI fade to 25%"). The Settings panel
    // below is deliberately NOT faded - it stays at full opacity during
    // pairing (the pair modal's scrim still blocks clicks over it).
    const bool pairing = in.pairing_active;
    if (pairing) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f);
    }
    const cosmic::ui::scene::CursorSmooth cursor = cosmic::ui::scene::smoothed_cursor();
    const int64_t now_unix = static_cast<int64_t>(std::time(nullptr));

    if (in.hosts.empty()) {
        // Empty state: a single beacon card at 8%/30% when there are no hosts.
        result.action = DrawEmptyCard(in, state, vp_size, scale);
    } else {
        // Machine cards orbit the scene center on a tilted ellipse.
        for (size_t i = 0; i < in.hosts.size(); ++i) {
            const ImVec2 center =
                CardOrbitCenter(in, cursor, static_cast<int>(i), vp_size, scale);
            const BridgeAction card_action =
                DrawMachineCard(in, state, in.hosts[i], static_cast<int>(i), center,
                                scale, now_unix);
            if (result.action.kind == BridgeAction::None) {
                result.action = card_action;
            }
        }
    }

    // Bottom dock + session status (fixed, never orbit). Always drawn; only
    // the action assignment is gated so a card action on this frame does not
    // make the dock/status flicker out for one frame.
    const BridgeAction dock_action = DrawDock(state, vp_size, scale);
    if (result.action.kind == BridgeAction::None) {
        result.action = dock_action;
    }
    const BridgeAction status_action = DrawSessionStatus(in, vp_size, scale);
    if (result.action.kind == BridgeAction::None) {
        result.action = status_action;
    }
    if (pairing) {
        ImGui::PopStyleVar();
    }

    // Settings panel (U4): drawn last so it appears above the cards and dock.
    // Always drawn when open; the action is only assigned when no earlier
    // element produced one this frame (same discipline as the dock/status).
    if (state->settings_open) {
        BridgeAction panel_action;
        draw_settings_panel(in, state, &panel_action);
        if (result.action.kind == BridgeAction::None) {
            result.action = panel_action;
        }
    }

    // PIN panel (U4): the viewer-side pairing PIN on the in-scene monitor.
    // Drawn after the chrome so it sits above the cards; emits no actions.
    draw_pin_panel(in, state);

    // Pair modal (U4): drawn last so its scrim covers everything above it.
    if (state->pair_modal_open) {
        BridgeAction modal_action;
        draw_pair_modal(in, state, &modal_action);
        if (result.action.kind == BridgeAction::None) {
            result.action = modal_action;
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();

    // Screen-logo opacity for the scene: full during boot, then fading to 0
    // over the 1.4s window after the overlay clears at 4.4s.
    float screen_logo_alpha = 1.0f;
    if (t >= kBootDurationS) {
        screen_logo_alpha = static_cast<float>(
            std::clamp((kBootDurationS + kLogoFadeS - t) / kLogoFadeS, 0.0, 1.0));
    }
    result.screen_logo_alpha = screen_logo_alpha;
    return result;
}

}  // namespace cosmic::ui::bridge
