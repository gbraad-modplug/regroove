# Building Regroove for Windows (Cross-compilation from Fedora Linux)


This guide explains how to cross-compile regroove for Windows using MinGW on Fedora Linux.

## Prerequisites

### Install MinGW cross-compilation toolchain and dependencies

```bash
sudo dnf install -y \
    mingw64-gcc \
    mingw64-gcc-c++ \
    mingw64-gcc-cpp \
    mingw64-SDL2 \
    mingw64-SDL2-static \
    mingw64-zlib \
    mingw64-zlib-static \
    mingw64-libogg \
    cmake \
    wget \
    zip
```

## Build Process

### 1. Build libopenmpt with interactive extensions

libopenmpt needs to be built from source with the `LIBOPENMPT_EXT_C_INTERFACE` flag enabled for interactive volume/panning control:

```bash
./build-libopenmpt-mingw.sh
```

This will:
- Download libopenmpt 0.7.11
- Configure with interactive extensions enabled
- Cross-compile for Windows using MinGW
- Install to `/usr/x86_64-w64-mingw32/`

### 2. Build RtMidi with WinMM support

RtMidi needs to be built with Windows Multimedia (WinMM) API support:

```bash
./build-rtmidi-mingw.sh
```

This will:
- Download RtMidi 6.0.0
- Configure with WinMM API enabled (Windows MIDI)
- Cross-compile for Windows using MinGW
- Install to `/usr/x86_64-w64-mingw32/`

### 3. Build regroove

The main build script orchestrates everything:

```bash
./build-regroove-mingw.sh
```

This will:
- Build libopenmpt (if needed)
- Build RtMidi (if needed)
- Configure regroove with the correct MinGW toolchain
- Build regroove-gui.exe

### 4. Package for distribution

To create a distributable package with all DLLs:

```bash
./package-windows.sh
```
