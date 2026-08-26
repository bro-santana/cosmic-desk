/*
 * COSMIC MODIFICATION: vendored shim for <uuid/uuid.h>.
 *
 * moonlight-embedded's client.c uses libuuid (uuid_generate_random /
 * uuid_unparse) to build the uniqueid/uuid query parameters. MSYS2 UCRT64 has
 * no mingw-w64 libuuid package, so on Windows we provide the same API on top
 * of the system RPC UUID functions (UuidCreate, linked from rpcrt4). On Linux
 * the system <uuid/uuid.h> is used unchanged.
 *
 * The shim is only on the include path on Windows (see CMakeLists.txt); on
 * Linux client.c's <uuid/uuid.h> resolves straight to the system header.
 *
 * uuid_t must be unsigned char[16] exactly like libuuid: client.c calls
 * uuid_generate_random(uuid) relying on array-decay so the caller's buffer is
 * filled (a struct typedef would be passed by value and never written back).
 */

#ifndef COSMIC_UUID_SHIM_H
#define COSMIC_UUID_SHIM_H

#ifdef _WIN32

#include <rpc.h>
#include <stdio.h>
#include <string.h>

/* MinGW's rpcdce.h maps uuid_t to UUID; only that header uses the name, so it
 * is safe to undef and use libuuid's layout. */
#ifdef uuid_t
#undef uuid_t
#endif
typedef unsigned char uuid_t[16];

static inline void uuid_generate_random(uuid_t out) {
  UUID u;
  UuidCreate(&u);
  memcpy(out, &u, sizeof(u));
}

static inline void uuid_unparse(const uuid_t uu, char *out) {
  /* Same lowercase 8-4-4-4-12 format as libuuid's uuid_unparse(). */
  UUID u;
  memcpy(&u, uu, sizeof(u));
  sprintf(out, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          (unsigned) u.Data1, (unsigned) u.Data2, (unsigned) u.Data3,
          (unsigned) u.Data4[0], (unsigned) u.Data4[1],
          (unsigned) u.Data4[2], (unsigned) u.Data4[3],
          (unsigned) u.Data4[4], (unsigned) u.Data4[5],
          (unsigned) u.Data4[6], (unsigned) u.Data4[7]);
}

#else

#include <uuid/uuid.h>

#endif

#endif