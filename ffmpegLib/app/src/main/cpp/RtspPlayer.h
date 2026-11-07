#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

#include "VideoFrameBuffer.h"
#undef LOGD
#include "ffplay.h"
#include "native_error.h"
#include "RawStreamDumper.h"

extern "C" {
#include "include/libavformat/avformat.h"
}

class RtspPlayer {
public:
    enum class PlayerState {
        Idle, //空闲，没在播
        Running, //正在播
        Stopping, //正在停，后台线程还在收尾
    };

    explicit RtspPlayer(JavaVM* jvm);
    ~RtspPlayer();

    void bindCallback(JNIEnv* env, jobject callback);
    void start(const std::string& url);
    void stop();
    void requestStop();
    // 临时暂停网络拉流，保留播放器对象和恢复所需的 URL。
    void pauseForBandwidth();
    // 恢复暂停前的网络拉流。
    void resumeAfterBandwidth();
    // 等待后台读包线程完成清理并回到 Idle。
    bool waitUntilIdle(int timeoutMs);

    // 开始原始码流录制：filePath 为完整 ES 文件路径，同步写 <filePath>.csv sidecar；
    // 切流/重连不断流不轮转、无大小上限，只有 stopRecord() 或播放器析构结束保存。
    void startRecord(const std::string& filePath);
    // 停止原始码流录制。
    void stopRecord();

private:
    enum class RetryReason {
        ConnectFailure,
        ReadFailure,
    };

    int handleFrame(const AVFrame* frame, double ptsSeconds);
    void readThreadLoop();
    ReadExitReason readPacketLoop(AVFormatContext* fmtCtx, int videoStreamIndex);
    void resetHevcGateState();
    // 为当前流准备一个常驻可复用的解码器：
    // 同参数时复用已有 AVCodecContext，参数不兼容时 teardown 后重建。
    int ensureDecoderForStream(AVFormatContext* fmtCtx, int streamIndex);
    // 判断当前解码器是否还能复用到新流。
    bool canReuseDecoderForStream(AVFormatContext* fmtCtx, int streamIndex) const;
    int rebuildDecoderForStream(AVFormatContext* fmtCtx, int streamIndex, AVStream* stream);

    bool waitRetryInterruptible(std::chrono::milliseconds delay);
    std::chrono::milliseconds nextRetryDelay(RetryReason reason);
    void resetRetryBackoff();

    int initDecoderWithFallback(AVFormatContext* fmtCtx, int streamIndex, AVCodecContext** avctx, int* useHw);
    int streamComponentOpen(int streamIndex, AVFormatContext* fmtCtx);

    // 添加打断
    static int64_t nowMonotonicUs();
    static int64_t elapsedMsFromUs(int64_t baseUs, int64_t pointUs);
    void armInterruptTimeout(int64_t timeoutUs);
    void clearInterruptTimeout();
    bool shouldInterrupt() const;
    static int ffmpegInterruptCallback(void* opaque);
    void resetSwitchTraceLocked();
    void beginSwitchTraceLocked();
    void logSwitchTrace(const char* eventName, int64_t eventUs);

    void notifyJavaError(int errorCode);
    void notifySeiToJava(const uint8_t* sei, int size);
    void reportFfmpegError(int ffErr);
    bool extractHevcSeiAndCallback(const uint8_t* data, int size);
    void ensureFrameBufferLocked();

    void cleanupPlaybackState(AVFormatContext** fmtCtx);
    void releaseCallbackRefs(JNIEnv* env);
    void releaseOwnedResources();
    void requestStopInternal(bool clearPendingSwitch);

    static bool isRealtimeUrl(const std::string& url);
    static std::string avErrorStr(int errnum);
    static int nextPlayerId();

    static constexpr int64_t kConnectInterruptTimeoutUs = 5 * 1000 * 1000;
    static constexpr int64_t kReadInterruptTimeoutUs = 2 * 1000 * 1000;
    static constexpr int64_t kVideoPacketWarnTimeoutUs = 3 * 1000 * 1000;
    static constexpr int64_t kVideoPacketHardTimeoutUs = 5 * 1000 * 1000;

    JavaVM* jvm_ = nullptr;
    const int playerId_;
    jobject callbackGlobalRef_ = nullptr;
    jclass callbackClassGlobalRef_ = nullptr;
    jmethodID onErrorMethod_ = nullptr;
    jmethodID onSeiMethod_ = nullptr;
    jmethodID onI420DataMethod_ = nullptr;
    // SEI 回调复用缓冲（全局引用），稳态零分配；Java 侧不得在回调返回后继续持有该数组
    // （内容会被下一次回调覆盖），需要保留时必须当场拷贝。
    jbyteArray seiBufGlobalRef_ = nullptr;
    int seiBufCapacity_ = 0;

    std::unique_ptr<VideoFrameBuffer> frameBuffer_;
    std::thread readThread_;
    std::atomic<bool> abortRequested_{false};
    std::atomic<bool> threadRunning_{false};
    std::atomic<PlayerState> state_{PlayerState::Idle};

    // 播放器创建时初始化一次，析构时统一销毁；切流时只 flush 并复用，不再反复 init/destroy。
    PacketQueue videoQueue_{};
    FrameQueue frameQueue_{};
    // 解码器由 ensureDecoderForStream() 管理生命周期：同参数切流复用已有 AVCodecContext，
    // 参数不兼容时 teardown 后重建；decoder/refresh 线程首次启动后保持常驻。
    Decoder videoDecoder_{};
    // 原始码流录制器（默认关闭，由 startRecord 开启，写调用方指定 ES 文件 + CSV sidecar）。
    RawStreamDumper rawDumper_;

    int isRealtime_ = 0;
    int useHw_ = 0;
    std::string rtspUrl_;
    std::string pendingRtspUrl_;
    bool hasPendingRtspUrl_ = false;
    //播放器的“带宽暂停状态标记”，用于表示当前播放器是否处于临时暂停拉流状态
    bool bandwidthPaused_ = false;
    std::string bandwidthPausedUrl_;
    bool hevcParamsReady_ = false;
    bool hevcStartedOnIrap_ = false;
    bool hevcSeenVps_ = false;
    bool hevcSeenSps_ = false;
    bool hevcSeenPps_ = false;
    std::mutex stateMutex_;
    std::condition_variable stateCond_;
    std::mutex retryMutex_;
    std::condition_variable retryCond_;
    int connectRetryCount_ = 0;
    int readRetryCount_ = 0;

    std::atomic<int64_t> interruptDeadlineUs_{0};
    std::atomic<int64_t> lastVideoPacketRecvUs_{0};
    std::atomic<bool> videoPacketTimeoutWarned_{false};
    // 单调递增的切流编号，用来把同一次切流的日志串起来看。
    std::atomic<int64_t> nextSwitchId_{1};
    // 当前正在追踪的切流编号；为 0 表示没有切流追踪任务。
    std::atomic<int64_t> activeSwitchId_{0};
    // 切流耗时分析的时间戳。
    std::atomic<int64_t> switchRequestUs_{0};
    // 旧流清理完成的时间戳，对应“旧线程退出/旧解码器销毁完”的阶段。
    std::atomic<int64_t> switchCleanupUs_{0};
    // avformat_open_input 成功返回的时间戳，对应“新流建连完成”的阶段。
    std::atomic<int64_t> switchOpenInputUs_{0};
    // avformat_find_stream_info 成功返回的时间戳，对应“流信息探测完成”的阶段。
    std::atomic<int64_t> switchFindStreamInfoUs_{0};
    // refresh_start 调用完成的时间戳，用来观察刷新线程拉起是否耗时明显。
    std::atomic<int64_t> switchRefreshThreadUs_{0};
    // readPacketLoop 收到新流首个关键帧包的时间戳，用来判断是否卡在等关键帧。
    std::atomic<int64_t> switchFirstKeyUs_{0};
    // native 首次把新流画面通过 onI420Data 回调给 Java 的时间戳。
    std::atomic<int64_t> switchFirstFrameUs_{0};
    // 只在切流过程中开启追踪。
    std::atomic<bool> switchTraceActive_{false};
    // 保证同一次切流只记录一次 first_frame。
    std::atomic<bool> switchFirstFrameLogged_{false};
};
