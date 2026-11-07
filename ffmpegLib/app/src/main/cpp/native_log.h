#pragma once

#include <android/log.h>

#ifndef LOG_TAG
#define LOG_TAG "NATIVE_LOG"
#endif

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define LOGD_PLAYER(player_id, fmt, ...) \
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[player=%d] " fmt, player_id, ##__VA_ARGS__)
#define LOGI_PLAYER(player_id, fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[player=%d] " fmt, player_id, ##__VA_ARGS__)
#define LOGW_PLAYER(player_id, fmt, ...) \
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "[player=%d] " fmt, player_id, ##__VA_ARGS__)
#define LOG_FRAME_PLAYER(player_id, ...) do { if (kEnableFrameTraceLog) LOGD_PLAYER(player_id, __VA_ARGS__); } while (0)
#define LOG_QUEUE_PLAYER(player_id, ...) do { if (kEnableQueueTraceLog) LOGD_PLAYER(player_id, __VA_ARGS__); } while (0)
#define LOG_SEI_PLAYER(player_id, ...) do { if (kEnableSeiTraceLog) LOGD_PLAYER(player_id, __VA_ARGS__); } while (0)
#define LOG_BUFFER_PLAYER(player_id, ...) do { if (kEnableBufferTraceLog) LOGD_PLAYER(player_id, __VA_ARGS__); } while (0)

// 这些宏由 Gradle debug/release buildType 通过 CMake arguments 传入。
// 未传入时默认关闭高频日志，只保留错误、状态切换和关键统计日志。
#ifndef NATIVE_ENABLE_FRAME_TRACE
#define NATIVE_ENABLE_FRAME_TRACE 0
#endif

#ifndef NATIVE_ENABLE_QUEUE_TRACE
#define NATIVE_ENABLE_QUEUE_TRACE 0
#endif

#ifndef NATIVE_ENABLE_SEI_TRACE
#define NATIVE_ENABLE_SEI_TRACE 0
#endif

#ifndef NATIVE_ENABLE_BUFFER_TRACE
#define NATIVE_ENABLE_BUFFER_TRACE 0
#endif

static constexpr bool kEnableFrameTraceLog = NATIVE_ENABLE_FRAME_TRACE != 0;
static constexpr bool kEnableQueueTraceLog = NATIVE_ENABLE_QUEUE_TRACE != 0;
static constexpr bool kEnableSeiTraceLog = NATIVE_ENABLE_SEI_TRACE != 0;
static constexpr bool kEnableBufferTraceLog = NATIVE_ENABLE_BUFFER_TRACE != 0;

#define LOG_FRAME(...) do { if (kEnableFrameTraceLog) LOGD(__VA_ARGS__); } while (0)
#define LOG_QUEUE(...) do { if (kEnableQueueTraceLog) LOGD(__VA_ARGS__); } while (0)
#define LOG_SEI(...) do { if (kEnableSeiTraceLog) LOGD(__VA_ARGS__); } while (0)
#define LOG_BUFFER(...) do { if (kEnableBufferTraceLog) LOGD(__VA_ARGS__); } while (0)
