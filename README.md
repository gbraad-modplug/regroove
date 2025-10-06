MP1210: Regroover
=================


```sh
gcc main.c regroove.c -o modplayer \
    $(pkg-config --cflags --libs sdl2 libopenmpt)
```