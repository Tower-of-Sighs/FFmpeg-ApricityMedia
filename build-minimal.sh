#!/bin/bash
# ================================================================
# build-minimal.sh — Build minimal FFmpeg 8.1 + JNI wrapper (.so/.dylib)
#
# Prerequisites (platform-dependent):
#   Linux:   gcc make pkg-config libssl-dev zlib1g-dev nasm
#   macOS:   brew install make pkg-config
#   Android: ANDROID_NDK_HOME must be set
#
# Usage:
#   ./build-minimal.sh configure        # Step 1 — detect platform
#   ./build-minimal.sh build-ffmpeg     # Step 2
#   JAVA_HOME=/path/to/jdk-21 \
#   ./build-minimal.sh build-jni        # Step 3
#
#   JAVA_HOME=/path/to/jdk-17 \
#   ./build-minimal.sh build-jni        # JNI with different JDK
#
#   ./build-minimal.sh all              # All in one shot
#
# Platform auto-detection:
#   ANDROID_NDK_HOME set → android (cross-compile aarch64)
#   uname = Darwin        → macos  (Apple clang + SecureTransport)
#   otherwise             → linux  (gcc + OpenSSL)
# ================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-minimal"
FFMPEG_SRC="$SCRIPT_DIR"
JNI_DIR="$SCRIPT_DIR/jni"

JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# ---- Platform detection ----
detect_platform() {
    if [ -n "${ANDROID_NDK_HOME:-}" ]; then
        echo "android"
    elif [ "$(uname)" = "Darwin" ]; then
        echo "macos"
    else
        echo "linux"
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
        linux)
            extra+=(
                --enable-openssl
                --enable-protocol=http
                --enable-protocol=https
                --enable-protocol=file
                --enable-protocol=tcp
            )
            ;;
        macos)
            extra+=(
                --enable-securetransport
                --enable-protocol=http
                --enable-protocol=https
                --enable-protocol=file
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
                # No TLS libs available when cross-compiling
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

    local cc="" lib_ext="" jni_os="" extra_libs=""

    case "$platform" in
        linux)
            cc="gcc"
            lib_ext="so"
            jni_os="linux"
            extra_libs="-lpthread -ldl"
            ;;
        macos)
            cc="clang"
            lib_ext="dylib"
            jni_os="darwin"
            extra_libs=""
            ;;
        android)
            local ndk="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set}"
            cc="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang"
            lib_ext="so"
            jni_os="linux"
            extra_libs="-llog"
            ;;
    esac

    if [ -z "${JAVA_HOME:-}" ]; then
        echo "JAVA_HOME is not set — cannot build JNI." >&2
        return 1
    fi

    local ffinc="$BUILD_DIR/dist/include"
    local fflib="$BUILD_DIR/dist/lib"

    if [ ! -d "$ffinc/libavcodec" ]; then
        echo "FFmpeg headers not found at $ffinc — run 'build-ffmpeg' first." >&2
        return 1
    fi

    mkdir -p "$BUILD_DIR/dist/bin"

    local out="$BUILD_DIR/dist/bin/libapricitymedia-jni.$lib_ext"

    $cc -shared -o "$out" \
        -I"$JAVA_HOME/include" \
        -I"$JAVA_HOME/include/$jni_os" \
        -I"$ffinc" \
        -L"$fflib" \
        "$JNI_DIR/jni_ffmpeg.c" \
        -lavformat -lavcodec -lavutil -lswresample -lswscale \
        $extra_libs -lm -O2 -s

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
        echo "Usage: $0 {configure|build-ffmpeg|build-jni|all} [linux|macos|android]" >&2
        exit 1
        ;;
esac
