// Cosmic Desk - independently flowing SVG color layers behind a fixed mask.
#pragma once

#include <SDL3/SDL.h>
#include <memory>
#include <string>

namespace cosmic::ui::scene {

class NebulaFlow {
public:
    NebulaFlow();
    ~NebulaFlow();
    NebulaFlow(const NebulaFlow&) = delete;
    NebulaFlow& operator=(const NebulaFlow&) = delete;

    // All methods that touch SDL must run on the renderer/main thread.
    // renderer is borrowed and must outlive this object (or a call to reset()).
    // SVG contract: see assets/ui/layers/nebula-flow-source.svg and README.md.
    // On failure, returns false and sets SDL_GetError(); an existing load is kept.
    bool load(SDL_Renderer* renderer, const std::string& svg_path,
              int raster_width, int raster_height);

    // Re-rasterizes ONLY when the effective texture size changes. On failure,
    // keeps the previous textures, so they can still be drawn during a resize.
    bool resize(int raster_width, int raster_height);

    // time_s is monotonic, unscaled real time, preferably SDL_GetTicksNS()*1e-9.
    // The animation itself runs at 0.5x: a 12-second cycle takes 24 real seconds.
    // Does not clear/present the window, move the mask, or draw stars/planets.
    // Restores the caller's render target and the renderer state it changes.
    bool draw(const SDL_FRect& destination, double time_s, float opacity = 1.0f);

    bool ready() const;
    void reset();  // Call BEFORE SDL_DestroyRenderer. Idempotent.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cosmic::ui::scene
