// Cosmic Desk - SDL3 Renderer + the project's vendored lunasvg.
// No SDL_image, SDL_GPUDevice, custom shader compiler, or per-frame SVG parsing.
#include "ui/bridge/nebula_flow.h"

#include <lunasvg.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace cosmic::ui::scene {
namespace {
constexpr int kLayerCount = 8;
constexpr int kGridColumns = 64;
constexpr int kGridRows = 48;
// 10 RGBA textures at most: 8 colors, 1 mask, 1 render target.
// At the supplied SVG's aspect ratio, this cap is about 67 MiB of texture data.
constexpr int kMaxRasterDimension = 1536;
constexpr double kCycleSeconds = 12.0;
constexpr double kPlaybackSpeed = 0.5;
constexpr double kTau = 6.283185307179586476925286766559;

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const {
        if (texture) SDL_DestroyTexture(texture);
    }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;
struct FileDeleter {
    void operator()(void* data) const { SDL_free(data); }
};

// SVG elements are authored in the root coordinate system. Element::render()
// renders that element alone, not its ancestor clip. This deliberately leaves
// the color sheets UNCLIPPED; the fixed mask is applied after compositing.
Texture RasterizeElement(SDL_Renderer* renderer, const lunasvg::Element& element,
                         const lunasvg::Matrix& matrix, int width, int height,
                         SDL_BlendMode blend) {
    lunasvg::Bitmap bitmap(width, height);
    if (bitmap.isNull()) {
        SDL_SetError("nebula_flow: lunasvg bitmap allocation failed");
        return {};
    }
    bitmap.clear(0x00000000);
    element.render(bitmap, matrix);
    Texture texture(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STATIC, width, height));
    if (!texture) return {};
    // lunasvg/plutovg's native pixels are packed, PREMULTIPLIED 0xAARRGGBB.
    // Keep them premultiplied all the way to the window: do not convertToRGBA().
    if (!SDL_UpdateTexture(texture.get(), nullptr, bitmap.data(), bitmap.stride()) ||
        !SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR) ||
        !SDL_SetTextureBlendMode(texture.get(), blend)) {
        return {};
    }
    return texture;
}

// Only draw color and texture addressing are global states modified here.
// SDL3 retains viewport/clip/scale/logical presentation separately per target.
class RenderStateGuard {
public:
    explicit RenderStateGuard(SDL_Renderer* renderer)
        : renderer_(renderer), target_(SDL_GetRenderTarget(renderer)) {
        valid_ = SDL_GetRenderDrawColorFloat(renderer_, &color_.r, &color_.g,
                                             &color_.b, &color_.a) &&
                 SDL_GetRenderTextureAddressMode(renderer_, &u_mode_, &v_mode_);
    }
    ~RenderStateGuard() {
        if (active_) restore();
    }
    bool valid() const { return valid_; }
    bool restore() {
        if (!active_) return true;
        active_ = false;
        // Do not short-circuit: attempt all restores even after one fails.
        bool ok = SDL_SetRenderTarget(renderer_, target_);
        ok = SDL_SetRenderDrawColorFloat(renderer_, color_.r, color_.g,
                                        color_.b, color_.a) && ok;
        ok = SDL_SetRenderTextureAddressMode(renderer_, u_mode_, v_mode_) && ok;
        return ok;
    }
    void activate() { active_ = true; }
private:
    SDL_Renderer* renderer_;
    SDL_Texture* target_;
    SDL_FColor color_{};
    SDL_TextureAddressMode u_mode_ = SDL_TEXTURE_ADDRESS_CLAMP;
    SDL_TextureAddressMode v_mode_ = SDL_TEXTURE_ADDRESS_CLAMP;
    bool valid_ = false;
    bool active_ = false;
};

// An inverse mapping: destination positions never move, only source UVs do.
// The shared wave gives coherence; smaller phase-shifted waves give each color
// its own flow. Integer time harmonics make the whole effect loop exactly.
SDL_FPoint SampleUV(float u, float v, int layer, double phase) {
    const double x = u;
    const double y = v;
    const double q = kTau * (1.15 * x - 0.78 * y);
    const double r = kTau * (0.32 * x + 1.07 * y);
    const double offset = 0.71 * layer;
    const double rate = (layer % 3 == 0) ? 2.0 : 1.0;
    const double dx = 0.010 * (std::sin(q - phase) - std::sin(q)) +
        0.005 * (std::sin(r + rate * phase + offset) - std::sin(r + offset));
    const double dy = 0.014 * (std::sin(q - phase + 0.6) - std::sin(q + 0.6)) +
        0.006 * (std::cos(1.35 * q - rate * phase + offset) -
                 std::cos(1.35 * q + offset));
    // The asset's contour is inset far enough that clamping only affects the
    // invisible area outside the window, not the visible colored boundaries.
    return {static_cast<float>(std::clamp(x + dx, 0.0, 1.0)),
            static_cast<float>(std::clamp(y + dy, 0.0, 1.0))};
}
}  // namespace

struct NebulaFlow::Impl {
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<lunasvg::Document> document;
    std::array<lunasvg::Element, kLayerCount> elements;
    lunasvg::Element window;
    float svg_width = 0.0f;
    float svg_height = 0.0f;
    std::array<Texture, kLayerCount> layers;
    Texture mask;
    Texture output;
    int width = 0;
    int height = 0;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    bool rebuild(int requested_width, int requested_height) {
        if (requested_width <= 0 || requested_height <= 0) {
            return SDL_SetError("nebula_flow: raster dimensions must be positive");
        }
        const double limit = std::min(1.0, static_cast<double>(kMaxRasterDimension) /
            std::max(requested_width, requested_height));
        const int w = std::max(1, static_cast<int>(std::lround(requested_width * limit)));
        const int h = std::max(1, static_cast<int>(std::lround(requested_height * limit)));
        if (output && w == width && h == height) return true;

        // Transactional rebuild: keep old resources if any allocation fails.
        std::array<Texture, kLayerCount> new_layers;
        const lunasvg::Matrix matrix(static_cast<float>(w) / svg_width, 0, 0,
                                     static_cast<float>(h) / svg_height, 0, 0);
        const SDL_BlendMode multiply_alpha = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
        Texture new_mask = RasterizeElement(renderer, window, matrix, w, h,
                                            multiply_alpha);
        if (!new_mask) {
            const std::string detail = SDL_GetError();
            return SDL_SetError("nebula_flow: mask texture/blend unavailable: %s",
                                detail.c_str());
        }
        for (int i = 0; i < kLayerCount; ++i) {
            new_layers[i] = RasterizeElement(renderer, elements[i], matrix, w, h,
                                             SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            if (!new_layers[i]) return false;
        }
        Texture new_output(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_TARGET, w, h));
        if (!new_output ||
            !SDL_SetTextureScaleMode(new_output.get(), SDL_SCALEMODE_LINEAR) ||
            !SDL_SetTextureBlendMode(new_output.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED)) {
            return false;
        }
        std::vector<SDL_Vertex> new_vertices;
        std::vector<int> new_indices;
        new_vertices.reserve((kGridColumns + 1) * (kGridRows + 1));
        new_indices.reserve(kGridColumns * kGridRows * 6);
        for (int y = 0; y <= kGridRows; ++y) {
            for (int x = 0; x <= kGridColumns; ++x) {
                const float u = static_cast<float>(x) / kGridColumns;
                const float v = static_cast<float>(y) / kGridRows;
                new_vertices.push_back({{u * w, v * h}, {1, 1, 1, 1}, {u, v}});
            }
        }
        for (int y = 0; y < kGridRows; ++y) {
            for (int x = 0; x < kGridColumns; ++x) {
                const int a = y * (kGridColumns + 1) + x;
                const int b = a + 1;
                const int c = a + kGridColumns + 1;
                const int d = c + 1;
                new_indices.insert(new_indices.end(), {a, b, c, b, d, c});
            }
        }
        layers = std::move(new_layers);
        mask = std::move(new_mask);
        output = std::move(new_output);
        vertices = std::move(new_vertices);
        indices = std::move(new_indices);
        width = w;
        height = h;
        return true;
    }

    bool compose(double time_s) {
        RenderStateGuard state(renderer);
        if (!state.valid()) return false;
        state.activate();
        if (!SDL_SetRenderTarget(renderer, output.get()) ||
            !SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) ||
            !SDL_SetRenderScale(renderer, 1.0f, 1.0f) ||
            !SDL_SetRenderViewport(renderer, nullptr) ||
            !SDL_SetRenderClipRect(renderer, nullptr) ||
            !SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_CLAMP,
                                             SDL_TEXTURE_ADDRESS_CLAMP) ||
            !SDL_SetRenderDrawColorFloat(renderer, 0, 0, 0, 0) ||
            !SDL_RenderClear(renderer)) {
            const std::string detail = SDL_GetError();
            state.restore();
            return SDL_SetError("nebula_flow: render target setup failed: %s", detail.c_str());
        }
        const double period = kCycleSeconds / kPlaybackSpeed;
        const double phase = kTau * std::fmod(time_s, period) / period;
        // The bottom sheet fills the entire canvas. It guarantees no holes
        // between differently displaced colored sheets, even near the mask.
        bool ok = SDL_RenderTexture(renderer, layers[0].get(), nullptr, nullptr);
        for (int layer = 1; ok && layer < kLayerCount; ++layer) {
            for (int y = 0; y <= kGridRows; ++y) {
                for (int x = 0; x <= kGridColumns; ++x) {
                    const float u = static_cast<float>(x) / kGridColumns;
                    const float v = static_cast<float>(y) / kGridRows;
                    vertices[y * (kGridColumns + 1) + x].tex_coord =
                        SampleUV(u, v, layer, phase);
                }
            }
            ok = SDL_RenderGeometry(renderer, layers[layer].get(), vertices.data(),
                                     static_cast<int>(vertices.size()), indices.data(),
                                     static_cast<int>(indices.size()));
        }
        // dstRGBA = dstRGBA * maskA. Unlike SDL_BLENDMODE_MOD, this clips ALPHA
        // as well as RGB, leaving a properly premultiplied transparent result.
        if (ok) ok = SDL_RenderTexture(renderer, mask.get(), nullptr, nullptr);
        const std::string detail = ok ? std::string{} : SDL_GetError();
        const bool restored = state.restore();
        if (!ok) return SDL_SetError("nebula_flow: composition failed: %s", detail.c_str());
        return restored;
    }
};

NebulaFlow::NebulaFlow() = default;
NebulaFlow::~NebulaFlow() = default;

bool NebulaFlow::load(SDL_Renderer* renderer, const std::string& path,
                      int raster_width, int raster_height) {
    if (!renderer) return SDL_SetError("nebula_flow: null renderer");
    try {
        size_t size = 0;
        std::unique_ptr<void, FileDeleter> data(SDL_LoadFile(path.c_str(), &size));
        if (!data) return false;
        auto next = std::make_unique<Impl>();
        next->renderer = renderer;
        next->document = lunasvg::Document::loadFromData(
            static_cast<const char*>(data.get()), size);
        if (!next->document) return SDL_SetError("nebula_flow: cannot parse %s", path.c_str());
        next->svg_width = next->document->width();
        next->svg_height = next->document->height();
        if (!std::isfinite(next->svg_width) || !std::isfinite(next->svg_height) ||
            next->svg_width <= 0 || next->svg_height <= 0) {
            return SDL_SetError("nebula_flow: invalid SVG canvas dimensions");
        }
        // Explicit opt-in contract, not a guess based on colors, path order,
        // Inkscape IDs, or a promise to play arbitrary CSS/SMIL animation.
        const auto metadata = next->document->getElementById("nebula-flow-format-v1");
        if (!metadata) return SDL_SetError("nebula_flow: SVG must use the supplied v1 layer format");
        next->window = next->document->getElementById("nebula-window");
        if (!next->window) return SDL_SetError("nebula_flow: SVG is missing nebula-window");
        for (int i = 0; i < kLayerCount; ++i) {
            const std::string id = "nebula-layer-" + std::to_string(i);
            next->elements[i] = next->document->getElementById(id);
            if (!next->elements[i]) return SDL_SetError("nebula_flow: SVG is missing %s", id.c_str());
        }
        if (!next->rebuild(raster_width, raster_height)) return false;
        // Probe the actual renderer path now. Drivers without target/custom-
        // blend/geometry support fail load(), allowing the scene's old fallback.
        if (!next->compose(0.0)) return false;
        impl_ = std::move(next);
        return true;
    } catch (const std::bad_alloc&) {
        return SDL_SetError("nebula_flow: allocation failed");
    }
}

bool NebulaFlow::resize(int width, int height) {
    if (!ready()) return SDL_SetError("nebula_flow: call load before resize");
    try {
        return impl_->rebuild(width, height);
    } catch (const std::bad_alloc&) {
        return SDL_SetError("nebula_flow: resize allocation failed");
    }
}

bool NebulaFlow::draw(const SDL_FRect& destination, double time_s, float opacity) {
    if (!ready()) return SDL_SetError("nebula_flow: call load before draw");
    if (!std::isfinite(time_s) || !std::isfinite(opacity) ||
        !std::isfinite(destination.x) || !std::isfinite(destination.y) ||
        !std::isfinite(destination.w) || !std::isfinite(destination.h) ||
        destination.w <= 0 || destination.h <= 0) {
        return SDL_SetError("nebula_flow: invalid draw arguments");
    }
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity == 0.0f) return true;
    if (!impl_->compose(time_s)) return false;
    // Premultiplied textures must fade RGB AND alpha, not just alpha.
    return SDL_SetTextureColorModFloat(impl_->output.get(), opacity, opacity, opacity) &&
           SDL_SetTextureAlphaModFloat(impl_->output.get(), opacity) &&
           SDL_RenderTexture(impl_->renderer, impl_->output.get(), nullptr, &destination);
}

bool NebulaFlow::ready() const { return impl_ && impl_->output; }
void NebulaFlow::reset() { impl_.reset(); }

}  // namespace cosmic::ui::scene
