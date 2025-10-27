#!/bin/bash
# Script to cross-compile regroove for Android

set -e

# Configuration
ANDROID_NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.2.9519653}"
ANDROID_ABI="arm64-v8a"  # Options: armeabi-v7a, arm64-v8a, x86, x86_64
ANDROID_API_LEVEL=23     # Minimum for MIDI API support
BUILD_DIR="build-android-${ANDROID_ABI}"
INSTALL_PREFIX="$(pwd)/android-libs/${ANDROID_ABI}"

# Check if Android NDK is available
if [ ! -d "$ANDROID_NDK" ]; then
    echo "Error: Android NDK not found at: $ANDROID_NDK"
    echo "Please set ANDROID_NDK_HOME or install Android NDK"
    echo ""
    echo "Download from: https://developer.android.com/ndk/downloads"
    echo "Or install via Android Studio SDK Manager"
    exit 1
fi

echo "Using Android NDK: $ANDROID_NDK"
echo "Target ABI: $ANDROID_ABI"
echo "API Level: $ANDROID_API_LEVEL"
echo ""

# Create install prefix directory
mkdir -p "$INSTALL_PREFIX"

# Build libopenmpt for Android
echo "=== Building libopenmpt for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/libopenmpt.a" ]; then
    OPENMPT_BUILD="build-libopenmpt-android-${ANDROID_ABI}"
    mkdir -p "$OPENMPT_BUILD"
    cd "$OPENMPT_BUILD"

    # Download libopenmpt if needed
    LIBOPENMPT_VERSION="0.7.11"
    if [ ! -f "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz" ]; then
        wget "https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz"
        tar xzf "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools.tar.gz"
    fi

    cd "libopenmpt-${LIBOPENMPT_VERSION}+release.autotools"

    # Set up NDK toolchain
    export TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
    export TARGET=""
    case "$ANDROID_ABI" in
        armeabi-v7a) TARGET="armv7a-linux-androideabi" ;;
        arm64-v8a)   TARGET="aarch64-linux-android" ;;
        x86)         TARGET="i686-linux-android" ;;
        x86_64)      TARGET="x86_64-linux-android" ;;
    esac

    export AR="$TOOLCHAIN/bin/llvm-ar"
    export CC="$TOOLCHAIN/bin/${TARGET}${ANDROID_API_LEVEL}-clang"
    export CXX="$TOOLCHAIN/bin/${TARGET}${ANDROID_API_LEVEL}-clang++"
    export LD="$TOOLCHAIN/bin/ld"
    export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
    export STRIP="$TOOLCHAIN/bin/llvm-strip"

    ./configure \
        --host="$TARGET" \
        --prefix="$INSTALL_PREFIX" \
        --enable-static \
        --disable-shared \
        --disable-openmpt123 \
        --disable-examples \
        --disable-tests \
        CFLAGS="-O2 -DLIBOPENMPT_EXT_C_INTERFACE" \
        CXXFLAGS="-O2 -DLIBOPENMPT_EXT_C_INTERFACE"

    make -j$(nproc)
    make install
    cd ../..
else
    echo "libopenmpt already built (skipping)"
fi

# Build RtMidi for Android
echo ""
echo "=== Building RtMidi for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/librtmidi.a" ]; then
    RTMIDI_BUILD="build-rtmidi-android-${ANDROID_ABI}"
    mkdir -p "$RTMIDI_BUILD"
    cd "$RTMIDI_BUILD"

    # Download RtMidi if needed
    RTMIDI_VERSION="6.0.0"
    if [ ! -f "rtmidi-${RTMIDI_VERSION}.tar.gz" ]; then
        wget "https://github.com/thestk/rtmidi/archive/refs/tags/${RTMIDI_VERSION}.tar.gz" -O "rtmidi-${RTMIDI_VERSION}.tar.gz"
        tar xzf "rtmidi-${RTMIDI_VERSION}.tar.gz"
    fi

    cd "rtmidi-${RTMIDI_VERSION}"
    mkdir -p build-android
    cd build-android

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DRTMIDI_API_ANDROID=ON \
        -DRTMIDI_BUILD_TESTING=OFF

    make -j$(nproc)
    make install
    cd ../../..
else
    echo "RtMidi already built (skipping)"
fi

# Build SDL2 for Android
echo ""
echo "=== Building SDL2 for Android ==="
if [ ! -f "$INSTALL_PREFIX/lib/libSDL2.so" ]; then
    SDL2_BUILD="build-sdl2-android-${ANDROID_ABI}"
    mkdir -p "$SDL2_BUILD"
    cd "$SDL2_BUILD"

    # Download SDL2 if needed
    SDL2_VERSION="2.30.0"
    if [ ! -f "SDL2-${SDL2_VERSION}.tar.gz" ]; then
        wget "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
        tar xzf "SDL2-${SDL2_VERSION}.tar.gz"
    fi

    cd "SDL2-${SDL2_VERSION}"
    mkdir -p build-android
    cd build-android

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON

    make -j$(nproc)
    make install
    cd ../../..
else
    echo "SDL2 already built (skipping)"
fi

# Build regroove for Android
echo ""
echo "=== Building regroove for Android ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-${ANDROID_API_LEVEL}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DANDROID_BUILD=ON \
    -DBUILD_SHARED_LIBS=ON

make -j$(nproc)
make install

cd ..

echo ""
echo "=== Android Build Complete ==="
echo "Library: $INSTALL_PREFIX/lib/libregroove.so"
echo "ABI: $ANDROID_ABI"
echo ""
echo "To use in Android Studio:"
echo "1. Copy $INSTALL_PREFIX/lib/*.so to app/src/main/jniLibs/$ANDROID_ABI/"
echo "2. Copy $INSTALL_PREFIX/include/ headers for JNI wrapper"
