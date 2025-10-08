MP-1210: Direct Interaction Groove Interface
============================================


![](https://avatars.githubusercontent.com/u/236030309?s=400&u=f3b55edae873527de225c234bb4a6680d7e46254&v=4)

### Console version with MIDI

```sh
gcc -o modplayer main.c regroove.c \
    $(pkg-config --cflags --libs sdl2 libopenmpt)
```

### GUI version

```sh
g++ -o modplayer main-gui.cpp \
    regroove.c midi.c \
    imgui/*.cpp imgui/backends/imgui_impl_sdl2.cpp imgui/backends/imgui_impl_opengl2.cpp \
    -I. -Iimgui -Iimgui/backends \
    -lGL -ldl -lpthread \
    $(pkg-config --cflags --libs sdl2 libopenmpt rtmidi)
```

