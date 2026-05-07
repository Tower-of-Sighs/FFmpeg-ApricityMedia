/**
 * jni_ffmpeg.c — JNI bridge to minimal FFmpeg (8.1 API).
 *
 * Replaces JavaCPP-based decoders with direct JNI for:
 *   - Video decoding (libavcodec + libavformat + libswscale → RGBA)
 *   - Audio decoding (libavcodec + libavformat + libswresample → S16LE 48 kHz stereo)
 *
 * Improvements over the original:
 *   - Frame pooling for video (zero malloc per frame after init)
 *   - av_find_best_stream for proper stream selection
 *   - Error-tolerance flags (SHOW_ALL, IGNORE_ERR) for live streams
 *   - Proper send/receive retry loops with EAGAIN handling
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

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
#define VIDEO_FRAME_POOL_SIZE  8

/* ================================================================
 *  Internal helpers
 * ================================================================ */

static int is_remote(const char *path) {
    if (!path) return 0;
    return strstr(path, "://") != NULL;
}

static char g_last_error[1024] = "";

static void clear_last_error(void) {
    g_last_error[0] = '\0';
}

static void set_last_error_from_code(const char *op, const char *path, int err) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    snprintf(g_last_error, sizeof(g_last_error),
             "%s failed (%d): %s, path=%s",
             op ? op : "native op", err, errbuf, path ? path : "<null>");
    g_last_error[sizeof(g_last_error) - 1] = '\0';
    av_log(NULL, AV_LOG_ERROR, "%s\n", g_last_error);
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

static int find_best_stream(AVFormatContext *fmt_ctx, enum AVMediaType type) {
    int idx = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (idx >= 0) return idx;
    /* Fallback: pick first stream matching type */
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

static enum AVPixelFormat choose_software_pix_fmt(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx;
    if (!pix_fmts) return AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return pix_fmts[0];
}

/* ================================================================
 *  VideoDecoder — with frame pool
 * ================================================================ */

typedef struct {
    uint8_t *rgba_data;    /* width * height * 4, pre-allocated into pool */
    int      width;
    int      height;
    int64_t  pts_ms;
    int      duration_ms;
    int      capacity;
} VideoFrame;

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

    /* Frame pool — sws_scale writes directly into pooled buffers */
    int               pool_size;
    VideoFrame       *pool;           /* array[pool_size] */
    int              *free_stack;     /* stack of free indices */
    int               free_count;

    int               eof;
    int64_t           fallback_pts_ms;
    int64_t           last_emitted_pts_ms;
} VideoDecoder;

/* --- Pool helpers --- */

static int pool_pop(VideoDecoder *d) {
    if (d->free_count <= 0) return -1;
    return d->free_stack[--d->free_count];
}

static void pool_push(VideoDecoder *d, int idx) {
    if (idx < 0 || idx >= d->pool_size) return;
    d->free_stack[d->free_count++] = idx;
}

static int pool_init(VideoDecoder *d, int count, int w, int h) {
    int cap = av_image_get_buffer_size(AV_PIX_FMT_RGBA, w, h, 1);
    if (cap <= 0) return AVERROR(EINVAL);

    d->pool_size = count;
    d->pool = (VideoFrame *)av_mallocz((size_t)count * sizeof(VideoFrame));
    d->free_stack = (int *)av_malloc((size_t)count * sizeof(int));
    if (!d->pool || !d->free_stack) return AVERROR(ENOMEM);

    for (int i = 0; i < count; i++) {
        d->pool[i].rgba_data = (uint8_t *)av_malloc(cap);
        if (!d->pool[i].rgba_data) return AVERROR(ENOMEM);
        d->pool[i].capacity = cap;
        d->free_stack[i] = i;
    }
    d->free_count = count;
    return 0;
}

static void pool_free(VideoDecoder *d) {
    if (!d->pool) return;
    for (int i = 0; i < d->pool_size; i++) {
        if (d->pool[i].rgba_data) av_free(d->pool[i].rgba_data);
    }
    av_free(d->pool);
    av_free(d->free_stack);
    d->pool = NULL;
    d->free_stack = NULL;
    d->pool_size = 0;
    d->free_count = 0;
}

/* --- Decoder lifecycle --- */

static VideoDecoder *vd_alloc(void) {
    return (VideoDecoder *)av_mallocz(sizeof(VideoDecoder));
}

static void vd_free(VideoDecoder *d) {
    if (!d) return;
    if (d->codec_ctx)     avcodec_free_context(&d->codec_ctx);
    if (d->fmt_ctx)       avformat_close_input(&d->fmt_ctx);
    if (d->sws_ctx)       sws_freeContext(d->sws_ctx);
    if (d->decoded_frame) av_frame_free(&d->decoded_frame);
    if (d->pkt)           av_packet_free(&d->pkt);
    pool_free(d);
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

    d->stream_index = find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_VIDEO);
    if (d->stream_index < 0) return AVERROR_STREAM_NOT_FOUND;

    AVStream *st = d->fmt_ctx->streams[d->stream_index];
    d->time_base = st->time_base;

    const AVCodec *codec = NULL;
    if (st->codecpar->codec_id == AV_CODEC_ID_AV1) {
        codec = avcodec_find_decoder_by_name("libdav1d");
    }
    if (!codec) {
        codec = avcodec_find_decoder(st->codecpar->codec_id);
    }
    if (!codec) return AVERROR_DECODER_NOT_FOUND;

    d->codec_ctx = avcodec_alloc_context3(codec);
    if (!d->codec_ctx) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(d->codec_ctx, st->codecpar);
    if (ret < 0) return ret;

    /* Error tolerance for live streams / imperfect sources */
    d->codec_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    d->codec_ctx->err_recognition |= AV_EF_IGNORE_ERR;
    d->codec_ctx->get_format = choose_software_pix_fmt;

    /* Multi-threaded frame decoding — critical for 4K60 */
    d->codec_ctx->thread_count = 0;  /* auto = one thread per logical core */

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

    /* SWS_FAST_BILINEAR: x86 SIMD fast-path (MMXEXT), falls back to
       SWS_BILINEAR on non-x86 — see libswscale/utils.c:1215 */
    d->sws_ctx = sws_getContext(d->src_width, d->src_height, d->codec_ctx->pix_fmt,
                                 d->out_width, d->out_height, AV_PIX_FMT_RGBA,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!d->sws_ctx) return AVERROR(ENOMEM);

    ret = pool_init(d, VIDEO_FRAME_POOL_SIZE, d->out_width, d->out_height);
    if (ret < 0) return ret;

    d->last_emitted_pts_ms = INT64_MIN;
    return 0;
}

/* Decode one frame into a pooled VideoFrame slot. Returns pool index or -1.
 *
 * State machine: receive first (drain buffered frames), then read+send.
 * This ensures we never block on av_read_frame while the decoder still has
 * decoded frames waiting — critical for codecs that produce multiple frames
 * per packet (B-frames) and for live streams. */
static int vd_decode_into_pool(VideoDecoder *d) {
    if (!d) return -1;

    int drain_started = 0;

    for (;;) {
        /* 1. Always drain buffered frames first */
        int recv_ret = avcodec_receive_frame(d->codec_ctx, d->decoded_frame);
        if (recv_ret == 0) goto produce;
        if (recv_ret == AVERROR_EOF) return -1;

        /* 2. If EOF, start drain (send NULL packet) then loop back to step 1 */
        if (d->eof) {
            if (!drain_started) {
                drain_started = 1;
                avcodec_send_packet(d->codec_ctx, NULL);
            } else {
                return -1;  /* Drain complete, no more frames */
            }
            continue;
        }

        /* 3. Read next packet from input */
        int read_ret = av_read_frame(d->fmt_ctx, d->pkt);
        if (read_ret >= 0) {
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;
            }

            /* Send with EAGAIN retry */
            for (;;) {
                int send_ret = avcodec_send_packet(d->codec_ctx, d->pkt);
                if (send_ret == 0) break;
                if (send_ret == AVERROR(EAGAIN)) {
                    /* Decoder buffer full: drain one frame if available, then keep decoding. */
                    av_packet_unref(d->pkt);
                    int ret = avcodec_receive_frame(d->codec_ctx, d->decoded_frame);
                    if (ret == 0) goto produce;
                    continue;
                }
                break;  /* Other error, drop packet */
            }
            av_packet_unref(d->pkt);
            /* Loop back to step 1 — receive the frame we just fed */
            continue;
        }

        if (read_ret == AVERROR(EAGAIN)) {
            continue;  /* Temporary unavailability */
        }

        /* Real read error → EOF */
        d->eof = 1;
        /* Loop back to step 1, will enter drain */
    }

produce:
    {
        int64_t pts_ms = resolve_pts(d->decoded_frame, d->time_base, &d->fallback_pts_ms);
        int dur_ms     = resolve_dur(d->decoded_frame, d->time_base, d->default_duration_ms);

        if (d->min_frame_interval_ms > 0 &&
            d->last_emitted_pts_ms != INT64_MIN &&
            (pts_ms - d->last_emitted_pts_ms) < d->min_frame_interval_ms)
            return -1;

        int idx = pool_pop(d);
        if (idx < 0) return -1;  /* pool exhausted, caller must release frames first */

        /* sws_scale writes directly into the pooled frame's RGBA buffer.
           Pool buffers come from av_malloc (64-byte aligned on x86_64 —
           see libavutil/mem.c:65), optimal for sws SIMD fast-paths. */
        VideoFrame *vf = &d->pool[idx];
        uint8_t *dst[] = { vf->rgba_data, NULL, NULL, NULL };
        int dst_stride[] = { d->out_width * 4, 0, 0, 0 };
        int scaled = sws_scale(d->sws_ctx,
                               (const uint8_t *const *)d->decoded_frame->data,
                               d->decoded_frame->linesize,
                               0, d->src_height, dst, dst_stride);
        if (scaled <= 0) {
            pool_push(d, idx);
            return -1;
        }

        vf->width       = d->out_width;
        vf->height      = d->out_height;
        vf->pts_ms      = pts_ms;
        vf->duration_ms = dur_ms;

        d->last_emitted_pts_ms = pts_ms;
        return idx;
    }
}

/* Returns pool index → opaque handle. Caller releases with vd_release_frame. */
static int vd_read_frame_pool_idx(VideoDecoder *d) {
    if (!d) return -1;
    return vd_decode_into_pool(d);
}

static void vd_release_frame(VideoDecoder *d, int pool_idx) {
    if (!d || pool_idx < 0 || pool_idx >= d->pool_size) return;
    pool_push(d, pool_idx);
}

static VideoFrame *vd_get_frame(VideoDecoder *d, int pool_idx) {
    if (!d || pool_idx < 0 || pool_idx >= d->pool_size) return NULL;
    return &d->pool[pool_idx];
}

static void vd_rewind(VideoDecoder *d) {
    if (!d) return;
    d->eof = 0;
    d->fallback_pts_ms = 0;
    d->last_emitted_pts_ms = INT64_MIN;
    av_seek_frame(d->fmt_ctx, d->stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(d->codec_ctx);
}

/* ================================================================
 *  AudioDecoder
 * ================================================================ */

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

    d->stream_index = find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_AUDIO);
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
    av_log_set_level(AV_LOG_ERROR);
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
    avformat_network_init();
}

JNIEXPORT jstring JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_lastError
    (JNIEnv *env, jclass clazz)
{
    (void)clazz;
    const char *msg = g_last_error[0] ? g_last_error : "";
    return (*env)->NewStringUTF(env, msg);
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
    clear_last_error();
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return 0;

    VideoDecoder *d = vd_alloc();
    if (!d) { (*env)->ReleaseStringUTFChars(env, jpath, path); return 0; }

    int ret = vd_open(d, path, (int)tw, (int)th, (double)max_fps,
                       (int)tmo, (int)buf_kb, (int)recon);
    if (ret < 0) {
        set_last_error_from_code("videoOpen", path, ret);
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        vd_free(d);
        return 0;
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (jlong)(intptr_t)d;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * Method:    videoReadFrame
 *
 * Returns an opaque handle encoding both the decoder pointer and pool index.
 * Bit layout: [decoder_ptr (48 bits)] | [pool_index (16 bits)]
 * 0 on EOF or no frame available.
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoReadFrame
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    VideoDecoder *d = (VideoDecoder *)(intptr_t)handle;
    if (!d) return 0;

    int idx = vd_read_frame_pool_idx(d);
    if (idx < 0) return 0;

    /* Pack: upper bits = decoder, lower 16 bits = pool index */
    return (jlong)(((uintptr_t)d << 16) | (uint16_t)idx);
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
    uintptr_t packed = (uintptr_t)frame_handle;
    VideoDecoder *d = (VideoDecoder *)(packed >> 16);
    int idx = (int)(packed & 0xFFFF);

    VideoFrame *vf = vd_get_frame(d, idx);
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
    uintptr_t packed = (uintptr_t)frame_handle;
    VideoDecoder *d = (VideoDecoder *)(packed >> 16);
    int idx = (int)(packed & 0xFFFF);

    VideoFrame *vf = vd_get_frame(d, idx);
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
    uintptr_t packed = (uintptr_t)frame_handle;
    VideoDecoder *d = (VideoDecoder *)(packed >> 16);
    int idx = (int)(packed & 0xFFFF);

    vd_release_frame(d, idx);
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
 * Method:    videoGetDurationMs
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_videoGetDurationMs
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    VideoDecoder *d = (VideoDecoder *)(intptr_t)handle;
    if (!d || !d->fmt_ctx) return -1;
    int64_t dur = d->fmt_ctx->duration;
    if (dur <= 0 || dur == AV_NOPTS_VALUE) return -1;
    return (jlong)(dur / 1000);
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
    clear_last_error();
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return 0;

    AudioDecoder *d = ad_alloc();
    if (!d) { (*env)->ReleaseStringUTFChars(env, jpath, path); return 0; }

    int ret = ad_open(d, path, (int)tmo, (int)buf_kb, (int)recon);
    if (ret < 0) {
        set_last_error_from_code("audioOpen", path, ret);
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        ad_free(d);
        return 0;
    }
    (*env)->ReleaseStringUTFChars(env, jpath, path);
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

        int read_ret = av_read_frame(d->fmt_ctx, d->pkt);
        if (read_ret >= 0) {
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;
            }

            /* Send with EAGAIN retry */
            int sent = 0;
            for (;;) {
                int send_ret = avcodec_send_packet(d->codec_ctx, d->pkt);
                if (send_ret == 0) { sent = 1; break; }
                if (send_ret == AVERROR(EAGAIN)) {
                    /* Decoder input full — drain one frame and serve it */
                    if (ad_receive(d) > 0 && d->pending_bytes > d->pending_pos) {
                        av_packet_unref(d->pkt);
                        int avail = d->pending_bytes - d->pending_pos;
                        int copy  = (max_copy < avail) ? max_copy : avail;
                        if (copy > 0) {
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
                    continue;  /* Retry send after draining */
                }
                break;  /* Other error, drop packet */
            }
            av_packet_unref(d->pkt);

            /* Receive decoded audio after successful send */
            if (ad_receive(d) > 0 && d->pending_bytes > d->pending_pos) {
                int avail = d->pending_bytes - d->pending_pos;
                int copy  = (max_copy < avail) ? max_copy : avail;
                if (copy > 0) {
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
            continue;
        }

        if (read_ret == AVERROR(EAGAIN))
            return 0;  /* Temporary unavailability */

        d->eof = 1;
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
 * Method:    audioGetDurationMs
 */
JNIEXPORT jlong JNICALL Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_audioGetDurationMs
    (JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    AudioDecoder *d = (AudioDecoder *)(intptr_t)handle;
    if (!d || !d->fmt_ctx) return -1;
    int64_t dur = d->fmt_ctx->duration;
    if (dur <= 0 || dur == AV_NOPTS_VALUE) return -1;
    return (jlong)(dur / 1000);
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






