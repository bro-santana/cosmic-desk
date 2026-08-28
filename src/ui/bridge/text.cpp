// Cosmic Desk — letter-spaced text implementation. See text.h for the
// contract.
//
// The vendored ImGui (1.91.9) has no per-glyph spacing parameter on
// ImFont::CalcTextSizeA / ImFont::RenderText, so the glyphs are walked here
// and each one is drawn with an extra advance of `spacing` px. The measured
// width sums the same per-glyph advances, so TextSpacedSize always matches
// what TextSpaced draws.

#include "ui/bridge/text.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace cosmic::ui {
namespace {

// Calls `fn(glyph, byte_begin, byte_end)` for every drawable glyph in `text`.
// Control characters and glyphs missing from `font` are skipped, mirroring
// ImFont::RenderText so the measured width matches what is drawn.
template <typename Fn>
void ForEachGlyph(ImFont* font, const char* text, Fn&& fn) {
    const char* s = text;
    while (*s != '\0') {
        const char* glyph_begin = s;
        unsigned int c = static_cast<unsigned int>(*s);
        if (c < 0x80) {
            s += 1;
        } else {
            s += ImTextCharFromUtf8(&c, s, nullptr);
        }
        if (c < 32) {
            continue;
        }
        const ImFontGlyph* glyph = font->FindGlyph(static_cast<ImWchar>(c));
        if (glyph == nullptr) {
            continue;
        }
        fn(glyph, glyph_begin, s);
    }
}

}  // namespace

void TextSpaced(const char* text, float tracking_em) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return;
    }

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float spacing = tracking_em * font_size;
    const float scale = font_size / font->FontSize;

    const ImVec2 text_pos(window->DC.CursorPos.x,
                          window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    float width = 0.0f;
    ForEachGlyph(font, text, [&](const ImFontGlyph* glyph, const char*, const char*) {
        width += glyph->AdvanceX * scale + spacing;
    });
    const ImVec2 text_size(width, font_size);

    ImRect bb(text_pos, ImVec2(text_pos.x + text_size.x, text_pos.y + text_size.y));
    ImGui::ItemSize(text_size, 0.0f);
    if (!ImGui::ItemAdd(bb, 0)) {
        return;
    }

    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec4 clip_rect = window->ClipRect.ToVec4();
    float x = text_pos.x;
    ForEachGlyph(font, text, [&](const ImFontGlyph* glyph, const char* glyph_begin,
                                 const char* glyph_end) {
        font->RenderText(window->DrawList, font_size, ImVec2(x, text_pos.y), col,
                         clip_rect, glyph_begin, glyph_end, 0.0f, false);
        x += glyph->AdvanceX * scale + spacing;
    });
}

ImVec2 TextSpacedSize(const char* text, float tracking_em) {
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float spacing = tracking_em * font_size;
    const float scale = font_size / font->FontSize;
    float width = 0.0f;
    ForEachGlyph(font, text, [&](const ImFontGlyph* glyph, const char*, const char*) {
        width += glyph->AdvanceX * scale + spacing;
    });
    return ImVec2(width, font_size);
}

}  // namespace cosmic::ui