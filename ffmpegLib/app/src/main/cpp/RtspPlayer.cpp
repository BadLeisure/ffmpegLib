#include "RtspPlayer.h"

#include <chrono>
#include <cstring>
#include <errno.h>
#include <string>
#include <vector>

extern "C" {
#include "include/libavcodec/avcodec.h"
#include "include/libavformat/avformat.h"
#include "include/libavutil/avutil.h"
#include "include/libavutil/time.h"
}

#define LOG_TAG "RtspPlayer"
#include "native_log.h"

namespace {
constexpr int kInitialWidth = 2560;
constexpr int kInitialHeight = 1440;

JNIEnv* getJNIEnv(JavaVM* jvm, bool* needDetach = nullptr) {
    if (!jvm) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    const int status = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        if (needDetach) {
            *needDetach = false;
        }
        return env;
    }

    if (status == JNI_EDETACHED && jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        if (needDetach) {
            *needDetach = true;
        }
        return env;
    }

    return nullptr;
}


std::vector<uint8_t> hevcEbspToRbsp(const uint8_t* data, int size) {
    std::vector<uint8_t> rbsp;
    if (!data || size <= 0) {
        return rbsp;
    }

    rbsp.reserve(size);
    int zeroCount = 0;
    for (int i = 0; i < size; ++i) {
        const uint8_t byte = data[i];
        if (zeroCount >= 2 && byte == 0x03) {
            zeroCount = 0;
            continue;
        }

        rbsp.push_back(byte);
        if (byte == 0x00) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }

    return rbsp;
}

bool isAnnexBStartCode(const uint8_t* p, const uint8_t* end, int* startLen = nullptr) {
    if (!p || !end || p + 3 > end) {
        return false;
    }
    if (p[0] != 0 || p[1] != 0) {
        return false;
    }
    if (p[2] == 1) {
        if (startLen) {
            *startLen = 3;
        }
        return true;
    }
    if (p + 4 <= end && p[2] == 0 && p[3] == 1) {
        if (startLen) {
            *startLen = 4;
        }
        return true;
    }
    return false;
}

bool isHevcIrapNalType(uint8_t nalType) {
    return nalType >= 16 && nalType <= 23;
}

void updateHevcGateFlags(const uint8_t* data,
                         int size,
                         bool* seenVps,
                         bool* seenSps,
                         bool* seenPps,
                         bool* sawIrap) {
    if (!data || size <= 0) {
        return;
    }

    const uint8_t* p = data;
    const uint8_t* end = data + size;
    while (p + 3 < end) {
        int startLen = 0;
        if (!isAnnexBStartCode(p, end, &startLen)) {
            ++p;
            continue;
        }

        const uint8_t* nal = p + startLen;
        if (nal + 2 > end) {
            break;
        }

        const uint8_t nalType = (nal[0] >> 1) & 0x3F;
        if (seenVps && nalType == 32) *seenVps = true;
        if (seenSps && nalType == 33) *seenSps = true;
        if (seenPps && nalType == 34) *seenPps = true;
        
        if (sawIrap && isHevcIrapNalType(nalType)) *sawIrap = true;

        const uint8_t* next = nal;
        while (next + 3 < end && !isAnnexBStartCode(next, end, nullptr)) {
            ++next;
        }
        if (next + 3 >= end && !isAnnexBStartCode(next, end, nullptr)) {
            break;
        }
        p = next;
    }
}

}

int64_t RtspPlayer::nowMonotonicUs() {
    return av_gettime_relative();
}

void RtspPlayer::armInterruptTimeout(int64_t timeoutUs) {
    if (timeoutUs <= 0) {
        interruptDeadlineUs_.store(0);
        return;
    }
    interruptDeadlineUs_.store(nowMonotonicUs() + timeoutUs);
}

void RtspPlayer::clearInterruptTimeout() {
    interruptDeadlineUs_.store(0);
}

bool RtspPlayer::shouldInterrupt() const {
    if (abortRequested_.load()) {
        return true;
    }

    const int64_t deadlineUs = interruptDeadlineUs_.load();
    if (deadlineUs <= 0) {
        return false;
    }

    return nowMonotonicUs() >= deadlineUs;
}

int RtspPlayer::ffmpegInterruptCallback(void* opaque) {
    RtspPlayer* player = static_cast<RtspPlayer*>(opaque);
    if (!player) {
        return 0;
    }

    const bool shouldStop = player->shouldInterrupt();
    if (shouldStop) {
        LOGD_PLAYER(player->playerId_, "interrupt_callback: return 1, abort=%d, deadlineUs=%lld, nowUs=%lld",
             player->abortRequested_.load() ? 1 : 0,
             static_cast<long long>(player->interruptDeadlineUs_.load()),
             static_cast<long long>(RtspPlayer::nowMonotonicUs()));
    }
    return shouldStop ? 1 : 0;
}

namespace {
std::atomic<int> gNextPlayerId{1};
}

int RtspPlayer::nextPlayerId() {
    return gNextPlayerId.fetch_add(1);
}

RtspPlayer::RtspPlayer(JavaVM* jvm) : jvm_(jvm), playerId_(nextPlayerId()) {
    videoQueue_.owner_player_id = playerId_;
    frameQueue_.owner_player_id = playerId_;
    videoDecoder_.setOwnerPlayerId(playerId_);
    videoDecoder_.framePool_.owner_player_id = playerId_;
}

//可中断等待
bool RtspPlayer::waitRetryInterruptible(std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(retryMutex_);
    return retryCond_.wait_for(lock, delay, [this]() {
        return abortRequested_.load();
    });
}

std::chrono::milliseconds RtspPlayer::nextRetryDelay(RetryReason reason) {
    static constexpr std::chrono::milliseconds kConnectRetrySchedule[] = {
            std::chrono::milliseconds(1000),
            std::chrono::milliseconds(2000),
            std::chrono::milliseconds(2000),
    };
    static constexpr std::chrono::milliseconds kReadRetrySchedule[] = {
            std::chrono::milliseconds(200),
            std::chrono::milliseconds(500),
            std::chrono::milliseconds(1000),
    };

    if (reason == RetryReason::ConnectFailure) {
        readRetryCount_ = 0;
        const int index = std::min(connectRetryCount_,
                                   static_cast<int>(sizeof(kConnectRetrySchedule) / sizeof(kConnectRetrySchedule[0])) - 1);
        const std::chrono::milliseconds delay = kConnectRetrySchedule[index];
        ++connectRetryCount_;
        return delay;
    }

    connectRetryCount_ = 0;
    const int index = std::min(readRetryCount_,
                               static_cast<int>(sizeof(kReadRetrySchedule) / sizeof(kReadRetrySchedule[0])) - 1);
    const std::chrono::milliseconds delay = kReadRetrySchedule[index];
    ++readRetryCount_;
    return delay;
}

void RtspPlayer::resetRetryBackoff() {
    connectRetryCount_ = 0;
    readRetryCount_ = 0;
}

RtspPlayer::~RtspPlayer() {
    requestStop();
    if (readThread_.joinable()) {
        LOGD_PLAYER(playerId_, "~RtspPlayer before readThread join");
        readThread_.join();
        LOGD_PLAYER(playerId_, "~RtspPlayer after readThread join");
    }

    bool needDetach = false;
    JNIEnv* env = getJNIEnv(jvm_, &needDetach);
    LOGD_PLAYER(playerId_, "start to release");
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        // 析构阶段做“实例级”回收：
        // 1. 先把播放状态字段清回 Idle；
        // 2. 再彻底释放 frameBuffer_ 持有的双缓冲和 DirectByteBuffer 全局引用。
        releaseOwnedResources();
        frameBuffer_.reset();
        // callback global ref 的生命周期跟 player 一致，由析构统一释放。
        if (env) {
            releaseCallbackRefs(env);
        }
    }
    if (env && needDetach) {
        jvm_->DetachCurrentThread();
    }
}

void RtspPlayer::bindCallback(JNIEnv* env, jobject callback) {
    LOGD_PLAYER(playerId_, "RtspPlayer bindCallback");
    std::lock_guard<std::mutex> lock(stateMutex_);

    // 当前约定是 1 player 对应 1 组 callback。
    // 如果上层重新绑定，先释放旧 global ref，再持有新的 callback。
    releaseCallbackRefs(env);

    callbackGlobalRef_ = env->NewGlobalRef(callback);
    callbackClassGlobalRef_ = static_cast<jclass>(env->NewGlobalRef(env->GetObjectClass(callback)));
    onErrorMethod_ = env->GetMethodID(callbackClassGlobalRef_, "onError", "(I)V");
    onI420DataMethod_ = env->GetMethodID(callbackClassGlobalRef_, "onI420Data", "(Ljava/nio/ByteBuffer;IID)V");
    onSeiMethod_ = env->GetMethodID(callbackClassGlobalRef_, "onSEIData", "([B)V");

    ensureFrameBufferLocked();

    videoDecoder_.setOwnerPlayerId(playerId_);
    videoDecoder_.framePool_.owner_player_id = playerId_;
    videoDecoder_.AddVideoRefreshCallback([this](const AVFrame* frame, double ptsSeconds) {
        return handleFrame(frame, ptsSeconds);
    });
}

void RtspPlayer::start(const std::string& url) {
    bool shouldJoinPreviousThread = false;
    bool shouldRequestStop = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);

        // start 依赖 bindCallback() 建好的 Java 回调；没绑定时直接拒绝启动。
        if (!callbackGlobalRef_) {
        LOGD_PLAYER(playerId_, "start ignored because callback is not bound");
            return;
        }

        const PlayerState currentState = state_.load();
        const std::string& currentOrPendingUrl = hasPendingRtspUrl_ ? pendingRtspUrl_ : rtspUrl_;

        if (url == currentOrPendingUrl && currentState != PlayerState::Running) {
            LOGD_PLAYER(playerId_, "start ignored because url is already pending");
            return;
        }

        //说明还在正常播放
        if (currentState == PlayerState::Running) {
            if (url == rtspUrl_) {
                LOGD_PLAYER(playerId_, "start ignored because url is already playing");
                return;
            }
            LOGD_PLAYER(playerId_, "start new url=%s", url.c_str());
            pendingRtspUrl_ = url;
            hasPendingRtspUrl_ = true;
            shouldRequestStop = true;
            //说明上一次 stopPlayer() 已经发出去了，但旧线程还在清理中，还没完全回到 Idle。这时再调用 start(newUrl)，就是“清理中的再次切流
        } else if (currentState == PlayerState::Stopping) {
            pendingRtspUrl_ = url;
            hasPendingRtspUrl_ = true;
            LOGD_PLAYER(playerId_, "start saved pending url while player is stopping");
            return;
            //旧流已经停完，state_ 可能已经回到 Idle，但线程对象还挂着没收尸；这时新的 start() 进来，就先 join() 上一个线程，再启动新流
        } else if (readThread_.joinable()) {
            // 如果上一次 readThread 已经退出但 thread 对象还没 join，这里先标记出来，
            // 后面在锁外 join，避免持锁等待线程回收。
            shouldJoinPreviousThread = true;
        }
            if (shouldRequestStop) {
                requestStopInternal(false);
                return;
            }
        if (threadRunning_.load() && !shouldJoinPreviousThread) {
            LOGD_PLAYER(playerId_, "player thread already running");
            return;
        }
    }

    if (shouldJoinPreviousThread) {
        LOGD_PLAYER(playerId_, "start before joining previous read thread");
        readThread_.join();
        LOGD_PLAYER(playerId_, "start after joining previous read thread");
        std::lock_guard<std::mutex> lock(stateMutex_);
        // join 完以后再把上一次播放留下的状态字段清干净，避免脏状态带到新会话。
        releaseOwnedResources();
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    // 重新拿锁后二次确认状态，防止 join 窗口里并发 stop/start 改变状态。
    if (threadRunning_.load() || state_.load() == PlayerState::Stopping) {
        LOGD_PLAYER(playerId_, "start cancelled during restart window, state=%d running=%d",
             static_cast<int>(state_.load()),
             threadRunning_.load() ? 1 : 0);
        return;
    }

    ensureFrameBufferLocked();
    videoDecoder_.setOwnerPlayerId(playerId_);

    // 这里开始写入本次播放会话的运行参数，并把状态切到 Running。
    rtspUrl_ = url;
    isRealtime_ = isRealtimeUrl(url) ? 1 : 0;
    abortRequested_.store(false);
    threadRunning_.store(true);
    state_.store(PlayerState::Running);
    resetHevcGateState();

    // 真正的网络连接、读包、解码和清理都在后台线程完成，避免阻塞调用 start/stop 的线程。
    readThread_ = std::thread([this]() {
        readThreadLoop();
    });
}

void RtspPlayer::ensureFrameBufferLocked() {
    if (!frameBuffer_) {
        frameBuffer_ = std::make_unique<VideoFrameBuffer>(kInitialWidth, kInitialHeight, jvm_);
    }
}

void RtspPlayer::stop() {
    requestStop();
}

void RtspPlayer::requestStop() {
    requestStopInternal(true);
}

//true：先清掉 pendingRtspUrl_ / hasPendingRtspUrl_，再发 stop用在 stop() / destroy() 这种“我就是要停，不想再自动拉起”的场景
//false：不清 pending，直接发 stop，用在 start(newUrl) 触发的自动切流场景
void RtspPlayer::requestStopInternal(bool clearPendingSwitch) {
    if (clearPendingSwitch) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingRtspUrl_.clear();
        hasPendingRtspUrl_ = false;
    }

    PlayerState expected = PlayerState::Running;
    //检查 state_ 当前的值是否等于 expected（即 Running）。
    //交换：如果相等，就把 state_ 的值修改为 Stopping,并且返回true
    if (!state_.compare_exchange_strong(expected, PlayerState::Stopping)) {
        // Idle 状态下顺手把 abort 标志清掉，避免影响后续下一次 start。
        if (state_.load() == PlayerState::Idle) {
            abortRequested_.store(false);
        }
        LOGD_PLAYER(playerId_, "requestStop ignored, state=%d", static_cast<int>(state_.load()));
        return;
    }

    LOGD_PLAYER(playerId_, "requestStop begin");
    // 这里只负责“发停止请求”，不在调用线程里做 join/close_input/decoder_abort；
    // 真正的退出和资源清理由 readThreadLoop() 在后台线程里完成。
    abortRequested_.store(true);
    retryCond_.notify_all();
    // 主动打断可能阻塞在队列上的解码/渲染线程，让它们尽快从 wait 中返回并退出。
    packet_queue_abort(&videoQueue_);
    frame_queue_abort(&frameQueue_);
}

int RtspPlayer::handleFrame(const AVFrame* frame, double ptsSeconds) {
    if (!frame || !frameBuffer_ || !callbackGlobalRef_ || !onI420DataMethod_) {
        return -1;
    }

    bool needDetach = false;
    JNIEnv* env = getJNIEnv(jvm_, &needDetach);
    if (!env) {
        return -1;
    }

    if (!frameBuffer_->copyFromFrame(frame)) {
        if (needDetach) {
            jvm_->DetachCurrentThread();
        }
        return -1;
    }
    jobject byteBuffer = frameBuffer_->getDirectByteBuffer(env);
    env->CallVoidMethod(callbackGlobalRef_, onI420DataMethod_, byteBuffer, frame->width, frame->height, ptsSeconds);
    if (byteBuffer) {
        env->DeleteLocalRef(byteBuffer);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (needDetach) {
        jvm_->DetachCurrentThread();
    }

    return 0;
}

void RtspPlayer::readThreadLoop() {
    AVFormatContext* fmtCtx = nullptr;
    int videoStreamIndex = -1;
    ReadState state = ReadState::Init;
    RetryReason retryReason = RetryReason::ConnectFailure;

    auto finishReadThread = [this]() -> bool {
        LOGD_PLAYER(playerId_, "readThreadLoop finishing");
        std::lock_guard<std::mutex> lock(stateMutex_);
        releaseOwnedResources();
        if (!hasPendingRtspUrl_) {
            return false;
        }

        rtspUrl_ = pendingRtspUrl_;
        pendingRtspUrl_.clear();
        hasPendingRtspUrl_ = false;
        isRealtime_ = isRealtimeUrl(rtspUrl_) ? 1 : 0;
        abortRequested_.store(false);
        threadRunning_.store(true);
        state_.store(PlayerState::Running);
        resetHevcGateState();
        LOGD_PLAYER(playerId_, "restart with pending url after cleanup");
        return true;
    };

    for (;;) {
        while (!abortRequested_.load()) {
            switch (state) {
                case ReadState::Init: {
                    fmtCtx = avformat_alloc_context();
                    if (!fmtCtx) {
                        retryReason = RetryReason::ConnectFailure;
                        state = ReadState::WaitRetry;
                        break;
                    }
                    fmtCtx->interrupt_callback.callback = &RtspPlayer::ffmpegInterruptCallback;
                    fmtCtx->interrupt_callback.opaque = this;
                    LOGD_PLAYER(playerId_, "AVFormatContext interrupt callback attached");

                    packet_queue_init(&videoQueue_);
                    frame_queue_init(&frameQueue_, VIDEO_PICTURE_QUEUE_SIZE);
                    state = ReadState::Connecting;
                    break;
                }

                case ReadState::Connecting: {
                    AVDictionary* opts = nullptr;
                    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
                    av_dict_set(&opts, "stimeout", "3000000", 0);
                    av_dict_set(&opts, "probesize", "1024000", 0);      // 1MB
                    av_dict_set(&opts, "analyzeduration", "400000", 0); // 400ms

                    armInterruptTimeout(kConnectInterruptTimeoutUs);
                    int ret = avformat_open_input(&fmtCtx, rtspUrl_.c_str(), nullptr, &opts);
                    clearInterruptTimeout();  
                    av_dict_free(&opts);
                    if (ret < 0) {
                        LOGD_PLAYER(playerId_, "avformat_open_input failed: %s", avErrorStr(ret).c_str());
                        retryReason = RetryReason::ConnectFailure;
                        state = ReadState::WaitRetry;
                        break;
                    }
                    armInterruptTimeout(kConnectInterruptTimeoutUs);
                    ret = avformat_find_stream_info(fmtCtx, nullptr);
                    clearInterruptTimeout(); 
                    if (ret < 0) {
                        LOGD_PLAYER(playerId_, "avformat_find_stream_info failed: %s", avErrorStr(ret).c_str());
                        reportFfmpegError(ret);
                        retryReason = RetryReason::ConnectFailure;
                        state = ReadState::WaitRetry;
                        break;
                    }

                    videoStreamIndex = -1;
                    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
                        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                            videoStreamIndex = static_cast<int>(i);
                            break;
                        }
                    }

                    if (videoStreamIndex < 0) {
                        reportFfmpegError(AVERROR_STREAM_NOT_FOUND);
                        retryReason = RetryReason::ConnectFailure;
                        state = ReadState::WaitRetry;
                        break;
                    }

                    videoDecoder_.time_base = fmtCtx->streams[videoStreamIndex]->time_base;
                    ret = streamComponentOpen(videoStreamIndex, fmtCtx);
                    if (ret < 0) {
                        reportFfmpegError(ret);
                        retryReason = RetryReason::ConnectFailure;
                        state = ReadState::WaitRetry;
                        break;
                    }

                    resetRetryBackoff();
                    lastVideoPacketRecvUs_.store(nowMonotonicUs());
                    videoPacketTimeoutWarned_.store(false);
                    state = ReadState::Reading;
                    break;
                }

                case ReadState::Reading: {
                    const ReadExitReason reason = readPacketLoop(fmtCtx, videoStreamIndex);
                    if (reason == ReadExitReason::Abort) {
                        state = ReadState::Stopped;
                    } else if (reason == ReadExitReason::Eof || reason == ReadExitReason::Error) {
                        retryReason = RetryReason::ReadFailure;
                        state = isRealtime_ ? ReadState::WaitRetry : ReadState::Stopped;
                    }
                    break;
                }

                case ReadState::WaitRetry: {
                    const std::chrono::milliseconds retryDelay = nextRetryDelay(retryReason);
                    LOGD_PLAYER(playerId_, "state is waitRetry, reason=%s, connect_retry_count=%d, read_retry_count=%d, delay_ms=%lld",
                         retryReason == RetryReason::ConnectFailure ? "connect" : "read",
                         connectRetryCount_, readRetryCount_,
                         static_cast<long long>(retryDelay.count()));
                    cleanupPlaybackState(&fmtCtx);
                    videoStreamIndex = -1;

                    if (abortRequested_.load()) {
                        state = ReadState::Stopped;
                        break;
                    }

                    if (waitRetryInterruptible(retryDelay)) {
                        state = ReadState::Stopped;
                        break;
                    }
                    state = ReadState::Init;
                    break;
                }

                case ReadState::Stopped: {
                    LOGD_PLAYER(playerId_, "state is stopped");
                    cleanupPlaybackState(&fmtCtx);
                    if (finishReadThread()) {
                        videoStreamIndex = -1;
                        state = ReadState::Init;
                        retryReason = RetryReason::ConnectFailure;
                        continue;
                    }
                    return;
                }
            }
        }

        cleanupPlaybackState(&fmtCtx);
        if (finishReadThread()) {
            videoStreamIndex = -1;
            state = ReadState::Init;
            retryReason = RetryReason::ConnectFailure;
            continue;
        }
        return;
    }
}

ReadExitReason RtspPlayer::readPacketLoop(AVFormatContext* fmtCtx, int videoStreamIndex) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        return ReadExitReason::Error;
    }

    int eofCount = 0;
    ReadExitReason exitReason = ReadExitReason::Abort;
    bool waitVideoKeyFrame = true;

    while (!abortRequested_.load()) {
        if (videoQueue_.abort_request == 1) {
            LOGD_PLAYER(playerId_, "videoQueue_.abort_request = 1");
            exitReason = ReadExitReason::Abort;
            break;
        }

        const int64_t nowUs = nowMonotonicUs();
        const int64_t lastRecvUs = lastVideoPacketRecvUs_.load();
        if (lastRecvUs > 0) {
            const int64_t gapUs = nowUs - lastRecvUs;
            if (gapUs >= kVideoPacketHardTimeoutUs) {
                LOGW_PLAYER(playerId_, "no video packet for %lld ms, force reconnect",
                     static_cast<long long>(gapUs / 1000));
                exitReason = ReadExitReason::Error;
                break;
            }
            if (gapUs >= kVideoPacketWarnTimeoutUs &&
                !videoPacketTimeoutWarned_.exchange(true)) {
                LOGW_PLAYER(playerId_, "no video packet for %lld ms, stream may be stalled",
                     static_cast<long long>(gapUs / 1000));
            }
        }

        armInterruptTimeout(kReadInterruptTimeoutUs);
        const int ret = av_read_frame(fmtCtx, pkt);
        clearInterruptTimeout();
        if (ret < 0) {
            LOGD_PLAYER(playerId_, "av_read_frame failed: %s", avErrorStr(ret).c_str());
            if (ret == AVERROR_EXIT) {
                LOGD_PLAYER(playerId_, "av_read_frame interrupted by interrupt_callback");
                exitReason = abortRequested_.load() ? ReadExitReason::Abort
                                                    : ReadExitReason::Error;
                break;
            }
            if (ret == AVERROR_EOF || avio_feof(fmtCtx->pb)) {
                if (isRealtime_ && eofCount < 15) {
                    ++eofCount;
                    LOGD_PLAYER(playerId_, "eofCount = %d", eofCount);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                if (videoStreamIndex >= 0) {
                    packet_queue_put_nullpacket(&videoQueue_, videoStreamIndex);
                }
                exitReason = ReadExitReason::Eof;
                break;
            }

            if (fmtCtx->pb && fmtCtx->pb->error) {
                exitReason = ReadExitReason::Error;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (pkt->stream_index == videoStreamIndex) {
            //更新最近一帧视频的时间
            lastVideoPacketRecvUs_.store(nowMonotonicUs());
            videoPacketTimeoutWarned_.store(false);
            if (waitVideoKeyFrame && !(pkt->flags & AV_PKT_FLAG_KEY)) {
            LOG_QUEUE_PLAYER(playerId_, "drop non-key packet while waiting for stream start key frame");
                av_packet_unref(pkt);
                continue;
            }
            // if (useHw_ == 1 && !hevcStartedOnIrap_) {
            //     bool sawIrap = false;
            //    //updateHevcGateFlags() 会扫描当前这个 pkt，看看里面有没有 VPS/SPS/PPS/IRAP
            //     updateHevcGateFlags(pkt->data,
            //                         pkt->size,
            //                         &hevcSeenVps_,
            //                         &hevcSeenSps_,
            //                         &hevcSeenPps_,
            //                         &sawIrap);


            //         //参数集现在终于凑齐了，可以认为解码前置条件之一满足了
            //     if (!hevcParamsReady_ && hevcSeenVps_ && hevcSeenSps_ && hevcSeenPps_) {
            //         hevcParamsReady_ = true;
            //         LOGD("HEVC gate: in-band VPS/SPS/PPS ready");
            //     }

            //     //只要“参数集没准备好”或者“当前包里没看到 IRAP”，这个包就先丢掉，不送进解码器
            //     if (!hevcParamsReady_ || !sawIrap) {
            //         if (sawIrap && !hevcParamsReady_) {
            //             LOGD("HEVC gate: drop IRAP because VPS/SPS/PPS not ready yet");
            //         }
            //         av_packet_unref(pkt);
            //         continue;
            //     }

            //     hevcStartedOnIrap_ = true;
            //     LOGD("HEVC gate: params ready and IRAP seen, start feeding decoder");
            // }
            if (waitVideoKeyFrame) {
                waitVideoKeyFrame = false;
                LOGD_PLAYER(playerId_, "recv first key packet, start feeding decoder");
            }
            extractHevcSeiAndCallback(pkt->data, pkt->size);
            packet_queue_put(&videoQueue_, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
    return exitReason;
}

void RtspPlayer::resetHevcGateState() {
    hevcParamsReady_ = false;
    hevcStartedOnIrap_ = false;
    hevcSeenVps_ = false;
    hevcSeenSps_ = false;
    hevcSeenPps_ = false;
}

int RtspPlayer::initDecoderWithFallback(AVFormatContext* fmtCtx, int streamIndex, AVCodecContext** avctx, int* useHw) {
    const AVCodec* codec = avcodec_find_decoder_by_name("hevc_mediacodec");
    int ret = 0;
    hevcParamsReady_ = false;
    hevcStartedOnIrap_ = false;
    hevcSeenVps_ = false;
    hevcSeenSps_ = false;
    hevcSeenPps_ = false;

    if (codec) {
        *avctx = avcodec_alloc_context3(codec);
        if (!(*avctx)) {
            return AVERROR(ENOMEM);
        }

        ret = avcodec_parameters_to_context(*avctx, fmtCtx->streams[streamIndex]->codecpar);
        if (ret < 0) {
            avcodec_free_context(avctx);
            return ret;
        }

        (*avctx)->pkt_timebase = fmtCtx->streams[streamIndex]->time_base;
        ret = avcodec_open2(*avctx, codec, nullptr);
        if (ret >= 0) {
            *useHw = 1;
            return 0;
        }

        avcodec_free_context(avctx);
    }

    codec = avcodec_find_decoder(fmtCtx->streams[streamIndex]->codecpar->codec_id);
    if (!codec) {
        return AVERROR_DECODER_NOT_FOUND;
    }

    *avctx = avcodec_alloc_context3(codec);
    if (!(*avctx)) {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(*avctx, fmtCtx->streams[streamIndex]->codecpar);
    if (ret < 0) {
        avcodec_free_context(avctx);
        return ret;
    }

    (*avctx)->pkt_timebase = fmtCtx->streams[streamIndex]->time_base;
    ret = avcodec_open2(*avctx, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(avctx);
        return ret;
    }

    *useHw = 0;
    return 0;
}

int RtspPlayer::streamComponentOpen(int streamIndex, AVFormatContext* fmtCtx) {
    if (streamIndex < 0 || streamIndex >= static_cast<int>(fmtCtx->nb_streams)) {
        return AVERROR(EINVAL);
    }

    AVCodecContext* avctx = nullptr;
    const int ret = initDecoderWithFallback(fmtCtx, streamIndex, &avctx, &useHw_);
    if (ret < 0) {
        return ret;
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        videoDecoder_.decoder_init(avctx, &videoQueue_, &frameQueue_);
        videoDecoder_.decoder_start("video_thread");
        videoDecoder_.refresh_start("refresh_thread", &frameQueue_);
    }

    return 0;
}

void RtspPlayer::notifyJavaError(int errorCode) {
    if (!callbackGlobalRef_ || !onErrorMethod_) {
        return;
    }

    bool needDetach = false;
    JNIEnv* env = getJNIEnv(jvm_, &needDetach);
    if (!env) {
        return;
    }

    env->CallVoidMethod(callbackGlobalRef_, onErrorMethod_, errorCode);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (needDetach) {
        jvm_->DetachCurrentThread();
    }
}

void RtspPlayer::notifySeiToJava(const uint8_t* sei, int size) {
    if (!callbackGlobalRef_ || !onSeiMethod_ || !sei || size <= 0) {
        return;
    }

    bool needDetach = false;
    JNIEnv* env = getJNIEnv(jvm_, &needDetach);
    if (!env) {
        return;
    }

    jbyteArray array = env->NewByteArray(size);
    if (array) {
        env->SetByteArrayRegion(array, 0, size, reinterpret_cast<const jbyte*>(sei));
        env->CallVoidMethod(callbackGlobalRef_, onSeiMethod_, array);
        env->DeleteLocalRef(array);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (needDetach) {
        jvm_->DetachCurrentThread();
    }
}

void RtspPlayer::reportFfmpegError(int ffErr) {
    notifyJavaError(static_cast<int>(map_ffmpeg_error(ffErr)));
}


// 先把码流里为了防止误识别 start code 而插入的 0x03 去掉，再按真实 SEI 数据格式解析。
// 只有 payloadType=5(user_data_unregistered) 时，才打印业务日志并回调 Java。
//一个pkt里面可能有很多NALU，每个NALU里又可能有多个payload，所以要循环解析。
bool RtspPlayer::extractHevcSeiAndCallback(const uint8_t* data, int size) {
    if (!data || size <= 0) {
        return false;
    }

    const uint8_t* end = data + size;
    const uint8_t* p = data;
    bool found_sei = false;

    while (p + 4 < end) {
        if (p[0] == 0 && p[1] == 0 &&
            (p[2] == 1 || (p[2] == 0 && p[3] == 1))) {

            int start_len = (p[2] == 1) ? 3 : 4;
            const uint8_t* nal = p + start_len;
            if (nal + 2 > end) break;

            uint8_t nal_type = (nal[0] >> 1) & 0x3F;

            // 找下一个 start code
            const uint8_t* next = nal;
            while (next + 4 < end &&
                   !(next[0] == 0 && next[1] == 0 &&
                     (next[2] == 1 || (next[2] == 0 && next[3] == 1)))) {
                ++next;
            }

            int nal_size = next - nal;

            // ===== SEI NALU =====
            if (nal_type == 39 || nal_type == 40) {
                found_sei = true;

                if (nal_size > 2) {
                    const uint8_t* sei_ebsp = nal + 2; // 跳过NAL头
                    int sei_ebsp_size = nal_size - 2;
                    std::vector<uint8_t> sei_rbsp = hevcEbspToRbsp(sei_ebsp, sei_ebsp_size);
                    if (sei_rbsp.empty()) {
                        p = next;
                        continue;
                    }

                    const uint8_t* sei = sei_rbsp.data();
                    int sei_size = static_cast<int>(sei_rbsp.size());

                    LOG_SEI_PLAYER(playerId_, "==== SEI NALU found, size=%d ====", sei_size);

                    int offset = 0;

                    // ⭐⭐⭐ 支持多个 payload
                    while (offset + 1 < sei_size) {

                        // ===== payloadType =====
                        int payloadType = 0;
                        while (offset < sei_size && sei[offset] == 0xFF) {
                            payloadType += 255;
                            offset++;
                        }
                        if (offset >= sei_size) break;
                        payloadType += sei[offset++];

                        // ===== payloadSize =====
                        int payloadSize = 0;
                        while (offset < sei_size && sei[offset] == 0xFF) {
                            payloadSize += 255;
                            offset++;
                        }
                        if (offset >= sei_size) break;
                        payloadSize += sei[offset++];

                        LOG_SEI_PLAYER(playerId_, "payloadType=%d, payloadSize=%d", payloadType, payloadSize);

                        // 越界保护
                        if (offset + payloadSize > sei_size) {
        LOGW_PLAYER(playerId_, "SEI parse out of bounds, stop parsing");
                            break;
                        }

                        // ===== 解析 user_data_unregistered =====
                        // 业务相关的 SEI 数据通常放在 payloadType=5 的 user_data_unregistered 里，且前16字节是 UUID，可以用来区分不同类型的 SEI。
                        if (payloadType == 5 && payloadSize > 16) {
                        }

                        // 👉 跳到下一个 payload
                        offset += payloadSize;

                        // ⭐ 处理 rbsp_trailing_bits
                        if (offset < sei_size && sei[offset] == 0x80) {
                            offset++;
                            break;
                        }
                    }

                    // 回调给 Java 的是去掉 emulation prevention byte 后的真实 SEI RBSP。
                    notifySeiToJava(sei, sei_size);
                }
            }

            p = next;
        } else {
            ++p;
        }
    }

    return found_sei;
}


void RtspPlayer::releaseCallbackRefs(JNIEnv* env) {
    if (callbackGlobalRef_) {
        env->DeleteGlobalRef(callbackGlobalRef_);
        callbackGlobalRef_ = nullptr;
    }

    if (callbackClassGlobalRef_) {
        env->DeleteGlobalRef(callbackClassGlobalRef_);
        callbackClassGlobalRef_ = nullptr;
    }

    onErrorMethod_ = nullptr;
    onSeiMethod_ = nullptr;
    onI420DataMethod_ = nullptr;
}

void RtspPlayer::cleanupPlaybackState(AVFormatContext** fmtCtx) {
    if (fmtCtx && *fmtCtx) {
        LOGD_PLAYER(playerId_, "before avformat_close_input");
        armInterruptTimeout(kReadInterruptTimeoutUs);
        avformat_close_input(fmtCtx);
        LOGD_PLAYER(playerId_, "after avformat_close_input");
        clearInterruptTimeout();
    }

    videoDecoder_.decoder_abort();
    videoDecoder_.decoder_destroy();
    packet_queue_destroy(&videoQueue_);
    frame_queue_destroy(&frameQueue_);
    LOGD_PLAYER(playerId_, "cleanupPlaybackState");
}

void RtspPlayer::releaseOwnedResources() {
    threadRunning_.store(false);
    abortRequested_.store(false);
    state_.store(PlayerState::Idle);
    resetRetryBackoff();
    lastVideoPacketRecvUs_.store(0);
    videoPacketTimeoutWarned_.store(false);
    rtspUrl_.clear();
    useHw_ = 0;
    isRealtime_ = 0;
    // 这里只回收“会话级”状态，不销毁 frameBuffer_。
    // 这样 stop 后再次 start 可以继续复用同一个输出缓冲对象；
    // 真正的 frameBuffer_ 释放放到 destroy/析构阶段统一处理。
    LOGD_PLAYER(playerId_, "releaseOwnedResources over, keep frameBuffer for restart");

}

bool RtspPlayer::isRealtimeUrl(const std::string& url) {
    return url.rfind("rtp", 0) == 0 || url.rfind("rtsp", 0) == 0 || url.rfind("rtmp", 0) == 0;
}

std::string RtspPlayer::avErrorStr(int errnum) {
    char buf[256];
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}
