// Cosmic Desk — letter-spaced text helper.
//
// The Bridge design specifies letter-spacing in em units (docs/UI_MIGRATION.md
// §1 A5d); ImGui has no per-glyph tracking, so these helpers render text one
// glyph at a time with an extra advance of tracking_em * font-size px between
// glyphs. TextSpaced() behaves like ImGui::Text() (ImGuiCol_Text, cursor
// advance, item registration) and is intended for the single-line labels the
// design uses.

#pragma once
#include <imgui.h>

namespace cosmic::ui {

// Renders `text` at the current cursor position with per-character
// letter-spacing of tracking_em * current-font-size px (design values are
// em units: .08/.14/.24/.28/.35). Behaves like ImGui::Text: uses
// ImGuiCol_Text (PushStyleColor applies), advances the cursor, registers the
// item. Intended for single-line labels.
void TextSpaced(const char* text, float tracking_em);
// Size TextSpaced would occupy (for SameLine/alignment math).
ImVec2 TextSpacedSize(const char* text, float tracking_em);

}  // namespace cosmic::ui