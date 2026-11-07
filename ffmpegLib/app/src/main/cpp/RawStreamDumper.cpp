#include "RawStreamDumper.h"

#include <cerrno>
#include <cstring>

extern "C" {
#include "include/libavutil/time.h"
}

#define LOG_TAG "RawStreamDumper"
#include "native_log.h"

namespace {
constexpr int64_t kProgressLogIntervalUs = 2 * 1000 * 1000;

// extradata 只有在本身就是 Annex B（带 start code）时才写入 ES 文件头，
// 避免把 hvcC/hvCC 格式的参数集误写进裸流，导致离线解码失败。
bool isAnnexBExtradata(const AVCodecParameters* params) {
    if (!params || params->extradata_size < 4 || !params->extradata) {
        return false;
    }
    const uint8_t* d = params->extradata;
    return d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
}

// 判断流参数集是否和录制文件里已写入的一致（换流时决定是否补写参数集）。
bool sameExtradata(const std::vector<uint8_t>& written, const AVCodecParameters* params) {
    if (!params || params->extradata_size <= 0 || !params->extradata) {
        return written.empty();
    }
    return written.size() == static_cast<size_t>(params->extradata_size) &&
           std::memcmp(written.data(), params->extradata, params->extradata_size) == 0;
}
} // namespace

RawStreamDumper::~RawStreamDumper() {
    stop();
}

void RawStreamDumper::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopLocked();
}

void RawStreamDumper::stopLocked() {
    if (!active_.exchange(false)) {
        return;
    }
    closeFilesLocked();
    clearCachedParamsLocked();
    writtenExtradata_.clear();
    LOGD_PLAYER(playerId_, "record stop: packets=%lld, bytes=%lld, path=%s",
         static_cast<long long>(packetCount_),
         static_cast<long long>(bytesWritten_),
         esPath_.c_str());
}

void RawStreamDumper::startRecord(int playerId, const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    stopLocked();
    if (filePath.empty()) {
        LOGW_PLAYER(playerId_, "startRecord ignored: empty file path");
        return;
    }

    playerId_ = playerId;
    recordPath_ = filePath;
    bytesWritten_ = 0;
    packetCount_ = 0;
    lastProgressLogUs_ = 0;
    writeErrorLogged_ = false;
    recordFreshFile_ = true;
    writtenExtradata_.clear();
    active_.store(true);
    LOGD_PLAYER(playerId_, "startRecord begin, file=%s", filePath.c_str());
    // 播放中途开始录制时直接建文件；还没拿到流参数就等 openForStream。
    if (cachedParams_) {
        openFilesLocked(cachedParams_);
    }
}

bool RawStreamDumper::isActive() const {
    return active_.load(std::memory_order_relaxed);
}

void RawStreamDumper::openForStream(const AVCodecParameters* params) {
    if (!params) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cacheParamsLocked(params);
    if (!active_.load()) {
        return;
    }
    if (esFile_) {
        // 文件持续打开：切流/重连后继续追加，仅当流参数变化时补写新参数集。
        if (!sameExtradata(writtenExtradata_, cachedParams_)) {
            LOGD_PLAYER(playerId_, "record stream params changed, append new extradata");
            writeExtradataLocked(cachedParams_);
        }
        return;
    }
    openFilesLocked(cachedParams_);
}

void RawStreamDumper::closeForStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 流会话结束只 flush，文件保持打开；切流/重连后继续追加同一文件。
    if (esFile_) {
        fflush(esFile_);
        if (csvFile_) {
            fflush(csvFile_);
        }
        LOGD_PLAYER(playerId_, "record stream session end, keep file open: bytes=%lld, path=%s",
             static_cast<long long>(bytesWritten_), esPath_.c_str());
    }
    // 流会话结束，缓存参数随之过期；下一个会话的 openForStream 会重新缓存。
    clearCachedParamsLocked();
}

void RawStreamDumper::writePacket(const AVPacket* pkt, int64_t recvUs) {
    // 未激活时快速返回，正常播放路径只有这一次原子读开销。
    if (!active_.load(std::memory_order_relaxed)) {
        return;
    }
    if (!pkt || pkt->size <= 0 || !pkt->data) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!esFile_) {
        return;
    }

    const size_t expected = static_cast<size_t>(pkt->size);
    const size_t written = fwrite(pkt->data, 1, expected, esFile_);
    if (written != expected && !writeErrorLogged_) {
        writeErrorLogged_ = true;
        LOGW_PLAYER(playerId_, "raw_dump ES write failed, errno=%d", errno);
    }
    bytesWritten_ += static_cast<int64_t>(pkt->size);
    ++packetCount_;

    if (csvFile_) {
        if (pkt->pts == AV_NOPTS_VALUE) {
            fprintf(csvFile_, "%lld,NA,%d,0x%x\n",
                    static_cast<long long>(recvUs), pkt->size, pkt->flags);
        } else {
            fprintf(csvFile_, "%lld,%lld,%d,0x%x\n",
                    static_cast<long long>(recvUs),
                    static_cast<long long>(pkt->pts), pkt->size, pkt->flags);
        }
    }

    // 低频进度日志，确认录制在工作以及当前落盘量；首包只记录锚点不打日志。
    const int64_t nowUs = av_gettime_relative();
    if (lastProgressLogUs_ == 0) {
        lastProgressLogUs_ = nowUs;
    } else if (nowUs - lastProgressLogUs_ >= kProgressLogIntervalUs) {
        LOGD_PLAYER(playerId_, "record progress: packets=%lld, bytes=%lld, path=%s",
             static_cast<long long>(packetCount_),
             static_cast<long long>(bytesWritten_),
             esPath_.c_str());
        lastProgressLogUs_ = nowUs;
    }
}

void RawStreamDumper::openFilesLocked(const AVCodecParameters* params) {
    esPath_ = recordPath_;
    csvPath_ = esPath_ + ".csv";
    // startRecord 后首次建文件覆盖写；之后流会话重开（切流/重连/重新播放）追加同一文件。
    const bool fresh = recordFreshFile_;
    esFile_ = fopen(esPath_.c_str(), fresh ? "wb" : "ab");
    csvFile_ = fopen(csvPath_.c_str(), fresh ? "wb" : "ab");
    if (esFile_) {
        setvbuf(esFile_, nullptr, _IOFBF, 64 * 1024);
    }
    if (csvFile_) {
        setvbuf(csvFile_, nullptr, _IOFBF, 16 * 1024);
    }
    if (!esFile_) {
        LOGW_PLAYER(playerId_, "record open es file failed: %s, errno=%d",
             esPath_.c_str(), errno);
        closeFilesLocked();
        active_.store(false);
        return;
    }
    if (!csvFile_) {
        // CSV 是辅助时间轴文件，打开失败不阻断录制，只丢 sidecar。
        LOGW_PLAYER(playerId_, "record open csv file failed: %s, errno=%d",
             csvPath_.c_str(), errno);
    }

    if (fresh) {
        // 参数集只在新文件开头写一次；后续换流由 openForStream 检测变化后补写。
        if (isAnnexBExtradata(params)) {
            writeExtradataLocked(params);
        }
        if (csvFile_) {
            fprintf(csvFile_, "recv_us,pts,size,flags\n");
        }
    }
    recordFreshFile_ = false;
    LOGD_PLAYER(playerId_, "record file open: %s (fresh=%d)", esPath_.c_str(), fresh ? 1 : 0);
}

void RawStreamDumper::closeFilesLocked() {
    if (esFile_) {
        fclose(esFile_);
        esFile_ = nullptr;
    }
    if (csvFile_) {
        fclose(csvFile_);
        csvFile_ = nullptr;
    }
}

void RawStreamDumper::cacheParamsLocked(const AVCodecParameters* params) {
    if (!cachedParams_) {
        cachedParams_ = avcodec_parameters_alloc();
    }
    if (cachedParams_) {
        avcodec_parameters_copy(cachedParams_, params);
    }
}

void RawStreamDumper::clearCachedParamsLocked() {
    if (cachedParams_) {
        avcodec_parameters_free(&cachedParams_);
        cachedParams_ = nullptr;
    }
}

void RawStreamDumper::writeExtradataLocked(const AVCodecParameters* params) {
    if (!params || params->extradata_size <= 0 || !params->extradata || !esFile_) {
        return;
    }
    fwrite(params->extradata, 1, static_cast<size_t>(params->extradata_size), esFile_);
    bytesWritten_ += params->extradata_size;
    writtenExtradata_.assign(params->extradata,
                             params->extradata + params->extradata_size);
}
