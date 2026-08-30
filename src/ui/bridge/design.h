// Cosmic Desk — Bridge design tokens (docs/UI_MIGRATION.md §4).
//
// Every color the Bridge UI and scene renderer use lives here so the two layers
// can never drift apart. Values are lifted 1:1 from the design handoff
// ("Design Tokens" in the handoff README and the prototype's inline styles).
//
// All Bridge geometry is specified in DESIGN PIXELS (the prototype's CSS px at
// 96 DPI) and must be multiplied by cosmic::ui::scale() at draw time, exactly
// like the rest of the classic UI does. Text sizes use the same rule.

#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace cosmic::ui {

// 0xRRGGBBAA packed colors. Helper constructors convert to ImVec4 / SDL_Color.

// Space background (prototype body background, radial gradient center/mid).
inline constexpr uint32_t kBg          = 0x101226FF;  // deep indigo #101226
inline constexpr uint32_t kBgGradCenter = 0x1E2140FF;  // radial 60%/55% center
inline constexpr uint32_t kBgGradMid    = 0x161834FF;  // radial mid stop

// Panels / frames.
inline constexpr uint32_t kPanel      = 0x14172EFF;  // glass panel fill  #14172e
inline constexpr uint32_t kPanelInput = 0x1A1C37FF;  // input fill        #1a1c37
inline constexpr uint32_t kFrame      = 0x23284AFF;  // alt frame fill    #23284a
inline constexpr uint32_t kBorder     = 0x3A3F6BE6;  // rgba(58,63,107,.9)
inline constexpr uint32_t kBorderDim  = 0x6A74BB66;  // rgba(106,116,187,.4)

// Text.
inline constexpr uint32_t kText   = 0xEDF2FBFF;  // primary        #edf2fb
inline constexpr uint32_t kTextDim = 0xA9B1D6FF;  // secondary      #a9b1d6
inline constexpr uint32_t kMuted  = 0x565E86FF;  // muted          #565e86

// Accents.
inline constexpr uint32_t kCyan         = 0x8AC7E5FF;  // data / info    #8ac7e5
inline constexpr uint32_t kLavender     = 0x6A74BBFF;  // interactive    #6a74bb
inline constexpr uint32_t kLavenderDeep = 0x8E6DB8FF;  // hover purple   #8e6db8
inline constexpr uint32_t kPurple       = 0xB897D3FF;  // focus/hover    #b897d3
inline constexpr uint32_t kGreen        = 0x8AC49CFF;  // positive/on    #8ac49c
inline constexpr uint32_t kGreenBtn     = 0x438A70FF;  // button fill    #438a70
inline constexpr uint32_t kGreenHover   = 0x5CAE8AFF;  // button hover   #5cae8a
inline constexpr uint32_t kGreenSel     = 0x609E75FF;  // selected bg    #609e75
inline constexpr uint32_t kAmber        = 0xFFCE54FF;  // standby/warn   #ffce54
inline constexpr uint32_t kRed          = 0xB0556BFF;  // destructive    #b0556b
inline constexpr uint32_t kRedHover     = 0xC9758AFF;  // destructive h  #c9758a

// Overlays.
inline constexpr uint32_t kScrim    = 0x06081480;  // rgba(6,8,20,.5) modal scrim
inline constexpr uint32_t kVignette = 0x0608148C;  // rgba(6,8,20,.55) edge fade

// Beams.
inline constexpr uint32_t kBeam      = 0x8AC7E5FF;  // idle tether beam
inline constexpr uint32_t kBeamGreen = 0x8AC49CFF;  // docking tether beam

// Converts a packed 0xRRGGBBAA token to an ImVec4 (0..1, straight alpha).
inline ImVec4 Rgba(uint32_t packed) {
    return ImVec4(static_cast<float>((packed >> 24) & 0xFF) / 255.0f,
                  static_cast<float>((packed >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((packed >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(packed & 0xFF) / 255.0f);
}

// Converts a packed 0xRRGGBBAA token to an SDL_Color.
inline SDL_Color SdlColor(uint32_t packed) {
    return SDL_Color{static_cast<Uint8>((packed >> 24) & 0xFF),
                     static_cast<Uint8>((packed >> 16) & 0xFF),
                     static_cast<Uint8>((packed >> 8) & 0xFF),
                     static_cast<Uint8>(packed & 0xFF)};
}

// Returns the token brightened (factor > 1) or darkened (factor < 1); alpha is
// unchanged. Used for hover/active tints of arbitrary tokens.
inline uint32_t ScaleBrightness(uint32_t packed, float factor) {
    const auto scale = [factor](uint8_t c) {
        return static_cast<uint8_t>(std::min(
            255.0f, std::max(0.0f, static_cast<float>(c) * factor)));
    };
    const uint8_t r = scale(static_cast<uint8_t>((packed >> 24) & 0xFF));
    const uint8_t g = scale(static_cast<uint8_t>((packed >> 16) & 0xFF));
    const uint8_t b = scale(static_cast<uint8_t>((packed >> 8) & 0xFF));
    return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) | (packed & 0xFF);
}

}  // namespace cosmic::ui
