#!/bin/bash
# ================================================================
# build-minimal.sh — Build minimal FFmpeg 8.1 + JNI wrapper
#
# Supports: linux, macos, android (cross-compile), windows (MSYS2)
#
# Prerequisites (platform-dependent):
#   Linux:   gcc make pkg-config libssl-dev zlib1g-dev nasm
#   macOS:   brew install make pkg-config
#   Android: ANDROID_NDK_HOME must be set
#   Windows: run inside MSYS2 MINGW64 (bash --login, MSYSTEM=MINGW64)
#
# Usage:
#   ./build-minimal.sh configure        # Step 1 — detect platform
#   ./build-minimal.sh build-ffmpeg     # Step 2
#   JAVA_HOME=/path/to/jdk ./build-minimal.sh build-jni  # Step 3
#   ./build-minimal.sh all              # All in one shot
#
# Platform auto-detection:
#   MSYSTEM is set           → windows (MSYS2 MINGW64)
#   uname = Darwin           → macos
#   ANDROID_NDK_HOME set     → android (cross-compile aarch64)
#   otherwise                → linux
# ================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-minimal"
FFMPEG_SRC="$SCRIPT_DIR"
JNI_DIR="$SCRIPT_DIR/jni"

JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# ---- Platform detection ----
detect_platform() {
    if [ -n "${MSYSTEM:-}" ]; then
        echo "windows"
    elif [ "$(uname)" = "Darwin" ]; then
        echo "macos"
    elif [ -n "${ANDROID_NDK_HOME:-}" ]; then
        echo "android"
    else
        echo "linux"
    fi
}

# Convert JAVA_HOME to a POSIX path (on Windows via cygpath, pass-through elsewhere)
resolve_javahome() {
    if command -v cygpath &>/dev/null; then
        cygpath -u "$JAVA_HOME"
    else
        echo "$JAVA_HOME"
    fi
}

# ---- Common configure flags (identical to build-minimal.ps1) ----
BASE_FLAGS=(
    --enable-shared
    --disable-static
    --disable-programs
    --disable-doc
    --disable-everything
    --disable-avdevice
    --disable-avfilter
    --disable-swscale-alpha
    --disable-runtime-cpudetect
    --disable-debug
    --enable-small

    # --- Video decoders ---
    --enable-decoder=h264
    --enable-decoder=hevc
    --enable-decoder=vp8
    --enable-decoder=vp9
    --enable-decoder=av1
    --enable-decoder=mpeg4
    --enable-decoder=mpeg2video

    # --- Audio decoders ---
    --enable-decoder=aac
    --enable-decoder=aac_fixed
    --enable-decoder=mp3float
    --enable-decoder=vorbis
    --enable-decoder=opus
    --enable-decoder=flac
    --enable-decoder=alac
    --enable-decoder=pcm_s16le
    --enable-decoder=pcm_s24le
    --enable-decoder=pcm_f32le
    --enable-decoder=pcm_s32le
    --enable-decoder=ac3
    --enable-decoder=ac3_fixed
    --enable-decoder=eac3
    --enable-decoder=wmav2

    # --- Demuxers ---
    --enable-demuxer=mov
    --enable-demuxer=matroska
    --enable-demuxer=mp3
    --enable-demuxer=ogg
    --enable-demuxer=flac
    --enable-demuxer=wav
    --enable-demuxer=aac
    --enable-demuxer=ac3
    --enable-demuxer=eac3
    --enable-demuxer=mpegts
    --enable-demuxer=hls
    --enable-demuxer=flv
    --enable-demuxer=aiff
    --enable-demuxer=asf

    # --- Bitstream filters ---
    --enable-bsf=h264_mp4toannexb
    --enable-bsf=hevc_mp4toannexb

    # --- Parsers ---
    --enable-parser=h264
    --enable-parser=hevc
    --enable-parser=vp8
    --enable-parser=vp9
    --enable-parser=aac
    --enable-parser=opus
    --enable-parser=vorbis
    --enable-parser=flac
    --enable-parser=mpegaudio
)

# ================================================================
# Commands
# ================================================================

configure() {
    local platform="${1:-$(detect_platform)}"
    echo "=== Configuring FFmpeg for $platform ==="

    local extra=()

    case "$platform" in
        windows)
            extra+=(
                --enable-schannel
                --enable-protocol=file
                --enable-protocol=http
                --enable-protocol=https
                --enable-protocol=tcp
            )
            ;;
        linux)
            extra+=(
                --enable-openssl
                --enable-protocol=file
                --enable-protocol=http
                --enable-protocol=https
                --enable-protocol=tcp
            )
            ;;
        macos)
            extra+=(
                --enable-securetransport
                --enable-protocol=file
                --enable-protocol=http
                --enable-protocol=https
                --enable-protocol=tcp
            )
            ;;
        android)
            local ndk="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set}"
            local tc="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
            extra+=(
                --enable-cross-compile
                --target-os=android
                --arch=aarch64
                --cc="$tc/bin/aarch64-linux-android21-clang"
                --cxx="$tc/bin/aarch64-linux-android21-clang++"
                --ar="$tc/bin/llvm-ar"
                --nm="$tc/bin/llvm-nm"
                --strip="$tc/bin/llvm-strip"
                --sysroot="$tc/sysroot"
                --extra-ldflags="-Wl,--gc-sections"
                --enable-protocol=file
                --enable-protocol=tcp
            )
            ;;
    esac

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    "$FFMPEG_SRC/configure" \
        "${BASE_FLAGS[@]}" \
        "${extra[@]}" \
        --prefix="$BUILD_DIR/dist" \
        --libdir="$BUILD_DIR/dist/lib" \
        --incdir="$BUILD_DIR/dist/include" \
        --bindir="$BUILD_DIR/dist/bin"
    cd "$SCRIPT_DIR"

    echo "Configure done."
}

build_ffmpeg() {
    echo "=== Building FFmpeg ==="

    if [ ! -f "$BUILD_DIR/Makefile" ]; then
        echo "Makefile not found — run configure first." >&2
        return 1
    fi

    cd "$BUILD_DIR"
    make -j"$JOBS"
    make install
    cd "$SCRIPT_DIR"

    echo "FFmpeg build complete. Libraries in: $BUILD_DIR/dist/lib"
}

build_jni() {
    local platform="${1:-$(detect_platform)}"
    echo "=== Building JNI wrapper for $platform ==="

    local cc="" lib_ext="" jni_os="" extra_libs="" extra_ldflags=""

    case "$platform" in
        windows)
            cc="gcc"
            lib_ext="dll"
            jni_os="win32"
            extra_libs="-lole32 -lpsapi -lbcrypt -static-libgcc -static-libstdc++"
            extra_ldflags="-Wl,--enable-runtime-pseudo-reloc"
            ;;
        linux)
            cc="gcc"
            lib_ext="so"
            jni_os="linux"
            extra_libs="-lpthread -ldl"
            extra_ldflags=""
            ;;
        macos)
            cc="clang"
            lib_ext="dylib"
            jni_os="darwin"
            extra_libs=""
            extra_ldflags=""
            ;;
        android)
            local ndk="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set}"
            cc="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang"
            lib_ext="so"
            jni_os="linux"
            extra_libs="-llog"
            extra_ldflags=""
            ;;
    esac

    if [ -z "${JAVA_HOME:-}" ]; then
        echo "JAVA_HOME is not set — cannot build JNI." >&2
        return 1
    fi

    # On Windows, JAVA_HOME is a Windows path (C:\...); convert for MSYS2 gcc
    local jh
    jh="$(resolve_javahome)"

    local ffinc="$BUILD_DIR/dist/include"
    local fflib="$BUILD_DIR/dist/lib"

    if [ ! -d "$ffinc/libavcodec" ]; then
        echo "FFmpeg headers not found at $ffinc — run 'build-ffmpeg' first." >&2
        return 1
    fi

    mkdir -p "$BUILD_DIR/dist/bin"

    local jni_basename="apricitymedia-jni"
    local out="$BUILD_DIR/dist/bin/$jni_basename.$lib_ext"

    $cc -shared -o "$out" \
        -I"$jh/include" \
        -I"$jh/include/$jni_os" \
        -I"$ffinc" \
        -L"$fflib" \
        "$JNI_DIR/jni_ffmpeg.c" \
        -lavformat -lavcodec -lavutil -lswresample -lswscale \
        $extra_libs -lm -O2 -s $extra_ldflags

    echo "JNI library built: $out"
}

all() {
    local platform="${1:-$(detect_platform)}"
    configure "$platform"
    build_ffmpeg
    build_jni "$platform"
    echo "=== All done ==="
    echo "Output: $BUILD_DIR/dist/bin/"
}

# ================================================================
# Entry point
# ================================================================

cmd="${1:-all}"
platform="${2:-}"
if [ -z "$platform" ]; then
    platform="$(detect_platform)"
fi

case "$cmd" in
    configure)     configure "$platform" ;;
    build-ffmpeg)  build_ffmpeg ;;
    build-jni)     build_jni "$platform" ;;
    all)           all "$platform" ;;
    *)
        echo "Usage: $0 {configure|build-ffmpeg|build-jni|all} [linux|macos|android|windows]" >&2
        exit 1
        ;;
esac
