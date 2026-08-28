# Cosmic Desk — Bridge UI Migration Plan

**Goal:** Replace the classic ImGui host-list window with the parallax "exploded
view" space launcher described and prototyped in
`design_handoff_cosmic_desk_launcher`, recreating the prototype 1:1 on the
existing **SDL2 + Dear ImGui + SDL_Renderer** stack. No shaders, no OpenGL.

The handoff bundle lives at
`C:\Users\bro\Downloads\Parallax cosmic UI design\design_handoff_cosmic_desk_launcher`
(`Cosmic Desk.dc.html` = working prototype with all math/timings; `layers/*.svg` =
pre-split parallax art; `screenshots/` = 5 reference frames; `README.md` = design
spec with depths, timings, palette).

**Execution target:** an inexperienced coder. Every milestone has file-level
tasks and "run X, see Y" acceptance. Total estimate: **4–6 weeks**.

## 1. Architecture decisions (read first)

| # | Decision | Rationale |
|---|---|---|
| A1 | **Stay on SDL2.** `SDL_RenderCopyEx` (rotated/scaled sprites) + `SDL_RenderGeometry` (dashed beams, ring, twinkles) cover everything the design needs. | SDL3 gains nothing here; migration churns viewer/audio/input/tray for no visual benefit. Revisit post-v1. |
| A2 | **Layer sprites rasterized at runtime from SVG via vendored `lunasvg`** (MIT, `third-party/lunasvg`). On window resize, re-render each layer into an `SDL_Texture` at display size. | Repo stays small (SVGs are 9–60 KB), always crisp at any DPI/resolution, no pre-bake step to forget. Verified: no rasterizable layer contains `<text>`. |
| A3 | **Rendering order per frame (Bridge mode):** SDL draws the scene (bg gradient → twinkles → nebula → stars-far → stars-mid → planets → shooting star → warp flash → desk group → orbit ring → tether beams → vignette), then the existing `ImGui_ImplSDLRenderer2_RenderDrawData` draws all UI on top. | Mirrors the prototype's DOM stacking (the HTML draws ring/beams/vignette after the art box; stars-far/stars-mid sit above planets); reuses today's main-loop structure (main.cpp:766–841) unchanged in shape. |
| A4 | **All Bridge UI is one fullscreen borderless ImGui window.** Cards are absolutely-positioned child windows inside it (`SetCursorPos` + `BeginChild`, ID = `"###"+address`). Panels (Settings, Pair) are children too. | Avoids ImGui multi-window z-order pitfalls; keeps the established *action-struct-after-frame* pattern: new `BridgeAction` mirrors today's `HostListAction` so main.cpp's action-application block survives almost unchanged (main.cpp:848–879). |
| A5 | **Documented deviations from the prototype** (all invisible-in-practice): (a) no `rotateY/rotateX` 3D tilt on cards/desk — ImGui cannot rotate text; parallax translation is kept; (b) depth-of-field blur approximated as opacity fade of far cards (no per-draw blur in SDL_Renderer); (c) card drop-shadow halo drawn from a runtime-generated radial texture (polish, U6); (d) letter-spacing via a per-glyph `TextSpaced()` helper; (e) dock backdrop blur omitted (backdrop is near-opaque in practice). | The screenshots show cards unrotated and flat; these are the only losses. |
| A6 | **Old UI deleted at the end** (U7). | Git history is the fallback. |

**Fonts:** IBM Plex Mono 400/500/700 + IBM Plex Sans 400/500/600 TTFs committed to
`assets/fonts/` (OFL — license file included). Michroma is **not** used: the
prototype loads it but never applies it (confirmed in the HTML and screenshots).
The font atlas is built in `ui/scale.cpp` at `design_px × ui::scale()`; the
ASCII-only input filters stay (hostnames/addresses), but the "ASCII only" comment
constraints can be relaxed for display text.

## 2. New / changed files

```
docs/UI_MIGRATION.md              this plan
third-party/lunasvg/              vendored (MIT), static lib via its own CMakeLists
assets/ui/layers/                 desk, monitor, screen-logo, reflex, stars-far,
                                  stars-mid, planets, obj-g11/18/22/24/33 .svg
                                  + nebula.svg (EXTRACTED from the handoff HTML —
                                  it is inlined there, not in layers/)
assets/fonts/                     IBMPlexMono-{Regular,Medium,Bold}.ttf,
                                  IBMPlexSans-{Regular,Medium,SemiBold}.ttf, OFL.txt
src/ui/bridge/
  bridge.{h,cpp}                  orchestrator: BridgeState, BridgeAction, draws the
                                  fullscreen window: cards + dock + session status +
                                  panels; calls into scene/panels/warp
  scene.{h,cpp}                   SDL layer textures (lunasvg), parallax tick
                                  (c += (t−c)·0.055), depth table, art-box mapping,
                                  twinkle LCG (seed 42 — exact replica), bg gradient,
                                  vignette, shooting star, glow, reflex, flash,
                                  dashed-line + dashed-ellipse helpers (beams, ring)
  panels.{h,cpp}                  Settings panel (depth 70), Pair modal (depth 80),
                                  PIN-on-monitor panel (screen rect)
  warp.{h,cpp}                    warpT easing (0.05/frame toward target), sky scale/
                                  opacity, card exit math, flash envelope (peak 72%
                                  of 2.2 s)
  text.{h,cpp}                    TextSpaced() letter-spacing helper, font pushers
src/app/presence.{h,cpp}          NEW (U6): worker thread, GET http://host:port/
                                  serverinfo (curl easy, 1.5 s timeout) per host every
                                  10 s; thread-safe {address → online} snapshot
src/app/settings.{h,cpp}          add int64_t last_connected (unix s, 0 = never)
src/ui/pin_dialog.cpp             visual restyle only (glass panel), API unchanged
src/ui/scale.cpp                  font atlas: Plex Sans default + Plex Mono/weights
src/ui/theme.cpp                  extend with the full design-token set (§4)
src/main.cpp                      swap host_list→bridge; AppMode stays 3-state — warp
                                  is UI-level; gate the Viewing switch on warp
src/ui/host_list.{h,cpp}          DELETED in U7
CMakeLists.txt                    add lunasvg + bridge sources; remove host_list (U7)
docs/VENDOR.md                    lunasvg + fonts provenance rows
```

## 3. Milestones

### U0 — Groundwork: fonts, assets, scene scaffold (4–6 days)

1. Vendor `lunasvg` (pinned release tag, MIT); commit fonts + layer SVGs;
   **extract `nebula.svg`** from the handoff HTML (the inline `<svg>` inside the
   `data-depth="12"` div, `Cosmic Desk.dc.html` line 63; add `xmlns` and drop the
   inline `style` attribute — CSS animations are re-implemented in C++).
2. `ui/text.{h,cpp}`: `TextSpaced()` + font push helpers. Extend `scale.cpp` font
   atlas (Plex Sans default, Plex Mono Medium/Bold variants); extend `theme.cpp`
   with all §4 tokens.
3. `ui/bridge/scene.{h,cpp}`: load layers, rasterize-on-resize
   (`SDL_TEXTUREACCESS_STATIC`, `SDL_BLENDMODE_BLEND`, `SDL_SetTextureAlphaMod` for
   animation), parallax tick, **Bridge view**: bg radial (runtime-generated
   texture) + 44 twinkles (exact LCG replica: seed 42, `s=1+rnd()·2.2` px,
   periods 2–5.5 s, delays rnd()·5 s, sine opacity .12↔.9).
4. `main.cpp`: add the scene behind the UI (window default size bumped to
   ~1280×720 × scale, min-size clamp; still hides to tray; HiddenToTray path
   untouched). The classic window content stays drawn as a plain overlay window
   during U0–U2 so Pair/Connect/Settings remain usable; U3 replaces it with the
   machine cards and U4 with the new panels, U7 deletes `host_list.cpp`.

**Accept:** `ninja -C build && ./build/cosmicdesk` → fullscreen deep-indigo space,
stars twinkle, moving the mouse slides the background with the documented weight
(0.055 smoothing); resize re-rasterizes crisply; DPI scale on a HiDPI panel is
correct; tray/hidden/viewer paths unaffected.

**U0 accepted simplifications** (reviewed and locked at the U0 milestone):
- The depth-8 background (radial gradient + twinkles) is drawn static — no ±8 px
  parallax and no 1.03 scale. Imperceptible for a radial gradient; revisit in
  the U6/U7 fidelity pass.
- `screen-logo.svg` renders at full alpha continuously until U2 adds the boot
  fade.
- `reflex.svg` rasterizes empty under lunasvg (its clip/`<use>` references the
  monitor layer's screen rect — the handoff README line 130 itself flags that
  the clip mask needs to be made self-contained). U2 inlines the references; the
  per-frame glint math is already in place and inert until then.
- The opaque classic overlay window occludes the left half of the scene during
  U0–U2 (it is removed by U3/U4). Expected, not a bug.
- Parallax mouse input: when the cursor leaves the window ImGui reports
  (-FLT_MAX,-FLT_MAX); the scene holds the last smoothed position (isfinite
  guards) — matching the prototype, which only updates on in-window mousemove.

### U1 — Sky complete (3–4 days)

1. Add remaining sky layers in prototype order with the handoff's depth/ex/ey/es
   values (bg 8, nebula 12 drift 34 s, planets 13 drift2 32 s −8 s, stars-far 10,
   stars-mid 20 drift2 26 s). Art box = `max(104vw,140vh)` wide, aspect
   1620.8481/1200, centered (replicates HTML line 57 so parallax never reveals
   edges).
2. `dashed_line()` / `dashed_ellipse()` geometry helpers (SDL_RenderGeometry; dash
   3 px gap 7 px at scale, animated `dashoffset`).
3. Orbit ring (dashed ellipse, min(82vw,1240) × min(74vh,720), center 50/47 %,
   rotate −8°, `ringspin` 22 s sway −8°→−5°). Vignette (runtime radial texture
   from 55 %). Shooting star (170×2 streak texture, 74 %/9 %, −26°, 14 s cycle
   from t=3 s, visible first ~13 %). Warp-flash white radial texture (hidden,
   used by U5).

**Accept:** full sky indistinguishable from `00-bridge-main.png` minus cards/desk;
beams/ring helpers proven with a temporary debug draw.

### U2 — Desk scene (3–5 days)

1. Desk group = one layer (depth 26, extra y +10): `desk.svg` → `monitor.svg` →
   `screen-logo.svg` (boot only, fade 1.4 s) → `obj-g*.svg` ×5 → `reflex.svg`.
   `screen_rect()` helper: left 38.86 %, top 29.57 %, w 28.56 %, h 23.66 % of the
   art box.
2. Boot sequence (once per launch, first Bridge visibility): 5 mono lines at
   0.3/1.0/1.7/2.4/3.1 s, final line `#8ac7e5` "BRIDGE ONLINE — WELCOME", ends
   4.4 s; OK/FAIL per line driven by **real** state (`hostglue` running, real
   `port_base`); screen logo fades after.
3. Hosting beacon: green dot at 52.9 %/55.4 % (`beacon` 2.6 s pulse) +
   `HOSTING :<port_base> · <paired_client_count()> PAIRED` pill. Screen-glow
   radial with `flick` 7 s keyframes. Reflex opacity `glint²`,
   `glint = clamp(0.5 + 1.4·c.x − 0.6·c.y, 0, 1)`.

**Accept:** desk scene matches the screenshot; boot plays with real port/paired
values; beacon label updates when a client pairs; beacon/glow/reflex react to the
mouse like the prototype.

### U3 — Machine cards + orbit (5–7 days)

1. `settings.h`: add `int64_t last_connected` (+ JSON field, missing = 0); set it
   in the viewer session on successful stream start.
2. `bridge.cpp`: orbit math per card i — base angle `−π/2 + (i−1)·1.15`, sway
   `0.07·sin(0.09·t + 2.1·i) + 0.05·c.x`, ellipse rx=min(26vw,400)/ry=min(24vh,235)
   tilted −8°, depths 88/94/102, `×0.35` parallax factor, on-screen clamp (8 px
   margin), float bob (7/8.5/9.5 s, 0↔−8 px). Cards positioned as children;
   hover: border `rgba(138,199,229,.85)` + glow approximation; selected border
   purple.
3. Card anatomy per the handoff (border-tab title row with status dot +
   ellipsized name, state label right, divider gradient, mono address, muted
   last-connected, CONNECT `#438a70` / PAIR ghost); state mapping:
   `online&&paired → LINK READY`, `paired → STANDBY`, `!paired → NOT PAIRED`
   (online arrives in U6; until then LINK READY never shows). Inline rename (✎)
   → prefilled input → `Edit` action (uppercase, Enter/Esc).
4. Bottom dock (PAIR MACHINE / SETTINGS, fixed, never orbits) + session status
   (bottom-left) + DISCONNECT (red, when connecting/connected) + empty-state
   beacon card (8 %/30 %, 270 px) when no hosts.
5. Wire everything through a new `BridgeAction` (same kinds as `HostListAction`)
   — main.cpp's application block needs only renames. Keep the pair-latch logic
   (main.cpp:578–615) intact.

**Accept:** with 2–3 machines the Bridge matches `00-bridge-main.png`; click
Connect → existing session starts (video view unchanged); Pair opens the *old*
modal still (U4 restyles it); rename/remove/edit work; empty state shows with
zero hosts; cards never leave the viewport while orbiting.

### U4 — Panels: Settings, Pair modal, PIN on monitor (5–7 days)

1. **Settings panel** (right 4 %/top 10 %, 348 px, depth 70): segmented
   NATIVE/1080P/1440P/4K (maps to `ResolutionMode`; Custom stays settable via
   cosmic.json), FPS stepper 10–240 step 10, PORT BASE stepper 1024–65400
   step 1, BITRATE slider 5–150 (`bitrate_kbps/1000`, value in `#ffce54`),
   Autostart toggle (40×20, wired to `app::autostart` like today), footnote.
   Replaces `settings_window.cpp` usage.
2. **Pair modal** (372 px, depth 80, scrim `rgba(6,8,20,.5)` click-closes when
   idle): address + nickname inputs, default-port checkbox, PAIR (disabled until
   non-empty) / CLOSE; while pairing: divider + "◈ HANDSHAKE IN TRANSIT — PIN IS
   ON THE MONITOR" + CANCEL, scrim + other UI fade to 25 %
   (`PushStyleVar(ImGuiStyleVar_Alpha)`), panel slides lower-left
   (left max(16 %,220 px), top 72 %).
3. **PIN on the monitor**: inside `screen_rect()` — scanlines (repeating 2 px
   alpha quads), 3 px sweep line bottom→top 2.4 s, "ENTER THIS PIN ON THE HOST" +
   large PIN (glow approximated by a soft texture behind) + "AWAITING
   CONFIRMATION…", slide-up entry. Fed by the real `pairing.show_pin/pin` latch.
4. Restyle `pin_dialog.cpp` (incoming host-side pair request) to the same glass
   panel. Error display for failed pairing stays sticky per today's `pair_error`
   behavior.

**Accept:** matches `01/02/03` screenshots; settings changes persist and affect
the next stream; a real two-machine pairing shows the PIN on the in-scene monitor
and completes; cancel/error paths behave exactly as today.

### U5 — Warp (3–5 days)

1. `warp.cpp`: `warpT += (target − warpT)·0.05/frame`. On Connect: target 1;
   stars-far/mid/planets scale →9/12/16 and fade out; card orbit offsets multiply
   `1 + w²·5`, scale `1 + 0.6w`; flash peaks at 72 % of a 2.2 s window; beams go
   green, dashoffset −60 px/s.
2. `main.cpp` wiring: on `Connect` start the session immediately (already async)
   and raise warp; **enter Viewing only when `Streaming` && warpT ≥ 0.95**; on
   session end/Disconnect → warp target 0 → Bridge reassembles. Failed connect:
   warp target 0 + sticky error surfaced in the session status line.
   `HiddenToTray` during warp skips drawing (safe).

**Accept:** matches `04-warp-connected.png` semantics; connect → cards fly out,
sky zooms, flash, then video; disconnect → scene reassembles; 10×
connect/disconnect loop is stable; a dead host warps back with an error, never a
stuck state.

### U6 — Presence + polish (3–5 days)

1. `app/presence.{h,cpp}`: curl worker as specced in §2; cards show LINK READY
   (green) for reachable hosts; the host's own beacon counts pairs. Handles: host
   down, port override, config reload.
2. Polish pass: DOF approximation (far-card alpha fade by cursor distance),
   shadow-halo texture behind cards, hover tether-beam highlight, `chromeOpacity`
   fades, sub-pixel jitter fixes, frame-time sanity on a 60 Hz vsync loop.

**Accept:** status dots reflect reality within ~10 s (flip machine off/on);
visual diff against all 5 screenshots passes at normal glance.

### U7 — Cleanup + docs (2–3 days)

1. Delete `src/ui/host_list.{h,cpp}` + its CMake entry and main.cpp remnants.
   Verify zip/deb bundling picks up `assets/ui/layers` + `assets/fonts`
   (make-zip.ps1 copies `assets/` wholesale — confirm; Linux
   `install(DIRECTORY assets/)` likewise).
2. `docs/VENDOR.md`: lunasvg + IBM Plex rows. `README.md` + `PLAN.md`: link
   `docs/UI_MIGRATION.md`, note the new UI. CI green on MSYS2 + Ubuntu.
3. Final fidelity pass: side-by-side screenshots vs the handoff on Win + Linux;
   tune per-OS font sizes.

**Accept:** fresh clone builds on both OSes; no classic-UI code remains; release
zip/deb show the Bridge; docs complete.

## 4. Design-token constants (verbatim into theme.cpp / scene.cpp)

```
bg deep #101226 · panel #14172e/#1a1c37/#23284a · border rgba(58,63,107,.9) ·
divider rgba(106,116,187,.4→.5)
text #edf2fb · secondary #a9b1d6 · muted #565e86 · data-cyan #8ac7e5 ·
lavender #6a74bb/#8e6db8 · purple #b897d3
green #8ac49c · btn #438a70 hover #5cae8a · selected #609e75 · amber #ffce54 ·
red #b0556b hover #c9758a
Depths: bg 8 · nebula 12 · stars-far 10 · stars-mid 20 · planets 13 · desk 26 ·
settings 70 · pair 80 · cards 88/94/102
Timings: parallax smooth .055 · ring 22s · nebula drift 34s · drift2 26/32s ·
twk 2–5.5s · beacon 2.6s · flick 7s · boot 0.3/1.0/1.7/2.4/3.1s (end 4.4s) ·
pinup .7s · scan sweep 2.4s · pinpulse 2.4s · shoot 14s @3s · bob 7/8.5/9.5s ·
warp ease .05/frame, sky scale 9/12/16, card ×(1+w²·5), flash 2.2s peak 72%
```

## 5. Risks & guardrails for the implementer

- **Texture lifetime on resize:** rebuild all layer textures in one place on
  `SDL_WINDOWEVENT_SIZE_CHANGED`; never cache raw pointers across frames.
  (Biggest novice trap — coded first in U0, tested hard.)
- **ImGui per-frame repositioning:** always `SetNextWindowPos/SetCursorPos` with
  `ImGuiCond_Always`; child IDs pinned via `"###"+address` so renames don't drop
  held clicks (pattern already used in host_list.cpp:95).
- **Never call session/SDL state mid-frame:** every action flows out as a struct
  applied after `ImGui::Render()` — the codebase's existing invariant.
- **lunasvg on MSYS2:** static lib via `add_subdirectory`; no external deps. If a
  layer renders wrong (gradients/clips), fall back to rasterizing that layer at a
  fixed 2560×1895 and committing the PNG — documented escape hatch.
- **Don't touch the viewer path:** the `Viewing` branch of the main loop is
  off-limits except the two marked gate points in U5.
- **Order matters:** no milestone starts before the previous one's acceptance
  passes; each leaves the app runnable.
