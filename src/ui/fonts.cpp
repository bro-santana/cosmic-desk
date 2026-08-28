// Cosmic Desk — IBM Plex font atlas implementation. See fonts.h for the
// contract.

#include "ui/fonts.h"

#include <SDL.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace cosmic::ui {
namespace {

// ImGui's built-in ProggyClean is embedded as a TTF, so it rasterizes cleanly
// at any size; 13 px is its design size and therefore our 100% baseline.
constexpr float kBaseFontPx = 13.0f;

// Glyph ranges: Basic Latin + Latin-1 Supplement (the design's `·` middot,
// U+00B7) plus the general-punctuation slice with the dashes and ellipsis
// (U+2013 en dash .. U+2026 ellipsis). IBM Plex covers all of these; symbols
// it lacks (pencil, diamond, close X) stay drawn as ImDrawList primitives.
// The atlas keeps this pointer until Build(), so the array must be static.
constexpr ImWchar kGlyphRanges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};

ImFont* g_sans_regular = nullptr;
ImFont* g_sans_medium = nullptr;
ImFont* g_sans_semibold = nullptr;
ImFont* g_mono_regular = nullptr;
ImFont* g_mono_medium = nullptr;
ImFont* g_mono_bold = nullptr;

// Absolute path to `assets/fonts/<name>` next to the executable. Empty when
// SDL cannot determine the base path.
std::string FontPath(const char* name) {
    char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return std::string();
    }
    const std::string path = std::string(base) + "assets/fonts/" + name;
    SDL_free(base);
    return path;
}

// Loads one face at the scaled size. Logs to stderr and returns nullptr when
// the file is missing or unreadable; the default face is replaced by ImGui's
// built-in font so io.FontDefault always has a valid pointer.
ImFont* LoadFace(ImFontAtlas* atlas, const ImFontConfig& cfg, const char* name,
                 bool is_default) {
    const std::string path = FontPath(name);
    if (path.empty() || !std::filesystem::exists(path)) {
        std::fprintf(stderr, "[ui] font file missing: %s\n", name);
        return is_default ? atlas->AddFontDefault(&cfg) : nullptr;
    }
    ImFont* font = atlas->AddFontFromFileTTF(path.c_str(), cfg.SizePixels, &cfg);
    if (font == nullptr) {
        std::fprintf(stderr, "[ui] failed to load font: %s\n", name);
        return is_default ? atlas->AddFontDefault(&cfg) : nullptr;
    }
    return font;
}

}  // namespace

void LoadFonts(float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.SizePixels = std::floor(kBaseFontPx * ui_scale);
    cfg.GlyphRanges = kGlyphRanges;

    g_sans_regular = LoadFace(io.Fonts, cfg, "IBMPlexSans-Regular.ttf", true);
    g_sans_medium = LoadFace(io.Fonts, cfg, "IBMPlexSans-Medium.ttf", false);
    g_sans_semibold = LoadFace(io.Fonts, cfg, "IBMPlexSans-SemiBold.ttf", false);
    g_mono_regular = LoadFace(io.Fonts, cfg, "IBMPlexMono-Regular.ttf", false);
    g_mono_medium = LoadFace(io.Fonts, cfg, "IBMPlexMono-Medium.ttf", false);
    g_mono_bold = LoadFace(io.Fonts, cfg, "IBMPlexMono-Bold.ttf", false);

    io.Fonts->Build();
    io.FontDefault = g_sans_regular;
}

ImFont* FontSansRegular() {
    return g_sans_regular;
}

ImFont* FontSansMedium() {
    return g_sans_medium;
}

ImFont* FontSansSemiBold() {
    return g_sans_semibold;
}

ImFont* FontMonoRegular() {
    return g_mono_regular;
}

ImFont* FontMonoMedium() {
    return g_mono_medium;
}

ImFont* FontMonoBold() {
    return g_mono_bold;
}

}  // namespace cosmic::ui