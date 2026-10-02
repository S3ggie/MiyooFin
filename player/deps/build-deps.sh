#!/bin/sh
# Builds LINK-ONLY copies of the libraries the device already ships, so
# miyoofin-player can be compiled against exactly the same ABI as Onion's
# ffplay (FFmpeg 2.4.14, SDL 1.2.15, SDL_ttf 2.0.11). None of these libraries
# is packaged: the device's own copies are loaded at runtime. Runs inside the
# miyoofin-toolchain container; output goes to $OUT (default output/player-deps).
set -eu
OUT=${OUT:-$(pwd)/output/player-deps}
SRC=${SRC:-$OUT/src}
CROSS=${CROSS:-arm-linux-gnueabihf}
JOBS=${JOBS:-$(nproc)}
mkdir -p "$SRC" "$OUT/prefix"
PREFIX=$OUT/prefix

fetch() { # url sha256 file
    if [ ! -f "$SRC/$3" ]; then
        wget -q -O "$SRC/$3.tmp" "$1" && mv -f "$SRC/$3.tmp" "$SRC/$3"
    fi
    echo "$2  $SRC/$3" | sha256sum -c - >/dev/null || { echo "build-deps: checksum mismatch: $3" >&2; rm -f "$SRC/$3"; exit 1; }
}

fetch https://ffmpeg.org/releases/ffmpeg-2.4.14.tar.xz \
    736567ee95115876d7af7f99f2fa565417c351b748bdc05973618980da1df037 ffmpeg-2.4.14.tar.xz
fetch https://www.libsdl.org/release/SDL-1.2.15.tar.gz \
    d6d316a793e5e348155f0dd93b979798933fb98aa1edebcc108829d6474aad00 SDL-1.2.15.tar.gz
fetch https://www.libsdl.org/projects/SDL_ttf/release/SDL_ttf-2.0.11.tar.gz \
    724cd895ecf4da319a3ef164892b72078bd92632a5d812111261cde248ebcdb7 SDL_ttf-2.0.11.tar.gz

fetch https://download.savannah.nongnu.org/releases/freetype/freetype-2.10.4.tar.xz \
    86a854d8905b19698bbc8f23b860bc104246ce4854dcea8e3b0fb21284f75784 freetype-2.10.4.tar.xz

if [ ! -f "$PREFIX/lib/libavcodec.so" ]; then
    rm -rf "$SRC/ffmpeg-2.4.14"; tar -C "$SRC" -xf "$SRC/ffmpeg-2.4.14.tar.xz"
    (cd "$SRC/ffmpeg-2.4.14" && ./configure --prefix="$PREFIX" --cross-prefix=$CROSS- \
        --enable-cross-compile --target-os=linux --arch=arm --enable-shared --disable-static \
        --enable-gpl --enable-pthreads --enable-avresample --enable-swscale --disable-yasm \
        --disable-programs --disable-doc --disable-encoders --disable-muxers >/dev/null \
      && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi
echo "build-deps: FFmpeg ok"

# Link-only SDL 1.2 (video/audio drivers off: the device's patched libSDL is
# loaded at runtime; only the public ABI matters here).
if [ ! -f "$PREFIX/lib/libSDL.so" ]; then
    rm -rf "$SRC/SDL-1.2.15"; tar -C "$SRC" -xf "$SRC/SDL-1.2.15.tar.gz"
    (cd "$SRC/SDL-1.2.15" && ./configure --host=$CROSS --prefix="$PREFIX" --enable-shared \
        --disable-static --disable-video-x11 --disable-pulseaudio --disable-esd --disable-arts \
        --disable-nas --disable-alsa --disable-input-tslib --disable-cdrom --disable-joystick \
        >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi
echo "build-deps: SDL ok"

if [ ! -f "$PREFIX/lib/libfreetype.so" ]; then
    rm -rf "$SRC/freetype-2.10.4"; tar -C "$SRC" -xf "$SRC/freetype-2.10.4.tar.xz"
    (cd "$SRC/freetype-2.10.4" && ./configure --host=$CROSS --prefix="$PREFIX" --enable-shared \
        --disable-static --with-zlib=no --with-bzip2=no --with-png=no --with-harfbuzz=no \
        --with-brotli=no >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi
echo "build-deps: freetype ok"

if [ ! -f "$PREFIX/lib/libSDL_ttf.so" ]; then
    rm -rf "$SRC/SDL_ttf-2.0.11"; tar -C "$SRC" -xf "$SRC/SDL_ttf-2.0.11.tar.gz"
    # Cross freetype ships no usable freetype-config; give SDL_ttf a tiny one.
    mkdir -p "$PREFIX/bin"
    cat > "$PREFIX/bin/freetype-config" <<CFG
#!/bin/sh
case "\$1" in
    --cflags) echo "-I$PREFIX/include/freetype2" ;;
    --libs) echo "-L$PREFIX/lib -lfreetype" ;;
    --version) echo "2.10.4" ;;
esac
CFG
    chmod +x "$PREFIX/bin/freetype-config"
    (cd "$SRC/SDL_ttf-2.0.11" && ./configure --host=$CROSS --prefix="$PREFIX" --enable-shared \
        --disable-static --with-freetype-prefix="$PREFIX" --with-sdl-prefix="$PREFIX" \
        --without-x >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi
echo "build-deps: SDL_ttf ok"
