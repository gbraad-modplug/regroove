#!/bin/bash
# Package regroove for Windows distribution

set -e

VERSION="1.0.0"
BUILD_DIR="build-windows"
PACKAGE_NAME="samplecrate-${VERSION}-windows-x64"
PACKAGE_DIR="$PACKAGE_NAME"
MINGW_BIN="/usr/x86_64-w64-mingw32/sys-root/mingw/bin"

echo "=== Packaging regroove for Windows ==="

# Check if build exists
if [ ! -f "$BUILD_DIR/regroove-gui.exe" ]; then
    echo "Error: regroove-gui.exe not found. Run ./build-windows.sh first."
    exit 1
fi

# Create package directory
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR"

# Copy executable
echo "Copying executable..."
cp "$BUILD_DIR/regroove-gui.exe" "$PACKAGE_DIR/"

# Copy required DLLs
echo "Copying DLLs..."
cp "$MINGW_BIN/SDL2.dll" "$PACKAGE_DIR/"
cp "$MINGW_BIN/libwinpthread-1.dll" "$PACKAGE_DIR/"
cp "$MINGW_BIN/zlib1.dll" "$PACKAGE_DIR/"


# Copy configuration files if they exist
echo "Copying configuration files..."
[ -f "regroove.ini" ] && cp regroove.ini "$PACKAGE_DIR/"

# Copy example files if they exist
echo "Copying examples..."
if [ -d "examples" ]; then
    cp -r examples "$PACKAGE_DIR/"
fi

# Create README
echo "Creating README..."
cat > "$PACKAGE_DIR/README.txt" <<EOF
Regroove for Windows v${VERSION}
=====================================

A groovebox for module files
EOF

# Create archive
echo ""
echo "Creating ZIP archive..."
zip -r "${PACKAGE_NAME}.zip" "$PACKAGE_DIR/"

echo ""
echo "=========================================="
echo "Package created successfully!"
echo "=========================================="
echo ""
echo "Distribution directory: $PACKAGE_DIR/"
echo "ZIP archive: ${PACKAGE_NAME}.zip"
echo ""
echo "Contents:"
ls -lh "$PACKAGE_DIR"
echo ""
echo "To test on Windows:"
echo "  1. Extract ${PACKAGE_NAME}.zip"
echo "  2. Run regroove.exe"
echo ""
echo "To test with Wine:"
echo "  cd $PACKAGE_DIR && wine regroove.exe"
echo ""
