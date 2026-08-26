# ---------------------------------------------------------------------------
# Cosmic Desk — vendored moonlight-embedded libgamestream, built as a static
# library.
#
# COSMIC MODIFICATION: this file is new. Upstream moonlight-embedded builds
# libgamestream as a shared library against avahi + its own moonlight-common-c
# copy. Cosmic Desk vendors only the pairing/HTTPS client subset (client.c,
# http.c, mkcert.c, xml.c — no discover.c, no sps.c) as the static library
# `cosmic_libgamestream` against SYSTEM packages only:
#   - libcurl (pkg-config), expat (pkg-config), OpenSSL (find_package)
#   - UUID: Windows uses the vendored shim third-party/libgamestream/uuid/
#     (UuidCreate from <rpc.h>, linked from rpcrt4 — MSYS2 UCRT64 has no
#     mingw-w64 libuuid); Linux uses the system libuuid via pkg-config.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

project(cosmic_libgamestream LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(EXPAT REQUIRED expat)

if(WIN32)
    # No mingw-w64 libuuid package exists for UCRT64; the vendored shim
    # (uuid/uuid.h) provides uuid_generate_random/uuid_unparse on top of
    # UuidCreate from rpcrt4.
    set(COSMIC_LIBGAMESTREAM_UUID_LIBRARIES rpcrt4)
else()
    pkg_check_modules(UUID REQUIRED uuid)
    set(COSMIC_LIBGAMESTREAM_UUID_LIBRARIES ${UUID_LIBRARIES})
endif()

add_library(cosmic_libgamestream STATIC
    client.c
    http.c
    mkcert.c
    xml.c
)

# client.h/client.c include <Limelight.h> from the moonlight-common-c submodule.
target_include_directories(cosmic_libgamestream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../moonlight-common-c/src
    ${CURL_INCLUDE_DIRS}
    ${EXPAT_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)
if(WIN32)
    # Only on Windows: the vendored uuid/uuid.h shim must shadow the system
    # header for client.c's <uuid/uuid.h>. On Linux the shim directory must
    # NOT be on the include path, or it would shadow the system <uuid/uuid.h>
    # that the shim itself includes.
    target_include_directories(cosmic_libgamestream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Vendored code: keep the build quiet, never -Werror.
target_compile_options(cosmic_libgamestream PRIVATE -w)

# Static libcurl/expat on MSYS2 need their static-link defines (mirrors the
# Cflags.private of libcurl.pc/expat.pc and host/sunshine's CURL_STATICLIB).
target_compile_definitions(cosmic_libgamestream PRIVATE CURL_STATICLIB XML_STATIC)

# PUBLIC so the cosmicdesk executable inherits curl/expat/OpenSSL/rpcrt4 at the
# final link (static libs don't carry their own dependencies).
target_link_libraries(cosmic_libgamestream PUBLIC
    ${CURL_LIBRARIES}
    ${EXPAT_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${COSMIC_LIBGAMESTREAM_UUID_LIBRARIES}
)