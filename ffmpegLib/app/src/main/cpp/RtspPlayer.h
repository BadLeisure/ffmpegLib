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
    bool waitRetryInterruptible(std::chrono::milliseconds delay);
    std::chrono::milliseconds nextRetryDelay(RetryReason reason);
    void resetRetryBackoff();

    int initDecoderWithFallback(AVFormatContext* fmtCtx, int streamIndex, AVCodecContext** avctx, int* useHw);
    int streamComponentOpen(int streamIndex, AVFormatContext* fmtCtx);

    //添加打断
    static int64_t nowMonotonicUs();
    void armInterruptTimeout(int64_t timeoutUs);
    void clearInterruptTimeout();
    bool shouldInterrupt() const;
    static int ffmpegInterruptCallback(void* opaque);

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

    PacketQueue videoQueue_{};
    FrameQueue frameQueue_{};
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
};
