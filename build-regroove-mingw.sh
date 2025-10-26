#!/bin/bash
# Complete build script for Windows cross-compilation

set -e

echo "=== Building Regroove for Windows ==="
echo ""

# Step 1: Build libopenmpt with interactive extensions
if [ ! -f "/usr/x86_64-w64-mingw32/lib/libopenmpt.a" ]; then
    echo "Step 1: Building libopenmpt for Windows..."
    ./build-libopenmpt-mingw.sh
else
    echo "Step 1: libopenmpt already built (skipping)"
fi

# Step 2: Download SDL2 for Windows (if not present)
SDL2_MINGW_DIR="SDL2-mingw"
if [ ! -d "$SDL2_MINGW_DIR" ]; then
    echo ""
    echo "Step 2: Downloading SDL2 development libraries for MinGW..."
    mkdir -p "$SDL2_MINGW_DIR"
    cd "$SDL2_MINGW_DIR"

    # Download SDL2 MinGW development libraries
    SDL2_VERSION="2.28.5"
    SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"

    wget "$SDL2_URL" -O SDL2-mingw.tar.gz
    tar xzf SDL2-mingw.tar.gz
    cd ..
    echo "SDL2 downloaded and extracted"
else
    echo "Step 2: SDL2 already downloaded (skipping)"
fi

# Step 3: Download/build RtMidi for Windows
# Step 3: Build RtMidi for Windows
if [ ! -f "/usr/x86_64-w64-mingw32/lib/librtmidi.a" ]; then
    echo ""
    echo "Step 3: Building RtMidi for Windows..."
    ./build-rtmidi-mingw.sh
else
    echo "Step 3: RtMidi already built (skipping)"
fi

# Step 4: Configure and build regroove
echo ""
echo "Step 4: Building regroove for Windows..."

mkdir -p build-windows
cd build-windows

# Set PKG_CONFIG_LIBDIR to use MinGW libraries only (not Linux system libraries)
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig:/usr/x86_64-w64-mingw32/sys-root/mingw/lib/pkgconfig

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)

echo ""
echo "=== Build complete! ==="
echo "Windows executable is in: build-windows/"
echo "  - regroove-gui.exe"
echo ""
echo "Note: This is a static build with all dependencies included."
echo "The TUI version (regroove-tui) is not available on Windows due to terminal API differences."
