// Cosmic Desk — Bridge scene renderer implementation.
//
// Pure SDL3 (no ImGui dependency). Layer SVGs are rasterized into SDL textures
// with lunasvg on resize; the background gradient and the 44 twinkles are
// generated procedurally to replicate the prototype exactly.

#include "ui/bridge/scene.h"

#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// stb_image is a header-only library: exactly one translation unit in the
// whole program must define STB_IMAGE_IMPLEMENTATION to emit its function
// bodies, and this is it (PLAN.md D10(e), W3 backdrop decode). The cached
// wallpaper files wallcache.cpp writes are always JPEG, PNG or BMP, so the
// other decoders are compiled out; STBI_NO_STDIO keeps stb off its own fopen
// path since the file is read via std::ifstream below (see
// LoadBackdropTexture). Do not edit the vendored header itself.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include "ui/bridge/design.h"
#include "ui/scale.h"

namespace cosmic::ui::scene {

namespace {

// One parallax layer: the SVG file name, its depth (parallax weight), the
// prototype's ex/ey pixel offsets and es scale, and the base alpha. reflex.svg
// and screen-logo.svg are the exceptions: their alpha is driven per-frame
// instead (the glint value / the caller's boot fade). The drift fields animate
// a slow translate back and forth (prototype CSS keyframes drift/drift2);
// drift_period_s == 0 disables the drift.
struct Layer {
    const char* name;
    float depth;
    float ex;
    float ey;
    float es;
    float alpha;
    float drift_period_s;  // seconds per full cycle; 0 = no drift
    float drift_dx;        // design px at the 50% keyframe
    float drift_dy;        // design px at the 50% keyframe
    float drift_delay_s;   // start delay (negative = phase offset)
    // U5 warp: the end scale the layer reaches at warp 1 (9/12/16 for the
    // three sky layers, which also fade out; 1 = not affected by the warp).
    float warp_end_scale = 1.0f;
    // Nebula band sway: index into kNebulaSway, or -1 for no sway. The nebula
    // was split into 8 bands so each can sway independently; every band keeps
    // the whole-nebula drift/parallax above and adds its own per-band sway.
    int sway = -1;
};

// One nebula band's sway animation (prototype CSS keyframes k1..k8 with
// transform-box: fill-box and transform-origin: 50% 50%). Each band sways on
// its own period with ease-in-out keyframes at 0/25/50/75/100% of the cycle
// (0/100 = identity), staggered by a negative delay. Values are design px/deg;
// the tiny scale varies per keyframe (1 or 1.008). The rotation origin is the
// band's own fill-bbox center, given as canvas % (fx, fy).
struct NebulaSway {
    float period_s;
    float delay_s;  // negative: starts N seconds INTO the cycle
    float k25_tx, k25_ty, k25_rot, k25_sx, k25_sy;
    float k50_tx, k50_ty, k50_rot, k50_sx, k50_sy;
    float k75_tx, k75_ty, k75_rot, k75_sx, k75_sy;
    float fx, fy;  // band center as canvas % (fill-bbox center)
};

// The 8 nebula bands' sway specs, in band order (sway index 0..7). Periods
// stagger 9.6s..20.8s and delays -2.1s..-16.8s so the bands never move in
// lockstep; the rotate/scale keyframes are the prototype's k1..k8. The
// TRANSLATE keyframes are the prototype's values scaled to 30%: the design
// moves the bands inside a fixed clip mask (its clip-paths cannot be applied
// per-frame by SDL), and the full +-12..26px excursions would open visible
// seams and overlaps between the band silhouettes. 30% keeps the breathing
// motion without tearing the nebula apart.
constexpr NebulaSway kNebulaSway[8] = {
    {9.6f,  -2.1f,   5.7f, -5.1f,  0.37f, 1.008f, 1.0f,  -4.0f,  3.1f, 0.0f, 1.0f, 1.008f,  2.9f,  5.1f, -0.37f, 1.0f, 1.0f, 52.4059f, 38.9583f},
    {11.2f, -4.2f,   7.8f, -2.1f,  0.49f, 1.0f,   1.0f,  -5.5f,  1.3f, 0.0f, 1.0f, 1.0f,    3.9f,  2.1f, -0.49f, 1.0f, 1.0f, 52.5910f, 41.7917f},
    {12.8f, -6.3f,   3.6f, -3.6f,  0.61f, 1.008f, 1.0f,  -2.5f,  2.2f, 0.0f, 1.0f, 1.008f,  1.8f,  3.6f, -0.61f, 1.0f, 1.0f, 50.8637f, 39.2500f},
    {14.4f, -8.4f,   5.7f, -5.1f,  0.25f, 1.0f,   1.0f,  -4.0f,  3.1f, 0.0f, 1.0f, 1.0f,    2.9f,  5.1f, -0.25f, 1.0f, 1.0f, 48.3035f, 38.9583f},
    {16.0f, -10.5f,  7.8f, -2.1f,  0.37f, 1.008f, 1.0f,  -5.5f,  1.3f, 0.0f, 1.0f, 1.008f,  3.9f,  2.1f, -0.37f, 1.0f, 1.0f, 45.6817f, 38.9583f},
    {17.6f, -12.6f,  3.6f, -3.6f,  0.49f, 1.0f,   1.0f,  -2.5f,  2.2f, 0.0f, 1.0f, 1.0f,    1.8f,  3.6f, -0.49f, 1.0f, 1.0f, 43.0907f, 33.5000f},
    {19.2f, -14.7f,  5.7f, -5.1f,  0.61f, 1.008f, 1.0f,  -4.0f,  3.1f, 0.0f, 1.0f, 1.008f,  2.9f,  5.1f, -0.61f, 1.0f, 1.0f, 40.4997f, 31.1250f},
    {20.8f, -16.8f,  7.8f, -2.1f,  0.25f, 1.0f,   1.0f,  -5.5f,  1.3f, 0.0f, 1.0f, 1.0f,    3.9f,  2.1f, -0.25f, 1.0f, 1.0f, 41.2091f, 21.2500f},
};

// Draw order = array order (back -> front). Values from the handoff README
// parallax table and the prototype's data-ex/ey/es attributes; the last four
// columns are the drift spec (period/dx/dy/delay, 0 = no drift), the next is
// the U5 warp end scale (1 = not affected) and the last is the nebula sway
// index (-1 = none). The nebula was split into 8 bands (nebula-band-1..8.svg,
// one clip-path group each) so each band can sway independently; they all keep
// the original nebula's depth/offset/scale/alpha and whole-nebula drift.
constexpr Layer kLayers[] = {
{"nebula-band-1.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 0},
{"nebula-band-2.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 1},
{"nebula-band-3.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 2},
{"nebula-band-4.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 3},
{"nebula-band-5.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 4},
{"nebula-band-6.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 5},
{"nebula-band-7.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 6},
{"nebula-band-8.svg", 12.0f,   0.0f, -14.0f, 1.04f, 0.96f, 34.0f, -18.0f,  12.0f,   0.0f, 1.0f, 7},
    {"stars-far.svg",   10.0f,   0.0f, -10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f, 9.0f},
    {"stars-mid.svg",   20.0f,   0.0f, -20.0f, 1.05f, 1.00f, 26.0f,  14.0f, -10.0f,   0.0f, 12.0f},
    {"planets.svg",     13.0f,   0.0f, -32.0f, 1.02f, 1.00f, 32.0f,  14.0f, -10.0f,  -8.0f, 16.0f},
    {"desk.svg",        26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"monitor.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"monitor-bezel.svg", 26.0f, 0.0f, 10.0f, 1.03f, 1.00f, 0.0f, 0.0f, 0.0f, 0.0f},
    {"screen-logo.svg", 26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},  // alpha overridden per-frame (U2)
    {"obj-g11.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"obj-g18.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"obj-g22.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"obj-g24.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"obj-g33.svg",     26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
    {"reflex.svg",      26.0f,   0.0f,  10.0f, 1.03f, 1.00f,  0.0f,   0.0f,   0.0f,   0.0f},
};
constexpr int kLayerCount = static_cast<int>(sizeof(kLayers) / sizeof(kLayers[0]));

// The reflex and screen-logo layers are the exceptions whose alpha is driven
// per-frame (the glint value / the caller's boot fade) instead of the table.
constexpr int kReflexIndex = kLayerCount - 1;
constexpr int kScreenLogoIndex = 14;  // screen-logo.svg entry in kLayers
// The monitor-bezel layer marks where the panel wallpaper draws (PLAN.md
// D10(e)): after monitor.svg's dark panel, before the bezel texture itself.
constexpr int kMonitorBezelIndex = 13;  // monitor-bezel.svg entry in kLayers

// Cursor chase tuning (per-frame factors at a nominal 60 fps, time-corrected
// in draw() like every other easing). The onset ramp mirrors the warp
// re-entry in reverse: while the target moves (or the layers are still
// catching up), the chase rate climbs exponentially from a gentle floor; once
// the motion is done the rate decays back, so the tail glides progressively
// slower -- the exact satisfying pace of the warp return, with the onset as
// its mirror and the whole motion a tad faster (0.06 vs the warp's 0.05).
constexpr float kRamp60 = 0.06f;     // onset/tail time constant
constexpr float kChase60 = 0.085f;   // full chase rate once ramped
constexpr float kFloorFrac = 0.30f;  // minimum rate as a fraction of full
constexpr float kMoveEps = 0.01f;    // target change beyond jitter -> ramp up
constexpr float kSettleEps = 0.02f;  // error below this -> ramp decays

// First layer of the desk group (desk.svg). The warp flash draws just before
// it, between the sky layers and the desk group (UI_MIGRATION A3).
constexpr int kDeskGroupIndex = 11;

// One twinkle dot, generated by the seeded LCG below.
struct Twinkle {
    float x;        // 0..1 across the viewport
    float y;        // 0..1 down the viewport
    float size;     // design px (multiplied by scale() at draw time)
    float period;   // seconds
    float delay;    // seconds
};

// One hyperspace streak particle (design handoff v2 warp). Radii/positions are
// device px derived from the viewport; `width` is a design-px base multiplied
// by scale() at draw time.
struct WarpStar {
    float ang;    // radial direction, radians
    float r;      // head distance from the origin, device px
    float sp;     // speed factor 0.6..2.4
    float width;  // base line width, design px 0.5..1.9
    bool blue;    // 25% ice-blue rgb(178,208,255), rest white
};

// One decoded backdrop texture, keyed by its cache file path (PLAN.md
// D10(e)). Owned by the LRU in State::backdrop_lru; last_use_ms drives
// eviction once the LRU is full. w/h are the decoded image's pixel size,
// needed for the cover-scale crop at draw time.
struct BackdropEntry {
    std::string path;
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
    uint64_t last_use_ms = 0;
};

// Module state. All textures are owned here and rebuilt on resize.
struct State {
    bool initialized = false;
    int tex_w = 0;   // last rasterized texture size (0 = never)
    int tex_h = 0;
    uint64_t last_rasterize_ms = 0;  // SDL_GetTicks() of last rasterize (0 = never)
    SDL_Texture* layers[kLayerCount] = {};
    SDL_Texture* bg = nullptr;
    SDL_Texture* flash = nullptr;  // warp flash (opacity driven by the U5 envelope)
    SDL_Texture* vignette = nullptr;  // full-viewport edge fade, drawn last
    SDL_Texture* streak = nullptr;    // shooting-star streak (fixed 170x2)
    SDL_Texture* glow = nullptr;      // screen glow (fixed 256x256)
    std::vector<Twinkle> twinkles;
    float cx = 0.0f;  // smoothed cursor, -1..1
    float cy = 0.0f;
    float ramp = 0.0f;  // onset ramp 0..1 (see the k* constants above)
    float tx_last = 0.0f;  // last valid cursor target (held while the mouse is
    float ty_last = 0.0f;  // outside the window so the chase keeps gliding)
    float last_time_s = -1.0f;  // previous frame's in.time_s (-1 = first frame)
    // U5 warp transition: warp_t eases toward warp_target each frame (1 =
    // streaming, 0 = bridge). flash_start_s anchors the 2.2s warp flash in
    // draw()'s clock (-1 = not flashing); trigger_warp_flash() only sets
    // flash_pending, which draw() converts to a start time once.
    float warp_t = 0.0f;
    float warp_target = 0.0f;
    double flash_start_s = -1.0;
    bool flash_pending = false;
    // Hyperspace streak field (handoff v2): particles live only while the warp
    // is active (warp_t >= 0.005); the vector is dropped when it ends so the
    // next connect gets a fresh burst. warp_seed drives spawn/respawn
    // randomness (visual only, no determinism requirement).
    std::vector<WarpStar> warp_stars;
    uint64_t warp_seed = 1234;
    // Backdrop (PLAN.md D10(e), W3): decoded wallpaper textures are
    // viewport-independent, so RasterizeAll/resize never touches this state.
    // backdrop_lru caches up to 4 decoded textures by path; backdrop_failed
    // remembers paths whose decode failed so they are logged and skipped once,
    // never retried per frame. backdrop_cur is the texture currently shown;
    // backdrop_prev holds the outgoing one during a crossfade (tex == nullptr
    // when there is none). backdrop_fade_start_s anchors the crossfade in
    // in.time_s (-1 = not crossfading); backdrop_eased_alpha is the current
    // alpha, eased every frame toward in.backdrop_alpha (or toward 0 when the
    // requested path is unresolvable, see backdrop_req_unresolvable).
    // backdrop_req_path/backdrop_req_unresolvable track the last non-empty
    // in.backdrop_path draw() looked up and whether it failed to decode,
    // separately from backdrop_cur.path: this is what lets a bad path be
    // resolved exactly once (not re-scanned every frame) and what tells
    // draw() to fade backdrop_cur's stale texture out rather than keep
    // showing the previous host's wallpaper under the new host's name.
    std::vector<BackdropEntry> backdrop_lru;
    std::vector<std::string> backdrop_failed;
    BackdropEntry backdrop_cur;
    BackdropEntry backdrop_prev;
    double backdrop_fade_start_s = -1.0;
    float backdrop_eased_alpha = 0.0f;
    std::string backdrop_req_path;
    bool backdrop_req_unresolvable = false;
};

State g_state;

// Logs a message to stderr (the Debug build keeps a console for log output).
void LogError(const char* msg) {
    std::fprintf(stderr, "[scene] %s\n", msg);
}

// Destroys a texture and nulls the slot. Safe on null.
void DestroyTexture(SDL_Texture*& tex) {
    if (tex != nullptr) {
        SDL_DestroyTexture(tex);
        tex = nullptr;
    }
}

// Linear interpolation between two 0xRRGGBBAA design tokens, producing a
// 0xAARRGGBB pixel (alpha in the MSB) as SDL_PIXELFORMAT_ARGB8888 expects.
uint32_t LerpColor(uint32_t from, uint32_t to, float t) {
    const auto lerp = [t](uint8_t ca, uint8_t cb) {
        return static_cast<uint8_t>(static_cast<float>(ca) +
                                    (static_cast<float>(cb) - static_cast<float>(ca)) * t);
    };
    const uint8_t r = lerp(static_cast<uint8_t>((from >> 24) & 0xFF),
                           static_cast<uint8_t>((to >> 24) & 0xFF));
    const uint8_t g = lerp(static_cast<uint8_t>((from >> 16) & 0xFF),
                           static_cast<uint8_t>((to >> 16) & 0xFF));
    const uint8_t b = lerp(static_cast<uint8_t>((from >> 8) & 0xFF),
                           static_cast<uint8_t>((to >> 8) & 0xFF));
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// Linear interpolation between two 0xRRGGBBAA design colors, alpha included,
// producing a 0xAARRGGBB pixel (alpha in the MSB) as SDL_PIXELFORMAT_ARGB8888
// expects. Like LerpColor but keeps the alpha ramp: the warp flash fades to
// transparent, which LerpColor cannot express.
uint32_t LerpColorAlpha(uint32_t from, uint32_t to, float t) {
    const auto lerp = [t](uint8_t ca, uint8_t cb) {
        return static_cast<uint8_t>(static_cast<float>(ca) +
                                    (static_cast<float>(cb) - static_cast<float>(ca)) * t);
    };
    const uint8_t r = lerp(static_cast<uint8_t>((from >> 24) & 0xFF),
                           static_cast<uint8_t>((to >> 24) & 0xFF));
    const uint8_t g = lerp(static_cast<uint8_t>((from >> 16) & 0xFF),
                           static_cast<uint8_t>((to >> 16) & 0xFF));
    const uint8_t b = lerp(static_cast<uint8_t>((from >> 8) & 0xFF),
                           static_cast<uint8_t>((to >> 8) & 0xFF));
    const uint8_t a = lerp(static_cast<uint8_t>(from & 0xFF),
                           static_cast<uint8_t>(to & 0xFF));
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// Rasterizes one layer SVG into a texture at (tex_w, tex_h). Returns null on
// any failure (after logging); the caller keeps the slot null.
SDL_Texture* RasterizeLayer(SDL_Renderer* renderer, const Layer& layer, int tex_w,
                            int tex_h) {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) {
        LogError("SDL_GetBasePath failed");
        return nullptr;
    }
    const std::string path = std::string(base) + "assets/ui/layers/" + layer.name;

    std::unique_ptr<lunasvg::Document> doc = lunasvg::Document::loadFromFile(path);
    if (!doc) {
        std::fprintf(stderr, "[scene] failed to load %s\n", path.c_str());
        return nullptr;
    }

    // lunasvg renders through plutovg, whose pixels are PREMULTIPLIED alpha
    // (0xAARRGGBB with the color channels already multiplied by alpha). SDL's
    // SDL_BLENDMODE_BLEND expects STRAIGHT alpha, so we must un-premultiply
    // before upload or the layers would look dark and muddy where translucent.
    lunasvg::Bitmap bitmap = doc->renderToBitmap(tex_w, tex_h, 0x00000000);
    if (bitmap.width() != tex_w || bitmap.height() != tex_h) {
        LogError("lunasvg rendered at an unexpected size");
        return nullptr;
    }

    std::vector<uint32_t> pixels(static_cast<size_t>(tex_w) * tex_h);
    const uint8_t* src = bitmap.data();
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t v = static_cast<uint32_t>(src[i * 4]) |
                           (static_cast<uint32_t>(src[i * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(src[i * 4 + 2]) << 16) |
                           (static_cast<uint32_t>(src[i * 4 + 3]) << 24);
        const uint32_t a = (v >> 24) & 0xFF;
        if (a == 0) {
            pixels[i] = 0;
            continue;
        }
        // Un-premultiply with rounding: straight = (premul * 255 + a/2) / a.
        auto un = [a](uint32_t c) {
            return std::min(255u, (c * 255u + a / 2u) / a);
        };
        const uint32_t r = un((v >> 16) & 0xFF);
        const uint32_t g = un((v >> 8) & 0xFF);
        const uint32_t b = un(v & 0xFF);
        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), tex_w * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    // reflex.svg's alpha mod (1.0 -> 255) is overridden per-frame via glint.
    SDL_SetTextureAlphaMod(tex, static_cast<Uint8>(layer.alpha * 255.0f));
    return tex;
}

// Builds the procedural radial-gradient background texture at (tex_w, tex_h).
// Replicates `radial-gradient(60% 55% at 53% 42%, #1e2140 0%, #161834 55%,
// #101226 100%)`.
SDL_Texture* BuildBackground(SDL_Renderer* renderer, int tex_w, int tex_h) {
    std::vector<uint32_t> pixels(static_cast<size_t>(tex_w) * tex_h);
    for (int y = 0; y < tex_h; ++y) {
        for (int x = 0; x < tex_w; ++x) {
            // Normalized distance from the gradient center (53%, 42%) scaled
            // by the ellipse radii (60% w, 55% h).
            const float dx = (static_cast<float>(x) - 0.53f * tex_w) / (0.60f * tex_w);
            const float dy = (static_cast<float>(y) - 0.42f * tex_h) / (0.55f * tex_h);
            const float d = std::hypot(dx, dy);
            uint32_t color;
            if (d < 0.55f) {
                color = LerpColor(kBgGradCenter, kBgGradMid, d / 0.55f);
            } else {
                color = LerpColor(kBgGradMid, kBg, (d - 0.55f) / 0.45f);
            }
            pixels[static_cast<size_t>(y) * tex_w + x] = color;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (bg) failed: %s\n", SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), tex_w * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (bg) failed: %s\n", SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Warp-flash gradient stops, lifted from the prototype's flash div:
// `radial-gradient(60% 55% at 50% 45%, #ffffff 0%, #cfe4ff 35%,
// rgba(138,199,229,.6) 60%, rgba(16,18,38,0) 100%)`. 0xRRGGBBAA packed.
constexpr uint32_t kFlashCore = 0xFFFFFFFF;  // #ffffff
constexpr uint32_t kFlashMid  = 0xCFE4FFFF;  // #cfe4ff
constexpr uint32_t kFlashSoft = 0x8AC7E599;  // rgba(138,199,229,.6)
constexpr uint32_t kFlashEdge = 0x10122600;  // rgba(16,18,38,0)

// Builds the warp-flash texture at (tex_w, tex_h): a white radial gradient
// that U5 fades in and out behind the desk during the warp. Replicates the
// prototype's flash div gradient (center 50%/45%, ellipse radii 60%/55%). The
// texture is fully opaque white where lit; draw() fades the whole thing with
// SDL_SetTextureAlphaMod.
SDL_Texture* BuildFlash(SDL_Renderer* renderer, int tex_w, int tex_h) {
    std::vector<uint32_t> pixels(static_cast<size_t>(tex_w) * tex_h);
    for (int y = 0; y < tex_h; ++y) {
        for (int x = 0; x < tex_w; ++x) {
            // Normalized distance from the gradient center (50%, 45%) scaled
            // by the ellipse radii (60% w, 55% h), like BuildBackground.
            const float dx = (static_cast<float>(x) - 0.50f * tex_w) / (0.60f * tex_w);
            const float dy = (static_cast<float>(y) - 0.45f * tex_h) / (0.55f * tex_h);
            const float d = std::hypot(dx, dy);
            uint32_t color;
            if (d < 0.35f) {
                color = LerpColorAlpha(kFlashCore, kFlashMid, d / 0.35f);
            } else if (d < 0.60f) {
                color = LerpColorAlpha(kFlashMid, kFlashSoft, (d - 0.35f) / 0.25f);
            } else {
                // Beyond the 100% stop a CSS radial-gradient is the final
                // color (transparent), so clamp the last lerp.
                color = LerpColorAlpha(kFlashSoft, kFlashEdge,
                                       std::min(1.0f, (d - 0.60f) / 0.40f));
            }
            pixels[static_cast<size_t>(y) * tex_w + x] = color;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (flash) failed: %s\n",
                     SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), tex_w * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (flash) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Builds the vignette texture at (tex_w, tex_h): a radial edge fade that
// darkens the scene corners. Replicates the prototype's vignette div
// `radial-gradient(120% 100% at 50% 45%, rgba(0,0,0,0) 55%, rgba(6,8,20,.55)
// 100%)` — transparent inside the 55% ellipse, ramping to rgba(6,8,20,.55) at
// the 100% stop and clamped beyond. Because the texture is drawn full→full
// over the viewport, the gradient fractions map 1:1 onto the viewport; the
// radii here are 60%/50% so the full 0.55 alpha is actually reached at the
// edges (with the prototype's literal 120%/100% radii the 100% stop would sit
// outside the viewport and the corners would barely darken). Color is the
// kVignette rgb with a computed alpha; emitted as 0xAARRGGBB.
SDL_Texture* BuildVignette(SDL_Renderer* renderer, int tex_w, int tex_h) {
    std::vector<uint32_t> pixels(static_cast<size_t>(tex_w) * tex_h);
    const uint32_t vr = (kVignette >> 24) & 0xFF;  // 6
    const uint32_t vg = (kVignette >> 16) & 0xFF;  // 8
    const uint32_t vb = (kVignette >> 8) & 0xFF;   // 20
    for (int y = 0; y < tex_h; ++y) {
        for (int x = 0; x < tex_w; ++x) {
            // Normalized distance from the gradient center (50%, 45%) scaled by
            // the ellipse radii (60% w, 50% h), like BuildBackground/Flash.
            const float dx = (static_cast<float>(x) - 0.50f * tex_w) / (0.60f * tex_w);
            const float dy = (static_cast<float>(y) - 0.45f * tex_h) / (0.50f * tex_h);
            const float d = std::hypot(dx, dy);
            // 0 inside the 55% stop, ramping to 0.55 at d = 1.0, clamped after.
            float a = 0.0f;
            if (d > 0.55f) {
                a = 0.55f * std::min(1.0f, (d - 0.55f) / 0.45f);
            }
            const uint32_t alpha = static_cast<uint32_t>(a * 255.0f);
            pixels[static_cast<size_t>(y) * tex_w + x] =
                (alpha << 24) | (vr << 16) | (vg << 8) | vb;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (vignette) failed: %s\n",
                     SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), tex_w * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (vignette) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Cap on decoded backdrop image dimensions (PLAN.md D10(e)): anything larger
// is treated as a decode failure rather than risking a huge allocation from a
// corrupt or oversized cache file.
constexpr int kBackdropMaxDim = 8192;

// Cap on the backdrop LRU (PLAN.md D10(e), W3): only the current and previous
// (crossfade) textures ever need to stay resident; 4 gives headroom for quick
// back-and-forth hovering between cards without unbounded growth.
constexpr int kBackdropLruCap = 4;

// Backdrop fade tuning (PLAN.md D10(e), W3). kBackdropEaseRate60 is the
// per-frame factor (time-corrected like every other easing in this file,
// e.g. warp_k below) that ramps the eased alpha toward in.backdrop_alpha,
// chosen so the ramp takes ~250ms; kBackdropCrossfadeS is the duration of the
// path-change crossfade; kBackdropImageAlpha/kBackdropScrimAlpha are the
// image/scrim alpha at full presence (eased alpha 1, warp_t 0);
// kBackdropMinAlpha is the visibility floor below which a draw is skipped
// entirely (like kMoveEps/kSettleEps above, named rather than a bare literal).
constexpr float kBackdropEaseRate60 = 0.13f;
constexpr float kBackdropCrossfadeS = 0.25f;
constexpr float kBackdropImageAlpha = 0.40f;
constexpr float kBackdropScrimAlpha = 0.45f;
constexpr float kBackdropMinAlpha = 0.002f;
// The in-scene monitor panel's wallpaper (PLAN.md D10(e)) is not capped at
// kBackdropImageAlpha and has no scrim: unlike the fullscreen backdrop behind
// the UI, it must read as a real desktop rather than a dim silhouette.
constexpr float kBackdropPanelAlpha = 1.0f;

// Decodes a cached wallpaper image (JPEG/PNG/BMP only -- the only formats
// wallcache.cpp ever writes) into an RGBA SDL texture. Reads the whole file
// into memory with std::ifstream rather than stb's own fopen path
// (STBI_NO_STDIO), then decodes via stb, which also magic-byte-sniffs the
// format itself, so any other file type simply fails to decode here.
// stbi_load_from_memory(req_comp=4) always emits bytes in R,G,B,A memory
// order; SDL_PIXELFORMAT_ABGR8888 names a packed 32-bit pixel whose bytes are,
// on a little-endian machine, that same R,G,B,A order (SDL_pixels.h documents
// SDL_Color's r,g,b,a struct layout as matching "SDL_PIXELFORMAT_ABGR8888 on
// little-endian systems"), so the decoded bytes upload with no channel swap.
// Returns null on any failure (unreadable/corrupt/oversized); the caller logs
// once per path via LogError.
SDL_Texture* LoadBackdropTexture(SDL_Renderer* renderer, const std::string& path,
                                 int& out_w, int& out_h) {
    // `path` is UTF-8 (wallcache.cpp builds it from std::filesystem::path);
    // std::ifstream(const std::string&) would open it via the CRT's narrow
    // fopen, which decodes bytes with the process ANSI code page and mangles
    // any non-ASCII profile directory (e.g. C:\Users\Jos\xe9\...). Going
    // through std::filesystem::path(const char*) instead decodes the UTF-8
    // correctly on this (libstdc++/MinGW) toolchain, so the open round-trips.
    // Do not "simplify" this back to a bare std::string overload.
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        return nullptr;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return nullptr;
    }

    // stbi_info_from_memory reads just the header, so an oversized image is
    // rejected before the full decode/allocation below.
    int w = 0;
    int h = 0;
    int comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w,
                               &h, &comp) ||
        w <= 0 || h <= 0 || w > kBackdropMaxDim || h > kBackdropMaxDim) {
        return nullptr;
    }

    unsigned char* pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (pixels == nullptr) {
        return nullptr;
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (backdrop) failed: %s\n",
                     SDL_GetError());
        stbi_image_free(pixels);
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels, w * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (backdrop) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyTexture(tex);
        stbi_image_free(pixels);
        return nullptr;
    }
    stbi_image_free(pixels);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    out_w = w;
    out_h = h;
    return tex;
}

// Fetches the decoded texture for `path` from the LRU, decoding and inserting
// it on a miss. Returns null -- after logging once and remembering the path in
// backdrop_failed -- for a path that fails to decode; a path already in
// backdrop_failed is never retried. On a cache hit this is a linear scan over
// at most 4 entries: no decode, no file I/O, no allocation.
BackdropEntry* ResolveBackdropTexture(SDL_Renderer* renderer, const std::string& path,
                                      uint64_t now_ms) {
    for (BackdropEntry& e : g_state.backdrop_lru) {
        if (e.path == path) {
            e.last_use_ms = now_ms;
            return &e;
        }
    }
    if (std::find(g_state.backdrop_failed.begin(), g_state.backdrop_failed.end(),
                 path) != g_state.backdrop_failed.end()) {
        return nullptr;
    }

    int w = 0;
    int h = 0;
    SDL_Texture* tex = LoadBackdropTexture(renderer, path, w, h);
    if (tex == nullptr) {
        const std::string msg = "failed to decode backdrop " + path;
        LogError(msg.c_str());
        g_state.backdrop_failed.push_back(path);
        return nullptr;
    }

    if (static_cast<int>(g_state.backdrop_lru.size()) >= kBackdropLruCap) {
        // Evict the least-recently-used entry that isn't pinned as the
        // current or previous (crossfade) texture; with a cap of 4 and at
        // most 2 pinned slots there is always an evictable entry.
        auto victim = g_state.backdrop_lru.end();
        for (auto it = g_state.backdrop_lru.begin();
             it != g_state.backdrop_lru.end(); ++it) {
            if (it->tex == g_state.backdrop_cur.tex ||
                it->tex == g_state.backdrop_prev.tex) {
                continue;
            }
            if (victim == g_state.backdrop_lru.end() ||
                it->last_use_ms < victim->last_use_ms) {
                victim = it;
            }
        }
        if (victim != g_state.backdrop_lru.end()) {
            DestroyTexture(victim->tex);
            g_state.backdrop_lru.erase(victim);
        }
    }

    g_state.backdrop_lru.push_back(BackdropEntry{path, tex, w, h, now_ms});
    return &g_state.backdrop_lru.back();
}

// Draws a decoded backdrop entry cover-scaled into `dst`, cropping whichever
// source axis overflows dst's aspect so no letterboxing shows. Shared by the
// fullscreen backdrop and the in-scene monitor panel wallpaper (PLAN.md
// D10(e)): both crop the same way, only the dest rect and alpha differ.
// No-ops on a null/empty entry or an alpha at or below kBackdropMinAlpha.
void DrawBackdropCovered(SDL_Renderer* renderer, const BackdropEntry& e,
                         const SDL_FRect& dst, float alpha) {
    if (e.tex == nullptr || e.w <= 0 || e.h <= 0 || alpha <= kBackdropMinAlpha) {
        return;
    }
    SDL_Rect src{0, 0, e.w, e.h};
    const float img_aspect = static_cast<float>(e.w) / static_cast<float>(e.h);
    const float dst_aspect = dst.w / dst.h;
    if (img_aspect > dst_aspect) {
        src.w = std::min(
            e.w, static_cast<int>(std::lround(static_cast<float>(e.h) * dst.w / dst.h)));
        src.x = (e.w - src.w) / 2;
    } else {
        src.h = std::min(
            e.h, static_cast<int>(std::lround(static_cast<float>(e.w) * dst.h / dst.w)));
        src.y = (e.h - src.h) / 2;
    }
    SDL_SetTextureAlphaMod(
        e.tex, static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    SDL_FRect fsrc;
    SDL_RectToFRect(&src, &fsrc);
    SDL_RenderTexture(renderer, e.tex, &fsrc, &dst);
}

// Builds the shooting-star streak texture at a fixed 170x2 (the dest rect
// scales it to 170*scale x 2*scale at draw time, so it is built once at init,
// not per resize). Replicates the prototype's streak div `linear-gradient(90deg,
// rgba(237,242,251,0), #edf2fb)`: transparent at the left end ramping to opaque
// kText at the right end — the bright head trails as the streak flies left-down.
SDL_Texture* BuildStreak(SDL_Renderer* renderer) {
    constexpr int kStreakW = 170;
    constexpr int kStreakH = 2;
    std::vector<uint32_t> pixels(static_cast<size_t>(kStreakW) * kStreakH);
    const uint32_t sr = (kText >> 24) & 0xFF;  // 237
    const uint32_t sg = (kText >> 16) & 0xFF;  // 242
    const uint32_t sb = (kText >> 8) & 0xFF;   // 251
    for (int y = 0; y < kStreakH; ++y) {
        for (int x = 0; x < kStreakW; ++x) {
            const float t = static_cast<float>(x) / (kStreakW - 1);
            const uint32_t alpha = static_cast<uint32_t>(t * 255.0f);
            pixels[static_cast<size_t>(y) * kStreakW + x] =
                (alpha << 24) | (sr << 16) | (sg << 8) | sb;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, kStreakW,
                                         kStreakH);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (streak) failed: %s\n",
                     SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), kStreakW * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (streak) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Builds the screen-glow texture at a fixed 256x256 (the dest rect scales it
// to 34%/30% of the desk rect at draw time, so it is built once at init, not
// per resize). Replicates the prototype's glow div `radial-gradient(closest-side,
// rgba(138,199,229,.28), rgba(138,199,229,0))`: kCyan rgb with alpha
// 0.28 * max(0, 1 - d), where d is the normalized distance from the center
// (closest-side = the box edge at d = 1, clamped beyond). The .28 is baked in;
// the per-frame flicker is applied via SDL_SetTextureAlphaMod.
SDL_Texture* BuildGlow(SDL_Renderer* renderer) {
    constexpr int kGlowSize = 256;
    std::vector<uint32_t> pixels(static_cast<size_t>(kGlowSize) * kGlowSize);
    const uint32_t gr = (kCyan >> 24) & 0xFF;  // 138
    const uint32_t gg = (kCyan >> 16) & 0xFF;  // 199
    const uint32_t gb = (kCyan >> 8) & 0xFF;   // 229
    for (int y = 0; y < kGlowSize; ++y) {
        for (int x = 0; x < kGlowSize; ++x) {
            const float dx =
                (static_cast<float>(x) - kGlowSize / 2.0f) / (kGlowSize / 2.0f);
            const float dy =
                (static_cast<float>(y) - kGlowSize / 2.0f) / (kGlowSize / 2.0f);
            const float d = std::hypot(dx, dy);
            const float a = 0.28f * std::max(0.0f, 1.0f - d);
            const uint32_t alpha = static_cast<uint32_t>(a * 255.0f);
            pixels[static_cast<size_t>(y) * kGlowSize + x] =
                (alpha << 24) | (gr << 16) | (gg << 8) | gb;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, kGlowSize,
                                         kGlowSize);
    if (tex == nullptr) {
        std::fprintf(stderr, "[scene] SDL_CreateTexture (glow) failed: %s\n",
                     SDL_GetError());
        return nullptr;
    }
    if (!SDL_UpdateTexture(tex, nullptr, pixels.data(), kGlowSize * 4)) {
        std::fprintf(stderr, "[scene] SDL_UpdateTexture (glow) failed: %s\n",
                     SDL_GetError());
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Generates the 44 twinkle dots with the prototype's exact seeded LCG (seed 42).
void BuildTwinkles() {
    g_state.twinkles.clear();
    g_state.twinkles.reserve(44);

    uint64_t seed = 42;
    const auto rnd = [&seed]() {
        // 64-bit arithmetic: the prototype runs this LCG in JavaScript, where
        // the multiply is exact (doubles), so a 32-bit seed would wrap early
        // and diverge from it.
        seed = (seed * 16807u) % 2147483647u;
        return static_cast<float>(seed) / 2147483647.0f;
    };

    for (int i = 0; i < 44; ++i) {
        Twinkle t;
        t.size = 1.0f + rnd() * 2.2f;
        t.x = rnd();
        t.y = rnd();
        t.period = 2.0f + rnd() * 3.5f;
        t.delay = rnd() * 5.0f;
        g_state.twinkles.push_back(t);
    }
}

// Scratch buffers reused across dashed draws so the hot path never allocates.
// Sized once (grown on first use, then reused); cleared per helper call.
std::vector<SDL_Vertex> g_dash_verts;
std::vector<int> g_dash_indices;

// Appends a filled quad spanning the segment (x0,y0)->(x1,y1) to the scratch
// geometry buffers. The quad is a rectangle of `width` centered on the segment
// and oriented perpendicular to it (the local normal). Used by both the dashed
// ellipse and dashed line helpers; the caller flushes the buffers once.
void AppendDashQuad(float x0, float y0, float x1, float y1, float width,
                    Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::hypot(dx, dy);
    if (len <= 0.0f) {
        return;
    }
    const float hw = width * 0.5f;
    const float nx = -dy / len * hw;
    const float ny = dx / len * hw;
    // SDL_Vertex::color is an SDL_FColor (0..1 floats), not the SDL_Color
    // (0..255 Uint8) the callers pass in, so normalize before packing.
    const SDL_FColor col{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    const size_t base = g_dash_verts.size();
    g_dash_verts.push_back(SDL_Vertex{{x0 - nx, y0 - ny}, col, {0.0f, 0.0f}});
    g_dash_verts.push_back(SDL_Vertex{{x0 + nx, y0 + ny}, col, {0.0f, 0.0f}});
    g_dash_verts.push_back(SDL_Vertex{{x1 + nx, y1 + ny}, col, {0.0f, 0.0f}});
    g_dash_verts.push_back(SDL_Vertex{{x1 - nx, y1 - ny}, col, {0.0f, 0.0f}});
    const int i0 = static_cast<int>(base);
    g_dash_indices.push_back(i0);
    g_dash_indices.push_back(i0 + 1);
    g_dash_indices.push_back(i0 + 2);
    g_dash_indices.push_back(i0);
    g_dash_indices.push_back(i0 + 2);
    g_dash_indices.push_back(i0 + 3);
}

// Draws everything accumulated in the scratch buffers as one geometry call,
// then clears them for the next dashed draw.
void FlushDashes(SDL_Renderer* renderer) {
    if (g_dash_indices.empty()) {
        return;
    }
    SDL_RenderGeometry(renderer, nullptr, g_dash_verts.data(),
                       static_cast<int>(g_dash_verts.size()),
                       g_dash_indices.data(),
                       static_cast<int>(g_dash_indices.size()));
    g_dash_verts.clear();
    g_dash_indices.clear();
}

// Forward declaration: DeskDest is defined further down (used by screen_rect
// too); the streak field needs it for its origin point.
SDL_FRect DeskDest(int out_w, int out_h);

// Hyperspace streak field (handoff v2): 520 particles radiating from the
// monitor beacon point (53.1%/41.4% of the desk art box -- where the machine
// cards tether), drawn between the shooting star and the warp flash so the
// desk group and all UI occlude them. Replicates
// the prototype's drawWarpStreaks() 1:1, with the per-frame radial step
// dt-corrected to the nominal 60fps the prototype assumes. Lines are emitted
// as rotated quads through the shared geometry scratch buffers (the handoff's
// own SDL2 note: round caps are not worth a texture at 0.5-3px widths).
void UpdateAndDrawWarpStreaks(SDL_Renderer* renderer, int out_w, int out_h,
                              float dt) {
    const float w = g_state.warp_t;
    if (w < 0.005f) {
        // Warp over: drop the field so the next connect starts a fresh burst.
        if (!g_state.warp_stars.empty()) {
            g_state.warp_stars.clear();
            g_state.warp_stars.shrink_to_fit();
        }
        return;
    }

    const auto rnd = [] {
        g_state.warp_seed = (g_state.warp_seed * 16807u) % 2147483647u;
        return static_cast<float>(g_state.warp_seed) / 2147483647.0f;
    };
    const float diag = std::hypot(static_cast<float>(out_w),
                                  static_cast<float>(out_h));
    if (g_state.warp_stars.empty()) {
        g_state.warp_stars.reserve(520);
        for (int i = 0; i < 520; ++i) {
            WarpStar s;
            s.ang = rnd() * 2.0f * 3.14159265f;
            s.r = 6.0f + std::pow(rnd(), 0.6f) * diag * 0.55f;
            s.sp = 0.6f + rnd() * 1.8f;
            s.width = 0.5f + rnd() * 1.4f;
            s.blue = rnd() < 0.25f;
            g_state.warp_stars.push_back(s);
        }
    }

    // Origin: the monitor beacon point (53.1%/41.4% of the desk art box) --
    // the same point the machine cards tether to in bridge.cpp -- rather than
    // a fixed 50%/45% of the viewport, which drifted from the on-screen
    // monitor as soon as the desk parallax moved.
    const SDL_FRect desk = DeskDest(out_w, out_h);
    const float origin_x = desk.x + 0.531f * desk.w;
    const float origin_y = desk.y + 0.414f * desk.h;
    const float max_r = diag * 0.62f;
    const float layer_alpha = std::min(1.0f, w * 2.2f);
    const float step_frames = dt * 60.0f;  // prototype step is per 60fps frame
    const float scale = cosmic::ui::scale();

    for (WarpStar& s : g_state.warp_stars) {
        s.r += s.sp * w * (2.0f + s.r * 0.045f) * step_frames;
        if (s.r > max_r) {
            s.r = 4.0f + rnd() * 30.0f;
            s.ang = rnd() * 2.0f * 3.14159265f;
        }
        const float cos_a = std::cos(s.ang);
        const float sin_a = std::sin(s.ang);
        const float tail = std::max(2.0f, s.r - (s.r * 0.55f + 30.0f) * w * s.sp);
        const float alpha = layer_alpha * std::min(1.0f, 0.15f + s.r / max_r);
        const float width = s.width * (0.6f + (s.r / max_r) * 1.6f) * scale;
        const Uint8 cr = s.blue ? 178 : 255;
        const Uint8 cg = s.blue ? 208 : 255;
        const Uint8 cb = 255;
        AppendDashQuad(origin_x + cos_a * tail, origin_y + sin_a * tail,
                       origin_x + cos_a * s.r, origin_y + sin_a * s.r, width,
                       cr, cg, cb,
                       static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    }
    FlushDashes(renderer);
}

// Returns the dash-pattern phase at arc length `len`, normalized to [0, period)
// so a negative dash_offset (allowed, e.g. the beams' drifting dashoffset)
// still lands in the correct window. A point is "on" when phase < dash_on.
float DashPhase(float len, float dash_offset, float period) {
    float phase = std::fmod(len + dash_offset, period);
    if (phase < 0.0f) {
        phase += period;
    }
    return phase;
}

// Draws a dashed ellipse centered at (cx,cy) with radii (rx,ry), rotated by
// rotation_deg. Walks the curve in ~2048 steps accumulating arc length and
// emits a dash quad wherever the running length lands in an on-window. Dash
// sizes and line width are in device px (the caller multiplies by ui::scale()).
void DrawDashedEllipse(SDL_Renderer* renderer, float cx, float cy, float rx,
                       float ry, float rotation_deg, Uint8 r, Uint8 g, Uint8 b,
                       Uint8 a, float dash_on, float dash_gap, float dash_offset,
                       float line_width) {
    if (rx <= 0.0f || ry <= 0.0f) {
        return;
    }
    const float period = dash_on + dash_gap;
    if (period <= 0.0f) {
        return;
    }
    const float rot = rotation_deg * 3.14159265f / 180.0f;
    const float cosr = std::cos(rot);
    const float sinr = std::sin(rot);

    constexpr int kSteps = 2048;
    float prev_x = 0.0f;
    float prev_y = 0.0f;
    float prev_len = 0.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        const float ang = 2.0f * 3.14159265f * t;
        const float ex = rx * std::cos(ang);
        const float ey = ry * std::sin(ang);
        // Rotation is applied to the sample points around the center.
        const float px = cx + ex * cosr - ey * sinr;
        const float py = cy + ex * sinr + ey * cosr;
        if (i > 0) {
            const float len = prev_len + std::hypot(px - prev_x, py - prev_y);
            if (DashPhase(len, dash_offset, period) < dash_on) {
                AppendDashQuad(prev_x, prev_y, px, py, line_width, r, g, b, a);
            }
            prev_len = len;
        }
        prev_x = px;
        prev_y = py;
    }
    FlushDashes(renderer);
}

// Draws a dashed line from (x1,y1) to (x2,y2). Walks the single segment in
// ~1 device-px steps accumulating arc length, emitting a dash quad wherever the
// running length lands in an on-window. Dash sizes and line width are in device
// px (the caller multiplies by ui::scale()). U3's tether beams reuse this.
void DrawDashedLine(SDL_Renderer* renderer, float x1, float y1, float x2,
                    float y2, Uint8 r, Uint8 g, Uint8 b, Uint8 a, float dash_on,
                    float dash_gap, float dash_offset, float line_width) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float total = std::hypot(dx, dy);
    if (total <= 0.0f) {
        return;
    }
    const float period = dash_on + dash_gap;
    if (period <= 0.0f) {
        return;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(total)));
    float prev_x = x1;
    float prev_y = y1;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        const float px = x1 + dx * t;
        const float py = y1 + dy * t;
        if (DashPhase(total * t, dash_offset, period) < dash_on) {
            AppendDashQuad(prev_x, prev_y, px, py, line_width, r, g, b, a);
        }
        prev_x = px;
        prev_y = py;
    }
    FlushDashes(renderer);
}

// Re-rasterizes every layer texture and the background at the given size.
// Destroys the old textures first. On any per-layer failure the slot stays
// null and the rest continue.
void RasterizeAll(SDL_Renderer* renderer, int tex_w, int tex_h) {
    for (int i = 0; i < kLayerCount; ++i) {
        DestroyTexture(g_state.layers[i]);
        g_state.layers[i] = RasterizeLayer(renderer, kLayers[i], tex_w, tex_h);
    }
    DestroyTexture(g_state.bg);
    g_state.bg = BuildBackground(renderer, tex_w, tex_h);
    DestroyTexture(g_state.flash);
    g_state.flash = BuildFlash(renderer, tex_w, tex_h);
    DestroyTexture(g_state.vignette);
    g_state.vignette = BuildVignette(renderer, tex_w, tex_h);
    g_state.tex_w = tex_w;
    g_state.tex_h = tex_h;
    g_state.last_rasterize_ms = SDL_GetTicks();
}

// Parallax strength for the current frame, guarded against non-finite values:
// U5 will animate motion, and a NaN would re-poison every dest rect the same
// way the mouse NaN did. screen_rect() has no motion input, so it uses the
// guarded default (1.0, which main.cpp always passes today).
float GuardedMotion(float motion) {
    return std::isfinite(motion) ? motion : 1.0f;
}

// The screen glow's 7s flicker opacity (prototype `flick` keyframes):
// piecewise-linear between the 0/42/48/55/70/100% cycle points at
// .5/.72/.42/.8/.58/.5.
float FlickerOpacity(float t7) {
    constexpr float kFlickTimes[] = {0.00f, 0.42f, 0.48f, 0.55f, 0.70f, 1.00f};
    constexpr float kFlickOpacities[] = {0.50f, 0.72f, 0.42f, 0.80f, 0.58f, 0.50f};
    constexpr int kFlickCount = 6;
    const float p = t7 / 7.0f;  // 0..1 across the cycle
    for (int i = 1; i < kFlickCount; ++i) {
        if (p <= kFlickTimes[i]) {
            const float t =
                (p - kFlickTimes[i - 1]) / (kFlickTimes[i] - kFlickTimes[i - 1]);
            return kFlickOpacities[i - 1] +
                   (kFlickOpacities[i] - kFlickOpacities[i - 1]) * t;
        }
    }
    return kFlickOpacities[kFlickCount - 1];
}

// The warp flash's 2.2s opacity envelope (prototype connect()): piecewise
// linear 0 -> 0 over the first 55% of the window, -> 1 at 72%, -> 0 at 100%
// (peak ~72% through the window, per the handoff README).
float WarpFlashAlpha(double elapsed_s) {
    const float p = static_cast<float>(elapsed_s / 2.2);
    if (p < 0.55f) {
        return 0.0f;
    }
    if (p < 0.72f) {
        return (p - 0.55f) / (0.72f - 0.55f);
    }
    if (p < 1.0f) {
        return 1.0f - (p - 0.72f) / (1.0f - 0.72f);
    }
    return 0.0f;
}

// Samples a nebula band's sway animation at the given time, returning the
// interpolated translate (design px), rotation (deg) and scale. Ease-in-out
// (Hermite) between the 0/25/50/75/100% keyframes; 0/100 are identity.
void SampleSway(const NebulaSway& s, float time_s, float& tx, float& ty,
                float& rot, float& sx, float& sy) {
    // delay_s is NEGATIVE, so (time - delay) starts the cycle N seconds INTO
    // it (CSS animation-delay semantics), like the drift phase above.
    float p = std::fmod((time_s - s.delay_s) / s.period_s, 1.0f);
    if (p < 0.0f) {
        p += 1.0f;
    }
    // Keyframe array: (frac, tx, ty, rot, sx, sy); 0 and 1 are identity.
    const float kf[5][6] = {
        {0.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        {0.25f, s.k25_tx, s.k25_ty, s.k25_rot, s.k25_sx, s.k25_sy},
        {0.50f, s.k50_tx, s.k50_ty, s.k50_rot, s.k50_sx, s.k50_sy},
        {0.75f, s.k75_tx, s.k75_ty, s.k75_rot, s.k75_sx, s.k75_sy},
        {1.00f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
    };
    // Find the segment containing p (p in [0,1) after fmod; p == 0 lands in
    // the first segment at u = 0, i.e. identity).
    int seg = 0;
    for (int i = 1; i < 5; ++i) {
        if (p <= kf[i][0]) {
            seg = i - 1;
            break;
        }
    }
    const float u = (p - kf[seg][0]) / (kf[seg + 1][0] - kf[seg][0]);
    const float e = u * u * (3.0f - 2.0f * u);  // Hermite ease-in-out
    const auto lerp = [e](float a, float b) { return a + (b - a) * e; };
    tx = lerp(kf[seg][1], kf[seg + 1][1]);
    ty = lerp(kf[seg][2], kf[seg + 1][2]);
    rot = lerp(kf[seg][3], kf[seg + 1][3]);
    sx = lerp(kf[seg][4], kf[seg + 1][4]);
    sy = lerp(kf[seg][5], kf[seg + 1][5]);
}

// The desk group's dest rect for the given viewport: the art box scaled by the
// desk layer's es and translated by its parallax offsets (ex/ey minus depth
// times the smoothed cursor). All desk-group layers share these values, so
// screen_rect() and the glow map percentages of the art box onto this rect.
// Motion is the guarded parallax strength (1.0 today: main.cpp always passes
// 1.0 and screen_rect has no motion input).
SDL_FRect DeskDest(int out_w, int out_h) {
    const float art_w = std::max(1.04f * out_w, 1.4f * out_h);
    const float art_h = art_w * 1200.0f / 1620.8481f;
    const Layer& desk = kLayers[kDeskGroupIndex];
    const float w = art_w * desk.es;
    const float h = art_h * desk.es;
    const float motion = GuardedMotion(1.0f);
    const float ox = desk.ex - g_state.cx * desk.depth * motion;
    const float oy = desk.ey - g_state.cy * desk.depth * motion;
    return SDL_FRect{
        static_cast<float>(out_w) / 2.0f - w / 2.0f + ox,
        static_cast<float>(out_h) / 2.0f - h / 2.0f + oy,
        w,
        h,
    };
}

// The monitor screen panel's rect within the desk art box, as fractions of
// DeskDest (handoff README, monitor.svg's PanelOff bbox: x=629.9053,
// y=354.88638, w=462.98959, h=283.96115 of the 1620.8481x1200 art box).
// screen_rect() (public) and the panel wallpaper draw in draw() both derive
// from ScreenPanelDest so the fractions exist once.
constexpr float kScreenPanelLeftFrac = 0.3886f;
constexpr float kScreenPanelTopFrac = 0.2957f;
constexpr float kScreenPanelWidthFrac = 0.2856f;
constexpr float kScreenPanelHeightFrac = 0.2366f;

SDL_FRect ScreenPanelDest(int out_w, int out_h) {
    const SDL_FRect desk = DeskDest(out_w, out_h);
    return SDL_FRect{
        desk.x + kScreenPanelLeftFrac * desk.w,
        desk.y + kScreenPanelTopFrac * desk.h,
        kScreenPanelWidthFrac * desk.w,
        kScreenPanelHeightFrac * desk.h,
    };
}

}  // namespace

void init(SDL_Renderer* renderer) {
    if (g_state.initialized) {
        return;
    }
    if (renderer == nullptr) {
        LogError("init called with a null renderer");
        return;
    }
    // Geometry draws (twinkles) use the blend path; set it once here and again
    // at the top of draw() because ImGui_ImplSDLRenderer3 resets its own state
    // each frame.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    BuildTwinkles();
    // The streak is a fixed design size (170x2) scaled via its dest rect, so it
    // is built once here rather than per resize like the viewport textures.
    g_state.streak = BuildStreak(renderer);
    g_state.glow = BuildGlow(renderer);
    g_state.initialized = true;
}

void shutdown(SDL_Renderer* renderer) {
    (void)renderer;
    for (int i = 0; i < kLayerCount; ++i) {
        DestroyTexture(g_state.layers[i]);
    }
    DestroyTexture(g_state.bg);
    DestroyTexture(g_state.flash);
    DestroyTexture(g_state.vignette);
    DestroyTexture(g_state.streak);
    DestroyTexture(g_state.glow);
    g_state.tex_w = 0;
    g_state.tex_h = 0;
    g_state.last_rasterize_ms = 0;
    g_state.twinkles.clear();
    g_state.cx = 0.0f;
    g_state.cy = 0.0f;
    g_state.ramp = 0.0f;
    g_state.tx_last = 0.0f;
    g_state.ty_last = 0.0f;
    g_state.last_time_s = -1.0f;
    g_state.warp_t = 0.0f;
    g_state.warp_target = 0.0f;
    g_state.flash_start_s = -1.0;
    g_state.flash_pending = false;
    g_state.warp_stars.clear();
    g_state.warp_stars.shrink_to_fit();
    // Backdrop (PLAN.md D10(e)): the LRU is the sole owner of every decoded
    // texture; backdrop_cur/backdrop_prev only ever alias an LRU entry's
    // pointer (see ResolveBackdropTexture), so destroying them a second time
    // here would double-free -- resetting the slots (no DestroyTexture call
    // on them) is correct.
    for (BackdropEntry& e : g_state.backdrop_lru) {
        DestroyTexture(e.tex);
    }
    g_state.backdrop_lru.clear();
    g_state.backdrop_lru.shrink_to_fit();
    g_state.backdrop_failed.clear();
    g_state.backdrop_failed.shrink_to_fit();
    g_state.backdrop_cur = BackdropEntry{};
    g_state.backdrop_prev = BackdropEntry{};
    g_state.backdrop_fade_start_s = -1.0;
    g_state.backdrop_eased_alpha = 0.0f;
    g_state.backdrop_req_path.clear();
    g_state.backdrop_req_unresolvable = false;
    g_state.initialized = false;
}

void draw(SDL_Renderer* renderer, int out_w, int out_h, const SceneInput& in) {
    if (renderer == nullptr || out_w <= 0 || out_h <= 0) {
        return;
    }
    if (!g_state.initialized) {
        init(renderer);
        if (!g_state.initialized) {
            return;
        }
    }

    // ImGui_ImplSDLRenderer3 sets its own blend state each frame, so re-assert
    // ours at the top of every draw. Leaving it set on return is harmless.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // 1. Smooth the cursor toward its target with an accelerating onset that
    // mirrors the warp re-entry in reverse: while the target moves - or the
    // layers are still catching up - the chase rate ramps up exponentially
    // from a gentle floor; once the motion is done the rate decays back, so
    // the tail glides progressively slower, the same satisfying pace as the
    // warp return. Deliberate feel change from the prototype's raw
    // 0.055/frame constant-rate exponential. dt is clamped so a hitch (or
    // the first frame) cannot teleport the scene.
    float dt = g_state.last_time_s >= 0.0f ? in.time_s - g_state.last_time_s
                                           : 1.0f / 60.0f;
    g_state.last_time_s = in.time_s;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float k_ramp = 1.0f - std::pow(1.0f - kRamp60, dt * 60.0f);
    const float k_chase = 1.0f - std::pow(1.0f - kChase60, dt * 60.0f);
    const float tx = 2.0f * in.mouse_x / static_cast<float>(out_w) - 1.0f;
    const float ty = 2.0f * in.mouse_y / static_cast<float>(out_h) - 1.0f;
    // ImGui reports the mouse position as (-FLT_MAX,-FLT_MAX) while the cursor
    // is outside the window. Chase the LAST VALID target instead of the
    // poisoned value: the layers keep gliding and decelerating, and NaN can
    // never enter the state.
    if (std::isfinite(tx) && std::isfinite(ty)) {
        // The ramp climbs while the gesture is live (the target moved beyond
        // jitter) or the layers still have ground to cover; it decays once
        // the motion settles, so every new gesture starts from a crawl.
        const float move =
            std::fabs(tx - g_state.tx_last) + std::fabs(ty - g_state.ty_last);
        const float err = std::fabs(tx - g_state.cx) + std::fabs(ty - g_state.cy);
        const float ramp_target = (move > kMoveEps || err > kSettleEps) ? 1.0f : 0.0f;
        g_state.ramp += (ramp_target - g_state.ramp) * k_ramp;
        g_state.tx_last = tx;
        g_state.ty_last = ty;
    } else {
        // Mouse left the window: ease the ramp down; the chase below finishes
        // the approach at the decaying rate (the warp-return glide).
        g_state.ramp += (0.0f - g_state.ramp) * k_ramp;
    }
    {
        const float rate = k_chase * (kFloorFrac + (1.0f - kFloorFrac) * g_state.ramp);
        g_state.cx += (g_state.tx_last - g_state.cx) * rate;
        g_state.cy += (g_state.ty_last - g_state.cy) * rate;
    }

    // Warp transition (U5): same time-corrected easing as the parallax above,
    // with the prototype's 0.05/frame factor (docs/UI_MIGRATION.md §5). The
    // scene warps out (target 1) while a session connects and reassembles
    // (target 0) when it ends.
    const float warp_k = 1.0f - std::pow(1.0f - 0.05f, dt * 60.0f);
    g_state.warp_t += (g_state.warp_target - g_state.warp_t) * warp_k;

    // A pending warp flash starts on the frame it was requested: draw() owns
    // the clock (trigger_warp_flash only sets the flag).
    if (g_state.flash_pending) {
        g_state.flash_start_s = in.time_s;
        g_state.flash_pending = false;
    }
    // The flash's 2.2s envelope (peak ~72%); it ends itself when the window
    // completes, so no explicit stop call is needed.
    float flash_alpha = 0.0f;
    if (g_state.flash_start_s >= 0.0) {
        const double elapsed = in.time_s - g_state.flash_start_s;
        if (elapsed >= 2.2) {
            g_state.flash_start_s = -1.0;
        } else {
            flash_alpha = WarpFlashAlpha(elapsed);
        }
    }

    // 2. Rasterize on resize (art box sized to the viewport). Re-rasterizing
    // is expensive (up to ~154M pixels at 4K), so throttle it to once per
    // 300 ms during a resize drag: the pending size change is picked up on
    // the first frame after the window, and meanwhile the old texture is
    // simply stretched.
    const float art_w = std::max(1.04f * out_w, 1.4f * out_h);
    const float art_h = art_w * 1200.0f / 1620.8481f;
    const int tex_w = static_cast<int>(std::ceil(art_w));
    const int tex_h = static_cast<int>(std::ceil(art_h));
    const uint64_t now_ms = SDL_GetTicks();
    if ((tex_w != g_state.tex_w || tex_h != g_state.tex_h) &&
        (g_state.last_rasterize_ms == 0 ||
         now_ms - g_state.last_rasterize_ms >= 300)) {
        RasterizeAll(renderer, tex_w, tex_h);
    }

    // 3. Background radial gradient, full viewport.
    if (g_state.bg != nullptr) {
        const SDL_FRect dst{0.0f, 0.0f, static_cast<float>(out_w),
                            static_cast<float>(out_h)};
        SDL_RenderTexture(renderer, g_state.bg, nullptr, &dst);
    }

    // 4. Backdrop: the focused host's cached wallpaper, cover-scaled and
    // scrimmed, drawn between the background gradient and the twinkle/star
    // layers (PLAN.md D10(e), W3). Viewport-independent -- RasterizeAll above
    // never touches these textures, so a resize does not re-decode.
    //
    // presence/backdrop_x are declared here (rather than inside the block
    // below) because the panel wallpaper draw in the layer loop further down
    // reuses this frame's exact values: the panel and the fullscreen backdrop
    // share one focus model, not two independently computed ones.
    float presence = 0.0f;
    float backdrop_x = 1.0f;
    {
        // Resolve a NON-EMPTY, changed path first (before the easing below,
        // which needs to know whether it succeeded). backdrop_req_path tracks
        // the last path looked up here, separately from backdrop_cur.path, so
        // an unresolvable path is only ever looked up once -- the LRU scan
        // and the backdrop_failed check are skipped on every later frame the
        // same bad path is requested, keeping the steady-state cost a single
        // string compare either way.
        if (!in.backdrop_path.empty() && in.backdrop_path != g_state.backdrop_req_path) {
            g_state.backdrop_req_path = in.backdrop_path;
            BackdropEntry* entry =
                ResolveBackdropTexture(renderer, in.backdrop_path, now_ms);
            g_state.backdrop_req_unresolvable = (entry == nullptr);
            if (entry != nullptr) {
                if (entry->tex == g_state.backdrop_cur.tex) {
                    // The resolved texture is already the one on screen (e.g.
                    // re-focusing a host after an intervening unresolvable
                    // one): just refresh backdrop_cur's bookkeeping (path may
                    // differ, LRU last_use_ms was already bumped by
                    // ResolveBackdropTexture) and leave any in-progress
                    // crossfade alone -- re-arming one here would blend the
                    // same image over itself and visibly over-brighten it.
                    g_state.backdrop_cur = *entry;
                } else {
                    const bool had_current = g_state.backdrop_cur.tex != nullptr;
                    g_state.backdrop_prev = g_state.backdrop_cur;
                    g_state.backdrop_cur = *entry;
                    // Only arm the crossfade when there was something to fade
                    // FROM. On the very first backdrop, backdrop_eased_alpha
                    // above already provides the fade-in; arming a
                    // from-nothing crossfade would ramp the image at
                    // presence*x while the scrim (below) ramps at presence
                    // alone, so the scrim would visibly darken the screen
                    // before the wallpaper caught up.
                    if (had_current) {
                        g_state.backdrop_fade_start_s = in.time_s;
                    }
                }
            }
        }

        // An empty path does NOT drop the current texture: it keeps drawing
        // while the eased alpha below falls to 0, which is what makes
        // hover-leave fade out instead of snapping. A NON-EMPTY but
        // unresolvable path (host has no cache yet, bad file, etc.) fades the
        // same way -- backdrop_cur is left showing the last host that DID
        // resolve, and easing its target to 0 here makes it fade out rather
        // than keep masquerading as the newly-focused host's wallpaper.
        const bool fade_to_zero =
            in.backdrop_path.empty() ||
            (in.backdrop_path == g_state.backdrop_req_path &&
             g_state.backdrop_req_unresolvable);
        const float backdrop_k =
            1.0f - std::pow(1.0f - kBackdropEaseRate60, dt * 60.0f);
        const float backdrop_target =
            fade_to_zero ? 0.0f : std::clamp(in.backdrop_alpha, 0.0f, 1.0f);
        g_state.backdrop_eased_alpha +=
            (backdrop_target - g_state.backdrop_eased_alpha) * backdrop_k;

        presence = g_state.backdrop_eased_alpha * (1.0f - g_state.warp_t);
        const float image_alpha = presence * kBackdropImageAlpha;

        // The fullscreen backdrop covers the full viewport; DrawBackdropCovered
        // crops whichever source axis overflows that dest's aspect so it shows
        // no letterboxing.
        const SDL_FRect full_dst{0.0f, 0.0f, static_cast<float>(out_w),
                                 static_cast<float>(out_h)};

        if (image_alpha > kBackdropMinAlpha) {
            // backdrop_x is the linear 0..1 crossfade progress since
            // backdrop_fade_start_s; 1.0 (fully faded in) when there is no
            // active crossfade. Computed before any drawing so the x >= 1
            // release below always runs on the frame the crossfade actually
            // completes, even if presence (and so image_alpha) had dropped to
            // ~0 and back while a fade was still armed (e.g. the warp, or the
            // weight dipping within 250ms of a path change) -- otherwise
            // backdrop_fade_start_s could stay stuck pointing at a long-past
            // anchor and the stale prev texture would flash back in for one
            // frame once presence returns.
            backdrop_x = 1.0f;
            if (g_state.backdrop_fade_start_s >= 0.0) {
                backdrop_x = std::clamp(
                    static_cast<float>((in.time_s - g_state.backdrop_fade_start_s) /
                                       kBackdropCrossfadeS),
                    0.0f, 1.0f);
                if (backdrop_x < 1.0f) {
                    // True cross-dissolve: prev ramps DOWN from image_alpha as
                    // cur ramps UP from 0, so total coverage stays close to
                    // image_alpha throughout instead of drawing prev at a
                    // constant image_alpha (which would grow coverage through
                    // the fade, then drop it in one frame at release -- e.g.
                    // at image_alpha = 0.40 that was background visibility
                    // 0.36 mid-fade snapping to 0.60 the instant prev is
                    // dropped).
                    DrawBackdropCovered(renderer, g_state.backdrop_prev, full_dst,
                                        image_alpha * (1.0f - backdrop_x));
                } else {
                    // Crossfade done: release the previous slot back to the
                    // LRU (the entry itself stays cached there; only this
                    // pinned reference is cleared) and stop drawing it.
                    g_state.backdrop_prev = BackdropEntry{};
                    g_state.backdrop_fade_start_s = -1.0;
                }
            }
            DrawBackdropCovered(renderer, g_state.backdrop_cur, full_dst,
                                image_alpha * backdrop_x);
        }

        // Dark scrim over the backdrop, scaling with presence so it fades out
        // together with the image. Also gated on backdrop_cur actually having
        // a texture: presence alone can be nonzero (e.g. main.cpp's hover
        // weight) even when there is nothing cached yet to show behind it, and
        // the scrim must never darken a backdrop-less scene.
        // SDL_BLENDMODE_BLEND is already set at the top of draw(); the
        // twinkle pass below sets its own draw color, so nothing to restore.
        const float scrim_alpha = presence * kBackdropScrimAlpha;
        if (scrim_alpha > kBackdropMinAlpha && g_state.backdrop_cur.tex != nullptr) {
            const SDL_FRect dst{0.0f, 0.0f, static_cast<float>(out_w),
                                static_cast<float>(out_h)};
            SDL_SetRenderDrawColor(
                renderer, 0, 0, 0,
                static_cast<Uint8>(std::clamp(scrim_alpha, 0.0f, 1.0f) * 255.0f));
            SDL_RenderFillRect(renderer, &dst);
        }
    }

    // 5. Twinkles (exact prototype replication).
    const float scale = cosmic::ui::scale();
    const SDL_Color text = SdlColor(kText);
    for (const Twinkle& t : g_state.twinkles) {
        const float phase = std::fmod((in.time_s + t.delay) / t.period, 1.0f);
        // Ease-in-out sine matching the CSS twk keyframes (0%/100% .12, 50% .9).
        const float o = 0.12f + 0.78f * (0.5f - 0.5f * std::cos(2.0f * 3.14159265f * phase));
        const float size = t.size * scale;
        // SDL_RenderFillRect takes an SDL_FRect, but the twinkle must land on the
        // whole-pixel grid: truncate each component through int first, then widen
        // back to float.
        const SDL_FRect rect{
            static_cast<float>(static_cast<int>(t.x * out_w)),
            static_cast<float>(static_cast<int>(t.y * out_h)),
            static_cast<float>(static_cast<int>(size)),
            static_cast<float>(static_cast<int>(size)),
        };
        SDL_SetRenderDrawColor(renderer, text.r, text.g, text.b,
                               static_cast<Uint8>(o * 255.0f));
        SDL_RenderFillRect(renderer, &rect);
    }

    // 6. Layer pass, back -> front.
    // Guard motion like the easing above: U5 will animate it, and a non-finite
    // value would re-poison every dest rect the same way the mouse NaN did.
    const float motion = GuardedMotion(in.motion);
    const float cx = g_state.cx * motion;
    const float cy = g_state.cy * motion;
    for (int i = 0; i < kLayerCount; ++i) {
        // The shooting star sits between the sky layers and the desk group
        // (UI_MIGRATION A3), alongside the warp flash. It animates over a 14s
        // cycle that starts 3s in and is visible only the first ~13% of it.
        if (i == kDeskGroupIndex && g_state.streak != nullptr) {
            const float cycle = std::fmod(in.time_s - 3.0f, 14.0f);
            const float p = cycle / 14.0f;
            // Negative p covers the first 3s before the cycle starts: nothing
            // may draw then (the dest rect would otherwise fly far off-screen).
            if (p >= 0.0f && p < 0.13f) {
                // k = 0..1 across the visible window; opacity ramps 0->0.9 over
                // the first 3/13 of it, then 0.9->0 over the remaining 10/13.
                const float k = p / 0.13f;
                float opacity;
                if (k < 3.0f / 13.0f) {
                    opacity = 0.9f * (k / (3.0f / 13.0f));
                } else {
                    opacity = 0.9f * (1.0f - (k - 3.0f / 13.0f) / (10.0f / 13.0f));
                }
                // Translate from the start (74%/9%, the streak's TOP-LEFT like
                // the prototype's positioned div) by k*(-56vw, +26vw).
                const float sw = 170.0f * scale;
                const float sh = 2.0f * scale;
                const float pos_x = 0.74f * out_w + sw / 2.0f + k * (-0.56f * out_w);
                const float pos_y = 0.09f * out_h + sh / 2.0f + k * (0.26f * out_h);
                SDL_SetTextureAlphaMod(
                    g_state.streak,
                    static_cast<Uint8>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
                const SDL_FRect dest{pos_x - sw / 2.0f, pos_y - sh / 2.0f, sw, sh};
                // -26deg counterclockwise (SDL angles are clockwise-positive).
                SDL_RenderTextureRotated(renderer, g_state.streak, nullptr, &dest,
                                         -26.0f, nullptr, SDL_FLIP_NONE);
            }
        }

        // The hyperspace streak field sits between the shooting star and the
        // warp flash (handoff v2 DOM order: shooting star -> canvas -> flash
        // -> desk group), so the desk and all UI occlude it.
        if (i == kDeskGroupIndex) {
            UpdateAndDrawWarpStreaks(renderer, out_w, out_h, dt);
        }

        // The warp flash sits between the sky layers and the desk group
        // (UI_MIGRATION A3), matching the prototype's DOM. The texture is
        // opaque white where lit, so alpha-mod fades the whole thing; the
        // alpha comes from the internal 2.2s envelope (peak at 72%).
        if (i == kDeskGroupIndex && g_state.flash != nullptr &&
            flash_alpha > 0.001f) {
            SDL_SetTextureAlphaMod(
                g_state.flash,
                static_cast<Uint8>(std::clamp(flash_alpha, 0.0f, 1.0f) * 255.0f));
            const SDL_FRect dst{0.0f, 0.0f, static_cast<float>(out_w),
                                static_cast<float>(out_h)};
            SDL_RenderTexture(renderer, g_state.flash, nullptr, &dst);
        }

        // The screen glow sits between the desk and the monitor (prototype DOM
        // order: boot overlay -> glow -> monitor). The texture is a baked
        // 0.28-alpha radial gradient; the per-frame flicker alpha-mods it.
        if (i == kDeskGroupIndex + 1 && g_state.glow != nullptr) {
            const float t7 = std::fmod(in.time_s, 7.0f);
            SDL_SetTextureAlphaMod(
                g_state.glow,
                static_cast<Uint8>(std::clamp(FlickerOpacity(t7), 0.0f, 1.0f) * 255.0f));
            // The glow div is 36%/26%/34%/30% of the art box, mapped through
            // the desk transform (same math as screen_rect).
            const SDL_FRect desk = DeskDest(out_w, out_h);
            const SDL_FRect glow_dest{
                desk.x + 0.36f * desk.w,
                desk.y + 0.26f * desk.h,
                0.34f * desk.w,
                0.30f * desk.h,
            };
            SDL_RenderTexture(renderer, g_state.glow, nullptr, &glow_dest);
        }

        // Panel wallpaper (PLAN.md D10(e)): the same focused-host backdrop
        // decoded in section 4 above also fills the in-scene monitor's screen
        // panel, framed by monitor-bezel.svg, so it reads as that PC's actual
        // desktop. Sits between monitor.svg's dark panel and the bezel texture
        // (draw order: monitor.svg -> panel wallpaper -> monitor-bezel.svg ->
        // screen-logo.svg -> obj-* -> reflex), reusing this frame's presence
        // and backdrop_x from section 4 -- the panel and the fullscreen
        // backdrop are driven by the SAME focus source, never a second one, so
        // when nothing is focused (including after a stream ends, once the
        // Bridge clears its stale selection) the panel fades out together
        // with the fullscreen backdrop. Unlike the fullscreen backdrop it is
        // not capped at kBackdropImageAlpha and has no scrim (kBackdropPanelAlpha):
        // it must read as a real desktop, not a dim silhouette behind the UI.
        // Also gated by (1 - screen_logo_alpha): screen-logo.svg is a disc, not
        // a full-panel fill, so without this the wallpaper would show through
        // in the panel corners around it during boot; fading in as the logo
        // fades out also reads as the PC turning on (PLAN.md D10(e)).
        if (i == kMonitorBezelIndex) {
            const float panel_alpha = presence * kBackdropPanelAlpha *
                                      std::clamp(1.0f - in.screen_logo_alpha, 0.0f, 1.0f);
            if (panel_alpha > kBackdropMinAlpha) {
                const SDL_FRect panel_dst = ScreenPanelDest(out_w, out_h);
                // Unlike section 4's true cross-dissolve (safe over the
                // scrimmed, capped fullscreen backdrop), the panel sits over an
                // OPAQUE dark PanelOff rect: ramping prev down while cur ramps
                // up would dip total coverage mid-fade and let that dark panel
                // show through. Draw prev at the full panel_alpha and let cur
                // ramp up over it instead, so coverage never drops below
                // panel_alpha during the crossfade.
                DrawBackdropCovered(renderer, g_state.backdrop_prev, panel_dst,
                                    panel_alpha);
                DrawBackdropCovered(renderer, g_state.backdrop_cur, panel_dst,
                                    panel_alpha * backdrop_x);
            }
        }

        SDL_Texture* tex = g_state.layers[i];
        if (tex == nullptr) {
            continue;
        }
        const Layer& layer = kLayers[i];

        // reflex.svg alpha is driven per-frame by the glint value.
        if (i == kReflexIndex) {
            const float glint =
                std::clamp(0.5f + 1.4f * g_state.cx - 0.6f * g_state.cy, 0.0f, 1.0f);
            SDL_SetTextureAlphaMod(tex, static_cast<Uint8>(glint * glint * 255.0f));
        }

        // screen-logo.svg alpha is driven per-frame by the caller's boot fade
        // (U2); the static table alpha stays 1.0.
        if (i == kScreenLogoIndex) {
            SDL_SetTextureAlphaMod(
                tex, static_cast<Uint8>(std::clamp(in.screen_logo_alpha, 0.0f, 1.0f) * 255.0f));
        }

        // Per-layer drift (prototype CSS keyframes drift/drift2): a slow
        // translate to (drift_dx, drift_dy) and back, eased with the same
        // cosine pattern the twinkles use (0 at 0%/100%, peak at 50%); CSS
        // ease-in-out is close enough. Offsets are design px, so scale them
        // to device px like the rest of the geometry.
        float anim_ox = 0.0f;
        float anim_oy = 0.0f;
        if (layer.drift_period_s > 0.0f) {
            // drift_delay_s mirrors CSS animation-delay: a NEGATIVE value
            // (planets: -8s) starts the animation that many seconds INTO its
            // cycle, so the phase is (t - delay) / period.
            const float phase = std::fmod((in.time_s - layer.drift_delay_s) /
                                              layer.drift_period_s,
                                          1.0f);
            const float ease =
                0.5f - 0.5f * std::cos(2.0f * 3.14159265f * phase);
            anim_ox = layer.drift_dx * ease * scale;
            anim_oy = layer.drift_dy * ease * scale;
        }

        // Warp (U5): the three sky layers (warp_end_scale != 1) scale toward
        // their end scale (9/12/16) and fade out as warp_t -> 1 while the desk
        // group stays put. The scale is about the dest center: the rect below
        // re-centers the scaled w/h. The alpha mod is re-applied per frame
        // while warping (the rasterize-time value stands at warp 0).
        float base_alpha = layer.alpha;
        float w = art_w * layer.es;
        float h = art_h * layer.es;
        if (layer.warp_end_scale != 1.0f) {
            const float factor = 1.0f + g_state.warp_t * (layer.warp_end_scale - 1.0f);
            w *= factor;
            h *= factor;
            base_alpha *= (1.0f - g_state.warp_t);
            if (g_state.warp_t > 0.0f) {
                SDL_SetTextureAlphaMod(
                    tex, static_cast<Uint8>(std::clamp(base_alpha, 0.0f, 1.0f) * 255.0f));
            }
        }
        const float ox = layer.ex - cx * layer.depth + anim_ox;
        const float oy = layer.ey - cy * layer.depth + anim_oy;
        const SDL_FRect dest0{
            static_cast<float>(out_w) / 2.0f - w / 2.0f + ox,
            static_cast<float>(out_h) / 2.0f - h / 2.0f + oy,
            w,
            h,
        };

        if (layer.sway >= 0) {
            // Nebula band sway: translate/rotate/scale the band about its own
            // fill-bbox center (fx/fy as canvas % of the art box), on top of
            // the shared drift/parallax dest0. The warp sky-scaling above does
            // not apply to the bands (warp_end_scale stays 1), so dest0 is the
            // normal path.
            const NebulaSway& s = kNebulaSway[layer.sway];
            float tx, ty, rot, sx, sy;
            SampleSway(s, in.time_s, tx, ty, rot, sx, sy);

            // Nebula warp fade (handoff v2): dim the band toward 12.5% of its
            // base alpha (.96 -> .12, the prototype's 1.6s CSS ease). Driven
            // from the warp progress rather than a wall clock: the Bridge
            // hands off to the viewer at warp 0.95, so a time-based 1.6s fade
            // would barely start before the scene disappears. Full dim by
            // warp 0.85 keeps the whole transition on screen, and the warp-back
            // brightens it symmetrically. Re-applied every frame (the
            // rasterize-time alpha mod stands only at warp 0).
            const float f = std::min(1.0f, g_state.warp_t / 0.85f);
            const float eased = f * f * (3.0f - 2.0f * f);
            const float band_alpha = base_alpha * (1.0f - 0.875f * eased);
            SDL_SetTextureAlphaMod(
                tex, static_cast<Uint8>(std::clamp(band_alpha, 0.0f, 1.0f) * 255.0f));

            // Band center on screen. s.fx/s.fy are stored as canvas PERCENT
            // (52.4 = 52.4%), so divide by 100 to get a 0..1 fraction of the
            // art-box dest rect.
            const float fx = s.fx * 0.01f;
            const float fy = s.fy * 0.01f;
            const float Px = dest0.x + fx * dest0.w;
            const float Py = dest0.y + fy * dest0.h;

            // Translate by the sway (design px x scale; rot/scale unscaled).
            const float tox = tx * scale;
            const float toy = ty * scale;
            const SDL_FRect dest1{dest0.x + tox, dest0.y + toy, dest0.w, dest0.h};
            const float P1x = Px + tox;
            const float P1y = Py + toy;

            // Scale about P1 (the translated band center).
            const SDL_FRect dest2{
                P1x - sx * (P1x - dest1.x),
                P1y - sy * (P1y - dest1.y),
                dest1.w * sx,
                dest1.h * sy,
            };

            // Rotation origin = the band center relative to dest2's top-left:
            // P1 sits at (fx*dest0.w, fy*dest0.h) inside dest1, and scaling
            // about P1 moves dest2's top-left by (1-sx)*(P1-dest1), so the
            // center lands at (sx*fx*dest0.w, sy*fy*dest0.h).
            const SDL_FPoint center{sx * fx * dest0.w, sy * fy * dest0.h};
            SDL_RenderTextureRotated(renderer, tex, nullptr, &dest2, rot, &center,
                                     SDL_FLIP_NONE);
        } else {
            SDL_RenderTexture(renderer, tex, nullptr, &dest0);
        }
    }

    // 7. Orbit ring (dashed ellipse), drawn after the desk group per A3 order
    // (...desk -> orbit ring -> beams -> vignette). Replicates the prototype's
    // ring div: dashed ellipse min(82vw,1240px) x min(74vh,720px) centered at
    // 50%/47%, rotated -8deg with a 22s ease-in-out sway to -5deg at the 50%
    // keyframe. Dash 3px on / 7px gap, 1px line, all x scale().
    {
        const float ring_cx = static_cast<float>(out_w) / 2.0f;
        const float ring_cy = 0.47f * out_h;
        const float ring_rx =
            std::min(0.82f * out_w, 1240.0f * scale) / 2.0f;
        const float ring_ry =
            std::min(0.74f * out_h, 720.0f * scale) / 2.0f;
        // ringspin: -8deg + 3deg*(0.5 - 0.5*cos(2pi*t/22)) sways -8 -> -5 -> -8.
        const float sway =
            0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (in.time_s / 22.0f));
        const float ring_rot = -8.0f + 3.0f * sway;
        // rgba(184,151,211,.18) = kPurple at alpha 46 (0.18 * 255).
        const uint32_t purple = kPurple;
        DrawDashedEllipse(renderer, ring_cx, ring_cy, ring_rx, ring_ry, ring_rot,
                          static_cast<Uint8>((purple >> 24) & 0xFF),
                          static_cast<Uint8>((purple >> 16) & 0xFF),
                          static_cast<Uint8>((purple >> 8) & 0xFF),
                          46, 3.0f * scale, 7.0f * scale, 0.0f, 1.0f * scale);
    }

    // 8. Vignette, drawn LAST in the SDL scene pass (A3). Full-viewport edge
    // fade; the texture is rebuilt on resize like the bg and drawn stretched.
    if (g_state.vignette != nullptr) {
        const SDL_FRect dst{0.0f, 0.0f, static_cast<float>(out_w),
                            static_cast<float>(out_h)};
        SDL_RenderTexture(renderer, g_state.vignette, nullptr, &dst);
    }
}

void set_warp_target(float target) {
    g_state.warp_target = std::clamp(target, 0.0f, 1.0f);
}

float warp_progress() {
    return g_state.warp_t;
}

void trigger_warp_flash() {
    // draw() owns the clock: the flag is converted to flash_start_s on the
    // next frame, so the envelope anchors to the frame the flash was asked for.
    g_state.flash_pending = true;
}

SDL_FRect screen_rect(int out_w, int out_h) {
    if (out_w <= 0 || out_h <= 0) {
        return SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
    }
    // Handoff README: the monitor screen is 38.86%/29.57%/28.56%/23.66% of the
    // art box (left/top/w/h), mapped through the desk transform; ScreenPanelDest
    // holds the fractions once (also used by the panel wallpaper draw).
    return ScreenPanelDest(out_w, out_h);
}

CursorSmooth smoothed_cursor() {
    return CursorSmooth{g_state.cx, g_state.cy};
}

}  // namespace cosmic::ui::scene
