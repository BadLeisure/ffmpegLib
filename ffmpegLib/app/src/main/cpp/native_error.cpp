//
// Created by 软件部- AA on 2026/1/16.
//
// native_error.cpp

#include "native_error.h"

#include <libavutil/error.h>
#include <errno.h>

/**
 * FFmpeg error code → NativeError
 */
NativeError map_ffmpeg_error(int ff_err) {
    if (ff_err >= 0) {
        return NATIVE_OK;
    }

    switch (ff_err) {
        case AVERROR(ETIMEDOUT):
            return NATIVE_OPEN_TIMEOUT;

        case AVERROR(ENOENT):
            return NATIVE_OPEN_NOT_FOUND;

        case AVERROR(EACCES):
            return NATIVE_OPEN_REFUSED;

        case AVERROR_STREAM_NOT_FOUND:
            return NATIVE_NO_STREAM;

        case AVERROR_EOF:
            return NATIVE_READ_EOF;

        case AVERROR(EIO):
            return NATIVE_READ_IO_ERROR;

        default:
            return NATIVE_OPEN_UNKNOWN;
    }
}


