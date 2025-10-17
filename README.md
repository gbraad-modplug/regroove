MP-1210: Direct Interaction Groove Interface
============================================


![](https://avatars.githubusercontent.com/u/236030309?s=400&u=f3b55edae873527de225c234bb4a6680d7e46254&v=4)

## Building

### Using CMake (recommended)

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

This will build both `regroove-tui` (console version) and `regroove-gui` (GUI version).

To build a specific target:
```sh
cmake --build . --target regroove-tui
cmake --build . --target regroove-gui
```

### Manual compilation

#### Console version with MIDI

```sh
gcc -o regroove-tui main-tui.c regroove.c midi.c \
    $(pkg-config --cflags --libs sdl2 libopenmpt rtmidi)
```

#### GUI version

```sh
g++ -o regroove-gui main-gui.cpp \
    regroove.c midi.c \
    imgui/*.cpp imgui/backends/imgui_impl_sdl2.cpp imgui/backends/imgui_impl_opengl2.cpp \
    -I. -Iimgui -Iimgui/backends \
    -lGL -ldl -lpthread \
    $(pkg-config --cflags --libs sdl2 libopenmpt rtmidi)
```

