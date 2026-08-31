// Cosmic Desk — clipboard image transcoding (see clipimage.h).
//
// Decoding reuses the vendored stb_image.h decoder that scene.cpp already
// emits STB_IMAGE_IMPLEMENTATION for; this TU only pulls in its
// declarations (STBI_NO_STDIO to match scene.cpp — nothing here reads from a
// file). Encoding needs stb_image_write.h, which has no other consumer in
// the tree, so this is the one TU that defines
// STB_IMAGE_WRITE_IMPLEMENTATION for it.

#include "app/clipimage.h"

#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace cosmic::clipimage {

namespace {

// Threaded through stb_image_write's callback so the encoded bytes land
// directly in the caller's std::string instead of a temporary buffer. stb
// invokes this from its own (effectively C) call frames, so an exception
// escaping it (e.g. std::bad_alloc from append()) would be undefined
// behavior; `failed` lets the C++ side notice and fail cleanly instead.
struct AppendContext {
    std::string* out;
    bool failed = false;
};

void AppendToString(void* context, void* data, int size) {
    AppendContext* ctx = static_cast<AppendContext*>(context);
    if (ctx->failed || size <= 0) {
        return;
    }
    try {
        ctx->out->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
    } catch (...) {
        ctx->failed = true;
    }
}

// Pixel-count cap applied before any decode. A compressed image's byte size
// says nothing about its decoded size — a ~1 MiB solid-colour 30000x30000
// PNG decodes to roughly 3.6 GB of RGBA — so stbi_load_from_memory must never
// be called before the declared dimensions are known to be sane; by the time
// it returns (or fails), it has already committed to the allocation and the
// decompression, which is exactly the multi-second main-thread stall this
// guards against. 64 megapixels comfortably covers real clipboard content —
// a five-monitor 4K span is ~41 MP, an 8K screenshot is ~33 MP — while
// bounding a 4-channel decode to roughly 256 MB instead of multiple GB.
constexpr std::uint64_t kMaxDecodedPixels = 64'000'000;

// Probes `size` bytes of `data` for its declared width/height without
// decoding any pixels, and reports whether stbi_load_from_memory should even
// be attempted: false if the probe itself fails, either dimension is
// non-positive, or the pixel count exceeds kMaxDecodedPixels. `size` must
// already be known to fit in an int (callers check this before calling in).
// Width and height are each bounded by stb's own int range, so their product
// widened to uint64_t cannot overflow.
bool DimensionsWithinCap(const unsigned char* data, int size) {
    int width = 0;
    int height = 0;
    int comp = 0;
    if (!stbi_info_from_memory(data, size, &width, &height, &comp)) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return pixel_count <= kMaxDecodedPixels;
}

}  // namespace

bool bmp_to_png(const void* bmp, std::size_t bmp_size, std::size_t max_png_bytes,
                std::string& out_png) {
    out_png.clear();

    // stbi_load_from_memory takes the buffer length as a plain int; a
    // caller-supplied size beyond INT_MAX would wrap when cast, so reject it
    // up front rather than hand stb a bogus length.
    if (bmp_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (!DimensionsWithinCap(static_cast<const unsigned char*>(bmp),
                             static_cast<int>(bmp_size))) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    // req_comp = 3: a plain Windows DIB's alpha byte is undefined (see
    // clipimage.h), so it is deliberately dropped here rather than trusted
    // into the PNG's alpha channel.
    unsigned char* pixels =
        stbi_load_from_memory(static_cast<const unsigned char*>(bmp), static_cast<int>(bmp_size),
                              &width, &height, &channels_in_file, 3);
    if (pixels == nullptr) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return false;
    }

    AppendContext ctx{&out_png};
    const int wrote =
        stbi_write_png_to_func(AppendToString, &ctx, width, height, 3, pixels, width * 3);
    stbi_image_free(pixels);

    if (!wrote || ctx.failed) {
        out_png.clear();
        return false;
    }
    if (out_png.size() > max_png_bytes) {
        // Not a truncation: the whole point of the cap is that an oversize
        // result is unusable, so it is discarded rather than kept partial.
        out_png.clear();
        return false;
    }
    return true;
}

bool png_to_bmp(const void* png, std::size_t png_size, std::string& out_bmp) {
    out_bmp.clear();

    if (png_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (!DimensionsWithinCap(static_cast<const unsigned char*>(png),
                             static_cast<int>(png_size))) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    // req_comp = 4: decode with alpha so it can be composited below — BMP's
    // BI_RGB target format has no alpha channel to carry it through as-is.
    unsigned char* pixels =
        stbi_load_from_memory(static_cast<const unsigned char*>(png), static_cast<int>(png_size),
                              &width, &height, &channels_in_file, 4);
    if (pixels == nullptr) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return false;
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    // w * h * 3 is the tightly-packed RGB buffer size; check both
    // multiplications for overflow before trusting the result to size().
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (w > kMax / h || (w * h) > kMax / 3) {
        stbi_image_free(pixels);
        return false;
    }
    const std::size_t rgb_size = w * h * 3;

    std::string rgb;
    try {
        rgb.resize(rgb_size);
    } catch (...) {
        stbi_image_free(pixels);
        return false;
    }

    // Composite RGBA over opaque white: out = src * a/255 + 255 * (1 -
    // a/255), computed in the 0..255*255 fixed-point domain and rounded to
    // nearest (the "+127, /255" trick) rather than truncated.
    unsigned char* dst = reinterpret_cast<unsigned char*>(rgb.data());
    const std::size_t pixel_count = w * h;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const unsigned char* src = pixels + i * 4;
        const unsigned int a = src[3];
        for (int c = 0; c < 3; ++c) {
            const unsigned int blended = src[c] * a + 255u * (255u - a);
            dst[i * 3 + c] = static_cast<unsigned char>((blended + 127) / 255);
        }
    }
    stbi_image_free(pixels);

    AppendContext ctx{&out_bmp};
    const int wrote = stbi_write_bmp_to_func(AppendToString, &ctx, width, height, 3, rgb.data());

    if (!wrote || ctx.failed) {
        out_bmp.clear();
        return false;
    }

    // stb_image_write's BMP encoder always writes biSizeImage = 0 (see
    // stb_image_write.h's stbi_write_bmp_core format string), and
    // WIN_ConvertBMPtoDIB (third-party/SDL/src/video/windows/
    // SDL_windowsclipboard.c) takes that field at face value when sizing the
    // pixel buffer of the DIB it puts on the clipboard — left at 0, SDL
    // builds a DIB that claims WxHx24bpp but carries zero pixel bytes. Patch
    // in the real, row-aligned pixel byte count before handing the buffer
    // back.
    const std::size_t row_bytes = (w * 3u + 3u) & ~std::size_t{3};  // rows are 4-byte aligned
    const std::size_t image_bytes = row_bytes * h;
    constexpr std::size_t kBmpHeaderBytes = 54;  // 14-byte file header + 40-byte info header
    if (out_bmp.size() != kBmpHeaderBytes + image_bytes ||
        image_bytes > std::numeric_limits<std::uint32_t>::max()) {
        // The buffer's shape does not match what the patch below assumes;
        // refuse to write into it rather than guess at an offset.
        out_bmp.clear();
        return false;
    }
    // biSizeImage sits at byte offset 34 (14 file header + 4 biSize + 4
    // width + 4 height + 2 planes + 2 bpp + 4 biCompression). Written byte
    // by byte, little-endian, rather than memcpy-ing a host-endian integer,
    // so this is correct regardless of host endianness.
    const std::uint32_t size_field = static_cast<std::uint32_t>(image_bytes);
    unsigned char* header = reinterpret_cast<unsigned char*>(out_bmp.data());
    header[34] = static_cast<unsigned char>(size_field & 0xff);
    header[35] = static_cast<unsigned char>((size_field >> 8) & 0xff);
    header[36] = static_cast<unsigned char>((size_field >> 16) & 0xff);
    header[37] = static_cast<unsigned char>((size_field >> 24) & 0xff);

    return true;
}

}  // namespace cosmic::clipimage
