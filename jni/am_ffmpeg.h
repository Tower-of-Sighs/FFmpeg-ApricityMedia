#ifndef AM_FFMPEG_H
#define AM_FFMPEG_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#  ifdef AM_BUILD_DLL
#    define AM_API __declspec(dllexport)
#  else
#    define AM_API __declspec(dllimport)
#  endif
#else
#  define AM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 *  Constants
 * --------------------------------------------------------------- */

#define AM_AUDIO_SAMPLE_RATE  48000
#define AM_AUDIO_CHANNELS     2

#define AM_FRAME_FMT_RGBA8888    0
#define AM_FRAME_FMT_YUV420P     1
#define AM_FRAME_FMT_NV12        2
#define AM_FRAME_FMT_YUV420P10LE 3
#define AM_FRAME_FMT_P010LE      4
#define AM_FRAME_FMT_RGBA16F     5

#define AM_HW_BACKEND_NONE         0
#define AM_HW_BACKEND_D3D11VA      1
#define AM_HW_BACKEND_NVDEC        2
#define AM_HW_BACKEND_DXVA2        3
#define AM_HW_BACKEND_VIDEOTOOLBOX 4
#define AM_HW_BACKEND_OPENGL       5
#define AM_HW_BACKEND_VULKAN       6

/* ---------------------------------------------------------------
 *  Log callback
 * --------------------------------------------------------------- */
typedef void (*am_log_callback)(int level, const char *msg, void *userdata);

/* ---------------------------------------------------------------
 *  Lifecycle
 * --------------------------------------------------------------- */

AM_API void am_init(am_log_callback log_cb, void *userdata);
AM_API const char *am_last_error(void);

/* ---------------------------------------------------------------
 *  Video
 * --------------------------------------------------------------- */

AM_API uint64_t am_video_open(const char *path,
                              int target_width,
                              int target_height,
                              double max_fps,
                              int network_timeout_ms,
                              int network_buffer_kb,
                              int network_reconnect,
                              int hw_decode_enabled,
                              int hw_nvdec_enabled,
                              const char *hw_preferred);

AM_API uint64_t am_video_read_frame(uint64_t decoder);

AM_API int  am_video_frame_get_info(uint64_t frame, int64_t out[4]);
AM_API void *am_video_frame_get_pixels(uint64_t frame);
AM_API int  am_video_frame_get_pixel_format(uint64_t frame);

AM_API int  am_video_frame_is_gpu(uint64_t frame);
AM_API int  am_video_frame_get_gpu_backend(uint64_t frame);
AM_API uint64_t am_video_frame_get_gpu_handle(uint64_t frame);
AM_API int  am_video_frame_get_gpu_subresource(uint64_t frame);
AM_API int  am_video_frame_get_gpu_surface_info(uint64_t frame, int out[2]);

AM_API int  am_video_frame_get_plane_count(uint64_t frame);
AM_API int  am_video_frame_get_plane_info(uint64_t frame, int plane, int out[3]);
AM_API void *am_video_frame_get_plane_buffer(uint64_t frame, int plane);
AM_API int  am_video_frame_get_color_info(uint64_t frame, int out[4]);
AM_API const char *am_video_frame_get_source_pixel_format(uint64_t frame);

AM_API void am_video_frame_release(uint64_t frame);
AM_API void am_video_rewind(uint64_t decoder);
AM_API int  am_video_seek_ms(uint64_t decoder, int64_t target_ms);
AM_API int64_t am_video_get_duration_ms(uint64_t decoder);
AM_API int  am_video_is_hardware_decode(uint64_t decoder);
AM_API const char *am_video_get_hardware_backend(uint64_t decoder);
AM_API const char *am_video_get_hardware_probe_message(uint64_t decoder);
AM_API uint64_t am_video_get_hardware_device_handle(uint64_t decoder);

AM_API void am_video_close(uint64_t decoder);

/* ---------------------------------------------------------------
 *  Audio
 * --------------------------------------------------------------- */

AM_API uint64_t am_audio_open(const char *path,
                              int network_timeout_ms,
                              int network_buffer_kb,
                              int network_reconnect);

AM_API int  am_audio_read_pcm(uint64_t decoder,
                              uint8_t *buffer, int offset, int length);
AM_API int  am_audio_sample_rate(uint64_t decoder);
AM_API int  am_audio_channels(uint64_t decoder);
AM_API void am_audio_rewind(uint64_t decoder);
AM_API int  am_audio_seek_ms(uint64_t decoder, int64_t target_ms);
AM_API int64_t am_audio_get_duration_ms(uint64_t decoder);
AM_API void am_audio_close(uint64_t decoder);

#ifdef __cplusplus
}
#endif

#endif /* AM_FFMPEG_H */
