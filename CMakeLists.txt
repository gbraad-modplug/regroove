cmake_minimum_required(VERSION 3.15)
project(regroove VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(OPENMPT REQUIRED libopenmpt)
pkg_check_modules(RTMIDI REQUIRED rtmidi)

# Console version with MIDI
add_executable(regroove-tui
    main-tui.c
    regroove.c
    regroove_common.c
    midi.c
    input_mappings.c
)

target_include_directories(regroove-tui PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${OPENMPT_INCLUDE_DIRS}
    ${RTMIDI_INCLUDE_DIRS}
)

target_link_libraries(regroove-tui PRIVATE
    ${SDL2_LIBRARIES}
    ${OPENMPT_LIBRARIES}
    ${RTMIDI_LIBRARIES}
)

target_compile_options(regroove-tui PRIVATE
    ${SDL2_CFLAGS_OTHER}
    ${OPENMPT_CFLAGS_OTHER}
    ${RTMIDI_CFLAGS_OTHER}
)

# GUI version
add_executable(regroove-gui
    main-gui.cpp
    regroove.c
    regroove_common.c
    midi.c
    input_mappings.c
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl2.cpp
    imgui/backends/imgui_impl_opengl2.cpp
)

target_include_directories(regroove-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    imgui
    imgui/backends
    ${SDL2_INCLUDE_DIRS}
    ${OPENMPT_INCLUDE_DIRS}
    ${RTMIDI_INCLUDE_DIRS}
)

target_link_libraries(regroove-gui PRIVATE
    ${SDL2_LIBRARIES}
    ${OPENMPT_LIBRARIES}
    ${RTMIDI_LIBRARIES}
    GL
    dl
    pthread
)

target_compile_options(regroove-gui PRIVATE
    ${SDL2_CFLAGS_OTHER}
    ${OPENMPT_CFLAGS_OTHER}
    ${RTMIDI_CFLAGS_OTHER}
)

# Installation rules
install(TARGETS regroove-tui regroove-gui
    RUNTIME DESTINATION bin
)
