# Building minimal FFmpeg + JNI for ApricityMedia

This directory contains everything needed to build an extremely slimmed-down
FFmpeg 8.1 DLL that replaces the JavaCPP-based decoder with direct JNI.

## What's included

| File | Purpose |
|------|---------|
| `build-minimal.sh` | Build script (configure + compile FFmpeg + compile JNI DLL) |
| `jni/ApricityMediaNative.java` | Java JNI class with `native` method declarations |
| `jni/jni_ffmpeg.c` | C implementation of the JNI bridge |

## FFmpeg footprint reduction

Starting from `--disable-everything`, only the following are enabled:

**Demuxers (14):** mov, matroska, mp3, ogg, flac, wav, aac, ac3, eac3, mpegts, hls, flv, aiff, asf

**Video decoders (7):** h264, hevc, vp8, vp9, av1, mpeg4, mpeg2video

**Audio decoders (18):** aac (fixed+float), mp3float, vorbis, opus, flac, alac, pcm_s16le, pcm_s24le, pcm_f32le, pcm_s32le, ac3 (fixed+float), eac3, wmav2

**Protocols (4):** file, http (with schannel TLS for https), tcp

**Disabled (saves ~90% of full build size):**
- All programs (ffmpeg/ffplay/ffprobe)
- All documentation
- All encoders, muxers, filters
- libavdevice, libavfilter, libpostproc
- Hardware acceleration
- Device support
- Debug symbols
- Runtime CPU detection
- Alpha channel in swscale

## Build prerequisites (Windows / MSYS2 MINGW64)

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make \
          mingw-w64-x86_64-nasm \
          mingw-w64-x86_64-pkg-config diffutils
```

Install JDK 17+ (for `jni.h`): https://adoptium.net/

## Build

```bash
# 1. Configure
./build-minimal.sh configure

# 2. Build FFmpeg shared libs
./build-minimal.sh build-ffmpeg

# 3. Build JNI wrapper DLL
export JAVA_HOME="/c/Program Files/Java/jdk-21"
./build-minimal.sh build-jni

# Or all at once:
JAVA_HOME="/c/Program Files/Java/jdk-21" ./build-minimal.sh all
```

Output goes to `build-minimal/dist/bin/`.

## JNI API surface

The `ApricityMediaNative` class exposes:

```
// Lifecycle
static native void init();

// Video
static native long videoOpen(String path, int targetW, int targetH,
                              double maxFps, int networkTimeoutMs,
                              int networkBufferKb, boolean networkReconnect);
static native long  videoReadFrame(long decoderHandle);
static native int   videoFrameGetInfo(long frameHandle, long[] info);
static native ByteBuffer videoFrameGetPixels(long frameHandle);
static native void  videoFrameRelease(long frameHandle);
static native void  videoRewind(long decoderHandle);
static native void  videoClose(long decoderHandle);

// Audio
static native long  audioOpen(String path, int networkTimeoutMs,
                               int networkBufferKb, boolean networkReconnect);
static native int   audioReadPcm(long decoderHandle, byte[] buffer,
                                  int offset, int length);
static native int   audioSampleRate(long decoderHandle);
static native int   audioChannels(long decoderHandle);
static native void  audioRewind(long decoderHandle);
static native void  audioClose(long decoderHandle);
```

## Switching from JavaCPP to JNI

In `FFmpegRuntimeBootstrap.java`:

1. Replace `Loader.load()` calls with `ApricityMediaNative.init()`
2. Replace `FFmpegVideoDecoder` constructor with `ApricityMediaNative.videoOpen(...)`
3. Replace `FFmpegAudioDecoder` constructor with `ApricityMediaNative.audioOpen(...)`
4. Replace frame access with `videoFrameGetInfo()` + `videoFrameGetPixels()` + `videoFrameRelease()`

The direct ByteBuffer returned by `videoFrameGetPixels()` works with
`MemoryUtil.memCopy()` just like the current JavaCPP buffer.
