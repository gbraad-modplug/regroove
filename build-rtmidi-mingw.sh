#!/bin/bash
# Script to cross-compile RtMidi for Windows

set -e

# Configuration
RTMIDI_VERSION="6.0.0"
RTMIDI_URL="https://github.com/thestk/rtmidi/archive/refs/tags/${RTMIDI_VERSION}.tar.gz"
BUILD_DIR="build-rtmidi-mingw"
INSTALL_PREFIX="/usr/x86_64-w64-mingw32"

# Check if MinGW is installed
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "Error: MinGW-w64 not found. Install with:"
    echo "  sudo dnf install mingw64-gcc mingw64-gcc-c++"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download RtMidi if not already downloaded
if [ ! -f "rtmidi-${RTMIDI_VERSION}.tar.gz" ]; then
    echo "Downloading RtMidi ${RTMIDI_VERSION}..."
    wget "$RTMIDI_URL" -O "rtmidi-${RTMIDI_VERSION}.tar.gz"
fi

# Extract
echo "Extracting..."
tar xzf "rtmidi-${RTMIDI_VERSION}.tar.gz"
cd "rtmidi-${RTMIDI_VERSION}"

# Create build directory
mkdir -p build-mingw
cd build-mingw

# Configure with CMake for Windows cross-compilation
echo "Configuring RtMidi for Windows..."
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../../../toolchain-mingw64.cmake \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DRTMIDI_API_WINMM=ON \
    -DRTMIDI_API_JACK=OFF \
    -DRTMIDI_BUILD_TESTING=OFF

# Build
echo "Building..."
make -j$(nproc)

# Install
echo "Installing to $INSTALL_PREFIX..."
sudo make install

echo ""
echo "RtMidi built successfully for Windows!"
echo "Installed to: $INSTALL_PREFIX"
echo ""
