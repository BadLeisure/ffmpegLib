// native_error.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Native 层对外统一错误码
 * 这些值一旦发布，不要修改
 */
typedef enum NativeError {
    NATIVE_OK = 0,

    // ---------- open input ----------
    NATIVE_OPEN_TIMEOUT        = 1001,
    NATIVE_OPEN_NOT_FOUND      = 1002,
    NATIVE_OPEN_REFUSED        = 1003,
    NATIVE_OPEN_UNKNOWN        = 1099,

    // ---------- stream info ----------
    NATIVE_NO_STREAM           = 1101,
    NATIVE_UNSUPPORTED_CODEC   = 1102,

    // ---------- runtime ----------
    NATIVE_READ_EOF            = 1201,
    NATIVE_READ_IO_ERROR       = 1202,

} NativeError;


enum class ReadState {
    Init,           // 初始化 AVFormatContext
    Connecting,     // 正在尝试连接 RTSP
    Reading,        // 正在读取数据
    WaitRetry,      // 等待重试
    Stopped         // 停止线程
};

enum class ReadExitReason {
    None,   // 正常退出（理论上不会）
    Eof,    // 遇到 EOF（实时流可能是推流断开）
    Error,  // 读取错误
    Abort   // 外部中止
};

/**
 * 将 FFmpeg 返回的 error code 映射为 NativeError
 *
 * @param ff_err FFmpeg 返回值（<0）
 * @return NativeError
 */
NativeError map_ffmpeg_error(int ff_err);

#ifdef __cplusplus
}
#endif
