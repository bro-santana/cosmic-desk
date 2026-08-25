# ---------------------------------------------------------------------------
# Cosmic Desk — vendored stripped Sunshine host, built as a static library.
#
# COSMIC MODIFICATION: this file is new. Upstream Sunshine builds an executable
# with its own CMake machinery (LizardByte prebuilt FFmpeg, CPM/FetchContent,
# web UI, tray, ViGEm, etc.). Cosmic Desk builds the vendored host core as the
# static library `cosmic_host` against SYSTEM packages only (plan D4/R3):
#   - no downloads, no CPM, no LizardByte prebuilt FFmpeg
#   - system FFmpeg/Boost/OpenSSL/Opus/curl/expat/nlohmann-json/MinHook
#   - vendored third-party: Simple-Web-Server, libdisplaydevice, nv-codec-headers,
#     nvapi (headers), moonlight-common-c submodule (enet + bundled nanors)
#   - glad (Linux only, needs Python+jinja2 at build time)
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

project(cosmic_host LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COSMIC_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third-party")

# ---------------------------------------------------------------------------
# Options mirroring upstream's cmake/compile_definitions/common.cmake
# ---------------------------------------------------------------------------
if(NOT SUNSHINE_ASSETS_DIR)
    set(SUNSHINE_ASSETS_DIR "assets")
endif()
set(SUNSHINE_TRAY 0)  # Cosmic Desk has its own tray (M0)
set(SUNSHINE_PUBLISHER_NAME "Cosmic Desk")
set(SUNSHINE_PUBLISHER_WEBSITE "https://github.com/cosmic-desk")
set(SUNSHINE_PUBLISHER_ISSUE_URL "https://github.com/cosmic-desk/cosmic-desk/issues")

# ---------------------------------------------------------------------------
# Dependencies (system packages only)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

# Boost: filesystem/log/program_options/system for Sunshine; algorithm/preprocessor/
# scope/uuid for libdisplaydevice on Windows (mirrors upstream Boost_Sunshine.cmake).
set(BOOST_COMPONENTS filesystem log program_options system)
if(WIN32)
    list(APPEND BOOST_COMPONENTS algorithm preprocessor scope uuid)
elseif(UNIX AND NOT APPLE)
    # platform/linux/input/inputtino_keyboard.cpp uses boost::locale::conv
    list(APPEND BOOST_COMPONENTS locale)
endif()
find_package(Boost CONFIG REQUIRED COMPONENTS ${BOOST_COMPONENTS})

pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(OPUS REQUIRED opus)
# FFmpeg: use the --static closure so the final link resolves libx264/libx265/etc.
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale libavfilter)
# MSYS2/MinGW has no libdl (dlopen lives in the CRT); FFmpeg's --static closure
# carries Unix-only link flags that break the final link on Windows.
if(WIN32)
    list(REMOVE_ITEM FFMPEG_STATIC_LIBRARIES "dl")
endif()

# moonlight-common-c submodule: enet static lib (unmodified)
set(ENET_NO_INSTALL ON CACHE BOOL "Don't install any libraries built for enet")
add_subdirectory("${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/enet" moonlight-common-c/enet)

# Simple-Web-Server (vendored header-only, nvhttp dependency)
add_subdirectory("${COSMIC_THIRD_PARTY_DIR}/Simple-Web-Server" Simple-Web-Server)

# libdisplaydevice (vendored; its own CMakeLists already disables docs/tests when
# added as a subproject, and points at system Boost/nlohmann-json)
add_subdirectory("${COSMIC_THIRD_PARTY_DIR}/libdisplaydevice" libdisplaydevice)

if(WIN32)
    # MinHook (system package)
    find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED)
    find_path(MINHOOK_INCLUDE_DIR MinHook.h PATH_SUFFIXES include REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(COSMIC_HOST_SOURCES
    src/audio.cpp
    src/config.cpp
    src/crypto.cpp
    src/display_device.cpp
    src/file_handler.cpp
    src/globals.cpp
    src/httpcommon.cpp
    src/input.cpp
    src/logging.cpp
    src/network.cpp
    src/nvhttp.cpp
    src/process.cpp
    src/rswrapper.c
    src/rtsp.cpp
    src/stat_trackers.cpp
    src/stream.cpp
    src/video.cpp
    src/video_colorspace.cpp
)
file(GLOB COSMIC_HOST_CORE_HEADERS CONFIGURE_DEPENDS src/*.h)
file(GLOB COSMIC_HOST_NVENC_SOURCES CONFIGURE_DEPENDS src/nvenc/*.cpp)
list(APPEND COSMIC_HOST_SOURCES ${COSMIC_HOST_CORE_HEADERS} ${COSMIC_HOST_NVENC_SOURCES})

if(WIN32)
    list(APPEND COSMIC_HOST_SOURCES
        src/platform/windows/publish.cpp
        src/platform/windows/misc.cpp
        src/platform/windows/input.cpp
        src/platform/windows/display_base.cpp
        src/platform/windows/display_vram.cpp
        src/platform/windows/display_ram.cpp
        src/platform/windows/audio.cpp
        src/platform/windows/utf_utils.cpp
    )
    file(GLOB COSMIC_HOST_NVPREFS_SOURCES CONFIGURE_DEPENDS src/platform/windows/nvprefs/*.cpp)
    list(APPEND COSMIC_HOST_SOURCES ${COSMIC_HOST_NVPREFS_SOURCES})
elseif(UNIX AND NOT APPLE)
    # Mirrors upstream cmake/compile_definitions/linux.cmake file list (X11-only v1).
    list(APPEND COSMIC_HOST_SOURCES
        src/platform/linux/audio.cpp
        src/platform/linux/graphics.cpp
        src/platform/linux/misc.cpp
        src/platform/linux/publish.cpp
        src/platform/linux/vaapi.cpp
        src/platform/linux/x11grab.cpp
    )
    file(GLOB COSMIC_HOST_INPUTTINO_SOURCES CONFIGURE_DEPENDS src/platform/linux/input/inputtino*.cpp)
    list(APPEND COSMIC_HOST_SOURCES ${COSMIC_HOST_INPUTTINO_SOURCES})
endif()

# nanors (Reed-Solomon FEC) bundled in the moonlight-common-c submodule.
# COSMIC MODIFICATION: upstream compiles the standalone third-party/nanors copy;
# we use the submodule's bundled nanors (rs.c + oblas_common.c + oblas_lite.c).
set(COSMIC_HOST_NANORS_SOURCES
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/nanors/rs.c"
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/nanors/deps/obl/oblas_common.c"
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/nanors/deps/obl/oblas_lite.c"
)

# moonlight-common-c RTSP parser (parseRtspMessage/createRtspResponse, used by rtsp.cpp)
set(COSMIC_HOST_MOONLIGHT_SOURCES
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/src/RtspParser.c"
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_library(cosmic_host STATIC ${COSMIC_HOST_SOURCES} ${COSMIC_HOST_NANORS_SOURCES} ${COSMIC_HOST_MOONLIGHT_SOURCES})

# The vendored Sunshine code requires C++23 (std::format, etc.).
target_compile_features(cosmic_host PUBLIC cxx_std_23)

set(COSMIC_HOST_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${COSMIC_THIRD_PARTY_DIR}"
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/enet/include"
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/nanors"
    "${COSMIC_THIRD_PARTY_DIR}/moonlight-common-c/nanors/deps/obl"
    "${COSMIC_THIRD_PARTY_DIR}/nv-codec-headers/include"
    ${OPENSSL_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OPUS_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)
list(FILTER COSMIC_HOST_INCLUDE_DIRS EXCLUDE REGEX "^$")
target_include_directories(cosmic_host SYSTEM PUBLIC ${COSMIC_HOST_INCLUDE_DIRS})

# COSMIC MODIFICATION: expose the top-level src/ dir so nvhttp.cpp can include
# "hostglue/pin_bridge.h" (the hook into our native PIN dialog, M1.4). Only the
# header is needed here; pin_bridge.cpp lives in the cosmicdesk target, so the
# notify_pair_request symbol resolves at the final link.
target_include_directories(cosmic_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

if(WIN32)
    # nvapi headers (vendored, MIT) — needed by the nvprefs sources.
    target_include_directories(cosmic_host SYSTEM PRIVATE "${COSMIC_THIRD_PARTY_DIR}/nvapi")
    target_include_directories(cosmic_host SYSTEM PRIVATE ${MINHOOK_INCLUDE_DIR})
endif()

# ---------------------------------------------------------------------------
# Compile definitions (mirrors upstream compile_definitions/*.cmake)
# ---------------------------------------------------------------------------
target_compile_definitions(cosmic_host PRIVATE
    SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR}"
    SUNSHINE_TRAY=${SUNSHINE_TRAY}
    SUNSHINE_PUBLISHER_NAME="${SUNSHINE_PUBLISHER_NAME}"
    SUNSHINE_PUBLISHER_WEBSITE="${SUNSHINE_PUBLISHER_WEBSITE}"
    SUNSHINE_PUBLISHER_ISSUE_URL="${SUNSHINE_PUBLISHER_ISSUE_URL}"
    CURL_STATICLIB
)
if(WIN32)
    # gcc bug 98723 is MinGW-specific; on Linux it would force Boost::regex in
    # Simple-Web-Server without the link dependency (upstream windows.cmake).
    target_compile_definitions(cosmic_host PRIVATE USE_BOOST_REGEX)
    target_compile_definitions(cosmic_host PRIVATE SUNSHINE_PLATFORM="windows")
else()
    target_compile_definitions(cosmic_host PRIVATE SUNSHINE_PLATFORM="linux")
endif()

# ---------------------------------------------------------------------------
# Compile options (mirrors upstream common.cmake; target-local only)
# ---------------------------------------------------------------------------
target_compile_options(cosmic_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wno-sign-compare>
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        target_compile_options(cosmic_host PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-maybe-uninitialized>)
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        target_compile_options(cosmic_host PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-uninitialized>)
    endif()
endif()
if(WIN32)
    # gcc complains about misleading indentation in some mingw includes (upstream)
    target_compile_options(cosmic_host PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-misleading-indentation>)
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(cosmic_host PUBLIC
    enet
    libdisplaydevice::display_device
    simple-web-server
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    Boost::filesystem
    Boost::log
    Boost::program_options
    Boost::system
    ${CURL_LIBRARIES}
    ${OPUS_LIBRARIES}
    ${FFMPEG_STATIC_LIBRARIES}
)

if(WIN32)
    # Mirrors upstream windows.cmake PLATFORM_LIBRARIES (minus ViGEm/tray/RC).
    # COSMIC MODIFICATION: shell32 added — platform/windows/misc.cpp appdata()
    # uses SHGetKnownFolderPath() for %APPDATA%\CosmicDesk (plan D6).
    target_link_libraries(cosmic_host PUBLIC
        ${MINHOOK_LIBRARY}
        avrt
        d3d11
        D3DCompiler
        dwmapi
        dxgi
        iphlpapi
        ksuser
        ntdll
        setupapi
        shell32
        shlwapi
        userenv
        version
        wtsapi32
        ws2_32
        wsock32
    )
elseif(UNIX AND NOT APPLE)
    # -----------------------------------------------------------------------
    # Linux-only dependencies (mirrors upstream linux.cmake; not verifiable here)
    # -----------------------------------------------------------------------
    find_package(X11 REQUIRED)
    target_compile_definitions(cosmic_host PRIVATE SUNSHINE_BUILD_X11)
    target_include_directories(cosmic_host SYSTEM PRIVATE ${X11_INCLUDE_DIR})

    pkg_check_modules(LIBVA REQUIRED libva libva-drm)
    target_compile_definitions(cosmic_host PRIVATE SUNSHINE_BUILD_VAAPI)
    target_include_directories(cosmic_host SYSTEM PRIVATE ${LIBVA_INCLUDE_DIRS})

    # libcap (platform/linux/graphics.cpp and misc.cpp call cap_get_proc/cap_set_proc)
    pkg_check_modules(LIBCAP REQUIRED libcap)

    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)

    # libevdev (inputtino dependency)
    pkg_check_modules(PC_EVDEV libevdev)
    if(PC_EVDEV_FOUND)
        find_path(EVDEV_INCLUDE_DIR libevdev/libevdev.h HINTS ${PC_EVDEV_INCLUDE_DIRS} ${PC_EVDEV_INCLUDEDIR})
        find_library(EVDEV_LIBRARY NAMES evdev libevdev)
        if(NOT EVDEV_INCLUDE_DIR OR NOT EVDEV_LIBRARY)
            message(FATAL_ERROR "libevdev headers/library not found")
        endif()
        target_include_directories(cosmic_host SYSTEM PRIVATE ${EVDEV_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "libevdev not found (pkg-config). Install libevdev-dev.")
    endif()

    # inputtino (Linux uinput injection, submodule)
    add_subdirectory("${COSMIC_THIRD_PARTY_DIR}/inputtino" inputtino)

    # glad (GL/EGL loader for VAAPI capture). Its generator needs Python >= 3.8
    # with jinja2 at build time (mirrors upstream dependencies/glad.cmake).
    find_package(Python COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -c "import jinja2; import pkg_resources"
        RESULT_VARIABLE _glad_deps_import_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT _glad_deps_import_result EQUAL 0)
        message(FATAL_ERROR
            "glad generator requires Python with jinja2 and setuptools (pkg_resources). "
            "Install them, e.g. 'pip install jinja2 setuptools', and re-run cmake.")
    endif()
    set(PYTHON_EXECUTABLE "${Python_EXECUTABLE}" CACHE FILEPATH "Python interpreter for glad" FORCE)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()
    add_subdirectory("${COSMIC_THIRD_PARTY_DIR}/glad/cmake" glad2-cmake)
    unset(CMAKE_POLICY_VERSION_MINIMUM)

    target_link_libraries(cosmic_host PUBLIC
        ${X11_LIBRARIES}
        ${LIBVA_LIBRARIES}
        ${LIBVA_DRM_LIBRARIES}
        Boost::locale
        PkgConfig::GIO
        PkgConfig::PULSE
        ${EVDEV_LIBRARY}
        inputtino::libinputtino
        glad
        ${LIBCAP_LIBRARIES}
        dl
    )
endif()