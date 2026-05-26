/**
 * am_ffmpeg.c — Pure C FFmpeg bridge for Java 25 FFM API (8.1 API).
 *
 * Replaces JavaCPP/JNI decoders. Plain C — callable via Linker.downcallHandle or jextract.
 *   - Video decoding (libavcodec + libavformat + libswscale → RGBA)
 *   - Audio decoding (libavcodec + libavformat + libswresample → S16LE 48 kHz stereo)
 *
 * Improvements over the original:
 *   - Frame pooling for video (zero malloc per frame after init)
 *   - av_find_best_stream for proper stream selection
 *   - Error-tolerance flags (SHOW_ALL, IGNORE_ERR) for live streams
 *   - Proper send/receive retry loops with EAGAIN handling
 */

#include "am_ffmpeg.h"
#define AM_BUILD_DLL
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* ================================================================
 *  Constants
 * ================================================================ */

#define AUDIO_OUT_SAMPLE_RATE  48000
#define AUDIO_OUT_SAMPLE_FMT   AV_SAMPLE_FMT_S16
#define AUDIO_OUT_CHANNELS     2
#define VIDEO_FRAME_POOL_SIZE  8

#define AP_FRAME_FMT_RGBA8888  0
#define AP_FRAME_FMT_YUV420P   1
#define AP_FRAME_FMT_NV12      2
#define AP_FRAME_FMT_YUV420P10LE 3
#define AP_FRAME_FMT_P010LE    4
#define AP_FRAME_FMT_RGBA16F   5

/* ================================================================
 *  Internal helpers
 * ================================================================ */

static int is_remote(const char *path) {
    if (!path) return 0;
    return strstr(path, "://") != NULL;
}

static char g_last_error[1024] = "";

/* Lightweight spin lock for JNI-side shared state (decoder registry / frame pool). */
static void spin_lock(volatile int *lock_var) {
    while (__sync_lock_test_and_set(lock_var, 1)) {
        while (*lock_var) { }
    }
}

static void spin_unlock(volatile int *lock_var) {
    __sync_lock_release(lock_var);
}

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

typedef enum {
    AP_HW_BACKEND_NONE = 0,
    AP_HW_BACKEND_D3D11VA = 1,
    AP_HW_BACKEND_NVDEC = 2,
    AP_HW_BACKEND_DXVA2 = 3,
    AP_HW_BACKEND_VIDEOTOOLBOX = 4
} ApHwBackend;

static const char *ap_hw_backend_name(ApHwBackend backend) {
    switch (backend) {
        case AP_HW_BACKEND_D3D11VA: return "d3d11va";
        case AP_HW_BACKEND_NVDEC: return "nvdec";
        case AP_HW_BACKEND_DXVA2: return "dxva2";
        case AP_HW_BACKEND_VIDEOTOOLBOX: return "videotoolbox";
        default: return "none";
    }
}

static ApHwBackend ap_parse_hw_preferred(const char *value) {
    if (!value || !value[0]) return AP_HW_BACKEND_NONE;
    if (!strcmp(value, "d3d11va")) return AP_HW_BACKEND_D3D11VA;
    if (!strcmp(value, "nvdec")) return AP_HW_BACKEND_NVDEC;
    if (!strcmp(value, "dxva2")) return AP_HW_BACKEND_DXVA2;
    if (!strcmp(value, "videotoolbox")) return AP_HW_BACKEND_VIDEOTOOLBOX;
    return AP_HW_BACKEND_NONE;
}

static enum AVHWDeviceType ap_backend_device_type(ApHwBackend backend) {
    switch (backend) {
        case AP_HW_BACKEND_D3D11VA: return AV_HWDEVICE_TYPE_D3D11VA;
        case AP_HW_BACKEND_NVDEC: return AV_HWDEVICE_TYPE_CUDA;
        case AP_HW_BACKEND_DXVA2: return AV_HWDEVICE_TYPE_DXVA2;
        case AP_HW_BACKEND_VIDEOTOOLBOX: return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        default: return AV_HWDEVICE_TYPE_NONE;
    }
}

static enum AVPixelFormat ap_backend_hw_pix_fmt(ApHwBackend backend) {
    switch (backend) {
        case AP_HW_BACKEND_D3D11VA: return AV_PIX_FMT_D3D11;
        case AP_HW_BACKEND_NVDEC: return AV_PIX_FMT_CUDA;
        case AP_HW_BACKEND_DXVA2: return AV_PIX_FMT_DXVA2_VLD;
        case AP_HW_BACKEND_VIDEOTOOLBOX: return AV_PIX_FMT_VIDEOTOOLBOX;
        default: return AV_PIX_FMT_NONE;
    }
}

/* ================================================================
 *  VideoDecoder — with frame pool
 * ================================================================ */

typedef struct {
    uint8_t *rgba_data;    /* width * height * 4, pre-allocated into pool */
    uint8_t *plane_data[4];
    int      plane_capacity[4];
    int      plane_size[4];
    int      plane_linesize[4];
    int      plane_pixel_stride[4];
    int      plane_count;
    int      pixel_format_tag;
    int      color_space;
    int      color_trc;
    int      color_primaries;
    int      color_range;
    int      source_pix_fmt;
    int      gpu_is_frame;
    int      gpu_backend_tag; /* 0=unknown,1=d3d11va,2=dxva2,3=nvdec,4=videotoolbox */
    int64_t  gpu_handle;      /* backend-specific native handle */
    int64_t  gpu_device_handle; /* e.g. ID3D11Device* */
    int      gpu_subresource; /* d3d11/dxva2 array index when applicable */
    int      gpu_surface_width;  /* actual backing surface width for interop */
    int      gpu_surface_height; /* actual backing surface height for interop */
#if defined(_WIN32)
    ID3D11Texture2D *gpu_interop_texture; /* RGBA intermediate texture for D3D11->GL interop */
    ID3D11VideoProcessorOutputView *gpu_interop_output_view;
#endif
    AVFrame *gpu_frame_ref;   /* hold HW surface lifetime for zero-copy interop */
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
    int               sws_src_width;
    int               sws_src_height;
    enum AVPixelFormat sws_src_fmt;

    int               hw_enabled;
    int               hw_nvdec_enabled;
    ApHwBackend       hw_preferred;
    enum AVHWDeviceType hw_device_type;
    enum AVPixelFormat hw_pix_fmt;
    AVBufferRef      *hw_device_ctx;
    int64_t           hw_device_handle;
    int               hw_active;
    char              hw_backend_name[24];
    char              hw_probe_detail[512];
    int               hw_zero_copy_logged;
    int               hw_get_format_logged;
#if defined(_WIN32)
    ID3D11VideoDevice *d3d11_video_device;
    ID3D11VideoContext *d3d11_video_context;
    ID3D11VideoProcessorEnumerator *d3d11_vp_enum;
    ID3D11VideoProcessor *d3d11_vp;
    UINT              d3d11_vp_in_w;
    UINT              d3d11_vp_in_h;
    DXGI_FORMAT       d3d11_vp_in_fmt;
    int               d3d11_vp_colorspace_logged;
#endif

    /* Frame pool — sws_scale writes directly into pooled buffers */
    int               pool_size;
    VideoFrame       *pool;           /* array[pool_size] */
    int              *free_stack;     /* stack of free indices */
    int              *in_use;         /* array[pool_size], 1=in use, 0=free */
    int               free_count;
    volatile int      pool_lock;
    volatile int      api_refs;
    volatile int      close_requested;

    int               eof;
    int64_t           fallback_pts_ms;
    int64_t           last_emitted_pts_ms;
} VideoDecoder;

#if defined(_WIN32)
static void ap_release_d3d11_video_processor(VideoDecoder *d);
static int ap_d3d11_convert_to_rgba_interop(VideoDecoder *d, VideoFrame *vf, const AVFrame *decoded_frame,
                                            int64_t *out_gpu_handle, int *out_surface_w, int *out_surface_h);
#ifndef AP_MAKEFOURCC
#define AP_MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((UINT)(uint8_t)(ch0) | ((UINT)(uint8_t)(ch1) << 8) | ((UINT)(uint8_t)(ch2) << 16) | ((UINT)(uint8_t)(ch3) << 24))
#endif

static UINT ap_d3d11_nominal_range_from_av(int color_range) {
    return color_range == AVCOL_RANGE_JPEG ? 2u : 1u; /* 2=0-255, 1=16-235 */
}

static UINT ap_d3d11_matrix_from_av(int color_space) {
    switch (color_space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return 1u; /* BT.709/HD matrix path */
        default:
            return 0u; /* BT.601/SD path */
    }
}

static UINT ap_d3d11_vp_fourcc_from_dxgi(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_P010: return AP_MAKEFOURCC('P', '0', '1', '0');
        case DXGI_FORMAT_NV12: return AP_MAKEFOURCC('N', 'V', '1', '2');
        default: return 0u; /* let driver use resource DXGI format */
    }
}
#endif

static void ap_hw_probe_reset(VideoDecoder *d) {
    if (!d) return;
    d->hw_probe_detail[0] = '\0';
}

static void ap_hw_probe_append(VideoDecoder *d, const char *msg) {
    if (!d || !msg || !msg[0]) return;
    size_t cur = strlen(d->hw_probe_detail);
    if (cur >= sizeof(d->hw_probe_detail) - 1) return;
    if (cur > 0) {
        d->hw_probe_detail[cur++] = ';';
        d->hw_probe_detail[cur++] = ' ';
        if (cur >= sizeof(d->hw_probe_detail) - 1) {
            d->hw_probe_detail[sizeof(d->hw_probe_detail) - 1] = '\0';
            return;
        }
    }
    snprintf(d->hw_probe_detail + cur, sizeof(d->hw_probe_detail) - cur, "%s", msg);
}

static void ap_hw_probe_appendf(VideoDecoder *d, const char *fmt, ...) {
    if (!d || !fmt) return;
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ap_hw_probe_append(d, line);
    fprintf(stderr, "[ApricityMediaDiag] %s\n", line);
    fflush(stderr);
}

static int ap_hw_backend_tag_from_device_type(enum AVHWDeviceType device_type) {
    switch (device_type) {
        case AV_HWDEVICE_TYPE_D3D11VA: return 1;
        case AV_HWDEVICE_TYPE_DXVA2: return 2;
        case AV_HWDEVICE_TYPE_CUDA: return 3;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return 4;
        default: return 0;
    }
}

static enum AVPixelFormat ap_hw_sw_pix_fmt_from_frame(const AVFrame *frame) {
    if (!frame || !frame->hw_frames_ctx || !frame->hw_frames_ctx->data) return AV_PIX_FMT_NONE;
    AVHWFramesContext *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    if (!frames) return AV_PIX_FMT_NONE;
    return frames->sw_format;
}

static int ap_frame_format_tag_from_pix_fmt(enum AVPixelFormat pix_fmt) {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P: return AP_FRAME_FMT_YUV420P;
        case AV_PIX_FMT_NV12: return AP_FRAME_FMT_NV12;
        case AV_PIX_FMT_YUV420P10LE: return AP_FRAME_FMT_YUV420P10LE;
        case AV_PIX_FMT_P010LE: return AP_FRAME_FMT_P010LE;
        default: return AP_FRAME_FMT_RGBA8888;
    }
}

static int ap_is_d3d11_hw_pix_fmt(enum AVPixelFormat fmt) {
    if (fmt == AV_PIX_FMT_D3D11) return 1;
#ifdef AV_PIX_FMT_D3D11VA_VLD
    if (fmt == AV_PIX_FMT_D3D11VA_VLD) return 1;
#endif
    return 0;
}

static int ap_can_zero_copy_interop(VideoDecoder *d, int gpu_backend_tag, const AVFrame *decoded_frame) {
#if defined(_WIN32)
    if (!d || !d->hw_active || gpu_backend_tag != 1 || d->hw_device_handle == 0 || !decoded_frame) return 0;
    enum AVPixelFormat sw_fmt = ap_hw_sw_pix_fmt_from_frame(decoded_frame);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sw_fmt);
    int rgba_compatible = 0;
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components == 4) {
        int max_depth = 0;
        for (int i = 0; i < desc->nb_components; i++) {
            if (desc->comp[i].depth > max_depth) max_depth = desc->comp[i].depth;
        }
        rgba_compatible = (max_depth > 0 && max_depth <= 8);
    }
    if (!rgba_compatible) {
        if (d->hw_zero_copy_logged == 0) {
            d->hw_zero_copy_logged = -1;
            ap_hw_probe_appendf(d,
                                "hardware zero-copy interop disabled backend=%s sw_fmt=%s reason=unsupported_dxgi_for_gl_direct_sample",
                                d->hw_backend_name[0] ? d->hw_backend_name : "unknown",
                                av_get_pix_fmt_name(sw_fmt) ? av_get_pix_fmt_name(sw_fmt) : "unknown");
        }
        return 0;
    }
    return 1;
#else
    (void)d;
    (void)gpu_backend_tag;
    (void)decoded_frame;
    return 0;
#endif
}

static enum AVPixelFormat choose_hw_or_software_pix_fmt(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    if (!pix_fmts) return AV_PIX_FMT_NONE;
    VideoDecoder *d = (VideoDecoder *)ctx->opaque;
    enum AVPixelFormat preferred = (d && d->hw_active) ? d->hw_pix_fmt : AV_PIX_FMT_NONE;
    enum AVPixelFormat hw_fallback = AV_PIX_FMT_NONE;
    if (preferred != AV_PIX_FMT_NONE) {
        for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == preferred) {
                if (d && d->hw_active && !d->hw_get_format_logged) {
                    d->hw_get_format_logged = 1;
                    char list[512] = {0};
                    int off = 0;
                    for (const enum AVPixelFormat *q = pix_fmts; *q != AV_PIX_FMT_NONE; q++) {
                        const char *name = av_get_pix_fmt_name(*q);
                        off += snprintf(list + off, sizeof(list) - off, "%s%s%s",
                                       name ? name : "?",
                                       (*q == preferred) ? "(match)" : "",
                                       (*(q + 1) != AV_PIX_FMT_NONE) ? ", " : "");
                        if (off >= (int)sizeof(list) - 1) break;
                    }
                    ap_hw_probe_appendf(d,
                            "get_format called preferred=%s offered=[%s] result=preferred_match",
                            av_get_pix_fmt_name(preferred) ? av_get_pix_fmt_name(preferred) : "none",
                            list[0] ? list : "empty");
                }
                return *p;
            }
            if (hw_fallback == AV_PIX_FMT_NONE) {
                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
                if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                    hw_fallback = *p;
                }
            }
        }
    }
    if (d && d->hw_active && hw_fallback != AV_PIX_FMT_NONE) {
        if (!d->hw_get_format_logged) {
            d->hw_get_format_logged = 1;
            char list[512] = {0};
            int off = 0;
            for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
                const char *name = av_get_pix_fmt_name(*p);
                off += snprintf(list + off, sizeof(list) - off, "%s%s%s",
                               name ? name : "?",
                               (*p == preferred) ? "(match)" : "",
                               (*(p + 1) != AV_PIX_FMT_NONE) ? ", " : "");
                if (off >= (int)sizeof(list) - 1) break;
            }
            ap_hw_probe_appendf(d,
                    "get_format called preferred=%s offered=[%s] result=hw_fallback(%s)",
                    av_get_pix_fmt_name(preferred) ? av_get_pix_fmt_name(preferred) : "none",
                    list[0] ? list : "empty",
                    av_get_pix_fmt_name(hw_fallback) ? av_get_pix_fmt_name(hw_fallback) : "unknown");
        }
        return hw_fallback;
    }
    if (d && d->hw_active && !d->hw_get_format_logged) {
        d->hw_get_format_logged = 1;
        char list[512] = {0};
        int off = 0;
        for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            const char *name = av_get_pix_fmt_name(*p);
            off += snprintf(list + off, sizeof(list) - off, "%s%s%s",
                           name ? name : "?",
                           (*p == preferred) ? "(match)" : "",
                           (*(p + 1) != AV_PIX_FMT_NONE) ? ", " : "");
            if (off >= (int)sizeof(list) - 1) break;
        }
        ap_hw_probe_appendf(d,
                "get_format called preferred=%s offered=[%s] result=%s",
                av_get_pix_fmt_name(preferred) ? av_get_pix_fmt_name(preferred) : "none",
                list[0] ? list : "empty",
                "software_fallback");
    }
    return choose_software_pix_fmt(ctx, pix_fmts);
}

static int codec_supports_hw_config(const AVCodec *codec,
                                    ApHwBackend backend,
                                    enum AVHWDeviceType device_type,
                                    enum AVPixelFormat hw_pix_fmt,
                                    enum AVPixelFormat *matched_hw_pix_fmt,
                                    int *matched_hw_methods)
{
    if (!codec || device_type == AV_HWDEVICE_TYPE_NONE || hw_pix_fmt == AV_PIX_FMT_NONE) return 0;
    const AVCodecHWConfig *best = NULL;
    int best_score = -1;
    for (int i = 0; ; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (!(cfg->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                              AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                              AV_CODEC_HW_CONFIG_METHOD_AD_HOC))) continue;
        if (backend == AP_HW_BACKEND_D3D11VA) {
            if (!(cfg->device_type == device_type || cfg->device_type == AV_HWDEVICE_TYPE_NONE)) continue;
            if (!ap_is_d3d11_hw_pix_fmt(cfg->pix_fmt)) continue;
        } else {
            if (cfg->device_type != device_type) continue;
            if (cfg->pix_fmt != hw_pix_fmt) continue;
        }
        /* Prefer modern hwdevice/hwframes configs over legacy AD_HOC entries. */
        int score = 0;
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) score += 4;
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) score += 2;
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_AD_HOC) score += 1;
        if (backend == AP_HW_BACKEND_D3D11VA && cfg->device_type == device_type) score += 1;
        if (score > best_score) {
            best = cfg;
            best_score = score;
        }
    }
    if (best) {
        if (matched_hw_pix_fmt) *matched_hw_pix_fmt = best->pix_fmt;
        if (matched_hw_methods) *matched_hw_methods = best->methods;
        return 1;
    }
    return 0;
}

static void ap_log_codec_hw_configs(VideoDecoder *d, const AVCodec *codec) {
    if (!d || !codec) return;
    int found = 0;
    for (int i = 0; ; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        found = 1;
        const char *dtype = av_hwdevice_get_type_name(cfg->device_type);
        const char *pfmt = av_get_pix_fmt_name(cfg->pix_fmt);
        const char *dname = dtype ? dtype : (cfg->device_type == AV_HWDEVICE_TYPE_NONE ? "none" : "unknown");
        ap_hw_probe_appendf(d,
                            "codec hwcfg[%d] device=%s pix_fmt=%s methods=0x%x",
                            i,
                            dname,
                            pfmt ? pfmt : "unknown",
                            cfg->methods);
    }
    if (!found) {
        ap_hw_probe_appendf(d, "codec has no hwcfg entries");
    }
}

static int vd_try_init_hw(VideoDecoder *d, const AVCodec *codec, ApHwBackend backend) {
    if (!d || !codec) return AVERROR(EINVAL);
    enum AVHWDeviceType device_type = ap_backend_device_type(backend);
    enum AVPixelFormat hw_pix_fmt = ap_backend_hw_pix_fmt(backend);
    enum AVPixelFormat matched_hw_pix_fmt = AV_PIX_FMT_NONE;
    int matched_hw_methods = 0;
    if (device_type == AV_HWDEVICE_TYPE_NONE || hw_pix_fmt == AV_PIX_FMT_NONE) return AVERROR(EINVAL);
    if (!codec_supports_hw_config(codec, backend, device_type, hw_pix_fmt, &matched_hw_pix_fmt, &matched_hw_methods)) return AVERROR(ENOSYS);
    if (backend == AP_HW_BACKEND_D3D11VA &&
        !(matched_hw_methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                                AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))) {
        ap_hw_probe_appendf(d,
                            "d3d11va matched AD_HOC-only hwcfg (pix_fmt=%s methods=0x%x), requires d3d11va2 hwaccel",
                            av_get_pix_fmt_name(matched_hw_pix_fmt) ? av_get_pix_fmt_name(matched_hw_pix_fmt) : "unknown",
                            matched_hw_methods);
        return AVERROR(ENOSYS);
    }

    AVBufferRef *device_ref = NULL;
    int ret = av_hwdevice_ctx_create(&device_ref, device_type, NULL, NULL, 0);
    if (ret < 0 || !device_ref) {
        if (device_ref) av_buffer_unref(&device_ref);
        return ret < 0 ? ret : AVERROR(EINVAL);
    }

    d->codec_ctx->hw_device_ctx = av_buffer_ref(device_ref);
    av_buffer_unref(&device_ref);
    if (!d->codec_ctx->hw_device_ctx) {
        return AVERROR(ENOMEM);
    }
    d->hw_device_ctx = av_buffer_ref(d->codec_ctx->hw_device_ctx);
    d->hw_device_type = device_type;
    d->hw_pix_fmt = matched_hw_pix_fmt != AV_PIX_FMT_NONE ? matched_hw_pix_fmt : hw_pix_fmt;
    d->hw_active = 1;
    d->hw_device_handle = 0;
#if defined(_WIN32)
    if (device_type == AV_HWDEVICE_TYPE_D3D11VA && d->hw_device_ctx && d->hw_device_ctx->data) {
        AVHWDeviceContext *hwdev = (AVHWDeviceContext *)d->hw_device_ctx->data;
        if (hwdev && hwdev->type == AV_HWDEVICE_TYPE_D3D11VA && hwdev->hwctx) {
            AVD3D11VADeviceContext *d3d11 = (AVD3D11VADeviceContext *)hwdev->hwctx;
            if (d3d11 && d3d11->device) {
                d->hw_device_handle = (int64_t)(intptr_t)d3d11->device;
            }
        }
    }
#endif
    snprintf(d->hw_backend_name, sizeof(d->hw_backend_name), "%s", ap_hw_backend_name(backend));
    return 0;
}

static void vd_setup_hw(VideoDecoder *d, const AVCodec *codec) {
    if (!d || !codec || !d->hw_enabled) return;
    ApHwBackend candidates[4];
    int count = 0;
    ap_hw_probe_reset(d);

#define AP_ADD_CANDIDATE(arr, cnt, val) \
    do { \
        int _exists = 0; \
        for (int _i = 0; _i < (cnt); _i++) { if ((arr)[_i] == (val)) { _exists = 1; break; } } \
        if (!_exists && (cnt) < (int)(sizeof(arr) / sizeof((arr)[0]))) { (arr)[(cnt)++] = (val); } \
    } while (0)

    if (d->hw_preferred != AP_HW_BACKEND_NONE) {
        AP_ADD_CANDIDATE(candidates, count, d->hw_preferred);
#if defined(_WIN32)
        if (d->hw_preferred == AP_HW_BACKEND_D3D11VA) {
            AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_DXVA2);
            if (d->hw_nvdec_enabled) AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_NVDEC);
        } else if (d->hw_preferred == AP_HW_BACKEND_DXVA2) {
            AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_D3D11VA);
            if (d->hw_nvdec_enabled) AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_NVDEC);
        } else if (d->hw_preferred == AP_HW_BACKEND_NVDEC) {
            AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_D3D11VA);
            AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_DXVA2);
        }
#endif
    } else {
#if defined(_WIN32)
        if (d->hw_nvdec_enabled) {
            AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_NVDEC);
        }
        AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_D3D11VA);
        AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_DXVA2);
#elif defined(__APPLE__)
        AP_ADD_CANDIDATE(candidates, count, AP_HW_BACKEND_VIDEOTOOLBOX);
#endif
    }

    ap_hw_probe_appendf(d, "hardware decode probing codec=%s preferred=%s",
                        codec->name ? codec->name : "unknown",
                        ap_hw_backend_name(d->hw_preferred));
    ap_log_codec_hw_configs(d, codec);

    for (int i = 0; i < count; i++) {
        ApHwBackend backend = candidates[i];
        if (backend == AP_HW_BACKEND_NVDEC && !d->hw_nvdec_enabled) continue;

        int ret = vd_try_init_hw(d, codec, backend);
        if (ret == 0) {
            ap_hw_probe_appendf(d, "hardware decode enabled backend=%s device=%s pix_fmt=%s",
                                d->hw_backend_name,
                                av_hwdevice_get_type_name(d->hw_device_type) ? av_hwdevice_get_type_name(d->hw_device_type) : "unknown",
                                av_get_pix_fmt_name(d->hw_pix_fmt) ? av_get_pix_fmt_name(d->hw_pix_fmt) : "unknown");
#undef AP_ADD_CANDIDATE
            return;
        }

        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        ap_hw_probe_appendf(d, "hardware backend probe failed backend=%s code=%d reason=%s",
                            ap_hw_backend_name(backend), ret, errbuf[0] ? errbuf : "unknown");
    }

    d->hw_active = 0;
    d->hw_device_type = AV_HWDEVICE_TYPE_NONE;
    d->hw_pix_fmt = AV_PIX_FMT_NONE;
    d->hw_device_handle = 0;
    snprintf(d->hw_backend_name, sizeof(d->hw_backend_name), "%s", "none");
    d->hw_zero_copy_logged = 0;
    d->hw_get_format_logged = 0;
    ap_hw_probe_appendf(d, "hardware decode unavailable, fallback to software");
#undef AP_ADD_CANDIDATE
}

/* --- Active decoder registry (prevents stale frame-handle deref after close) --- */
#define MAX_ACTIVE_DECODERS 256
static VideoDecoder *g_active_decoders[MAX_ACTIVE_DECODERS];
static int g_active_decoder_count = 0;
static volatile int g_decoder_registry_lock = 0;

static void decoder_registry_add(VideoDecoder *d) {
    if (!d) return;
    spin_lock(&g_decoder_registry_lock);
    if (g_active_decoder_count < MAX_ACTIVE_DECODERS) {
        g_active_decoders[g_active_decoder_count++] = d;
    }
    spin_unlock(&g_decoder_registry_lock);
}

static int decoder_registry_remove(VideoDecoder *d) {
    int removed = 0;
    if (!d) return 0;
    spin_lock(&g_decoder_registry_lock);
    for (int i = 0; i < g_active_decoder_count; i++) {
        if (g_active_decoders[i] == d) {
            g_active_decoders[i] = g_active_decoders[g_active_decoder_count - 1];
            g_active_decoders[g_active_decoder_count - 1] = NULL;
            g_active_decoder_count--;
            removed = 1;
            break;
        }
    }
    if (removed) {
        d->close_requested = 1;
    }
    spin_unlock(&g_decoder_registry_lock);
    return removed;
}

static VideoDecoder *decoder_registry_acquire(uintptr_t ptr) {
    VideoDecoder *d = NULL;
    spin_lock(&g_decoder_registry_lock);
    for (int i = 0; i < g_active_decoder_count; i++) {
        if ((uintptr_t)g_active_decoders[i] == ptr) {
            d = g_active_decoders[i];
            d->api_refs = __sync_add_and_fetch(&d->api_refs, 1);
            break;
        }
    }
    spin_unlock(&g_decoder_registry_lock);
    return d;
}

static void decoder_registry_release(VideoDecoder *d) {
    if (!d) return;
    __sync_sub_and_fetch(&d->api_refs, 1);
}

/* --- Pool helpers --- */

static int pool_pop(VideoDecoder *d) {
    int idx = -1;
    if (!d || !d->free_stack || !d->in_use) return -1;
    spin_lock(&d->pool_lock);
    while (d->free_count > 0) {
        idx = d->free_stack[--d->free_count];
        if (idx < 0 || idx >= d->pool_size) {
            idx = -1;
            continue;
        }
        if (d->in_use[idx]) {
            idx = -1;
            continue;
        }
        d->in_use[idx] = 1;
        break;
    }
    spin_unlock(&d->pool_lock);
    return idx;
}

static void pool_push(VideoDecoder *d, int idx) {
    if (!d || !d->free_stack || !d->in_use) return;
    if (idx < 0 || idx >= d->pool_size) return;
    spin_lock(&d->pool_lock);
    if (!d->in_use[idx]) {
        spin_unlock(&d->pool_lock);
        return; /* duplicate / stale release */
    }
    VideoFrame *vf = &d->pool[idx];
    if (vf->gpu_frame_ref) {
        av_frame_unref(vf->gpu_frame_ref);
    }
    vf->gpu_is_frame = 0;
    vf->gpu_backend_tag = 0;
    vf->gpu_handle = 0;
    vf->gpu_device_handle = 0;
    vf->gpu_subresource = 0;
    vf->gpu_surface_width = 0;
    vf->gpu_surface_height = 0;
    d->in_use[idx] = 0;
    if (d->free_count < d->pool_size) {
        d->free_stack[d->free_count++] = idx;
    }
    spin_unlock(&d->pool_lock);
}

static int pool_init(VideoDecoder *d, int count, int w, int h) {
    int cap = av_image_get_buffer_size(AV_PIX_FMT_RGBA, w, h, 1);
    if (cap <= 0) return AVERROR(EINVAL);

    d->pool_size = count;
    d->pool = (VideoFrame *)av_mallocz((size_t)count * sizeof(VideoFrame));
    d->free_stack = (int *)av_malloc((size_t)count * sizeof(int));
    d->in_use = (int *)av_mallocz((size_t)count * sizeof(int));
    if (!d->pool || !d->free_stack || !d->in_use) return AVERROR(ENOMEM);
    d->pool_lock = 0;

    for (int i = 0; i < count; i++) {
        d->pool[i].rgba_data = (uint8_t *)av_malloc(cap);
        if (!d->pool[i].rgba_data) return AVERROR(ENOMEM);
        d->pool[i].capacity = cap;
        for (int p = 0; p < 4; p++) {
            d->pool[i].plane_data[p] = NULL;
            d->pool[i].plane_capacity[p] = 0;
            d->pool[i].plane_size[p] = 0;
            d->pool[i].plane_linesize[p] = 0;
            d->pool[i].plane_pixel_stride[p] = 0;
        }
        d->pool[i].plane_count = 1;
        d->pool[i].pixel_format_tag = AP_FRAME_FMT_RGBA8888;
        d->pool[i].color_space = AVCOL_SPC_UNSPECIFIED;
        d->pool[i].color_trc = AVCOL_TRC_UNSPECIFIED;
        d->pool[i].color_primaries = AVCOL_PRI_UNSPECIFIED;
        d->pool[i].color_range = AVCOL_RANGE_UNSPECIFIED;
        d->pool[i].source_pix_fmt = AV_PIX_FMT_NONE;
        d->pool[i].gpu_is_frame = 0;
        d->pool[i].gpu_backend_tag = 0;
        d->pool[i].gpu_handle = 0;
        d->pool[i].gpu_device_handle = 0;
        d->pool[i].gpu_subresource = 0;
        d->pool[i].gpu_surface_width = 0;
        d->pool[i].gpu_surface_height = 0;
#if defined(_WIN32)
        d->pool[i].gpu_interop_texture = NULL;
        d->pool[i].gpu_interop_output_view = NULL;
#endif
        d->pool[i].gpu_frame_ref = av_frame_alloc();
        if (!d->pool[i].gpu_frame_ref) return AVERROR(ENOMEM);
        d->free_stack[i] = i;
    }
    d->free_count = count;
    return 0;
}

static void pool_free(VideoDecoder *d) {
    if (!d->pool) return;
    for (int i = 0; i < d->pool_size; i++) {
        if (d->pool[i].rgba_data) av_free(d->pool[i].rgba_data);
        if (d->pool[i].gpu_frame_ref) av_frame_free(&d->pool[i].gpu_frame_ref);
#if defined(_WIN32)
        if (d->pool[i].gpu_interop_output_view) {
            d->pool[i].gpu_interop_output_view->lpVtbl->Release(d->pool[i].gpu_interop_output_view);
            d->pool[i].gpu_interop_output_view = NULL;
        }
        if (d->pool[i].gpu_interop_texture) {
            d->pool[i].gpu_interop_texture->lpVtbl->Release(d->pool[i].gpu_interop_texture);
            d->pool[i].gpu_interop_texture = NULL;
        }
#endif
        for (int p = 0; p < 4; p++) {
            if (d->pool[i].plane_data[p] && d->pool[i].plane_data[p] != d->pool[i].rgba_data) {
                av_free(d->pool[i].plane_data[p]);
            }
        }
    }
    av_free(d->pool);
    av_free(d->free_stack);
    av_free(d->in_use);
    d->pool = NULL;
    d->free_stack = NULL;
    d->in_use = NULL;
    d->pool_size = 0;
    d->free_count = 0;
}

static int ensure_plane_capacity(VideoFrame *vf, int plane, int size) {
    if (!vf || plane < 0 || plane >= 4) return AVERROR(EINVAL);
    if (size <= 0) return AVERROR(EINVAL);
    if (vf->plane_data[plane] == vf->rgba_data) {
        vf->plane_data[plane] = NULL;
        vf->plane_capacity[plane] = 0;
    }
    if (vf->plane_data[plane] && vf->plane_capacity[plane] >= size) return 0;
    if (vf->plane_data[plane]) {
        av_free(vf->plane_data[plane]);
        vf->plane_data[plane] = NULL;
        vf->plane_capacity[plane] = 0;
    }
    vf->plane_data[plane] = (uint8_t *)av_malloc((size_t)size);
    if (!vf->plane_data[plane]) return AVERROR(ENOMEM);
    vf->plane_capacity[plane] = size;
    return 0;
}

static int copy_plane_rows(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
                           int row_bytes, int rows)
{
    if (!dst || !src || row_bytes <= 0 || rows <= 0) return AVERROR(EINVAL);
    if (dst_stride < row_bytes) return AVERROR(EINVAL);
    if (src_stride < 0) {
        src += (ptrdiff_t)(rows - 1) * (ptrdiff_t)src_stride;
    }
    for (int y = 0; y < rows; y++) {
        memcpy(dst + (ptrdiff_t)y * dst_stride, src + (ptrdiff_t)y * src_stride, (size_t)row_bytes);
    }
    return 0;
}

/* --- Decoder lifecycle --- */

static VideoDecoder *vd_alloc(void) {
    return (VideoDecoder *)av_mallocz(sizeof(VideoDecoder));
}

static void vd_free(VideoDecoder *d) {
    if (!d) return;
#if defined(_WIN32)
    ap_release_d3d11_video_processor(d);
#endif
    if (d->hw_device_ctx)  av_buffer_unref(&d->hw_device_ctx);
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
                   int tmo, int buf_kb, int recon,
                   int hw_enabled, int hw_nvdec_enabled, const char *hw_preferred)
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
        if (!hw_enabled) {
            codec = avcodec_find_decoder_by_name("libdav1d");
        }
        if (!codec) {
            codec = avcodec_find_decoder(st->codecpar->codec_id);
        }
    } else {
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
    d->codec_ctx->opaque = d;
    d->codec_ctx->get_format = choose_software_pix_fmt;

    d->hw_enabled = hw_enabled ? 1 : 0;
    d->hw_nvdec_enabled = hw_nvdec_enabled ? 1 : 0;
    d->hw_preferred = ap_parse_hw_preferred(hw_preferred);
    d->hw_active = 0;
    d->hw_device_type = AV_HWDEVICE_TYPE_NONE;
    d->hw_pix_fmt = AV_PIX_FMT_NONE;
    d->hw_device_ctx = NULL;
    d->hw_device_handle = 0;
    snprintf(d->hw_backend_name, sizeof(d->hw_backend_name), "%s", "none");
    ap_hw_probe_reset(d);
    if (d->hw_enabled) {
        vd_setup_hw(d, codec);
        if (d->hw_active) {
            d->codec_ctx->get_format = choose_hw_or_software_pix_fmt;
        }
    }

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

    d->sws_ctx = NULL;
    d->sws_src_width = 0;
    d->sws_src_height = 0;
    d->sws_src_fmt = AV_PIX_FMT_NONE;

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
        int gpu_is_frame = 0;
        int gpu_backend_tag = 0;
        int64_t gpu_handle = 0;
        int64_t gpu_device_handle = 0;
        int gpu_subresource = 0;
        int gpu_surface_width = 0;
        int gpu_surface_height = 0;
        if (d->hw_active && d->decoded_frame->format == d->hw_pix_fmt) {
            gpu_is_frame = 1;
            gpu_backend_tag = ap_hw_backend_tag_from_device_type(d->hw_device_type);
            gpu_surface_width = d->decoded_frame->width > 0 ? d->decoded_frame->width : d->out_width;
            gpu_surface_height = d->decoded_frame->height > 0 ? d->decoded_frame->height : d->out_height;
            if (gpu_backend_tag == 4) {
                if (d->decoded_frame->data[3]) {
                    gpu_handle = (int64_t)(intptr_t)d->decoded_frame->data[3];
                }
            } else if (d->decoded_frame->data[0]) {
                gpu_handle = (int64_t)(intptr_t)d->decoded_frame->data[0];
            }
            gpu_device_handle = d->hw_device_handle;
            if (d->decoded_frame->data[1]) {
                gpu_subresource = (int)(intptr_t)d->decoded_frame->data[1];
            }
#if defined(_WIN32)
            if (gpu_backend_tag == 1 && d->decoded_frame->data[0]) {
                ID3D11Texture2D *tex = (ID3D11Texture2D *)(intptr_t)d->decoded_frame->data[0];
                if (tex) {
                    D3D11_TEXTURE2D_DESC desc;
                    memset(&desc, 0, sizeof(desc));
                    tex->lpVtbl->GetDesc(tex, &desc);
                    if (desc.Width > 0) gpu_surface_width = (int)desc.Width;
                    if (desc.Height > 0) gpu_surface_height = (int)desc.Height;
                }
            }
#endif
            if (gpu_handle == 0 || gpu_backend_tag == 0) {
                gpu_is_frame = 0;
                gpu_backend_tag = 0;
                gpu_device_handle = 0;
                gpu_subresource = 0;
                gpu_surface_width = 0;
                gpu_surface_height = 0;
            }
        }

        int zero_copy_interop = ap_can_zero_copy_interop(d, gpu_backend_tag, d->decoded_frame);
        AVFrame *f = d->decoded_frame;
        AVFrame *sw_frame = NULL;
        if (d->hw_active && d->decoded_frame->format == d->hw_pix_fmt && !zero_copy_interop) {
            sw_frame = av_frame_alloc();
            if (!sw_frame) return -1;
            int tr = av_hwframe_transfer_data(sw_frame, d->decoded_frame, 0);
            if (tr < 0) {
                av_frame_free(&sw_frame);
                return -1;
            }
            sw_frame->pts = d->decoded_frame->pts;
            sw_frame->best_effort_timestamp = d->decoded_frame->best_effort_timestamp;
            sw_frame->duration = d->decoded_frame->duration;
            sw_frame->colorspace = d->decoded_frame->colorspace;
            sw_frame->color_trc = d->decoded_frame->color_trc;
            sw_frame->color_primaries = d->decoded_frame->color_primaries;
            sw_frame->color_range = d->decoded_frame->color_range;
            f = sw_frame;
        }

        int64_t pts_ms = resolve_pts(f, d->time_base, &d->fallback_pts_ms);
        int dur_ms     = resolve_dur(f, d->time_base, d->default_duration_ms);

        if (d->min_frame_interval_ms > 0 &&
            d->last_emitted_pts_ms != INT64_MIN &&
            (pts_ms - d->last_emitted_pts_ms) < d->min_frame_interval_ms) {
            if (sw_frame) av_frame_free(&sw_frame);
            return -1;
        }

        int idx = pool_pop(d);
        if (idx < 0) {
            if (sw_frame) av_frame_free(&sw_frame);
            return -1;  /* pool exhausted, caller must release frames first */
        }

        VideoFrame *vf = &d->pool[idx];
        if (vf->gpu_frame_ref) {
            av_frame_unref(vf->gpu_frame_ref);
        }

#if defined(_WIN32)
        if (!zero_copy_interop && gpu_is_frame && gpu_backend_tag == 1) {
            enum AVPixelFormat sw_fmt = ap_hw_sw_pix_fmt_from_frame(d->decoded_frame);
            int64_t interop_handle = 0;
            int interop_w = 0;
            int interop_h = 0;
            int cvt = ap_d3d11_convert_to_rgba_interop(d, vf, d->decoded_frame, &interop_handle, &interop_w, &interop_h);
            if (cvt == 0 && interop_handle != 0) {
                vf->width = d->decoded_frame->width > 0 ? d->decoded_frame->width : d->out_width;
                vf->height = d->decoded_frame->height > 0 ? d->decoded_frame->height : d->out_height;
                vf->pts_ms = pts_ms;
                vf->duration_ms = dur_ms;
                vf->pixel_format_tag = AP_FRAME_FMT_RGBA16F;
                vf->plane_count = 0;
                for (int p = 0; p < 4; p++) {
                    vf->plane_data[p] = NULL;
                    vf->plane_size[p] = 0;
                    vf->plane_linesize[p] = 0;
                    vf->plane_pixel_stride[p] = 0;
                }
                vf->color_space = d->decoded_frame->colorspace;
                vf->color_trc = d->decoded_frame->color_trc;
                vf->color_primaries = d->decoded_frame->color_primaries;
                vf->color_range = d->decoded_frame->color_range;
                vf->source_pix_fmt = sw_fmt != AV_PIX_FMT_NONE ? sw_fmt : d->decoded_frame->format;
                vf->gpu_is_frame = 1;
                vf->gpu_backend_tag = 1;
                vf->gpu_handle = interop_handle;
                vf->gpu_device_handle = d->hw_device_handle;
                vf->gpu_subresource = 0;
                vf->gpu_surface_width = interop_w > 0 ? interop_w : vf->width;
                vf->gpu_surface_height = interop_h > 0 ? interop_h : vf->height;
                d->last_emitted_pts_ms = pts_ms;
                if (sw_frame) av_frame_free(&sw_frame);
                return idx;
            }
        }
#endif

        if (zero_copy_interop) {
            enum AVPixelFormat sw_fmt = ap_hw_sw_pix_fmt_from_frame(d->decoded_frame);
            if (!d->hw_zero_copy_logged) {
                d->hw_zero_copy_logged = 1;
                av_log(NULL, AV_LOG_INFO,
                       "[ApricityMediaDiag] hardware zero-copy path active backend=%s hw_fmt=%s sw_fmt=%s\n",
                       d->hw_backend_name[0] ? d->hw_backend_name : "unknown",
                       av_get_pix_fmt_name(d->decoded_frame->format) ? av_get_pix_fmt_name(d->decoded_frame->format) : "unknown",
                       av_get_pix_fmt_name(sw_fmt) ? av_get_pix_fmt_name(sw_fmt) : "unknown");
            }
            vf->width = d->decoded_frame->width > 0 ? d->decoded_frame->width : d->out_width;
            vf->height = d->decoded_frame->height > 0 ? d->decoded_frame->height : d->out_height;
            vf->pts_ms = pts_ms;
            vf->duration_ms = dur_ms;
            if (gpu_backend_tag == 1) {
                /* D3D11 interop path presents VP-converted RGBA16F to GL. */
                vf->pixel_format_tag = AP_FRAME_FMT_RGBA16F;
            } else {
                vf->pixel_format_tag = ap_frame_format_tag_from_pix_fmt(sw_fmt);
            }
            vf->plane_count = 0;
            for (int p = 0; p < 4; p++) {
                vf->plane_data[p] = NULL;
                vf->plane_size[p] = 0;
                vf->plane_linesize[p] = 0;
                vf->plane_pixel_stride[p] = 0;
            }
            vf->color_space = d->decoded_frame->colorspace;
            vf->color_trc = d->decoded_frame->color_trc;
            vf->color_primaries = d->decoded_frame->color_primaries;
            vf->color_range = d->decoded_frame->color_range;
            vf->source_pix_fmt = sw_fmt != AV_PIX_FMT_NONE ? sw_fmt : d->decoded_frame->format;
            vf->gpu_is_frame = gpu_is_frame;
            vf->gpu_backend_tag = gpu_backend_tag;
            vf->gpu_handle = gpu_handle;
            vf->gpu_device_handle = gpu_device_handle;
            vf->gpu_subresource = gpu_subresource;
            vf->gpu_surface_width = gpu_surface_width;
            vf->gpu_surface_height = gpu_surface_height;
            if (vf->gpu_frame_ref && av_frame_ref(vf->gpu_frame_ref, d->decoded_frame) < 0) {
                pool_push(d, idx);
                return -1;
            }
            d->last_emitted_pts_ms = pts_ms;
            return idx;
        }

        /* sws_scale writes directly into the pooled frame's RGBA buffer.
           Pool buffers come from av_malloc (64-byte aligned on x86_64 —
           see libavutil/mem.c:65), optimal for sws SIMD fast-paths. */
        uint8_t *dst[] = { vf->rgba_data, NULL, NULL, NULL };
        int dst_stride[] = { d->out_width * 4, 0, 0, 0 };
        if (!d->sws_ctx ||
            d->sws_src_width != f->width ||
            d->sws_src_height != f->height ||
            d->sws_src_fmt != f->format)
        {
            if (d->sws_ctx) {
                sws_freeContext(d->sws_ctx);
                d->sws_ctx = NULL;
            }
            /* SWS_FAST_BILINEAR: x86 SIMD fast-path (MMXEXT), falls back to bilinear */
            d->sws_ctx = sws_getContext(
                    f->width, f->height, (enum AVPixelFormat)f->format,
                    d->out_width, d->out_height, AV_PIX_FMT_RGBA,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL);
            if (!d->sws_ctx) {
                pool_push(d, idx);
                if (sw_frame) av_frame_free(&sw_frame);
                return -1;
            }
            d->sws_src_width = f->width;
            d->sws_src_height = f->height;
            d->sws_src_fmt = (enum AVPixelFormat)f->format;
        }

        int scaled = sws_scale(d->sws_ctx,
                               (const uint8_t *const *)f->data,
                               f->linesize,
                               0, f->height, dst, dst_stride);
        if (scaled <= 0) {
            pool_push(d, idx);
            if (sw_frame) av_frame_free(&sw_frame);
            return -1;
        }

        vf->width          = d->out_width;
        vf->height         = d->out_height;
        vf->pts_ms         = pts_ms;
        vf->duration_ms    = dur_ms;
        if (vf->plane_data[0] && vf->plane_data[0] != vf->rgba_data) {
            av_free(vf->plane_data[0]);
            vf->plane_data[0] = NULL;
            vf->plane_capacity[0] = 0;
        }
        vf->pixel_format_tag = AP_FRAME_FMT_RGBA8888;
        vf->plane_count    = 1;
        vf->plane_data[0] = vf->rgba_data;
        vf->plane_capacity[0] = vf->capacity;
        vf->plane_size[0] = vf->width * vf->height * 4;
        vf->plane_linesize[0] = vf->width * 4;
        vf->plane_pixel_stride[0] = 4;
        vf->color_space = f->colorspace;
        vf->color_trc = f->color_trc;
        vf->color_primaries = f->color_primaries;
        vf->color_range = f->color_range;
        vf->source_pix_fmt = f->format;
        /* Non-zero-copy path exposes CPU frame only; do not surface stale GPU handles to Java. */
        vf->gpu_is_frame = 0;
        vf->gpu_backend_tag = 0;
        vf->gpu_handle = 0;
        vf->gpu_device_handle = 0;
        vf->gpu_subresource = 0;
        vf->gpu_surface_width = 0;
        vf->gpu_surface_height = 0;
        for (int p = 1; p < 4; p++) {
            vf->plane_size[p] = 0;
            vf->plane_linesize[p] = 0;
            vf->plane_pixel_stride[p] = 0;
        }

        int in_fmt = f->format;
        int allow_planar = (d->out_width == d->src_width) && (d->out_height == d->src_height);
        if (allow_planar && in_fmt == AV_PIX_FMT_YUV420P) {
            int y_w = f->width;
            int y_h = f->height;
            int uv_w = (y_w + 1) / 2;
            int uv_h = (y_h + 1) / 2;
            int y_stride = y_w;
            int uv_stride = uv_w;
            int y_bytes = y_stride * y_h;
            int u_bytes = uv_stride * uv_h;
            int v_bytes = uv_stride * uv_h;
            if (ensure_plane_capacity(vf, 0, y_bytes) == 0 &&
                ensure_plane_capacity(vf, 1, u_bytes) == 0 &&
                ensure_plane_capacity(vf, 2, v_bytes) == 0 &&
                copy_plane_rows(vf->plane_data[0], y_stride, f->data[0], f->linesize[0], y_w, y_h) == 0 &&
                copy_plane_rows(vf->plane_data[1], uv_stride, f->data[1], f->linesize[1], uv_w, uv_h) == 0 &&
                copy_plane_rows(vf->plane_data[2], uv_stride, f->data[2], f->linesize[2], uv_w, uv_h) == 0) {
                vf->pixel_format_tag = AP_FRAME_FMT_YUV420P;
                vf->plane_count = 3;
                vf->plane_size[0] = y_bytes;
                vf->plane_size[1] = u_bytes;
                vf->plane_size[2] = v_bytes;
                vf->plane_linesize[0] = y_stride;
                vf->plane_linesize[1] = uv_stride;
                vf->plane_linesize[2] = uv_stride;
                vf->plane_pixel_stride[0] = 1;
                vf->plane_pixel_stride[1] = 1;
                vf->plane_pixel_stride[2] = 1;
            }
        } else if (allow_planar && in_fmt == AV_PIX_FMT_NV12) {
            int y_w = f->width;
            int y_h = f->height;
            int uv_w = (y_w + 1) / 2;
            int uv_h = (y_h + 1) / 2;
            int y_stride = y_w;
            int uv_stride = uv_w * 2;
            int y_bytes = y_stride * y_h;
            int uv_bytes = uv_stride * uv_h;
            if (ensure_plane_capacity(vf, 0, y_bytes) == 0 &&
                ensure_plane_capacity(vf, 1, uv_bytes) == 0 &&
                copy_plane_rows(vf->plane_data[0], y_stride, f->data[0], f->linesize[0], y_w, y_h) == 0 &&
                copy_plane_rows(vf->plane_data[1], uv_stride, f->data[1], f->linesize[1], uv_w * 2, uv_h) == 0) {
                vf->pixel_format_tag = AP_FRAME_FMT_NV12;
                vf->plane_count = 2;
                vf->plane_size[0] = y_bytes;
                vf->plane_size[1] = uv_bytes;
                vf->plane_linesize[0] = y_stride;
                vf->plane_linesize[1] = uv_stride;
                vf->plane_pixel_stride[0] = 1;
                vf->plane_pixel_stride[1] = 2;
                vf->plane_size[2] = 0;
                vf->plane_linesize[2] = 0;
                vf->plane_pixel_stride[2] = 0;
            }
        } else if (allow_planar && in_fmt == AV_PIX_FMT_YUV420P10LE) {
            int y_w = f->width;
            int y_h = f->height;
            int uv_w = (y_w + 1) / 2;
            int uv_h = (y_h + 1) / 2;
            int y_stride = y_w * 2;
            int uv_stride = uv_w * 2;
            int y_bytes = y_stride * y_h;
            int u_bytes = uv_stride * uv_h;
            int v_bytes = uv_stride * uv_h;
            if (ensure_plane_capacity(vf, 0, y_bytes) == 0 &&
                ensure_plane_capacity(vf, 1, u_bytes) == 0 &&
                ensure_plane_capacity(vf, 2, v_bytes) == 0 &&
                copy_plane_rows(vf->plane_data[0], y_stride, f->data[0], f->linesize[0], y_w * 2, y_h) == 0 &&
                copy_plane_rows(vf->plane_data[1], uv_stride, f->data[1], f->linesize[1], uv_w * 2, uv_h) == 0 &&
                copy_plane_rows(vf->plane_data[2], uv_stride, f->data[2], f->linesize[2], uv_w * 2, uv_h) == 0) {
                vf->pixel_format_tag = AP_FRAME_FMT_YUV420P10LE;
                vf->plane_count = 3;
                vf->plane_size[0] = y_bytes;
                vf->plane_size[1] = u_bytes;
                vf->plane_size[2] = v_bytes;
                vf->plane_linesize[0] = y_stride;
                vf->plane_linesize[1] = uv_stride;
                vf->plane_linesize[2] = uv_stride;
                vf->plane_pixel_stride[0] = 2;
                vf->plane_pixel_stride[1] = 2;
                vf->plane_pixel_stride[2] = 2;
            }
        } else if (allow_planar && in_fmt == AV_PIX_FMT_P010LE) {
            int y_w = f->width;
            int y_h = f->height;
            int uv_w = (y_w + 1) / 2;
            int uv_h = (y_h + 1) / 2;
            int y_stride = y_w * 2;
            int uv_stride = uv_w * 4;
            int y_bytes = y_stride * y_h;
            int uv_bytes = uv_stride * uv_h;
            if (ensure_plane_capacity(vf, 0, y_bytes) == 0 &&
                ensure_plane_capacity(vf, 1, uv_bytes) == 0 &&
                copy_plane_rows(vf->plane_data[0], y_stride, f->data[0], f->linesize[0], y_w * 2, y_h) == 0 &&
                copy_plane_rows(vf->plane_data[1], uv_stride, f->data[1], f->linesize[1], uv_w * 4, uv_h) == 0) {
                vf->pixel_format_tag = AP_FRAME_FMT_P010LE;
                vf->plane_count = 2;
                vf->plane_size[0] = y_bytes;
                vf->plane_size[1] = uv_bytes;
                vf->plane_linesize[0] = y_stride;
                vf->plane_linesize[1] = uv_stride;
                vf->plane_pixel_stride[0] = 2;
                vf->plane_pixel_stride[1] = 4;
                vf->plane_size[2] = 0;
                vf->plane_linesize[2] = 0;
                vf->plane_pixel_stride[2] = 0;
            }
        }

        d->last_emitted_pts_ms = pts_ms;
        if (sw_frame) av_frame_free(&sw_frame);
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
    if (!d->in_use || !d->in_use[pool_idx]) return NULL;
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

static int vd_seek_ms(VideoDecoder *d, int64_t target_ms) {
    if (!d || !d->fmt_ctx || !d->codec_ctx) return AVERROR(EINVAL);
    if (target_ms < 0) target_ms = 0;
    int64_t ts = av_rescale_q(target_ms, (AVRational){1, 1000}, d->time_base);
    int ret = av_seek_frame(d->fmt_ctx, d->stream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        int64_t global_ts = av_rescale_q(target_ms, (AVRational){1, 1000}, AV_TIME_BASE_Q);
        ret = av_seek_frame(d->fmt_ctx, -1, global_ts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0) return ret;
    avcodec_flush_buffers(d->codec_ctx);
    d->eof = 0;
    d->fallback_pts_ms = target_ms;
    d->last_emitted_pts_ms = INT64_MIN;
    return 0;
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

static int ad_seek_ms(AudioDecoder *d, int64_t target_ms) {
    if (!d || !d->fmt_ctx || !d->codec_ctx) return AVERROR(EINVAL);
    if (target_ms < 0) target_ms = 0;
    AVStream *st = d->fmt_ctx->streams[d->stream_index];
    AVRational tb = (st && st->time_base.num > 0 && st->time_base.den > 0) ? st->time_base : (AVRational){1, 1000};
    int64_t ts = av_rescale_q(target_ms, (AVRational){1, 1000}, tb);
    int ret = av_seek_frame(d->fmt_ctx, d->stream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        int64_t global_ts = av_rescale_q(target_ms, (AVRational){1, 1000}, AV_TIME_BASE_Q);
        ret = av_seek_frame(d->fmt_ctx, -1, global_ts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0) return ret;
    avcodec_flush_buffers(d->codec_ctx);
    d->eof = 0;
    d->drain_started = 0;
    d->pending_bytes = 0;
    d->pending_pos = 0;
    swr_close(d->swr);
    swr_init(d->swr);
    return 0;
}

/* ================================================================
 *  Lifecycle
 * ================================================================ */

/* JNI_OnLoad removed — FFM API */

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_init()
{
    avformat_network_init();
}

const char* am_last_error()
{
    return g_last_error;
}

/* ================================================================
 *  Video
 * ================================================================ */

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
uint64_t am_video_open(const char* jpath, int tw, int th, double max_fps, int tmo, int buf_kb, int recon, int hw_enabled, int hw_nvdec_enabled, const char* jhw_preferred)
{
    clear_last_error();
    const char *path = jpath;
    if (!path) return 0;
    const char *hw_preferred = NULL;
    if (jhw_preferred) {
        hw_preferred = jhw_preferred;
    }

    VideoDecoder *d = vd_alloc();
    if (!d) {
        if (hw_preferred) 
        
        return 0;
    }

    int ret = vd_open(d, path, (int)tw, (int)th, (double)max_fps,
                       (int)tmo, (int)buf_kb, (int)recon,
                       (int)hw_enabled, (int)hw_nvdec_enabled, hw_preferred ? hw_preferred : "auto");
    if (ret < 0) {
        set_last_error_from_code("videoOpen", path, ret);
        if (hw_preferred) 
        
        vd_free(d);
        return 0;
    }
    if (hw_preferred) 
    
    d->api_refs = 0;
    d->close_requested = 0;
    decoder_registry_add(d);
    return (int64_t)(intptr_t)d;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 *
 * Returns an opaque handle encoding both the decoder pointer and pool index.
 * Bit layout: [decoder_ptr (48 bits)] | [pool_index (16 bits)]
 * 0 on EOF or no frame available.
 */
uint64_t am_video_read_frame(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) return 0;

    int idx = vd_read_frame_pool_idx(d);
    if (idx < 0) {
        decoder_registry_release(d);
        return 0;
    }

    /* Pack: upper bits = decoder, lower 16 bits = pool index */
    int64_t out = (int64_t)(((uintptr_t)d << 16) | (uint16_t)idx);
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 * Signature: (J[J)I  — info[4] = {width, height, ptsMs, durationMs}
 */
int am_video_frame_get_info(uint64_t frame, int64_t* jinfo)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);

    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf || !jinfo) {
        decoder_registry_release(d);
        return 0;
    }

    int64_t info[4];
    info[0] = (int64_t)vf->width;
    info[1] = (int64_t)vf->height;
    info[2] = (int64_t)vf->pts_ms;
    info[3] = (int64_t)vf->duration_ms;
    jinfo[0] = info[0]; jinfo[1] = info[1]; jinfo[2] = info[2]; jinfo[3] = info[3];
    int out = (int)(vf->width * vf->height * 4);
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void* am_video_frame_get_pixels(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);

    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf || !vf->rgba_data || vf->gpu_is_frame) {
        decoder_registry_release(d);
        return NULL;
    }
    vf->rgba_data;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_frame_get_pixel_format(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return AP_FRAME_FMT_RGBA8888;
    }
    int out = (int)vf->pixel_format_tag;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_frame_is_gpu(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int out = (vf->gpu_is_frame && vf->gpu_backend_tag > 0 && vf->gpu_handle != 0) ? 1 : 0;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_frame_get_gpu_backend(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int out = (int)vf->gpu_backend_tag;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
uint64_t am_video_frame_get_gpu_handle(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int64_t out = (int64_t)vf->gpu_handle;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_frame_get_gpu_subresource(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int out = (int)vf->gpu_subresource;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 * Signature: (J[I)I  info[2] = {surfaceWidth, surfaceHeight}
 */
int am_video_frame_get_gpu_surface_info(uint64_t frame, int* jinfo)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf || !jinfo) {
        decoder_registry_release(d);
        return 0;
    }
    int out_info[2];
    out_info[0] = (int)(vf->gpu_surface_width > 0 ? vf->gpu_surface_width : vf->width);
    out_info[1] = (int)(vf->gpu_surface_height > 0 ? vf->gpu_surface_height : vf->height);
    
    decoder_registry_release(d);
    return 1;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_frame_get_plane_count(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int out = (int)vf->plane_count;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 * Signature: (JI[I)I info[3] = {rowStride, pixelStride, planeBytes}
 */
int am_video_frame_get_plane_info(uint64_t frame, int plane_index, int* jinfo)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return 0;
    }
    int plane = (int)plane_index;
    if (plane < 0 || plane >= vf->plane_count || plane >= 4 || !jinfo) {
        decoder_registry_release(d);
        return 0;
    }

    int info[3];
    info[0] = (int)vf->plane_linesize[plane];
    info[1] = (int)vf->plane_pixel_stride[plane];
    info[2] = (int)vf->plane_size[plane];
    
    int out = (int)vf->plane_size[plane];
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void* am_video_frame_get_plane_buffer(uint64_t frame, int plane_index)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return NULL;
    }
    int plane = (int)plane_index;
    if (plane < 0 || plane >= vf->plane_count || plane >= 4) {
        decoder_registry_release(d);
        return NULL;
    }

    uint8_t *ptr = vf->plane_data[plane];
    int size = vf->plane_size[plane];
    if (!ptr || size <= 0) {
        decoder_registry_release(d);
        return NULL;
    }
    ptrsize);
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 * Signature: (J[I)I info[4] = {colorspace, color_trc, color_primaries, color_range}
 */
int am_video_frame_get_color_info(uint64_t frame, int* jinfo)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf || !jinfo) {
        decoder_registry_release(d);
        return 0;
    }
    int info[4];
    info[0] = (int)vf->color_space;
    info[1] = (int)vf->color_trc;
    info[2] = (int)vf->color_primaries;
    info[3] = (int)vf->color_range;
    
    decoder_registry_release(d);
    return 1;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
const char* am_video_frame_get_source_pixel_format(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    VideoFrame *vf = vd_get_frame(d, idx);
    if (!d || !vf) {
        decoder_registry_release(d);
        return "unknown";
    }
    const char *name = av_get_pix_fmt_name((enum AVPixelFormat)vf->source_pix_fmt);
    if (!name || !name[0]) name = "unknown";
    name;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_video_frame_release(uint64_t frame)
{
    uintptr_t packed = (uintptr_t)frame;
    VideoDecoder *d = decoder_registry_acquire(packed >> 16);
    int idx = (int)(packed & 0xFFFF);
    if (!d) return;
    vd_release_frame(d, idx);
    decoder_registry_release(d);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_video_rewind(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) return;
    vd_rewind(d);
    decoder_registry_release(d);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_seek_ms(uint64_t decoder, int64_t target_ms)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) return 0;
    int ret = vd_seek_ms(d, (int64_t)target_ms);
    decoder_registry_release(d);
    return ret >= 0 ? 1 : 0;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
uint64_t am_video_get_duration_ms(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d || !d->fmt_ctx) {
        decoder_registry_release(d);
        return -1;
    }
    int64_t dur = d->fmt_ctx->duration;
    if (dur <= 0 || dur == AV_NOPTS_VALUE) {
        decoder_registry_release(d);
        return -1;
    }
    int64_t out = (int64_t)(dur / 1000);
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_video_is_hardware_decode(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) {
        decoder_registry_release(d);
        return 0;
    }
    int out = d->hw_active ? 1 : 0;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
const char* am_video_get_hardware_backend(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) {
        decoder_registry_release(d);
        return "unknown";
    }
    const char *name = (d->hw_backend_name[0] != '\0') ? d->hw_backend_name : "none";
    name;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
const char* am_video_get_hardware_probe_message(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) {
        decoder_registry_release(d);
        return "";
    }
    const char *msg = d->hw_probe_detail[0] ? d->hw_probe_detail : "";
    msg;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
uint64_t am_video_get_hardware_device_handle(uint64_t decoder)
{
    VideoDecoder *d = decoder_registry_acquire((uintptr_t)(intptr_t)decoder);
    if (!d) {
        decoder_registry_release(d);
        return 0;
    }
    int64_t out = (int64_t)d->hw_device_handle;
    decoder_registry_release(d);
    return out;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_video_close(uint64_t decoder)
{
    VideoDecoder *d = (VideoDecoder *)(intptr_t)decoder;
    if (!d) return;
    if (!decoder_registry_remove(d)) {
        return; /* already closed / stale handle */
    }
    while (__sync_add_and_fetch(&d->api_refs, 0) > 0) {
        av_usleep(1000);
    }
    vd_free(d);
}

/* ================================================================
 *  Audio
 * ================================================================ */

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
uint64_t am_audio_open(const char* jpath, int tmo, int buf_kb, int recon)
{
    clear_last_error();
    const char *path = jpath;
    if (!path) return 0;

    AudioDecoder *d = ad_alloc();
    if (!d) {  return 0; }

    int ret = ad_open(d, path, (int)tmo, (int)buf_kb, (int)recon);
    if (ret < 0) {
        set_last_error_from_code("audioOpen", path, ret);
        
        ad_free(d);
        return 0;
    }
    
    return (int64_t)(intptr_t)d;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_audio_read_pcm(uint64_t decoder, uint8_t* jbuf, int offset, int length)
{
    AudioDecoder *d = (AudioDecoder *)(intptr_t)decoder;
    if (!d || !jbuf || length <= 0) return -2;
    int max_copy = length;
    /* Serve from pending */
    if (d->pending_bytes > d->pending_pos) {
        int avail = d->pending_bytes - d->pending_pos;
        int copy  = (max_copy < avail) ? max_copy : avail;
        memcpy(jbuf + offset, d->out_buffer + d->pending_pos, copy);
        d->pending_pos += copy;
        if (d->pending_pos >= d->pending_bytes) {
            d->pending_pos   = 0;
            d->pending_bytes = 0;
        }
        return (int)copy;
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

            memcpy(jbuf + offset, d->out_buffer + d->pending_pos, copy);
            d->pending_pos += copy;
            if (d->pending_pos >= d->pending_bytes) {
                d->pending_pos   = 0;
                d->pending_bytes = 0;
            }
            return (int)copy;
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
                            memcpy(jbuf + offset, d->out_buffer + d->pending_pos, copy);
                            d->pending_pos += copy;
                            if (d->pending_pos >= d->pending_bytes) {
                                d->pending_pos   = 0;
                                d->pending_bytes = 0;
                            }
                            return (int)copy;
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
                    memcpy(jbuf + offset, d->out_buffer + d->pending_pos, copy);
                    d->pending_pos += copy;
                    if (d->pending_pos >= d->pending_bytes) {
                        d->pending_pos   = 0;
                        d->pending_bytes = 0;
                    }
                    return (int)copy;
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
 * FFM API
 */
int am_audio_sample_rate(uint64_t decoder)
{
    (void)decoder;
    return AUDIO_OUT_SAMPLE_RATE;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_audio_channels(uint64_t decoder)
{
    (void)decoder;
    return AUDIO_OUT_CHANNELS;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_audio_rewind(uint64_t decoder)
{
    ad_rewind((AudioDecoder *)(intptr_t)decoder);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int am_audio_seek_ms(uint64_t decoder, int64_t target_ms)
{
    AudioDecoder *d = (AudioDecoder *)(intptr_t)decoder;
    if (!d) return 0;
    int ret = ad_seek_ms(d, (int64_t)target_ms);
    return ret >= 0 ? 1 : 0;
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
int64_t am_audio_get_duration_ms(uint64_t decoder)
{
    AudioDecoder *d = (AudioDecoder *)(intptr_t)decoder;
    if (!d || !d->fmt_ctx) return -1;
    int64_t dur = d->fmt_ctx->duration;
    if (dur <= 0 || dur == AV_NOPTS_VALUE) return -1;
    return (int64_t)(dur / 1000);
}

/*
 * Class:     cc_sighs_apricitymedia_jni_ApricityMediaNative
 * FFM API
 */
void am_audio_close(uint64_t decoder)
{
    ad_free((AudioDecoder *)(intptr_t)decoder);
}

#if defined(_WIN32)
static void ap_release_d3d11_video_processor(VideoDecoder *d) {
    if (!d) return;
    if (d->d3d11_vp) {
        d->d3d11_vp->lpVtbl->Release(d->d3d11_vp);
        d->d3d11_vp = NULL;
    }
    if (d->d3d11_vp_enum) {
        d->d3d11_vp_enum->lpVtbl->Release(d->d3d11_vp_enum);
        d->d3d11_vp_enum = NULL;
    }
    if (d->d3d11_video_context) {
        d->d3d11_video_context->lpVtbl->Release(d->d3d11_video_context);
        d->d3d11_video_context = NULL;
    }
    if (d->d3d11_video_device) {
        d->d3d11_video_device->lpVtbl->Release(d->d3d11_video_device);
        d->d3d11_video_device = NULL;
    }
    d->d3d11_vp_in_w = 0;
    d->d3d11_vp_in_h = 0;
    d->d3d11_vp_in_fmt = DXGI_FORMAT_UNKNOWN;
    d->d3d11_vp_colorspace_logged = 0;
}

static int ap_ensure_d3d11_video_processor(VideoDecoder *d, ID3D11Texture2D *src_tex, UINT src_w, UINT src_h) {
    if (!d || !src_tex || src_w == 0 || src_h == 0) return AVERROR(EINVAL);
    if (!d->d3d11_video_device || !d->d3d11_video_context) {
        if (!d->hw_device_ctx || !d->hw_device_ctx->data) return AVERROR(EINVAL);
        AVHWDeviceContext *hwdev = (AVHWDeviceContext *)d->hw_device_ctx->data;
        if (!hwdev || hwdev->type != AV_HWDEVICE_TYPE_D3D11VA || !hwdev->hwctx) return AVERROR(EINVAL);
        AVD3D11VADeviceContext *d3d11 = (AVD3D11VADeviceContext *)hwdev->hwctx;
        if (!d3d11 || !d3d11->video_device || !d3d11->video_context) return AVERROR(EINVAL);
        d->d3d11_video_device = d3d11->video_device;
        d->d3d11_video_context = d3d11->video_context;
        d->d3d11_video_device->lpVtbl->AddRef(d->d3d11_video_device);
        d->d3d11_video_context->lpVtbl->AddRef(d->d3d11_video_context);
    }

    D3D11_TEXTURE2D_DESC src_desc;
    memset(&src_desc, 0, sizeof(src_desc));
    src_tex->lpVtbl->GetDesc(src_tex, &src_desc);

    if (d->d3d11_vp && d->d3d11_vp_enum &&
        d->d3d11_vp_in_w == src_w &&
        d->d3d11_vp_in_h == src_h &&
        d->d3d11_vp_in_fmt == src_desc.Format) {
        return 0;
    }

    if (d->d3d11_vp) {
        d->d3d11_vp->lpVtbl->Release(d->d3d11_vp);
        d->d3d11_vp = NULL;
    }
    if (d->d3d11_vp_enum) {
        d->d3d11_vp_enum->lpVtbl->Release(d->d3d11_vp_enum);
        d->d3d11_vp_enum = NULL;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content;
    memset(&content, 0, sizeof(content));
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = src_w;
    content.InputHeight = src_h;
    content.OutputWidth = src_w;
    content.OutputHeight = src_h;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = d->d3d11_video_device->lpVtbl->CreateVideoProcessorEnumerator(
            d->d3d11_video_device, &content, &d->d3d11_vp_enum);
    if (FAILED(hr) || !d->d3d11_vp_enum) return AVERROR_EXTERNAL;

    hr = d->d3d11_video_device->lpVtbl->CreateVideoProcessor(
            d->d3d11_video_device, d->d3d11_vp_enum, 0, &d->d3d11_vp);
    if (FAILED(hr) || !d->d3d11_vp) return AVERROR_EXTERNAL;

    d->d3d11_vp_in_w = src_w;
    d->d3d11_vp_in_h = src_h;
    d->d3d11_vp_in_fmt = src_desc.Format;
    return 0;
}

static int ap_ensure_interop_output_slot(VideoDecoder *d, VideoFrame *vf, UINT out_w, UINT out_h) {
    if (!d || !vf || out_w == 0 || out_h == 0) return AVERROR(EINVAL);
    int recreate = 0;
    if (!vf->gpu_interop_texture || !vf->gpu_interop_output_view) {
        recreate = 1;
    } else {
        D3D11_TEXTURE2D_DESC cur_desc;
        memset(&cur_desc, 0, sizeof(cur_desc));
        vf->gpu_interop_texture->lpVtbl->GetDesc(vf->gpu_interop_texture, &cur_desc);
        if (cur_desc.Width != out_w || cur_desc.Height != out_h || cur_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            recreate = 1;
        }
    }
    if (!recreate) return 0;

    if (vf->gpu_interop_output_view) {
        vf->gpu_interop_output_view->lpVtbl->Release(vf->gpu_interop_output_view);
        vf->gpu_interop_output_view = NULL;
    }
    if (vf->gpu_interop_texture) {
        vf->gpu_interop_texture->lpVtbl->Release(vf->gpu_interop_texture);
        vf->gpu_interop_texture = NULL;
    }

    D3D11_TEXTURE2D_DESC out_desc;
    memset(&out_desc, 0, sizeof(out_desc));
    out_desc.Width = out_w;
    out_desc.Height = out_h;
    out_desc.MipLevels = 1;
    out_desc.ArraySize = 1;
    out_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    out_desc.SampleDesc.Count = 1;
    out_desc.Usage = D3D11_USAGE_DEFAULT;
    out_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    out_desc.CPUAccessFlags = 0;
    out_desc.MiscFlags = 0;

    ID3D11Device *dev = (ID3D11Device *)(intptr_t)d->hw_device_handle;
    if (!dev) return AVERROR(EINVAL);
    HRESULT hr = dev->lpVtbl->CreateTexture2D(dev, &out_desc, NULL, &vf->gpu_interop_texture);
    if (FAILED(hr) || !vf->gpu_interop_texture) return AVERROR_EXTERNAL;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc;
    memset(&out_view_desc, 0, sizeof(out_view_desc));
    out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_view_desc.Texture2D.MipSlice = 0;
    hr = d->d3d11_video_device->lpVtbl->CreateVideoProcessorOutputView(
            d->d3d11_video_device,
            (ID3D11Resource *)vf->gpu_interop_texture,
            d->d3d11_vp_enum,
            &out_view_desc,
            &vf->gpu_interop_output_view);
    if (FAILED(hr) || !vf->gpu_interop_output_view) return AVERROR_EXTERNAL;
    return 0;
}

static int ap_d3d11_convert_to_rgba_interop(VideoDecoder *d, VideoFrame *vf, const AVFrame *decoded_frame,
                                            int64_t *out_gpu_handle, int *out_surface_w, int *out_surface_h)
{
    if (!d || !vf || !decoded_frame || !out_gpu_handle || !out_surface_w || !out_surface_h) return AVERROR(EINVAL);
    if (!decoded_frame->data[0]) return AVERROR(EINVAL);
    ID3D11Texture2D *src_tex = (ID3D11Texture2D *)(intptr_t)decoded_frame->data[0];
    UINT src_slice = decoded_frame->data[1] ? (UINT)(intptr_t)decoded_frame->data[1] : 0;
    UINT visible_w = decoded_frame->width > 0 ? (UINT)decoded_frame->width : 0;
    UINT visible_h = decoded_frame->height > 0 ? (UINT)decoded_frame->height : 0;
    if (visible_w == 0 || visible_h == 0) return AVERROR(EINVAL);

    D3D11_TEXTURE2D_DESC src_desc;
    memset(&src_desc, 0, sizeof(src_desc));
    src_tex->lpVtbl->GetDesc(src_tex, &src_desc);
    int ret = ap_ensure_d3d11_video_processor(d, src_tex, src_desc.Width, src_desc.Height);
    if (ret < 0) return ret;
    ret = ap_ensure_interop_output_slot(d, vf, visible_w, visible_h);
    if (ret < 0) return ret;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc;
    memset(&in_desc, 0, sizeof(in_desc));
    in_desc.FourCC = ap_d3d11_vp_fourcc_from_dxgi(src_desc.Format);
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_desc.Texture2D.MipSlice = 0;
    in_desc.Texture2D.ArraySlice = src_slice;

    ID3D11VideoProcessorInputView *in_view = NULL;
    HRESULT hr = d->d3d11_video_device->lpVtbl->CreateVideoProcessorInputView(
            d->d3d11_video_device,
            (ID3D11Resource *)src_tex,
            d->d3d11_vp_enum,
            &in_desc,
            &in_view);
    if (FAILED(hr) || !in_view) return AVERROR_EXTERNAL;

    RECT src_rect = {0, 0, (LONG)visible_w, (LONG)visible_h};
    RECT dst_rect = {0, 0, (LONG)visible_w, (LONG)visible_h};
    d->d3d11_video_context->lpVtbl->VideoProcessorSetStreamSourceRect(d->d3d11_video_context, d->d3d11_vp, 0, TRUE, &src_rect);
    d->d3d11_video_context->lpVtbl->VideoProcessorSetStreamDestRect(d->d3d11_video_context, d->d3d11_vp, 0, TRUE, &dst_rect);
    d->d3d11_video_context->lpVtbl->VideoProcessorSetOutputTargetRect(d->d3d11_video_context, d->d3d11_vp, TRUE, &dst_rect);

    /* Explicitly pin VP into matrix/range CSC behavior on legacy color-space API.
       This API has no HDR transfer metadata (PQ/HLG), so HDR tone mapping is not done here. */
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE stream_cs;
    memset(&stream_cs, 0, sizeof(stream_cs));
    stream_cs.Usage = 0;
    stream_cs.RGB_Range = 0;
    stream_cs.YCbCr_Matrix = ap_d3d11_matrix_from_av(decoded_frame->colorspace);
    stream_cs.YCbCr_xvYCC = 0;
    stream_cs.Nominal_Range = ap_d3d11_nominal_range_from_av(decoded_frame->color_range);
    d->d3d11_video_context->lpVtbl->VideoProcessorSetStreamColorSpace(
            d->d3d11_video_context, d->d3d11_vp, 0, &stream_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs;
    memset(&output_cs, 0, sizeof(output_cs));
    output_cs.Usage = 0;
    /* D3D11 spec: RGB_Range = 0(full), 1(limited). */
    output_cs.RGB_Range = 0;
    output_cs.YCbCr_Matrix = stream_cs.YCbCr_Matrix;
    output_cs.YCbCr_xvYCC = 0;
    output_cs.Nominal_Range = 2; /* 0-255 */
    d->d3d11_video_context->lpVtbl->VideoProcessorSetOutputColorSpace(
            d->d3d11_video_context, d->d3d11_vp, &output_cs);

    if (!d->d3d11_vp_colorspace_logged) {
        d->d3d11_vp_colorspace_logged = 1;
        fprintf(stderr,
                "[ApricityMediaDiag] d3d11 vp colorspace mode=csc_only streamMatrix=%u streamRange=%u outputRange=%u outputFmt=R16G16B16A16_FLOAT inputDxgi=%d fourcc=0x%08x\n",
                (unsigned)stream_cs.YCbCr_Matrix,
                (unsigned)stream_cs.Nominal_Range,
                (unsigned)output_cs.Nominal_Range,
                (int)src_desc.Format,
                (unsigned)in_desc.FourCC);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream;
    memset(&stream, 0, sizeof(stream));
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = in_view;

    hr = d->d3d11_video_context->lpVtbl->VideoProcessorBlt(
            d->d3d11_video_context,
            d->d3d11_vp,
            vf->gpu_interop_output_view,
            0,
            1,
            &stream);
    in_view->lpVtbl->Release(in_view);
    if (FAILED(hr)) return AVERROR_EXTERNAL;

    *out_gpu_handle = (int64_t)(intptr_t)vf->gpu_interop_texture;
    *out_surface_w = (int)visible_w;
    *out_surface_h = (int)visible_h;
    return 0;
}
#endif
