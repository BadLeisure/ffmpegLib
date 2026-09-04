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

extern "C" {
#include "include/libavformat/avformat.h"
}

class RtspPlayer {
public:
    enum class PlayerState {
        Idle,
        Running,
        Stopping,
    };

    explicit RtspPlayer(JavaVM* jvm);
    ~RtspPlayer();

    void bindCallback(JNIEnv* env, jobject callback);
    void start(const std::string& url);
    void stop();
    void requestStop();

private:
    enum class RetryReason {
        ConnectFailure,
        ReadFailure,
    };

    int handleFrame(const AVFrame* frame, double ptsSeconds);
    void readThreadLoop();
    ReadExitReason readPacketLoop(AVFormatContext* fmtCtx, int videoStreamIndex);
    void resetHevcGateState();
    // 为当前流准备一个常驻可复用的解码器。
    // 后续会把 streamComponentOpen() 里的实现逐步挪到这里。
    int ensureDecoderForStream(AVFormatContext* fmtCtx, int streamIndex);
    // 判断当前解码器是否还能复用到新流。
    bool canReuseDecoderForStream(AVFormatContext* fmtCtx, int streamIndex) const;

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

    std::unique_ptr<VideoFrameBuffer> frameBuffer_;
    std::thread readThread_;
    std::atomic<bool> abortRequested_{false};
    std::atomic<bool> threadRunning_{false};
    std::atomic<PlayerState> state_{PlayerState::Idle};

    // 每次切流都会重建这两条队列。
    // 播放器创建时初始化，切流时只清空并复用。
    PacketQueue videoQueue_{};
    FrameQueue frameQueue_{};
    // 当前仍按播放周期管理解码器，后续会逐步改成常驻生命周期。
    Decoder videoDecoder_{};

    int isRealtime_ = 0;
    int useHw_ = 0;
    std::string rtspUrl_;
    std::string pendingRtspUrl_;
    bool hasPendingRtspUrl_ = false;
    bool hevcParamsReady_ = false;
    bool hevcStartedOnIrap_ = false;
    bool hevcSeenVps_ = false;
    bool hevcSeenSps_ = false;
    bool hevcSeenPps_ = false;
    std::mutex stateMutex_;
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
