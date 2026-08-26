cmake_minimum_required(VERSION 3.21)
project(cosmic-desk LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

find_package(SDL2 REQUIRED)
find_package(nlohmann_json REQUIRED)

add_subdirectory(third-party/imgui)

# ---------------------------------------------------------------------------
# cosmic_host: vendored stripped Sunshine host as a static library (M1).
# ---------------------------------------------------------------------------
add_subdirectory(host/sunshine)

# ---------------------------------------------------------------------------
# cosmic_libgamestream: vendored moonlight-embedded pairing/HTTPS client (M2).
# Linked into cosmicdesk (M2.2); its curl/expat/OpenSSL/rpcrt4 deps are PUBLIC
# so the executable inherits them.
# ---------------------------------------------------------------------------
add_subdirectory(third-party/libgamestream)

# ---------------------------------------------------------------------------
# cosmic_moonlight_common: full moonlight-common-c viewer library (M2).
# The submodule's own CMakeLists is NOT added as a subdirectory: it would
# re-add_subdirectory(enet), clashing with the enet target host/sunshine
# already creates. We mirror its source list and include dirs manually and
# link the existing enet target. Linked into cosmicdesk (M2.2).
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)

file(GLOB COSMIC_MOONLIGHT_COMMON_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/src/*.c
)
# RtspParser.c and the nanors trio (rs.c, oblas_common.c, oblas_lite.c) are
# compiled into cosmic_host (host/sunshine/CMakeLists.txt) and must not be
# duplicated here, or the final link fails with duplicate symbols. cosmic_host
# is listed AFTER cosmic_moonlight_common on the cosmicdesk link line, so the
# linker resolves the RtspParser/FEC references from cosmic_host's objects.
list(REMOVE_ITEM COSMIC_MOONLIGHT_COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c
)

add_library(cosmic_moonlight_common STATIC ${COSMIC_MOONLIGHT_COMMON_SOURCES})

# src/ is PUBLIC so future consumers (the viewer) can include <Limelight.h>.
target_include_directories(cosmic_moonlight_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/src
)
target_include_directories(cosmic_moonlight_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/nanors
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/nanors/deps/obl
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/enet/include
)

# Mirrors the submodule's CMakeLists compile definition.
target_compile_definitions(cosmic_moonlight_common PRIVATE HAS_SOCKLEN_T)

target_link_libraries(cosmic_moonlight_common PRIVATE
    enet
    OpenSSL::Crypto
)

# ---------------------------------------------------------------------------
# cosmicdesk: the single unified binary (host role + viewer role + UI).
# M0 scope: SDL window, ImGui, tray icon, settings file.
# M1.3: host glue (src/hostglue/host.cpp) boots the vendored host at launch.
# M2.2: viewer session (src/viewer/session.cpp) + decoder/audio stubs.
# ---------------------------------------------------------------------------
add_executable(cosmicdesk
    src/main.cpp
    src/app/settings.cpp
    src/hostglue/host.cpp
    src/hostglue/pin_bridge.cpp
    src/ui/pin_dialog.cpp
    src/ui/settings_window.cpp
    src/ui/tray.cpp
    src/ui/viewer_topbar.cpp
    src/viewer/session.cpp
    src/viewer/decoder.cpp
    src/viewer/vrenderer.cpp
    src/viewer/audio.cpp
    src/viewer/input.cpp
    src/viewer/keymap.cpp
)

target_include_directories(cosmicdesk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/tray
    # host.cpp includes vendored Sunshine headers as "src/..." (host/sunshine root).
    ${CMAKE_CURRENT_SOURCE_DIR}/host/sunshine
    # src/viewer/*.cpp include <Limelight.h> and <client.h>.
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/moonlight-common-c/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/libgamestream
)

# cosmic_host is C++23; the glue and everything else in this binary must agree.
target_compile_features(cosmicdesk PRIVATE cxx_std_23)

if(WIN32)
    # SDL2main is a static library that calls into SDL2, so it has to be listed
    # before SDL2 on the link line.
    target_link_libraries(cosmicdesk PRIVATE SDL2::SDL2main)
endif()

# Link order matters: cosmic_moonlight_common first, then cosmic_libgamestream,
# then cosmic_host. RtspParser.c and the nanors trio live only in cosmic_host,
# so the linker must see cosmic_moonlight_common's references to them before it
# reaches cosmic_host's objects that satisfy them.
target_link_libraries(cosmicdesk PRIVATE
    cosmic_moonlight_common
    cosmic_libgamestream
    cosmic_host
    cosmic_imgui
    nlohmann_json::nlohmann_json
    SDL2::SDL2
)

if(WIN32)
    # shell32 provides ExtractIconEx for the tray icon.
    target_link_libraries(cosmicdesk PRIVATE shell32 user32 gdi32)
    # Console window is useful for M0..M5 logs; M6 packaging switches this off.
    if(NOT COSMIC_CONSOLE)
        set_target_properties(cosmicdesk PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
else()
    # zserge/tray's AppIndicator backend pulls in GTK3 + libayatana-appindicator.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(APPINDICATOR REQUIRED IMPORTED_TARGET ayatana-appindicator3-0.1)
    target_link_libraries(cosmicdesk PRIVATE PkgConfig::APPINDICATOR)
    target_compile_definitions(cosmicdesk PRIVATE TRAY_AYATANA_APPINDICATOR=1)
endif()

# Tray icon and other runtime assets must sit next to the executable.
add_custom_command(TARGET cosmicdesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:cosmicdesk>/assets
    COMMENT "Copying assets next to cosmicdesk"
)
