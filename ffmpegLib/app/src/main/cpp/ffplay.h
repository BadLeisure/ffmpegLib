#ifndef FF_FFPLAY_H
#define FF_FFPLAY_H

#include "ffplay_def.h"
#include <atomic>
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
    std::thread *decoder_thread_ = NULL;

    std::thread *video_refresh_thread_ = NULL;

    std::function<int(const AVFrame*,double)> mVideoCallback;
    std::mutex decoder_mutex_;
    std::atomic<bool> switching_{false};
    std::mutex output_mutex_;
    int session_generation = -1;

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



};




#endif // FF_FFPLAY_H
