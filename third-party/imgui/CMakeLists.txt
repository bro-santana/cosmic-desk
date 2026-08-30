# Dear ImGui (vendored) + the SDL3 / SDL_Renderer backends.
# See docs/VENDOR.md for the upstream version this was taken from.
add_library(cosmic_imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_sdl3.cpp
    backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(cosmic_imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

target_link_libraries(cosmic_imgui PUBLIC SDL3::SDL3)
