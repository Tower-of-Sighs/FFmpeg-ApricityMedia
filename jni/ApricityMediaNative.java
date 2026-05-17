package cc.sighs.apricitymedia.jni;

import java.nio.ByteBuffer;

/**
 * Native JNI bridge to the minimal FFmpeg build.
 *
 * <p>This class replaces the JavaCPP-based FFmpegRuntimeBootstrap +
 * FFmpegAudioDecoder/FFmpegVideoDecoder with direct JNI calls.
 * All methods are static — the native side manages opaque handles (long).
 */
public final class ApricityMediaNative {

    static {
        System.loadLibrary("apricitymedia-jni");
    }

    private ApricityMediaNative() {}

    // ---------------------------------------------------------------
    //  Lifecycle
    // ---------------------------------------------------------------

    /** Initialize FFmpeg global state (log level, network). Call once at startup. */
    public static native void init();

    // ---------------------------------------------------------------
    //  Video
    // ---------------------------------------------------------------

    /**
     * Open a video file or stream.
     * @return opaque decoder handle (0 on failure)
     */
    public static native long videoOpen(String path,
                                        int targetWidth,
                                        int targetHeight,
                                        double maxFps,
                                        int networkTimeoutMs,
                                        int networkBufferKb,
                                        boolean networkReconnect);

    /**
     * Decode the next video frame.
     * @return opaque frame handle (0 on EOF or error)
     */
    public static native long videoReadFrame(long decoderHandle);

    /**
     * Get decoded frame metadata.
     * @param info  long[4] filled as [width, height, ptsMs, durationMs]
     * @return total pixel bytes (width * height * 4), or 0 on error
     */
    public static native int videoFrameGetInfo(long frameHandle, long[] info);

    /**
     * Get a direct ByteBuffer pointing to RGBA pixel data.
     * Valid until {@link #videoFrameRelease(long)} is called.
     */
    public static native ByteBuffer videoFrameGetPixels(long frameHandle);

    /** Pixel format tag: 0=RGBA8888, 1=YUV420P, 2=NV12, 3=YUV420P10LE, 4=P010LE. */
    public static native int videoFrameGetPixelFormat(long frameHandle);

    /** Number of valid planes for current frame. */
    public static native int videoFrameGetPlaneCount(long frameHandle);

    /**
     * Get plane metadata.
     * @param info int[3] filled as [rowStride, pixelStride, planeBytes]
     * @return plane bytes, or 0 on error
     */
    public static native int videoFrameGetPlaneInfo(long frameHandle, int planeIndex, int[] info);

    /**
     * Get a direct ByteBuffer for requested plane.
     * Valid until {@link #videoFrameRelease(long)} is called.
     */
    public static native ByteBuffer videoFrameGetPlaneBuffer(long frameHandle, int planeIndex);

    /**
     * Get color metadata.
     * @param info int[4] filled as [colorspace, colorTrc, colorPrimaries, colorRange]
     * @return 1 if filled, 0 on error
     */
    public static native int videoFrameGetColorInfo(long frameHandle, int[] info);

    /**
     * Get original decoded pixel format name from FFmpeg (e.g. yuv420p10le, nv12).
     * Returns "unknown" when unavailable.
     */
    public static native String videoFrameGetSourcePixelFormat(long frameHandle);

    /** Release a decoded frame returned by {@link #videoReadFrame(long)}. */
    public static native void videoFrameRelease(long frameHandle);

    /** Rewind the video decoder to the beginning. */
    public static native void videoRewind(long decoderHandle);

    /** Close the video decoder and free all native resources. */
    public static native void videoClose(long decoderHandle);

    // ---------------------------------------------------------------
    //  Audio
    // ---------------------------------------------------------------

    /**
     * Open an audio file or stream.
     * @return opaque decoder handle (0 on failure)
     */
    public static native long audioOpen(String path,
                                        int networkTimeoutMs,
                                        int networkBufferKb,
                                        boolean networkReconnect);

    /**
     * Read the next decoded PCM chunk (S16LE, 48000 Hz, stereo).
     * @return bytes written (>0), 0 (no data yet), -1 (EOF drained), -2 (error)
     */
    public static native int audioReadPcm(long decoderHandle,
                                          byte[] buffer,
                                          int offset,
                                          int length);

    /** Get the output sample rate (always 48000). */
    public static native int audioSampleRate(long decoderHandle);

    /** Get the output channel count (always 2). */
    public static native int audioChannels(long decoderHandle);

    /** Rewind the audio decoder to the beginning. */
    public static native void audioRewind(long decoderHandle);

    /** Close the audio decoder and free all native resources. */
    public static native void audioClose(long decoderHandle);
}
