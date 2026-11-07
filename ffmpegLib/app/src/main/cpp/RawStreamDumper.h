#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "include/libavcodec/avcodec.h"
}

// 把 av_read_frame 得到的原始视频包（Annex B ES）录制到调用方指定的完整文件路径：
// 切流/重连不断流不轮转、无大小上限，只有 stop()（宿主工程 stopRecord）或播放器析构结束。
// 同步写一份 CSV sidecar（<esPath>.csv：recv_us,pts,size,flags），用于和 video_diag/逐包日志对齐时间轴。
// 未激活时 writePacket 只有一次原子读的开销。
class RawStreamDumper {
public:
    RawStreamDumper() = default;
    ~RawStreamDumper();

    // 开始录制：filePath 为完整文件路径，只有 stop() 结束保存。
    void startRecord(int playerId, const std::string& filePath);
    // 停止录制并关闭当前文件，输出汇总日志。
    void stop();
    bool isActive() const;

    // 每个流会话开始时调用（拿到 codecpar 后）：缓存流参数；
    // 若已激活且尚未打开文件，则立即创建本会话的 ES/CSV 文件。
    // 这样“播放中途开启转储”也能立刻建文件，不用等下一次切流。
    void openForStream(const AVCodecParameters* params);
    // 流会话结束（切流/重连/停止）时调用：flush 并关闭文件；
    // 激活状态保持，下一个流会话的 openForStream 会自动开新文件（轮转）。
    void closeForStream();

    // 读线程逐包调用；未激活或文件未打开时直接返回。
    void writePacket(const AVPacket* pkt, int64_t recvUs);

private:
    void openFilesLocked(const AVCodecParameters* params);
    void closeFilesLocked();
    void cacheParamsLocked(const AVCodecParameters* params);
    void clearCachedParamsLocked();
    // 停止转储的锁内实现，start/startRecord/stop 共用。
    void stopLocked();
    // 把流参数集（VPS/SPS/PPS）写入 ES 文件，并记录到 writtenExtradata_。
    void writeExtradataLocked(const AVCodecParameters* params);

    std::mutex mutex_;
    std::atomic<bool> active_{false};
    int playerId_ = -1;
    // 当前流会话参数的缓存副本，供“播放中途开始录制”时创建文件。
    AVCodecParameters* cachedParams_ = nullptr;
    // 目标文件路径（完整路径，由调用方指定）；CSV sidecar 为 <recordPath_>.csv。
    std::string recordPath_;
    // 下一次 openFilesLocked 是否为 startRecord 后首次建文件（true 覆盖写，之后追加写）。
    bool recordFreshFile_ = false;
    // 已写入文件的 extradata 副本；流会话重开时仅当参数集变化才补写。
    std::vector<uint8_t> writtenExtradata_;

    FILE* esFile_ = nullptr;
    FILE* csvFile_ = nullptr;
    std::string esPath_;
    std::string csvPath_;
    int64_t bytesWritten_ = 0;
    int64_t packetCount_ = 0;
    int64_t lastProgressLogUs_ = 0;
    bool writeErrorLogged_ = false;
};
