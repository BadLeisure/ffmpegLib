//
// Created by 软件部- AA on 2026/1/6.
//

#ifndef FFMPEGNDK_FFPLAY_DEF_H
#define FFMPEGNDK_FFPLAY_DEF_H
// 平滑优先档：稳态积压 2~4 帧（延迟预算 ~66ms），8 帧容量为 TCP 突发留吸收余量；
// 池 = 队列容量 + 刷新线程持 1 + 解码线程持 1。
#define VIDEO_PICTURE_QUEUE_SIZE 8
#define VIDEO_FRAME_POOL_SIZE 10

// PacketQueue 高水位默认值：内存防护用，正常播放时队列远小于该值。
// size 达到 4MB 或包数达到 500 后，入队口进入受控丢包（非关键帧丢弃、关键帧清队重启），
// 防止解码/渲染停滞时网络包无界堆积导致 OOM。
#define PACKET_QUEUE_MAX_SIZE (4 * 1024 * 1024)
#define PACKET_QUEUE_MAX_PACKETS 500


extern  "C" {
#include "include/libavcodec/avcodec.h"
}
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>



typedef struct MyAVPacketList {
    AVPacket		pkt;    //解封装后的数据
    struct MyAVPacketList	*next;  //下一个节点
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList	*first_pkt, *last_pkt;  // 队首，队尾指针
    int		nb_packets;   // 包数量，也就是队列元素数量
    int		size;         // 队列所有元素的数据大小总和
    int64_t		duration; // 队列所有元素的数据播放持续时间
    std::atomic<int> abort_request{0}; // 用户退出请求标志
    std::mutex	mutex;     // 用于维持PacketQueue的多线程安全
    std::condition_variable	cond;      // 用于读、写线程相互通知

    std::mutex threadMutex;
    std::condition_variable	threadCond;
    // 每个 PacketQueue 自己持有一个 flush 包哨兵，避免多个实例共享全局 flush_pkt。
    AVPacket flush_pkt;
    int owner_player_id = -1;

    // 受控丢包高水位：size 达到 max_size 或包数达到 max_packets 时触发（入队口检查）。
    int64_t max_size = PACKET_QUEUE_MAX_SIZE;
    int max_packets = PACKET_QUEUE_MAX_PACKETS;
    // 溢出期间累计丢弃的包数（含关键帧清队），仅用于日志观测，flush 时清零。
    int64_t overflow_drops = 0;
    // 是否处于溢出 episode 中（用于限流日志与恢复判定）。
    bool overflow_active = false;
    int64_t last_overflow_log_us = 0;

} PacketQueue;


typedef struct FramePool {
    std::vector<AVFrame*> free_frames;
    std::mutex mutex;
    std::condition_variable cond;
    std::atomic<int> abort_request{0};
    int owner_player_id = -1;
} FramePool;

typedef struct FrameQueue {
    AVFrame* queue[VIDEO_PICTURE_QUEUE_SIZE];
    int64_t enqueue_us[VIDEO_PICTURE_QUEUE_SIZE]{};
    int rindex;
    int windex;
    std::atomic<int> size{0};
    int max_size;
    std::mutex frameMutex;
    std::condition_variable	frameCond;
    std::atomic<int> abort_request{0};
    std::atomic<int> drop{0}; // 队列满时丢帧
    std::atomic<int> late_drop{0}; // 渲染阶段晚到丢帧
    std::atomic<int> rendered{0}; // 实际渲染成功的帧数
    std::atomic<int> total{0}; // 总共入队的帧数
    FramePool* frame_pool = nullptr;
    // 当前是否进入“中度拥塞”档位。
    // 中度拥塞时，允许在未满队列阶段提前淘汰最老 P。
    std::atomic<int> adaptive_mid_congestion{0};
    // 当前是否进入“高度拥塞”档位。
    // 高度拥塞时，除了可提前淘汰旧 P，还可能直接拒收新 P。
    std::atomic<int> adaptive_high_congestion{0};
    // 当前是否进入“高帧率模式”。
    // 高帧率流更容易快速积压，所以水位线会再提前一帧触发。
    std::atomic<int> adaptive_high_fps{0};
    // 每次切流清空队列后递增，刷新线程据此重置时间基准。
    std::atomic<int> generation{0};
    int owner_player_id = -1;

} FrameQueue;

typedef struct FrameQueueStats {
    int total;
    int drop;
    int late_drop;
    int rendered;
} FrameQueueStats;

bool frame_is_i_frame(const AVFrame* frame);
void frame_queue_rebuild_locked(FrameQueue* fq, AVFrame* const* kept, int kept_count);
int packet_queue_put_private(PacketQueue *q, AVPacket *pkt);

int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
int packet_queue_init(PacketQueue *q);
bool packet_queue_is_flush_packet(const PacketQueue *q, const AVPacket *pkt);
void packet_queue_flush(PacketQueue *q);
void packet_queue_destroy(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
void packet_queue_start(PacketQueue *q);
double packet_queue_cache_duration(PacketQueue *q, AVRational time_base, double packet_duration);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);


int frame_queue_init(FrameQueue* fq, int max_size);
void frame_queue_push(FrameQueue *fq, AVFrame* frame, int64_t enqueue_us);
// 弹出 frame，队列为空则阻塞
AVFrame* frame_queue_pop(FrameQueue* fq, int64_t* enqueue_us);
void frame_queue_destroy(FrameQueue* fq);
void frame_queue_start(FrameQueue* fq);
void frame_queue_flush(FrameQueue* fq);

// 非阻塞获取当前队列大小
int frame_queue_size(FrameQueue* fq);

void frame_queue_abort(FrameQueue *fq);
void frame_queue_record_late_drop(FrameQueue *fq);
void frame_queue_record_render(FrameQueue *fq);
FrameQueueStats frame_queue_get_stats(FrameQueue *fq);
// 由刷新线程把最新的轻量拥塞状态同步给队列。
// frame_queue_push() 只关心中拥塞 / 高拥塞 / 高帧率这 3 个标志，不再依赖完整 EMA 数值。
void frame_queue_update_adaptive_state(FrameQueue *fq,
                                       bool mid_congestion,
                                       bool high_congestion,
                                       bool high_fps);

int frame_pool_init(FramePool* pool, int capacity);
AVFrame* frame_pool_acquire(FramePool* pool);
void frame_pool_release(FramePool* pool, AVFrame* frame);
void frame_pool_abort(FramePool* pool);
void frame_pool_destroy(FramePool* pool);



#endif //FFMPEGNDK_FFPLAY_DEF_H
