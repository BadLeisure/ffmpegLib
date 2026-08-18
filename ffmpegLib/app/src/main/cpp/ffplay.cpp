
#include "ffplay.h"
#include "libyuv/convert.h"
#include <algorithm>
#define LOG_TAG "Decoder"
#include "native_log.h"

extern "C"{
#include "libavutil/time.h"
#include "libavutil/pixdesc.h"

}


int Decoder::decoder_start( const char *thread_name)
{
    if (!queue_) {
        LOGD_PLAYER(owner_player_id_, "decoder_start skipped: queue_ is null");
        return -1;
    }
    // 启用包队列
    packet_queue_start(queue_);
    //创建一个新线程，让这个线程去执行 Decoder 对象的 video_thread 成员函数
    decoder_thread_ = new std::thread(&Decoder::video_thread, this);
    return 0;
}

int Decoder::refresh_start(const char *thread_name,FrameQueue *fq){
    if (!fq) {
        LOGD_PLAYER(owner_player_id_, "refresh_start skipped: frame queue is null");
        return -1;
    }

    video_refresh_thread_ = new std::thread(&Decoder::video_refresh_thread,this,fq);
    return 0;
}

int Decoder::video_thread()
{

    LOGD_PLAYER(owner_player_id_, "video_thread start");
    pthread_setname_np(pthread_self(), "VideoDecodeThread");
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    int ret;

    if (!frame) {
        return AVERROR(ENOMEM);
    }
    for (;;) {
        //解码得到的frame通过打印可以看出来是YUV420p格式的
        ret = get_video_frame(frame);
        if (ret < 0) {
            break;  // 解码结束
        }
        if (frame->pts == AV_NOPTS_VALUE ||
            frame->format == AV_PIX_FMT_NONE ||
            frame->data[0] == nullptr) {

            av_frame_unref(frame);
            continue;
        }

        // 包级 key frame gate 之外，再保留一层渲染侧兜底开闸。
        if (!renderKeyFrameReady_) {
            if (frame->key_frame != 1) {
                LOG_QUEUE_PLAYER(owner_player_id_, "drop decoded non-I frame before render gate opens");
                av_frame_unref(frame);
                continue;
            }
            renderKeyFrameReady_ = 1;
            LOGD_PLAYER(owner_player_id_, "render gate opened by first decoded I frame");
        }

        // 这里不再创建中间 I420 AVFrame，直接把解码原始帧入队。
        // 后续由 VideoFrameBuffer 直接把原始帧写入最终双缓冲输出区，queued_frame 变成一个独立的 AVFrame*
        AVFrame *queued_frame = frame_pool_acquire(&framePool_);
        if (!queued_frame) {
            av_frame_unref(frame);
            continue;
        }

        av_frame_unref(queued_frame);
        if (av_frame_ref(queued_frame, frame) < 0) {
            frame_pool_release(&framePool_, queued_frame);
            av_frame_unref(frame);
            continue;
        }

        frame_queue_push(framequeue, queued_frame);
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    FrameQueueStats stats = frame_queue_get_stats(framequeue);
    LOGD_PLAYER(owner_player_id_, "decode thread leave, total=%d, queue_drop=%d, late_drop=%d, rendered=%d, total_drop=%d",
         stats.total, stats.drop, stats.late_drop, stats.rendered, stats.drop + stats.late_drop);
    return 0;
}

// 第一版自适应策略：
// 1) 用 EMA 跟踪真实帧间隔，替代固定 REFRESH_RATE；
// 2) 用 EMA 跟踪 backlog，避免仅凭某一帧的瞬时积压就剧烈切换策略。
static constexpr double kDefaultFrameIntervalSec = 0.033;
static constexpr double kFrameIntervalEmaAlpha = 0.10;
static constexpr double kBacklogEmaAlpha = 0.15;
static constexpr double kMinLateThresholdSec = 0.02;
static constexpr double kNormalLateFrames = 1.8;
static constexpr double kBacklogLateFrames = 1.0;
static constexpr double kBacklogSleepFactor = 0.25;
static constexpr double kMinSleepCapSec = 0.004;
#define QUEUE_WM_MID 6             // 中水位：12 帧队列的 50%，开始缩短休眠
#define QUEUE_WM_HIGH 9            // 高水位：12 帧队列的 75%，跳过休眠快速追帧

static inline double now_seconds() {
    return av_gettime_relative() / 1000000.0;
}

static inline double update_ema(double old_value, double current_value, double alpha) {
    if (old_value <= 0.0) {
        return current_value;
    }
    return alpha * current_value + (1.0 - alpha) * old_value;
}

/**
 * video_refresh_thread：按队列积压动态调度

  - size >= 9：不 sleep，快速追帧。
  - size 6~8：缩短 sleep（最多 8ms）。
  - size <= 5：按正常节奏 sleep。
  - 同时把 MAX_LATE 改为动态阈值（积压时更严格丢晚帧）。
 * @param fq
 * @return
 */

int Decoder::video_refresh_thread(FrameQueue *fq)
{
    LOGD_PLAYER(owner_player_id_, "video_refresh_thread");
    pthread_setname_np(pthread_self(), "video_refresh_thread");

    double frame_timer = 0.0;     // 理论下一帧显示时间（秒）
    double last_frame_pts = NAN;  // 上一帧 pts（秒）
    double last_frame_duration = kDefaultFrameIntervalSec;
    //用 pts delta 持续更新 avgFrameIntervalSec，通过 EMA 算出来的平均帧间隔
    int64_t session_base_pts = AV_NOPTS_VALUE;
    double avgFrameIntervalSec = kDefaultFrameIntervalSec;
    // 用队列长度持续更新 avgBacklog，通过 EMA 算出来的平均队列积压
    double avgBacklog = 0.0;


    while (!fq->abort_request) {
        AVFrame *frame = frame_queue_pop(fq);
        if (!frame) continue;
        LOG_FRAME_PLAYER(owner_player_id_, "frame_queue_pop,frame->pts = %ld", frame->pts);

        // 按队列积压动态调节渲染策略：积压越高，越偏向追实时。
        const int queue_backlog = frame_queue_size(fq);
        avgBacklog = update_ema(avgBacklog, static_cast<double>(queue_backlog), kBacklogEmaAlpha);
        const bool high_backlog = avgBacklog >= QUEUE_WM_HIGH;
        const bool mid_backlog = avgBacklog >= QUEUE_WM_MID;
        const bool high_fps = avgFrameIntervalSec > 0.0 && avgFrameIntervalSec <= 0.025;
        // 把刷新线程观察到的“共享拥塞状态”写回 FrameQueue。
        // 这样 frame_queue_push() 入队时只需要看中拥塞 / 高拥塞 / 高帧率这 3 个轻量标志。
        frame_queue_update_adaptive_state(fq, mid_backlog, high_backlog, high_fps);
        const double late_threshold = std::max(
                avgFrameIntervalSec * (high_backlog ? kBacklogLateFrames : kNormalLateFrames),
                kMinLateThresholdSec);
        // 正常情况下，最多允许按“平均一帧的时间”来 sleep
        const double normal_sleep_cap = avgFrameIntervalSec;
        //当队列已经开始积压时，sleep 上限要明显缩短
        const double backlog_sleep_cap = std::max(avgFrameIntervalSec * kBacklogSleepFactor, kMinSleepCapSec);

        if(frame->pts == AV_NOPTS_VALUE){
            frame_pool_release(&framePool_,frame);
            continue;
        }

        if(session_base_pts == AV_NOPTS_VALUE){
            session_base_pts = frame->pts;
            LOGD_PLAYER(owner_player_id_, "sessopn_base_pts=%lld",static_cast<long long>(session_base_pts));
        }

        double pts_s = (frame->pts - session_base_pts) * av_q2d(time_base);
        double now = now_seconds();
        if (std::isnan(last_frame_pts)) {
            frame_timer = now;
            last_frame_pts = pts_s;

            mVideoCallback(frame, pts_s);
            frame_pool_release(&framePool_, frame);
            continue;
        }
        double delay = pts_s - last_frame_pts;
        LOG_FRAME_PLAYER(owner_player_id_, "pts_s = %f,last_frame_pts = %f,delay = %f,now = %f,frame_timer = %f",
                  pts_s, last_frame_pts, delay, now, frame_timer);
        if (delay <= 0 || delay > 1.0) {
            delay = last_frame_duration; // 防止 pts 异常或倒退
        } else {
            last_frame_duration = delay;
            avgFrameIntervalSec = update_ema(avgFrameIntervalSec, delay, kFrameIntervalEmaAlpha);
        }
         double target_time = frame_timer + delay;

/**
 * 典型情况有这几种：
        解码线程产帧太慢：当前这一帧终于拿到了，但它本来应该更早显示。比如网络抖动、解码耗时突然变长、I 帧特别重，都会导致 now 明显超过 target_time。
        渲染线程自己被阻塞了：哪怕队列里只有 1-2 帧，如果渲染线程被别的事情卡住了，比如 Java 层回调、锁竞争、主线程处理慢，等它恢复时这帧已经错过显示时机了
 */
        // 过期帧直接丢掉，但是要推进时间轴，防止连续丢帧。
        if (now - target_time > late_threshold) {
            frame_timer = now;
            last_frame_pts = pts_s;
            frame_queue_record_late_drop(fq);
            frame_pool_release(&framePool_, frame);
            LOG_QUEUE_PLAYER(owner_player_id_, "drop late frame, late=%f, threshold=%f, backlog=%d",
                      now - target_time, late_threshold, queue_backlog);
            continue;
        }

        if (now < target_time) {
            // 积压高时不休眠直接追帧；中水位时只短休眠，减少排队延迟。
            if (!high_backlog) {
                const double sleep_cap = mid_backlog ? backlog_sleep_cap : normal_sleep_cap;
                const double sleep_time = FFMIN(target_time - now, sleep_cap);
                LOG_FRAME_PLAYER(owner_player_id_, "sleep %f seconds to refresh", sleep_time);
                if (sleep_time > 0.0) {
                    av_usleep((int64_t)(sleep_time * 1000000));
                }
            }
            now = now_seconds();
        }

        //先渲染
        mVideoCallback(frame,pts_s);
        frame_queue_record_render(fq);
        // 到时间了 → 更新 frame_timer
        frame_timer = now;
        last_frame_pts = pts_s;
        frame_pool_release(&framePool_, frame);
    }

    FrameQueueStats stats = frame_queue_get_stats(fq);
    LOGD_PLAYER(owner_player_id_, "video_refresh_thread leave, total=%d, queue_drop=%d, late_drop=%d, rendered=%d, total_drop=%d",
         stats.total, stats.drop, stats.late_drop, stats.rendered, stats.drop + stats.late_drop);
    return 0;
}

// 返回值-1: 请求退出
//       0: 解码已经结束了，不再有数据可以读取
//       1: 获取到解码后的frame
int Decoder::decoder_decode_frame(AVFrame * frame)
{
    int ret = AVERROR(EAGAIN);
    for (;;) {
        AVPacket pkt;
        // 1. 流连续情况下获取解码后的帧
            do {

                // 1.2. 获取解码帧
                if(avctx_->codec_type == AVMEDIA_TYPE_VIDEO){
                    ret = avcodec_receive_frame(avctx_, frame);
                    if (ret >= 0) {
                        frame->pts = frame->best_effort_timestamp;
                    }
                }
                // 1.3. 检查解码是否已经结束，解码结束返回0
                //只有当代码发送（空数据包）后，解码器会启动 “结束流程”：处理剩余数据→输出所有帧→最终返回AVERROR_EOF，在这个代码里面是packet_queue_put_nullpacket放入的packet
                if (ret == AVERROR_EOF) {
                    LOGD_PLAYER(owner_player_id_, "avcodec_flush_buffers(avctx_)");
                    avcodec_flush_buffers(avctx_);
                    return 0;
                }
                // 1.4. 正常解码返回1
                if (ret >= 0) {
                    return 1;
                }
            } while (ret != AVERROR(EAGAIN));   // 1.5 没帧可读时ret返回EAGIN，需要继续送packet


            // 2.1 如果没有数据可读则唤醒read_thread, 实际是continue_read_thread SDL_cond
            //            if (queue_->nb_packets == 0)  // 没有数据可读
            //                SDL_CondSignal(empty_queue_cond);// 通知read_thread放入packet
            // 2.2 如果还有pending的packet则使用它


            if (packet_pending_) {
                av_packet_move_ref(&pkt, &pkt_);
                packet_pending_ = 0;
            } else {
                // 2.3 阻塞式读取packet
                if (packet_queue_get(queue_, &pkt, 1) < 0) {
                    return -1;
                }
            }

        // 3 将packet送入解码器
        if (packet_queue_is_flush_packet(queue_, &pkt)) {
            // when seeking or when switching to a different stream
            avcodec_flush_buffers(avctx_); //清空里面的缓存帧

        } else {
            //用于标记 “上次发送数据包时因解码器忙碌而暂存的包”（如 avcodec_send_packet 返回 AVERROR(EAGAIN) 时，会将包暂存为 pkt_）

            int result = avcodec_send_packet(avctx_,&pkt);

                if (result == AVERROR(EAGAIN)) {
                    //                    av_log(avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");

                    packet_pending_ = 1;
                    av_packet_move_ref(&pkt_, &pkt);
                }
            }

            av_packet_unref(&pkt);	// 一定要自己去释放音视频数据
        }

}

int Decoder::get_video_frame(AVFrame * frame)
{
    int got_picture;
    // 1. 获取解码后的视频帧
    if ((got_picture = decoder_decode_frame(frame)) < 0) {
        return -1; // 返回-1意味着要退出解码线程, 所以要分析decoder_decode_frame什么情况下返回-1
    }
    return got_picture;
}


void Decoder::decoder_abort()
{   
    LOGD_PLAYER(owner_player_id_, "decoder_abort start");
    PacketQueue* queue = queue_;
    FrameQueue* fq = framequeue;

    //会把packet的abort设置为1，这样从队列中取出packet的时候会返回-1，就可以退出无限循环
    if(queue != nullptr){
        packet_queue_abort(queue);     // 请求退出包队列
    }
    if(fq != nullptr){
        frame_queue_abort(fq);
    }
    frame_pool_abort(&framePool_);
    LOGD_PLAYER(owner_player_id_, "before decoder_thread join");
    if(decoder_thread_ && decoder_thread_->joinable()) {
        decoder_thread_->join(); // 等待解码线程退出
        delete decoder_thread_;
        decoder_thread_ = NULL;
    }
    LOGD_PLAYER(owner_player_id_, "after decoder_thread join");

    if(video_refresh_thread_ && video_refresh_thread_->joinable()){
        video_refresh_thread_->join(); // 等待解码线程退出
        delete video_refresh_thread_;
        video_refresh_thread_ = NULL;
    }
    LOGD_PLAYER(owner_player_id_, "after video_refresh_thread join");
    if (queue != nullptr) {
        packet_queue_flush(queue);  // 清空packet队列，并释放数据
    }
    if (fq != nullptr) {
        FrameQueueStats stats = frame_queue_get_stats(fq);
        LOGD_PLAYER(owner_player_id_, "decoder summary, total=%d, queue_drop=%d, late_drop=%d, rendered=%d, total_drop=%d",
             stats.total, stats.drop, stats.late_drop, stats.rendered, stats.drop + stats.late_drop);
    }
    LOGD_PLAYER(owner_player_id_, "thread join over");
    queue_ = nullptr;
    framequeue = nullptr;
    packet_pending_ = 0;
    LOGD_PLAYER(owner_player_id_, "decoder_abort_over");
}
void Decoder::decoder_destroy()
{
    if (pkt_.data) {
        av_packet_unref(&pkt_);

    }
    if (avctx_) {
        avcodec_free_context(&avctx_);
        avctx_ = nullptr;
    }
    queue_ = nullptr;
    framequeue = nullptr;
    packet_pending_ = 0;
    frame_pool_destroy(&framePool_);

}

void Decoder::decoder_init(AVCodecContext * avctx, PacketQueue * queue, FrameQueue *fQueue)
{
    avctx_ = avctx;
    queue_ = queue;
    framequeue = fQueue;
    if (framePool_.free_frames.empty()) {
        frame_pool_init(&framePool_, VIDEO_FRAME_POOL_SIZE);
    } else {
        std::lock_guard<std::mutex> lock(framePool_.mutex);
        framePool_.abort_request = 0;
    }
    if (framequeue) {
        framequeue->frame_pool = &framePool_;
    }
    packet_pending_ = 0;
    renderKeyFrameReady_ = 0;
    av_init_packet(&pkt_);
    pkt_.data = nullptr;
    pkt_.size = 0;
}

Decoder::~Decoder()
{

}
