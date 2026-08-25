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
# cosmicdesk: the single unified binary (host role + viewer role + UI).
# M0 scope: SDL window, ImGui, tray icon, settings file.
# ---------------------------------------------------------------------------
add_executable(cosmicdesk
    src/main.cpp
    src/app/settings.cpp
    src/ui/tray.cpp
)

target_include_directories(cosmicdesk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/tray
)

if(WIN32)
    # SDL2main is a static library that calls into SDL2, so it has to be listed
    # before SDL2 on the link line.
    target_link_libraries(cosmicdesk PRIVATE SDL2::SDL2main)
endif()

target_link_libraries(cosmicdesk PRIVATE
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
