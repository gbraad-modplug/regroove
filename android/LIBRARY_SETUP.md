# Regroove Android Library Setup

## Library Loading Configuration

### Shared Libraries (.so files)
Only these libraries are loaded as separate shared objects:

1. **libSDL2.so** - Built with `-DBUILD_SHARED_LIBS=ON`
   - Provides OpenGL ES context, input handling, audio
   - Must be loaded first

2. **libregroove.so** - Built with `-DBUILD_SHARED_LIBS=ON`
   - Main application code (main-gui.cpp)
   - Contains SDL_main entry point

### Statically Linked Libraries
These are compiled directly into libregroove.so:

1. **libopenmpt** - Built with `--enable-static --disable-shared`
   - Module playback engine
   - No separate .so file needed

2. **librtmidi** - Built with `-DBUILD_SHARED_LIBS=OFF`
   - MIDI handling (including Android USB MIDI)
   - No separate .so file needed

## Java Activity Configuration

### getLibraries() Method
```java
@Override
protected String[] getLibraries() {
    return new String[] {
        "SDL2",      // SDL2 shared library
        "regroove"   // Main app (contains statically linked openmpt + rtmidi)
    };
}
```

**Important:** Do NOT include "openmpt" or "rtmidi" in this array. They are statically linked into libregroove.so.

## USB MIDI Support

### RegrooveActivity Handles:
- **USB device attachment detection** - Listens for USB_DEVICE_ATTACHED broadcasts
- **Permission requests** - Prompts user for USB access when MIDI device is plugged in
- **Device filtering** - Only requests permissions for USB Class 1 Subclass 3 (MIDI) devices
- **Already-connected devices** - Checks for MIDI devices on app startup

### How it Works:
1. Activity registers BroadcastReceiver for USB events
2. When MIDI device is attached, activity requests permission
3. Once permission granted, RtMidi (in native code) can access the device
4. RtMidi handles actual MIDI communication

### Required Permissions in AndroidManifest.xml:
```xml
<uses-feature android:name="android.software.midi" android:required="false" />
```

## Build Process

### build-android.sh now:
1. Builds SDL2 as shared library
2. Builds openmpt as static library
3. Builds rtmidi as static library
4. Builds regroove as shared library (links openmpt + rtmidi statically)
5. Copies libSDL2.so and libregroove.so to jniLibs/{ABI}/
6. Copies SDL2 Java files to org/libsdl/app/

### Files Copied to jniLibs:
```
android/app/src/main/jniLibs/
├── arm64-v8a/
│   ├── libSDL2.so
│   └── libregroove.so
└── armeabi-v7a/
    ├── libSDL2.so
    └── libregroove.so
```

**Note:** libopenmpt.a and librtmidi.a are NOT copied because they're statically linked.

## Package Name
- Package: `nl.gbraad.regroove.app`
- Application ID: `nl.gbraad.regroove.app`
