/**
 * jni_ffmpeg.c — Direct JNI bridge to minimal FFmpeg (8.1 API).
 *
 * Replaces JavaCPP-based decoders with raw JNI for:
 *   - Video decoding (libavcodec + libavformat + libswscale → RGBA)
 *   - Audio decoding (libavcodec + libavformat + libswresample → S16LE 48 kHz stereo)
 *
 * Build (cross-compile with mingw-w64):
 *   x86_64-w64-mingw32-gcc -shared -o apricitymedia-jni.dll \
 *       -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
 *       -I/path/to/ffmpeg/build/include \
 *       -L/path/to/ffmpeg/build/lib \
 *       jni_ffmpeg.c -lavformat -lavcodec -lavutil -lswresample -lswscale \
 *       -lole32 -lpsapi -lbcrypt -lm \
 *       -Wl,--enable-runtime-pseudo-reloc \
 *       -static-libgcc -static-libstdc++ \
 *       -O2 -s
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* ================================================================
 *  Constants
 * ================================================================ */

#define AUDIO_OUT_SAMPLE_RATE  48000
#define AUDIO_OUT_SAMPLE_FMT   AV_SAMPLE_FMT_S16
#define AUDIO_OUT_CHANNELS     2

/* ================================================================
 *  Internal structs — opaque handles passed to Java as jlong
 * ================================================================ */

/* ---------- Video ---------- */

typedef struct {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    int               stream_index;
    AVRational        time_base;

    int               src_width;
    int               src_height;
    int               out_width;
    int               out_height;
    int               default_duration_ms;
    int64_t           min_frame_interval_ms;

    AVPacket         *pkt;
    AVFrame          *decoded_frame;
    struct SwsContext *sws_ctx;

    uint8_t          *rgba_buffer;      /* reusable intermediate */
    int               rgba_buffer_size;

    int               eof;
    int64_t           fallback_pts_ms;
    int64_t           last_emitted_pts_ms;
} VideoDecoder;

/* A single decoded RGBA frame, heap-allocated and returned to Java */
typedef struct {
    uint8_t *rgba_data;
    int      width;
    int      height;
    int64_t  pts_ms;
    int      duration_ms;
    int      capacity;   /* width * height * 4 */
} VideoFrame;

/* ---------- Audio ---------- */

typedef struct {
    AVFormatContext   *fmt_ctx;
    AVCodecContext    *codec_ctx;
    int                stream_index;

    AVPacket          *pkt;
    AVFrame           *frame;
    struct SwrContext *swr;
    AVChannelLayout    out_layout;
    AVChannelLayout    in_layout;

    int                eof;
    int                drain_started;

    uint8_t           *out_buffer;
    int                out_buffer_capacity;
    int                pending_bytes;
    int                pending_pos;
} AudioDecoder;

/* ================================================================
 *  Internal helpers
 * ================================================================ */

static int is_remote(const char *path) {
    if (!path) return 0;
    return strstr(path, "://") != NULL;
}

static void dict_set_int(AVDictionary **d, const char *key, long v) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%ld", v);
    av_dict_set(d, key, buf, 0);
}

static void set_network_opts(AVDictionary **opts,
                             int timeout_ms, int buf_kb, int reconnect)
{
    if (timeout_ms < 1000) timeout_ms = 15000;
    if (buf_kb     < 64)   buf_kb     = 512;

    dict_set_int(opts, "rw_timeout",                (long)timeout_ms * 1000L);
    dict_set_int(opts, "timeout",                   (long)timeout_ms * 1000L);
    dict_set_int(opts, "buffer_size",               (long)buf_kb * 1024L);
    av_dict_set(opts, "reconnect",                  reconnect ? "1" : "0", 0);
    av_dict_set(opts, "reconnect_streamed",         reconnect ? "1" : "0", 0);
    av_dict_set(opts, "reconnect_on_network_error", reconnect ? "1" : "0", 0);
    av_dict_set(opts, "reconnect_on_http_error",    reconnect ? "4xx,5xx" : "", 0);
    av_dict_set(opts, "reconnect_delay_max",        "2", 0);
    av_dict_set(opts, "http_persistent",            "1", 0);
    av_dict_set(opts, "multiple_requests",          "1", 0);
    av_dict_set(opts, "analyzeduration",            "5000000", 0);
    av_dict_set(opts, "probesize",                  "1048576", 0);
}

static int open_input(AVFormatContext **fmt_ctx, const char *path,
                       int timeout_ms, int buf_kb, int reconnect)
{
    AVDictionary *opts = NULL;
    if (is_remote(path))
        set_network_opts(&opts, timeout_ms, buf_kb, reconnect);

    int ret = avformat_open_input(fmt_ctx, path, NULL, &opts);
    av_dict_free(&opts);
    return ret;
}

static int find_stream_type(AVFormatContext *fmt_ctx, enum AVMediaType type) {
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *st = fmt_ctx->streams[i];
        if (st && st->codecpar && st->codecpar->codec_type == type)
            return (int)i;
    }
    return -1;
}

static double rat_dbl(AVRational r) {
    return (r.num == 0 || r.den == 0) ? 0.0 : (double)r.num / (double)r.den;
}

static int frame_duration_ms(AVStream *st) {
    double fps = rat_dbl(st->avg_frame_rate);
    if (fps <= 0.0001) fps = rat_dbl(st->r_frame_rate);
    if (fps <= 0.0001) fps = 30.0;
    return (int)fmax(1.0, round(1000.0 / fps));
}

static void resolve_size(int sw, int sh, int tw, int th, int *ow, int *oh) {
    if (sw <= 0 || sh <= 0) { *ow = tw > 0 ? tw : 1; *oh = th > 0 ? th : 1; return; }
    if (tw <= 0 && th <= 0) { *ow = sw; *oh = sh; return; }
    if (tw > 0 && th > 0)   { *ow = tw; *oh = th; return; }
    if (tw > 0) { *ow = tw; *oh = (int)fmax(1, round(1.0 * tw * sh / sw)); }
    else        { *oh = th; *ow = (int)fmax(1, round(1.0 * th * sw / sh)); }
}

static int64_t resolve_pts(const AVFrame *f, AVRational tb, int64_t *fb) {
    if (f->best_effort_timestamp < 0) { int64_t v = *fb; *fb += 16; return v; }
    double sec = f->best_effort_timestamp * rat_dbl(tb);
    int64_t ms = (int64_t)fmax(0, round(sec * 1000.0));
    *fb = ms;
    return ms;
}

static int resolve_dur(const AVFrame *f, AVRational tb, int def) {
    if (f->duration > 0) return (int)fmax(1, round(f->duration * rat_dbl(tb) * 1000.0));
    return def;
}

/* ================================================================
 *  VideoDecoder
 * ================================================================ */

static VideoDecoder *vd_alloc(void) {
    return (VideoDecoder *)av_mallocz(sizeof(VideoDecoder));
}

static void vd_free(VideoDecoder *d) {
    if (!d) return;
    if (d->codec_ctx)    avcodec_free_context(&d->codec_ctx);
    if (d->fmt_ctx)      avformat_close_input(&d->fmt_ctx);
    if (d->sws_ctx)      sws_freeContext(d->sws_ctx);
    if (d->decoded_frame) av_frame_free(&d->decoded_frame);
    if (d->pkt)          av_packet_free(&d->pkt);
    if (d->rgba_buffer)  av_free(d->rgba_buffer);
    av_free(d);
}

static int vd_open(VideoDecoder *d, const char *path,
                   int tw, int th, double max_fps,
                   int tmo, int buf_kb, int recon)
{
    d->fmt_ctx = avformat_alloc_context();
    if (!d->fmt_ctx) return AVERROR(ENOMEM);

    int ret = open_input(&d->fmt_ctx, path, tmo, buf_kb, recon);
    if (ret < 0) return ret;

    ret = avformat_find_stream_info(d->fmt_ctx, NULL);
    if (ret < 0) return ret;

    d->stream_index = find_stream_type(d->fmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (d->stream_index < 0) return AVERROR_STREAM_NOT_FOUND;

    AVStream *st = d->fmt_ctx->streams[d->stream_index];
    d->time_base = st->time_base;

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;

    d->codec_ctx = avcodec_alloc_context3(codec);
    if (!d->codec_ctx) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(d->codec_ctx, st->codecpar);
    if (ret < 0) return ret;

    ret = avcodec_open2(d->codec_ctx, codec, NULL);
    if (ret < 0) return ret;

    d->src_width  = d->codec_ctx->width;
    d->src_height = d->codec_ctx->height;
    resolve_size(d->src_width, d->src_height, tw, th,
                 &d->out_width, &d->out_height);
    d->default_duration_ms  = frame_duration_ms(st);
    d->min_frame_interval_ms = (max_fps > 0.0001)
        ? (int64_t)fmax(1, round(1000.0 / max_fps)) : 0;

    d->decoded_frame = av_frame_alloc();
    d->pkt           = av_packet_alloc();
    if (!d->decoded_frame || !d->pkt) return AVERROR(ENOMEM);

    d->sws_ctx = sws_getContext(d->src_width, d->src_height, d->codec_ctx->pix_fmt,
                                 d->out_width, d->out_height, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (!d->sws_ctx) return AVERROR(ENOMEM);

    d->rgba_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA,
                                                     d->out_width, d->out_height, 1);
    if (d->rgba_buffer_size <= 0) return AVERROR(EINVAL);

    d->rgba_buffer = (uint8_t *)av_malloc(d->rgba_buffer_size);
    if (!d->rgba_buffer) return AVERROR(ENOMEM);

    return 0;
}

/* Allocates and returns a new VideoFrame (caller frees with vf_free). */
static VideoFrame *vd_read_frame(VideoDecoder *d) {
    if (!d) return NULL;

    for (;;) {
        if (d->eof) {
            int ret = avcodec_receive_frame(d->codec_ctx, d->decoded_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return NULL;
            if (ret < 0) return NULL;
            goto produce;
        }

        while (av_read_frame(d->fmt_ctx, d->pkt) >= 0) {
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;
            }
            int ret = avcodec_send_packet(d->codec_ctx, d->pkt);
            av_packet_unref(d->pkt);
            if (ret < 0) continue;

            ret = avcodec_receive_frame(d->codec_ctx, d->decoded_frame);
            if (ret == AVERROR(EAGAIN)) continue;
            if (ret < 0) continue;
            goto produce;
        }

        d->eof = 1;
        avcodec_send_packet(d->codec_ctx, NULL);
        /* Retry drain */
    }

produce:
    {
        int64_t pts_ms = resolve_pts(d->decoded_frame, d->time_base, &d->fallback_pts_ms);
        int dur_ms     = resolve_dur(d->decoded_frame, d->time_base, d->default_duration_ms);

        if (d->min_frame_interval_ms > 0 &&
            d->last_emitted_pts_ms != INT64_MIN &&
            (pts_ms - d->last_emitted_pts_ms) < d->min_frame_interval_ms)
            return NULL;

        /* sws_scale */
        uint8_t *dst[] = { d->rgba_buffer, NULL, NULL, NULL };
        int dst_stride[] = { d->out_width * 4, 0, 0, 0 };
        int ret = sws_scale(d->sws_ctx,
                            (const uint8_t *const *)d->decoded_frame->data,
                            d->decoded_frame->linesize,
                            0, d->src_height, dst, dst_stride);
        if (ret <= 0) return NULL;

        VideoFrame *vf = (VideoFrame *)av_mallocz(sizeof(VideoFrame));
        if (!vf) return NULL;

        vf->width       = d->out_width;
        vf->height      = d->out_height;
        vf->pts_ms      = pts_ms;
        vf->duration_ms = dur_ms;
        vf->capacity    = d->rgba_buffer_size;

        vf->rgba_data = (uint8_t *)av_malloc(vf->capacity);
        if (!vf->rgba_data) { av_free(vf); return NULL; }

        memcpy(vf->rgba_data, d->rgba_buffer, vf->capacity);
        d->last_emitted_pts_ms = pts_ms;
        return vf;
    }
}

static void vd_rewind(VideoDecoder *d) {
    if (!d) return;
    d->eof = 0;
    d->fallback_pts_ms = 0;
    d->last_emitted_pts_ms = INT64_MIN;
    av_seek_frame(d->fmt_ctx, d->stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(d->codec_ctx);
}

static void vf_free(VideoFrame *vf) {
    if (!vf) return;
    if (vf->rgba_data) av_free(vf->rgba_data);
    av_free(vf);
}

/* ================================================================
 *  AudioDecoder
 * ================================================================ */

static AudioDecoder *ad_alloc(void) {
    AudioDecoder *d = (AudioDecoder *)av_mallocz(sizeof(AudioDecoder));
    if (d) av_channel_layout_default(&d->out_layout, AUDIO_OUT_CHANNELS);
    return d;
}

static void ad_free(AudioDecoder *d) {
    if (!d) return;
    if (d->fmt_ctx)    avformat_close_input(&d->fmt_ctx);
    if (d->codec_ctx)  avcodec_free_context(&d->codec_ctx);
    if (d->pkt)        av_packet_free(&d->pkt);
    if (d->frame)      av_frame_free(&d->frame);
    if (d->swr)       { swr_close(d->swr); swr_free(&d->swr); }
    av_channel_layout_uninit(&d->out_layout);
    av_channel_layout_uninit(&d->in_layout);
    if (d->out_buffer) av_free(d->out_buffer);
    av_free(d);
}

static int ad_open(AudioDecoder *d, const char *path,
                   int tmo, int buf_kb, int recon)
{
    d->fmt_ctx = avformat_alloc_context();
    if (!d->fmt_ctx) return AVERROR(ENOMEM);

    int ret = open_input(&d->fmt_ctx, path, tmo, buf_kb, recon);
    if (ret < 0) return ret;

    ret = avformat_find_stream_info(d->fmt_ctx, NULL);
    if (ret < 0) return ret;

    d->stream_index = find_stream_type(d->fmt_ctx, AVMEDIA_TYPE_AUDIO);
    if (d->stream_index < 0) return AVERROR_STREAM_NOT_FOUND;

    AVStream *st = d->fmt_ctx->streams[d->stream_index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;

    d->codec_ctx = avcodec_alloc_context3(codec);
    if (!d->codec_ctx) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(d->codec_ctx, st->codecpar);
    if (ret < 0) return ret;

    ret = avcodec_open2(d->codec_ctx, codec, NULL);
    if (ret < 0) return ret;

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) return AVERROR(ENOMEM);

    ret = av_channel_layout_copy(&d->in_layout, &d->codec_ctx->ch_layout);
    if (ret < 0 || d->in_layout.nb_channels <= 0) {
        av_channel_layout_uninit(&d->in_layout);
        av_channel_layout_default(&d->in_layout, AUDIO_OUT_CHANNELS);
    }

    d->swr = swr_alloc();
    if (!d->swr) return AVERROR(ENOMEM);

    ret = swr_alloc_set_opts2(&d->swr,
                               &d->out_layout, AUDIO_OUT_SAMPLE_FMT, AUDIO_OUT_SAMPLE_RATE,
                               &d->in_layout,  d->codec_ctx->sample_fmt, d->codec_ctx->sample_rate,
                               0, NULL);
    if (ret < 0) return ret;

    ret = swr_init(d->swr);
    if (ret < 0) return ret;

    return 0;
}

/* Decode + resample one frame into pending buffer. Returns bytes produced or <=0. */
static int ad_receive(AudioDecoder *d) {
    int ret = avcodec_receive_frame(d->codec_ctx, d->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    if (ret < 0) return 0;

    int in_samples = d->frame->nb_samples;
    if (in_samples <= 0) return 0;

    int out_samples = (int)swr_get_out_samples(d->swr, in_samples);
    if (out_samples <= 0) return 0;

    int buf_size = av_samples_get_buffer_size(NULL, AUDIO_OUT_CHANNELS,
                                               out_samples, AUDIO_OUT_SAMPLE_FMT, 1);
    if (buf_size <= 0) return 0;

    if (!d->out_buffer || d->out_buffer_capacity < buf_size) {
        if (d->out_buffer) av_free(d->out_buffer);
        d->out_buffer = (uint8_t *)av_malloc(buf_size);
        if (!d->out_buffer) return 0;
        d->out_buffer_capacity = buf_size;
    }

    uint8_t *out_ptrs[] = { d->out_buffer };
    int conv = swr_convert(d->swr, out_ptrs, out_samples,
                           (const uint8_t **)d->frame->data, in_samples);
    if (conv <= 0) return 0;

    int bytes = av_samples_get_buffer_size(NULL, AUDIO_OUT_CHANNELS,
                                            conv, AUDIO_OUT_SAMPLE_FMT, 1);
    if (bytes <= 0) return 0;

    d->pending_pos   = 0;
    d->pending_bytes = bytes;
    return bytes;
}

static void ad_rewind(AudioDecoder *d) {
    if (!d) return;
    d->eof = 0;
    d->drain_started = 0;
    d->pending_bytes = 0;
    d->pending_pos   = 0;
    av_seek_frame(d->fmt_ctx, d->stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(d->codec_ctx);
    swr_close(d->swr);
    swr_init(d->swr);
}

/* ================================================================
 *  JNI — lifecycle
 * ================================================================ */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
    return JNI_VERSION_1_6;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    init
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_init
    (JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
}

/* ================================================================
 *  JNI — video
 * ================================================================ */

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoOpen
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoOpen
    (JNIEnv *env, jclass clazz,
     jstring jpath, jint tw, jint th, jdouble max_fps,
     jint tmo, jint buf_kb, jboolean recon)
{
    (void)clazz;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return 0;

    VideoDecoder *d = vd_alloc();
    if (!d) { (*env)->ReleaseStringUTFChars(env, jpath, path); return 0; }

    int ret = vd_open(d, path, (int)tw, (int)th, (double)max_fps,
                       (int)tmo, (int)buf_kb, (int)recon);
    (*env)->ReleaseStringUTFChars(env, jpath, path);

    if (ret < 0) { vd_free(d); return 0; }
    return (jlong)(intptr_t)d;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoReadFrame
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoReadFrame
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    VideoDecoder *d = (VideoDecoder *)(intptr_t)handle;
    if (!d) return 0;
    VideoFrame *vf = vd_read_frame(d);
    return (jlong)(intptr_t)vf;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoFrameGetInfo
 * Signature: (J[J)I  — info[4] = {width, height, ptsMs, durationMs}
 */
JNIEXPORT jint JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoFrameGetInfo
    (JNIEnv *env, jclass clazz, jlong frame_handle, jlongArray jinfo)
{
    (void)clazz;
    VideoFrame *vf = (VideoFrame *)(intptr_t)frame_handle;
    if (!vf || !jinfo) return 0;

    jlong info[4];
    info[0] = (jlong)vf->width;
    info[1] = (jlong)vf->height;
    info[2] = (jlong)vf->pts_ms;
    info[3] = (jlong)vf->duration_ms;
    (*env)->SetLongArrayRegion(env, jinfo, 0, 4, info);

    return (jint)(vf->width * vf->height * 4);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoFrameGetPixels
 */
JNIEXPORT jobject JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoFrameGetPixels
    (JNIEnv *env, jclass clazz, jlong frame_handle)
{
    (void)clazz;
    VideoFrame *vf = (VideoFrame *)(intptr_t)frame_handle;
    if (!vf || !vf->rgba_data) return NULL;
    return (*env)->NewDirectByteBuffer(env, vf->rgba_data, (jlong)vf->capacity);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoFrameRelease
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoFrameRelease
    (JNIEnv *env, jclass clazz, jlong frame_handle)
{
    (void)env; (void)clazz;
    vf_free((VideoFrame *)(intptr_t)frame_handle);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoRewind
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoRewind
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    vd_rewind((VideoDecoder *)(intptr_t)handle);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoClose
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoClose
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    vd_free((VideoDecoder *)(intptr_t)handle);
}

/* ================================================================
 *  JNI — audio
 * ================================================================ */

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioOpen
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioOpen
    (JNIEnv *env, jclass clazz,
     jstring jpath, jint tmo, jint buf_kb, jboolean recon)
{
    (void)clazz;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return 0;

    AudioDecoder *d = ad_alloc();
    if (!d) { (*env)->ReleaseStringUTFChars(env, jpath, path); return 0; }

    int ret = ad_open(d, path, (int)tmo, (int)buf_kb, (int)recon);
    (*env)->ReleaseStringUTFChars(env, jpath, path);

    if (ret < 0) { ad_free(d); return 0; }
    return (jlong)(intptr_t)d;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioReadPcm
 */
JNIEXPORT jint JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioReadPcm
    (JNIEnv *env, jclass clazz,
     jlong handle, jbyteArray jbuf, jint offset, jint length)
{
    (void)clazz;
    AudioDecoder *d = (AudioDecoder *)(intptr_t)handle;
    if (!d || !jbuf || length <= 0) return -2;
    jsize buf_len = (*env)->GetArrayLength(env, jbuf);
    if (offset < 0 || offset >= buf_len) return -2;
    int max_copy = (length < buf_len - offset) ? length : (buf_len - offset);
    if (max_copy <= 0) return -2;

    /* Serve from pending */
    if (d->pending_bytes > d->pending_pos) {
        int avail = d->pending_bytes - d->pending_pos;
        int copy  = (max_copy < avail) ? max_copy : avail;
        (*env)->SetByteArrayRegion(env, jbuf, offset, copy,
                                    (jbyte *)(d->out_buffer + d->pending_pos));
        d->pending_pos += copy;
        if (d->pending_pos >= d->pending_bytes) {
            d->pending_pos   = 0;
            d->pending_bytes = 0;
        }
        return (jint)copy;
    }

    /* Decode loop */
    for (;;) {
        if (d->eof) {
            if (!d->drain_started) {
                d->drain_started = 1;
                avcodec_send_packet(d->codec_ctx, NULL);
            }
            if (ad_receive(d) <= 0) return -1;

            int avail = d->pending_bytes - d->pending_pos;
            int copy  = (max_copy < avail) ? max_copy : avail;
            if (copy <= 0) return 0;

            (*env)->SetByteArrayRegion(env, jbuf, offset, copy,
                                        (jbyte *)(d->out_buffer + d->pending_pos));
            d->pending_pos += copy;
            if (d->pending_pos >= d->pending_bytes) {
                d->pending_pos   = 0;
                d->pending_bytes = 0;
            }
            return (jint)copy;
        }

        while (av_read_frame(d->fmt_ctx, d->pkt) >= 0) {
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;
            }
            int ret = avcodec_send_packet(d->codec_ctx, d->pkt);
            av_packet_unref(d->pkt);
            if (ret < 0) continue;

            if (ad_receive(d) > 0) {
                int avail = d->pending_bytes - d->pending_pos;
                int copy  = (max_copy < avail) ? max_copy : avail;
                if (copy <= 0) return 0;

                (*env)->SetByteArrayRegion(env, jbuf, offset, copy,
                                            (jbyte *)(d->out_buffer + d->pending_pos));
                d->pending_pos += copy;
                if (d->pending_pos >= d->pending_bytes) {
                    d->pending_pos   = 0;
                    d->pending_bytes = 0;
                }
                return (jint)copy;
            }
        }

        d->eof = 1;
        /* loop to try drain */
    }
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioSampleRate
 */
JNIEXPORT jint JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioSampleRate
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return AUDIO_OUT_SAMPLE_RATE;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioChannels
 */
JNIEXPORT jint JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioChannels
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return AUDIO_OUT_CHANNELS;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioRewind
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioRewind
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    ad_rewind((AudioDecoder *)(intptr_t)handle);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    audioClose
 */
JNIEXPORT void JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioClose
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    ad_free((AudioDecoder *)(intptr_t)handle);
}
