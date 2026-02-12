#!/bin/bash
#
# Downloads pre-built GTK3 Windows libraries from the MSYS2 repository.
# Extracts them into deps/mingw64/ for cross-compilation with mingw-w64.
#
# Usage: ./scripts/setup-mingw-gtk.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPS_DIR="$PROJECT_DIR/deps"
PREFIX="$DEPS_DIR/mingw64"
CACHE_DIR="$DEPS_DIR/cache"

MIRROR="https://mirror.msys2.org/mingw/mingw64"

# Package list: name=version
# Update versions by checking https://packages.msys2.org/
PACKAGES=(
    "mingw-w64-x86_64-gtk3=3.24.51-2"
    "mingw-w64-x86_64-glib2=2.86.3-1"
    "mingw-w64-x86_64-cairo=1.18.4-4"
    "mingw-w64-x86_64-pango=1.56.4-3"
    "mingw-w64-x86_64-gdk-pixbuf2=2.44.5-1"
    "mingw-w64-x86_64-atk=2.58.3-1"
    "mingw-w64-x86_64-harfbuzz=12.3.2-1"
    "mingw-w64-x86_64-fribidi=1.0.16-1"
    "mingw-w64-x86_64-pixman=0.46.4-1"
    "mingw-w64-x86_64-libepoxy=1.5.10-7"
    "mingw-w64-x86_64-gettext-runtime=1.0-1"
    "mingw-w64-x86_64-fontconfig=2.17.1-1"
    "mingw-w64-x86_64-freetype=2.14.1-2"
    "mingw-w64-x86_64-libpng=1.6.55-1"
    "mingw-w64-x86_64-libjpeg-turbo=3.1.3-1"
    "mingw-w64-x86_64-libtiff=4.7.1-1"
    "mingw-w64-x86_64-expat=2.7.4-1"
    "mingw-w64-x86_64-pcre2=10.47-1"
    "mingw-w64-x86_64-graphite2=1.3.14-3"
    "mingw-w64-x86_64-bzip2=1.0.8-3"
    "mingw-w64-x86_64-brotli=1.2.0-1"
    "mingw-w64-x86_64-zlib=1.3.1-1"
)

# Check for required tools
for tool in curl zstd tar; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Error: '$tool' is required but not found. Install it with:"
        echo "  sudo apt install $tool"
        exit 1
    fi
done

# Skip if already set up
if [ -f "$PREFIX/.setup-done" ]; then
    echo "GTK3 Windows libraries already set up in $PREFIX"
    echo "To re-download, remove $PREFIX and run again."
    exit 0
fi

mkdir -p "$PREFIX" "$CACHE_DIR"

download_and_extract() {
    local name="$1"
    local version="$2"
    local filename="${name}-${version}-any.pkg.tar.zst"
    local url="${MIRROR}/${filename}"
    local cached="$CACHE_DIR/$filename"

    if [ ! -f "$cached" ]; then
        echo "  Downloading $filename ..."
        if ! curl -fSL -o "$cached.tmp" "$url"; then
            echo "  ERROR: Failed to download $url"
            rm -f "$cached.tmp"
            return 1
        fi
        mv "$cached.tmp" "$cached"
    else
        echo "  Using cached $filename"
    fi

    # Extract: MSYS2 packages contain a mingw64/ tree
    echo "  Extracting ..."
    zstd -d "$cached" --stdout | tar -xf - -C "$DEPS_DIR" 2>/dev/null || true
}

echo "=== Setting up GTK3 Windows libraries for mingw-w64 cross-compilation ==="
echo "Target: $PREFIX"
echo ""

for entry in "${PACKAGES[@]}"; do
    name="${entry%%=*}"
    version="${entry##*=}"
    echo "[$name $version]"
    download_and_extract "$name" "$version"
    echo ""
done

# Mark setup as complete
touch "$PREFIX/.setup-done"

echo "=== Done ==="
echo "GTK3 Windows libraries installed to: $PREFIX"
echo ""
echo "To cross-compile:"
echo "  cmake -B build-win --toolchain cmake/mingw-w64-x86_64.cmake"
echo "  cmake --build build-win"
