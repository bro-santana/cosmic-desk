// Cosmic Desk — Bridge scene renderer (docs/UI_MIGRATION.md U0/U1).
//
// Draws the parallax "exploded view" space launcher behind the ImGui UI. All
// layer art is rasterized at runtime from the vendored SVGs in
// assets/ui/layers/ via lunasvg, so it stays crisp at any DPI/resolution and
// needs no pre-bake step (decision A2).
//
// Design references (prototype `Cosmic Desk.dc.html`):
//   - Art box: the layers are authored on a 1620.8481 x 1200 canvas and drawn
//     at a width of max(104vw, 140vh), centered. This guarantees parallax
//     translation never reveals the art edges on any viewport.
//   - Parallax smoothing: the cursor is eased toward its target each frame
//     with the prototype's 0.055 per-frame factor converted to the time-based
//     form ease = 1 - pow(1 - 0.055, dt*60) (dt clamped to <= 0.1 s), so the
//     feel is identical at any vsync rate, not just the prototype's ~60 fps.
//   - Layer depth table (back -> front), from the handoff README parallax table
//     and the prototype's data-ex/ey/es attributes:
//
//         name            depth   ex   ey    es    alpha
//         nebula.svg       12      0   -14   1.04  0.96
//         stars-far.svg    10      0   -10   1.03  1.0
//         stars-mid.svg    20      0   -20   1.05  1.0
//         planets.svg      13      0   -32   1.02  1.0
//         desk.svg         26      0   +10   1.03  1.0
//         monitor.svg      26      0   +10   1.03  1.0
//         screen-logo.svg  26      0   +10   1.03  1.0   (U2: boot fade)
//         obj-g11.svg      26      0   +10   1.03  1.0
//         obj-g18.svg      26      0   +10   1.03  1.0
//         obj-g22.svg      26      0   +10   1.03  1.0
//         obj-g24.svg      26      0   +10   1.03  1.0
//         obj-g33.svg      26      0   +10   1.03  1.0
//         reflex.svg       26      0   +10   1.03  glint (per-frame)
//
//   The prototype's rotateY/rotateX 3D tilt on the desk group is deliberately
//   omitted (decision A5a): ImGui cannot rotate text, so only the parallax
//   translation is kept. U1 owns the per-layer drift animations, the warp
//   flash, the orbit ring, the vignette, the shooting star and the dashed
//   geometry helpers; U2 owns the screen glow, the screen-logo boot fade and
//   the screen_rect() helper; still to come are the nebula per-band sway
//   (later polish), the tether beams (U3) and the warp animation (U5); this
//   module is structured to make those easy to append without touching the
//   layer pipeline.

#pragma once
#include <SDL.h>

namespace cosmic::ui::scene {

// Per-frame inputs; main.cpp feeds these from SDL state each frame.
struct SceneInput {
    float mouse_x = 0.0f;  // mouse in renderer-output pixel coords
    float mouse_y = 0.0f;
    float time_s = 0.0f;   // SDL_GetTicks64() / 1000.0f
    float motion = 1.0f;   // parallax strength (design "motion" prop)
    // Screen-logo overlay opacity (0..1). U2 drives 1 during boot, fading to 0
    // over the 1.4s after boot completes; U0 had it always-on.
    float screen_logo_alpha = 1.0f;
};

// Loads nothing yet; prepares state. Call once after the renderer exists.
void init(SDL_Renderer* renderer);

// Destroys all textures. Idempotent.
void shutdown(SDL_Renderer* renderer);

// Draws the whole scene for the renderer output size (out_w,out_h), which is
// in device pixels (per-monitor-v2 DPI awareness). Re-rasterizes all layer
// textures automatically when the size changed since last frame. Call once
// per frame between SDL_RenderClear (main.cpp clears to kBg) and
// ImGui_ImplSDLRenderer2_RenderDrawData — i.e. after the ImGui frame has
// been built, so the scene lands underneath the UI. The SDLRenderer2 backend
// re-asserts its own renderer state in RenderDrawData. Does not touch ImGui
// state.
void draw(SDL_Renderer* renderer, int out_w, int out_h, const SceneInput& in);

// Warp transition (U5): eases toward the target (1 = streaming, 0 = bridge)
// with the prototype's 0.05/frame factor in time-corrected form. The sky
// layers scale/fade and the machine cards exit with it.
void set_warp_target(float target);
float warp_progress();
// Fires the 2.2 s warp flash (peak at 72%); U5 calls it on Connect.
void trigger_warp_flash();

// The smoothed parallax cursor in [-1,1]^2 (design "c"), as of the last draw.
// The Bridge UI uses it for the machine-card orbit and its parallax factor.
struct CursorSmooth { float x, y; };
CursorSmooth smoothed_cursor();

// The monitor screen rectangle (handoff README: left 38.86%, top 29.57%,
// width 28.56%, height 23.66% of the 1620.8481x1200 art box), transformed by
// the desk group's current parallax/scale, in renderer-output px for the given
// viewport. Uses the smoothed cursor state; the ImGui overlay calls it before
// scene draw of the same frame, so the rect lags one frame (imperceptible at
// the 0.055 easing rate).
SDL_FRect screen_rect(int out_w, int out_h);

}  // namespace cosmic::ui::scene
