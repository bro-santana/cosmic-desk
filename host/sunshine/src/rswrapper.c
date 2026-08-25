/**
 * @file src/rswrapper.c
 * @brief Wrappers for nanors vectorization
 *
 * COSMIC MODIFICATION: rewritten as a thin bridge to the nanors copy bundled in the
 * moonlight-common-c submodule (third-party/moonlight-common-c/nanors). That nanors
 * revision performs its own runtime SIMD dispatch via oblas_get_impl(), so the
 * upstream multi-include ISA-specialization pattern (which targeted the standalone
 * third-party/nanors copy) is no longer needed. The function-pointer indirection of
 * rswrapper.h is preserved so the streaming code (stream.cpp) is unchanged.
 *
 * The function pointers are initialized at static-init time because upstream's
 * main.cpp (which called reed_solomon_init()) was deleted; the bundled nanors
 * provides reed_solomon_init() itself (a no-op) to satisfy rswrapper.h.
 */

#include <rs.h>

// Capture the real nanors entry points before rswrapper.h's macros rename them.
static reed_solomon *(*const rs_new_impl)(int, int) = reed_solomon_new;
static void (*const rs_release_impl)(reed_solomon *) = reed_solomon_release;
static int (*const rs_encode_impl)(reed_solomon *, uint8_t **, int, int) = reed_solomon_encode;
static int (*const rs_decode_impl)(reed_solomon *, uint8_t **, uint8_t *, int, int) = reed_solomon_decode;

#include "rswrapper.h"

reed_solomon_new_t reed_solomon_new_fn = rs_new_impl;
reed_solomon_release_t reed_solomon_release_fn = rs_release_impl;
reed_solomon_encode_t reed_solomon_encode_fn = rs_encode_impl;
reed_solomon_decode_t reed_solomon_decode_fn = rs_decode_impl;