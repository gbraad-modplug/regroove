cmake_minimum_required(VERSION 3.15)
project(regroove VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add /usr/local to search paths
set(CMAKE_PREFIX_PATH "/usr/local;${CMAKE_PREFIX_PATH}")
list(APPEND CMAKE_LIBRARY_PATH /usr/local/lib)
list(APPEND CMAKE_INCLUDE_PATH /usr/local/include)

# Set pkg-config to also search /usr/local
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(OPENMPT REQUIRED libopenmpt)
pkg_check_modules(RTMIDI REQUIRED rtmidi)

# Console version with MIDI (Unix only - uses termios)
if(NOT WIN32)
    add_executable(regroove-tui
        main-tui.c
        regroove_engine.c
        regroove_common.c
        regroove_metadata.c
        regroove_performance.c
        regroove_phrase.c
        regroove_effects.c
        midi.c
        midi_output.c
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
        m
    )

    target_compile_options(regroove-tui PRIVATE
        ${SDL2_CFLAGS_OTHER}
        ${OPENMPT_CFLAGS_OTHER}
        ${RTMIDI_CFLAGS_OTHER}
    )
endif()

# GUI version
add_executable(regroove-gui
    main-gui.cpp
    regroove_engine.c
    regroove_common.c
    regroove_metadata.c
    regroove_performance.c
    regroove_phrase.c
    regroove_effects.c
    audio_input.c
    midi.c
    midi_output.c
    input_mappings.c
    lcd.c
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
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(regroove-gui PRIVATE
        opengl32
        winmm
        imm32
        version
        setupapi
        z
    )
else()
    target_link_libraries(regroove-gui PRIVATE
        GL
        dl
        pthread
        m
    )
endif()

target_compile_options(regroove-gui PRIVATE
    ${SDL2_CFLAGS_OTHER}
    ${OPENMPT_CFLAGS_OTHER}
    ${RTMIDI_CFLAGS_OTHER}
)

# Installation rules
if(WIN32)
    install(TARGETS regroove-gui
        RUNTIME DESTINATION bin
    )
else()
    install(TARGETS regroove-tui regroove-gui
        RUNTIME DESTINATION bin
    )
endif()
