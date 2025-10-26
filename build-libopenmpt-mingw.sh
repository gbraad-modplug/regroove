#!/bin/bash
# Script to cross-compile libopenmpt for Windows with interactive extensions

set -e

# Configuration
LIBOPENMPT_VERSION="0.7.11"
LIBOPENMPT_URL="https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz"
BUILD_DIR="build-libopenmpt-mingw"
INSTALL_PREFIX="/usr/x86_64-w64-mingw32"

# Check if MinGW is installed
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "Error: MinGW-w64 not found. Install with:"
    echo "  sudo apt-get install mingw-w64"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download libopenmpt if not already downloaded
if [ ! -f "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz" ]; then
    echo "Downloading libopenmpt ${LIBOPENMPT_VERSION}..."
    wget "$LIBOPENMPT_URL"
fi

# Extract
echo "Extracting..."
tar xzf "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz"
cd "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools"

# Configure for Windows cross-compilation with interactive extensions
echo "Configuring libopenmpt for Windows (with interactive extensions)..."
./configure \
    --host=x86_64-w64-mingw32 \
    --prefix="$INSTALL_PREFIX" \
    --enable-static \
    --disable-shared \
    --disable-openmpt123 \
    --disable-examples \
    --disable-tests \
    --without-mpg123 \
    --without-vorbis \
    --without-vorbisfile \
    --without-portaudio \
    --without-portaudiocpp \
    --without-pulseaudio \
    --without-sndfile \
    --without-flac \
    CFLAGS="-O2 -DLIBOPENMPT_EXT_C_INTERFACE" \
    CXXFLAGS="-O2 -DLIBOPENMPT_EXT_C_INTERFACE"

# Build
echo "Building..."
make -j$(nproc)

# Install
echo "Installing to $INSTALL_PREFIX..."
sudo make install

echo ""
echo "libopenmpt built successfully for Windows with interactive extensions!"
echo "Installed to: $INSTALL_PREFIX"
echo ""
echo "You can now build regroove with:"
echo "  mkdir build-windows"
echo "  cd build-windows"
echo "  cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake"
echo "  make"
