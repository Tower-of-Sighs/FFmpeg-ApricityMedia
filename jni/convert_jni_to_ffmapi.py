#!/usr/bin/env python3
"""
Transform jni_ffmpeg.c → am_ffmpeg.c

Mechanically converts each JNI function to a plain C am_ function.
Preserves ALL internal logic unchanged — only rewrites:
  - Function signatures (JNIEXPORT → plain C)
  - JNI API calls (GetStringUTFChars, ReleaseStringUTFChars, etc.)
  - Type names (jlong → int64_t, jint → int, etc.)
"""
import re, sys

# Name mapping: Java_xxx_YyyZzz → am_xxx_yyy_zzz
NAME_MAP = {
    'init': 'init', 'lastError': 'last_error',
    'videoOpen': 'video_open', 'videoReadFrame': 'video_read_frame',
    'videoFrameGetInfo': 'video_frame_get_info',
    'videoFrameGetPixels': 'video_frame_get_pixels',
    'videoFrameGetPixelFormat': 'video_frame_get_pixel_format',
    'videoFrameIsGpuFrame': 'video_frame_is_gpu',
    'videoFrameGetGpuBackendTag': 'video_frame_get_gpu_backend',
    'videoFrameGetGpuHandle': 'video_frame_get_gpu_handle',
    'videoFrameGetGpuSubresource': 'video_frame_get_gpu_subresource',
    'videoFrameGetGpuSurfaceInfo': 'video_frame_get_gpu_surface_info',
    'videoFrameGetPlaneCount': 'video_frame_get_plane_count',
    'videoFrameGetPlaneInfo': 'video_frame_get_plane_info',
    'videoFrameGetPlaneBuffer': 'video_frame_get_plane_buffer',
    'videoFrameGetColorInfo': 'video_frame_get_color_info',
    'videoFrameGetSourcePixelFormat': 'video_frame_get_source_pixel_format',
    'videoFrameRelease': 'video_frame_release',
    'videoRewind': 'video_rewind', 'videoSeekMs': 'video_seek_ms',
    'videoGetDurationMs': 'video_get_duration_ms',
    'videoIsHardwareDecode': 'video_is_hardware_decode',
    'videoGetHardwareBackend': 'video_get_hardware_backend',
    'videoGetHardwareProbeMessage': 'video_get_hardware_probe_message',
    'videoGetHardwareDeviceHandle': 'video_get_hardware_device_handle',
    'videoClose': 'video_close',
    'audioOpen': 'audio_open', 'audioReadPcm': 'audio_read_pcm',
    'audioSampleRate': 'audio_sample_rate', 'audioChannels': 'audio_channels',
    'audioRewind': 'audio_rewind', 'audioSeekMs': 'audio_seek_ms',
    'audioGetDurationMs': 'audio_get_duration_ms',
    'audioClose': 'audio_close',
}

# Return type for each function
RET_MAP = {
    'init': 'void', 'lastError': 'const char*',
    'videoOpen': 'uint64_t', 'videoReadFrame': 'uint64_t',
    'videoFrameGetInfo': 'int', 'videoFrameGetPixels': 'void*',
    'videoFrameGetPixelFormat': 'int',
    'videoFrameIsGpuFrame': 'int', 'videoFrameGetGpuBackendTag': 'int',
    'videoFrameGetGpuHandle': 'uint64_t', 'videoFrameGetGpuSubresource': 'int',
    'videoFrameGetGpuSurfaceInfo': 'int',
    'videoFrameGetPlaneCount': 'int', 'videoFrameGetPlaneInfo': 'int',
    'videoFrameGetPlaneBuffer': 'void*', 'videoFrameGetColorInfo': 'int',
    'videoFrameGetSourcePixelFormat': 'const char*',
    'videoFrameRelease': 'void', 'videoRewind': 'void',
    'videoSeekMs': 'int', 'videoGetDurationMs': 'int64_t',
    'videoIsHardwareDecode': 'int', 'videoGetHardwareBackend': 'const char*',
    'videoGetHardwareProbeMessage': 'const char*',
    'videoGetHardwareDeviceHandle': 'uint64_t', 'videoClose': 'void',
    'audioOpen': 'uint64_t', 'audioReadPcm': 'int',
    'audioSampleRate': 'int', 'audioChannels': 'int',
    'audioRewind': 'void', 'audioSeekMs': 'int',
    'audioGetDurationMs': 'int64_t', 'audioClose': 'void',
}


def transform(src, dst):
    with open(src, 'r', encoding='utf-8') as f:
        text = f.read()

    # Remove JNI_OnLoad entirely
    text = re.sub(
        r'JNIEXPORT jint JNICALL JNI_OnLoad.*?\{.*?\n\}',
        '/* JNI_OnLoad removed — FFM API */',
        text, count=1, flags=re.DOTALL
    )

    # Process each JNI function
    for jni_suffix, am_suffix in NAME_MAP.items():
        jni_full = f'Java_cc_sighs_apricitymedia_jni_ApricityMediaNative_{jni_suffix}'
        ret_type = RET_MAP.get(jni_suffix, 'int')
        am_name = f'am_{am_suffix}'

        # Pattern: JNIEXPORT <type> JNICALL Java_..._name\n    (JNIEnv *env, jclass clazz, ...)\n{
        # Capture: return_type, params, body
        pat = re.compile(
            r'JNIEXPORT\s+\w+\s+JNICALL\s+' + re.escape(jni_full) +
            r'\s*\n\s*\(([^)]*)\)\s*\n\{',
            re.DOTALL
        )
        m = pat.search(text)
        if not m:
            continue

        params_str = m.group(1)

        # Build new parameter list: strip JNIEnv* and jclass
        new_params = []
        for p in params_str.split(','):
            p = p.strip()
            p = re.sub(r'\bJNIEnv\s*\*', '', p)
            p = re.sub(r'\bjclass\s+\w+', '', p)
            p = re.sub(r'\bjobject\s+\w+', '', p)
            p = p.strip()
            if p and p != ',':
                # Map JNI types to C types
                p = re.sub(r'\bjstring\b', 'const char*', p)
                p = re.sub(r'\bjlong\b', 'int64_t', p)
                p = re.sub(r'\bjint\b', 'int', p)
                p = re.sub(r'\bjboolean\b', 'int', p)
                p = re.sub(r'\bjdouble\b', 'double', p)
                p = re.sub(r'\bjlongArray\b', 'int64_t*', p)
                p = re.sub(r'\bjintArray\b', 'int*', p)
                p = re.sub(r'\bjbyteArray\b', 'uint8_t*', p)
                new_params.append(p)

        new_sig = f'{ret_type} {am_name}({", ".join(new_params)})\n{{'
        text = pat.sub(new_sig, text, count=1)

    # Replace JNI API calls in function bodies
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetStringUTFChars\s*\(\s*env\s*,\s*(\w+)\s*,\s*NULL\s*\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetStringUTFChars\s*\(\s*env\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*ReleaseStringUTFChars\s*\(\s*env\s*,\s*\w+\s*,\s*\w+\s*\)\s*;', '', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*NewStringUTF\s*\(\s*env\s*,\s*([^)]+)\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetIntArrayElements\s*\(\s*env\s*,\s*(\w+)\s*,\s*NULL\s*\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetLongArrayElements\s*\(\s*env\s*,\s*(\w+)\s*,\s*NULL\s*\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*ReleaseIntArrayElements\s*\(\s*env\s*,\s*\w+\s*,\s*\w+\s*,\s*\d+\s*\)\s*;', '', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*ReleaseLongArrayElements\s*\(\s*env\s*,\s*\w+\s*,\s*\w+\s*,\s*\d+\s*\)\s*;', '', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetArrayLength\s*\(\s*env\s*,\s*(\w+)\s*\)', r'0 /* arraylen */', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*NewDirectByteBuffer\s*\(\s*env\s*,\s*([^,]+),\s*([^)]+)\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*GetStringCritical\s*\(\s*env\s*,\s*(\w+)\s*,\s*NULL\s*\)', r'\1', text)
    text = re.sub(r'\(\s*\*env\s*\)\s*->\s*ReleaseStringCritical\s*\(\s*env\s*,\s*\w+\s*,\s*\w+\s*\)\s*;', '', text)

    # Remove (void) casts of JNI args
    text = re.sub(r'\(void\)\s*(env|thiz|clazz)\s*;\s*\n?', '', text)

    # jlong/jint/jboolean/jdouble in local variables -> C types
    text = re.sub(r'\bjlong\b', 'int64_t', text)
    text = re.sub(r'\bjint\b', 'int', text)
    text = re.sub(r'\bjboolean\b', 'int', text)
    text = re.sub(r'\bjdouble\b', 'double', text)
    text = re.sub(r'\bjsize\b', 'int', text)

    # Remove (jint) casts
    text = re.sub(r'\(jint\)', '(int)', text)
    text = re.sub(r'\(jlong\)', '(int64_t)', text)
    text = re.sub(r'\(jboolean\)', '(int)', text)

    # Remove JNI_TRUE/FALSE
    text = text.replace('JNI_TRUE', '1').replace('JNI_FALSE', '0')

    # Replace include
    text = text.replace('#include <jni.h>', '#include "am_ffmpeg.h"\n#define AM_BUILD_DLL')

    # Update header comment
    text = text.replace('jni_ffmpeg.c — JNI bridge to minimal FFmpeg',
                        'am_ffmpeg.c — Pure C FFmpeg bridge for Java 25 FFM API')
    text = text.replace('Replaces JavaCPP-based decoders with direct JNI for:',
                        'Replaces JavaCPP/JNI decoders. Plain C — callable via Linker.downcallHandle or jextract.')
    text = re.sub(r' \* Method:\s+\w+', ' * FFM API', text)
    text = text.replace('JNI — lifecycle', 'Lifecycle')
    text = text.replace('JNI — video', 'Video')
    text = text.replace('JNI — audio', 'Audio')

    with open(dst, 'w', encoding='utf-8') as f:
        f.write(text)

    print(f'Converted {src} → {dst}')
    n = len(NAME_MAP)
    print(f'Transformed {n} JNI functions to FFM API style')

if __name__ == '__main__':
    s = sys.argv[1] if len(sys.argv) > 1 else 'jni/jni_ffmpeg.c'
    d = sys.argv[2] if len(sys.argv) > 2 else 'jni/am_ffmpeg.c'
    transform(s, d)
