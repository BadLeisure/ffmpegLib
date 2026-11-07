#ifndef FF_FFPLAY_H
#define FF_FFPLAY_H

#include "ffplay_def.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

extern "C"{
#include "libavcodec/avcodec.h"
}

class Decoder
{
public:
    int owner_player_id_ = -1;
    int packet_pending_ = 0;
    AVPacket pkt_;
    PacketQueue	*queue_ = nullptr;         // 数据包队列
    AVCodecContext	*avctx_ = nullptr;     // 解码器上下文

    AVRational time_base;
    FrameQueue *framequeue = nullptr;
    FramePool framePool_{};
    int renderKeyFrameReady_ = 0; // 渲染侧兜底开闸：收到首个可渲染 I 帧后置为 1


    //解码线程
    std::thread decoder_thread_;

    std::thread video_refresh_thread_;

    std::function<int(const AVFrame*,double)> mVideoCallback;
    std::mutex decoder_mutex_;
    std::atomic<bool> switching_{false};
    std::mutex output_mutex_;
    int session_generation = -1;

    // Low-frequency video pipeline diagnostics.  Values are accumulated in
    // memory and emitted at most once per second to avoid logcat I/O jitter.
    std::mutex video_diag_mutex_;
    int64_t video_diag_window_start_us_ = 0;
    int64_t last_packet_recv_us_ = 0;
    double last_packet_pts_seconds_ = 0.0;
    bool has_last_packet_pts_ = false;
    int video_diag_packets_ = 0;
    int video_diag_decoded_frames_ = 0;
    int video_diag_rendered_frames_ = 0;
    int video_diag_late_drops_ = 0;
    int video_diag_large_pts_gap_count_ = 0;
    int64_t last_decoded_pts_ = AV_NOPTS_VALUE;
    int video_diag_decoded_pts_interval_count_ = 0;
    double video_diag_decoded_pts_interval_sum_ms_ = 0.0;
    double video_diag_decoded_pts_interval_min_ms_ = 0.0;
    double video_diag_decoded_pts_interval_max_ms_ = 0.0;
    double video_diag_packet_recv_gap_max_ms_ = 0.0;
    double video_diag_packet_pts_gap_max_ms_ = 0.0;
    double video_diag_packet_queue_wait_max_ms_ = 0.0;
    double video_diag_decode_max_ms_ = 0.0;
    double video_diag_frame_wait_max_ms_ = 0.0;
    double video_diag_frame_residence_max_ms_ = 0.0;
    double video_diag_schedule_late_max_ms_ = 0.0;
    double video_diag_callback_max_ms_ = 0.0;

    void decoder_abort();
    // 切流时清空队列并刷新解码器，但不退出常驻线程。
    void decoder_flush_for_switch();
    // 切流或重新播放时重新绑定队列。
    void decoder_rebind_queue(PacketQueue* queue, FrameQueue* frameQueue);
    //判断解码线程 video_thread 是否存在
    bool decoder_thread_running() const;
    //判断刷新线程 video_refresh_thread 是否存在
    bool refresh_thread_running() const;

    ~Decoder();

    void decoder_init(AVCodecContext *avctx, PacketQueue *queue,FrameQueue *frameQueue);

    // 创建和启动线程
    int decoder_start( const char *thread_name);

    int refresh_start(const char *thread_name,FrameQueue *fq);

    // 销毁
    void decoder_destroy();

    //真正的解码逻辑
    int decoder_decode_frame(AVFrame *frame);

    //获取一帧
    int get_video_frame(AVFrame *frame);

    //在解码线程中执行这个函数
    int video_thread();

    int video_refresh_thread(FrameQueue *fq);


    void AddVideoRefreshCallback(std::function<int(const AVFrame*,double)> callback) {
        mVideoCallback = std::move(callback);
    }

    void setOwnerPlayerId(int playerId) {
        owner_player_id_ = playerId;
    }

    void recordVideoPacket(int64_t recvUs, double ptsSeconds);
    void recordDecodedFrame(int64_t pts, AVRational frameTimeBase);
    void recordRenderedFrame();
    void recordLateDrop();
    void recordPacketQueueWait(double waitMs);
    void recordDecodeCost(double costMs);
    void recordFrameQueueWait(double waitMs);
    void recordFrameResidence(double residenceMs);
    void recordScheduleLate(double lateMs);
    void recordCallbackCost(double costMs);



};




#endif // FF_FFPLAY_H
