# Regroove Android App

This directory contains the Android Studio project for the Regroove tracker player.

## Architecture

The app uses SDL2 for cross-platform graphics and input handling:

- **Native code**: The main C++ code (`main-gui.cpp`) is compiled as `libregroove.so`
- **SDL2**: Handles OpenGL ES 2.0 context, input events, and audio
- **Java wrapper**: `RegrooveActivity` extends `SDLActivity` to launch the native code

## Directory Structure

```
android/
├── app/
│   ├── src/main/
│   │   ├── java/
│   │   │   ├── com/regroove/player/
│   │   │   │   └── RegrooveActivity.java      # Main activity
│   │   │   └── org/libsdl/app/                # SDL2 Java classes (copied from SDL source)
│   │   ├── jniLibs/                           # Native libraries (*.so files)
│   │   │   ├── arm64-v8a/
│   │   │   └── armeabi-v7a/
│   │   ├── res/                               # Android resources
│   │   └── AndroidManifest.xml
│   └── build.gradle                           # App-level build configuration
├── build.gradle                               # Project-level build configuration
├── settings.gradle                            # Gradle settings
└── gradle.properties                          # Gradle properties
```

## Building

### 1. Build Native Libraries

From the project root, run the Android build script:

```bash
cd /home/gbraad/Projects/regroove
./build-android.sh
```

This script will:
- Build SDL2 from source for Android
- Build libopenmpt and rtmidi
- Build regroove native library
- Copy all .so files to `android/app/src/main/jniLibs/{ABI}/`
- Copy SDL2 Java files to `android/app/src/main/java/org/libsdl/app/`

### 2. Build Android APK

Open the `android/` directory in Android Studio:

```bash
cd android
android-studio .
```

Or build from command line using Gradle:

```bash
cd android
./gradlew assembleDebug
```

The APK will be generated at: `app/build/outputs/apk/debug/app-debug.apk`

## SDL2 Java Files

The app requires SDL2 Java classes (SDLActivity.java, etc.) to be present in:
```
android/app/src/main/java/org/libsdl/app/
```

The build script attempts to copy these automatically. If they're missing, manually copy from:
```bash
cp -r build-sdl2-android-{ABI}/SDL2-{version}/android-project/app/src/main/java/org/libsdl/app/*.java \
  android/app/src/main/java/org/libsdl/app/
```

## Library Loading Order

The `RegrooveActivity.getLibraries()` method specifies the load order:

1. **SDL2** - Must be loaded first
2. **openmpt** - Module playback library
3. **rtmidi** - MIDI library
4. **regroove** - Main application (contains SDL_main)

## Entry Point

The native entry point is `int main(int argc, char* argv[])` in `main-gui.cpp`.

On Android, SDL_main.h automatically remaps this to `SDL_main` via preprocessor macros. SDL's Java code calls this function after setting up the OpenGL ES 2.0 context.

## Requirements

- Android NDK 25+
- Android SDK with API level 23+ (Android 6.0+)
- OpenGL ES 2.0 capable device

## Notes

- The app runs in landscape orientation only
- OpenGL ES 2.0 is explicitly requested (not OpenGL 2.1)
- ImGui uses the OpenGL3 backend with GLSL ES 1.0 shaders (`#version 100`)
