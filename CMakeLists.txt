cmake_minimum_required(VERSION 3.21)

# The release version is derived from git once and shared with the packaging
# scripts; see cmake/CosmicDeskVersion.cmake. Included before project() so the
# derived numeric triple can be passed to it.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CosmicDeskVersion)

project(cosmic-desk VERSION ${COSMICDESK_VERSION} LANGUAGES C CXX)

# Writes build/packaging/version.env and build/packaging/windows/version.iss,
# which make-deb.sh and installer.iss read instead of re-deriving the version.
cosmicdesk_write_version_files()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# SDL3: Windows gets it from the MSYS2 system package (mingw-w64-ucrt-x86_64-sdl3);
# Linux builds the vendored third-party/SDL submodule statically instead, since
# Ubuntu 24.04 (the Linux baseline per docs/BUILDING.md) ships no libsdl3-dev.
# This supersedes decision A1 in docs/UI_MIGRATION.md.
# 3.2 floor: everything the port uses is in SDL3's initial stable ABI, so an
# older pre-release system SDL3 must fall through to the vendored build.
find_package(SDL3 3.2 CONFIG QUIET)
if(NOT SDL3_FOUND)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(third-party/SDL)
endif()
find_package(nlohmann_json REQUIRED)

add_subdirectory(third-party/imgui)

# UI migration (docs/UI_MIGRATION.md U0): lunasvg rasterizes the parallax layer
# SVGs into SDL textures at runtime. Self-contained (bundles plutovg), MIT.
add_subdirectory(third-party/lunasvg)

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
# M0 scope: SDL3 window, ImGui, tray icon, settings file.
# M1.3: host glue (src/hostglue/host.cpp) boots the vendored host at launch.
# M2.2: viewer session (src/viewer/session.cpp) + decoder/audio stubs.
# ---------------------------------------------------------------------------
add_executable(cosmicdesk
    src/main.cpp
    src/app/autostart.cpp
    src/app/presence.cpp
    src/app/settings.cpp
    src/app/wallcache.cpp
    src/app/clipsync.cpp
    src/app/service_ctrl.cpp
    src/app/single_instance.cpp
    src/hostglue/host.cpp
    src/hostglue/pin_bridge.cpp
    src/hostglue/displays.cpp
    src/hostglue/wallpaper.cpp
    src/hostglue/clipboard.cpp
    src/ui/pin_dialog.cpp
    src/ui/scale.cpp
    src/ui/theme.cpp
    src/ui/tray.cpp
    src/ui/viewer_topbar.cpp
    src/ui/fonts.cpp
    src/ui/bridge/bridge.cpp
    src/ui/bridge/panels.cpp
    src/ui/bridge/scene.cpp
    src/ui/bridge/text.cpp
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
    # src/ui/bridge/scene.cpp includes <stb_image.h> to decode the Bridge wallpaper backdrop.
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/stb
)

# cosmic_host is C++23; the glue and everything else in this binary must agree.
target_compile_features(cosmicdesk PRIVATE cxx_std_23)

if(WIN32)
    # Embed assets/icon.ico as the executable's icon (Explorer/taskbar) instead
    # of the generic one. The .rc is generated so the icon path is absolute —
    # windres resolves relative paths against the build dir, not the source.
    set(COSMICDESK_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/packaging/windows")
    configure_file(packaging/windows/cosmicdesk.rc.in
                   ${CMAKE_BINARY_DIR}/packaging/windows/cosmicdesk.rc @ONLY)
    target_sources(cosmicdesk PRIVATE
        ${CMAKE_BINARY_DIR}/packaging/windows/cosmicdesk.rc)
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
    lunasvg::lunasvg
    nlohmann_json::nlohmann_json
    SDL3::SDL3
)

if(WIN32)
    # shell32 provides ExtractIconEx for the tray icon; iphlpapi provides
    # GetTcpTable for the --shortcut readiness poll (plan M9.1); wtsapi32
    # provides WTSQuerySessionInformationW/WTSFreeMemory for the wallpaper
    # provider's off-session fallback (plan D10c).
    target_link_libraries(cosmicdesk PRIVATE shell32 user32 gdi32 iphlpapi wtsapi32)
    # GUI app by default (WIN32_EXECUTABLE TRUE): no console window, so closing
    # the (never-shown) console cannot kill the tray app. Debug keeps a console
    # for log output; override either way with -DCOSMIC_CONSOLE=ON/OFF.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(COSMIC_CONSOLE_DEFAULT ON)
    else()
        set(COSMIC_CONSOLE_DEFAULT OFF)
    endif()
    option(COSMIC_CONSOLE "Keep the console window for log output" ${COSMIC_CONSOLE_DEFAULT})
    if(NOT COSMIC_CONSOLE)
        set_target_properties(cosmicdesk PROPERTIES WIN32_EXECUTABLE TRUE)
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

# ---------------------------------------------------------------------------
# tools: Windows-only helpers. cosmicsvc (M7) is the LocalSystem service
# launcher that spawns cosmicdesk.exe as SYSTEM in the active console session.
# ---------------------------------------------------------------------------
if(WIN32)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Linux packaging (plan M6.3). Install rules are UNIX-only: on Windows the
# bundle is produced by packaging/windows/make-zip.ps1 instead. Everything
# lives in one self-contained dir next to the binary
# (${CMAKE_INSTALL_LIBDIR}/cosmicdesk), mirroring the Windows bundle; the menu
# entry and udev rule go to their standard share/ locations. The udev rule is
# installed into share/cosmicdesk but NOT activated - users copy it to
# /etc/udev/rules.d/ themselves (see docs/BUILDING.md).
# packaging/linux/make-tarball.sh wraps these rules into a self-contained
# tarball.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    # The .desktop Exec must point at the real install prefix (default
    # /usr/local, not /usr), so it is generated from a template at configure
    # time. GNUInstallDirs defines CMAKE_INSTALL_FULL_LIBDIR.
    configure_file(packaging/linux/cosmicdesk.desktop.in
                   ${CMAKE_BINARY_DIR}/packaging/linux/cosmicdesk.desktop @ONLY)
    install(TARGETS cosmicdesk RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/cosmicdesk)
    install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_LIBDIR}/cosmicdesk/assets)
    install(FILES ${CMAKE_BINARY_DIR}/packaging/linux/cosmicdesk.desktop DESTINATION share/applications)
    install(FILES packaging/linux/60-cosmicdesk-input.rules DESTINATION share/cosmicdesk)
    install(FILES assets/icon.png DESTINATION share/icons/hicolor/64x64/apps RENAME cosmicdesk.png)
endif()
