// Cosmic Desk — clipboard image transcoding (BMP file <-> PNG).
//
// SDL's Windows clipboard backend hands out and accepts complete BMP *files*
// (BITMAPFILEHEADER followed by the DIB) for CF_DIB/CF_DIBV5 — see
// third-party/SDL/src/video/windows/SDL_windowsclipboard.c's
// WIN_ConvertDIBtoBMP and WIN_ConvertBMPtoDIB. Phase 2 clipboard image sync
// wants to move a much smaller, well-understood format over the wire, so
// this module transcodes between that BMP file and PNG, backed by the
// already-vendored stb_image decoder (third-party/stb/stb_image.h, whose one
// implementation TU is src/ui/bridge/scene.cpp) and the newly vendored
// stb_image_write encoder (third-party/stb/stb_image_write.h, implemented in
// clipimage.cpp — the only TU that emits it).
#pragma once
#include <cstddef>
#include <string>

namespace cosmic::clipimage {

// Decodes a BMP file (`bmp`/`bmp_size`) and re-encodes it as PNG into
// `out_png`, returning true on success. A Windows DIB's alpha byte is
// undefined for a plain (non-DIBV5) bitmap, so it is never trusted here:
// decoding always drops to 3 channels (RGB), and the PNG is written fully
// opaque. The declared dimensions are checked, without decoding any pixels,
// against a 64-megapixel cap before decoding is attempted at all — a
// pathological input could otherwise force a multi-hundred-megabyte
// allocation and a multi-second stall before this function got a chance to
// fail. Fails — returning false and leaving `out_png` empty — if the BMP
// fails to decode, has a non-positive width or height, its pixel count
// exceeds that cap, the encoder fails, or the encoded PNG would exceed
// `max_png_bytes` (the caller's own size cap, e.g. the host's clipboard body
// limit).
bool bmp_to_png(const void* bmp, std::size_t bmp_size, std::size_t max_png_bytes,
                std::string& out_png);

// Decodes a PNG (`png`/`png_size`) and re-encodes it as a 24-bit BI_RGB BMP
// file into `out_bmp`, returning true on success. A PNG's alpha channel, if
// present, is composited over an opaque white background rather than carried
// through (BMP's BI_RGB has no alpha channel to put it in). stb_image_write
// always emits biSizeImage = 0, which SDL's WIN_ConvertBMPtoDIB takes at face
// value when sizing the DIB it hands to the clipboard — so this function
// patches the correct row-aligned pixel byte count into biSizeImage before
// returning, which is what makes the result acceptable to WIN_ConvertBMPtoDIB.
// A compressed PNG's byte size says nothing about its decoded size (a ~1 MiB
// solid-colour image can decode to gigabytes of RGBA), so the declared
// dimensions are checked against a 64-megapixel cap — without decoding any
// pixels — before decoding is attempted at all; PNG bytes arrive here from an
// authenticated peer over the clipboard route, and this call runs on the
// main/render thread, so a decode-then-reject ordering would itself be the
// stall this guards against. Fails — returning false and leaving `out_bmp`
// empty — if the PNG fails to decode, has a non-positive width or height, its
// pixel count exceeds that cap, the encoder fails, or the encoded buffer's
// shape does not match what the biSizeImage patch expects.
bool png_to_bmp(const void* png, std::size_t png_size, std::string& out_bmp);

}  // namespace cosmic::clipimage
