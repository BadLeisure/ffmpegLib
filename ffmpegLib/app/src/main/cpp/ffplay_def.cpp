//
#include "ffplay_def.h"
#define LOG_TAG "FrameQueue"
#include "native_log.h"

namespace {
constexpr std::memory_order kQueueStateOrder = std::memory_order_relaxed;

// 统一释放队列里的 AVFrame。
// 如果当前 FrameQueue 绑定了对象池，就回收到对象池；
// 否则直接 av_frame_free，兼容没有对象池的老路径。
void release_frame_to_pool(FrameQueue* fq, AVFrame* frame) {
    if (!frame) {
        return;
    }
    if (fq && fq->frame_pool) {
        frame_pool_release(fq->frame_pool, frame);
        return;
    }
    av_frame_free(&frame);
}

// 在当前环形队列里，从“逻辑上的最老帧”开始往后找，
// 返回第一个非 I 帧（通常就是 P 帧）的位置。
// 返回的是逻辑位置，不是 queue[] 的物理下标。
int find_oldest_non_i_logical_pos_locked(FrameQueue* fq) {
    const int size = fq->size.load(kQueueStateOrder);
    for (int i = 0; i < size; ++i) {
        const int idx = (fq->rindex + i) % fq->max_size;
        if (!frame_is_i_frame(fq->queue[idx])) {
            return i;
        }
    }
    return -1;
}

// 删除环形队列里的某个“逻辑位置”元素，并把后面的元素向前压一格。
// 这个方法用于局部删除最老 P，避免再走“拉平成数组后整体重建”的重路径。
// 调用方必须已经持有 frameMutex。
void erase_logical_pos_locked(FrameQueue* fq, int logical_pos) {
    const int size = fq ? fq->size.load(kQueueStateOrder) : 0;
    if (!fq || logical_pos < 0 || logical_pos >= size) {
        return;
    }
    const int drop_idx = (fq->rindex + logical_pos) % fq->max_size;
    release_frame_to_pool(fq, fq->queue[drop_idx]);
    for (int i = logical_pos; i < size - 1; ++i) {
        const int from = (fq->rindex + i + 1) % fq->max_size;
        const int to = (fq->rindex + i) % fq->max_size;
        fq->queue[to] = fq->queue[from];
    }
    const int tail_idx = (fq->rindex + size - 1) % fq->max_size;
    fq->queue[tail_idx] = nullptr;
    fq->size.store(size - 1, kQueueStateOrder);
    fq->windex = (fq->rindex + size - 1) % fq->max_size;
}

// 根据当前轻量共享状态，计算某个拥塞动作的触发水位。
// base_watermark 是基础阈值；如果刷新线程已经判定这是高帧率流，
// 就把阈值再提前 1 帧，因为同样的 backlog 在高帧率下会更快转化成可见时延。
// 调用方必须已经持有 frameMutex。
int compute_congestion_watermark_locked(const FrameQueue* fq, int base_watermark) {
    if (!fq) {
        return base_watermark;
    }
    int watermark = base_watermark;
    // 高帧率流更容易因为积压放大时延，适当提前触发拥塞控制。
    if (fq->adaptive_high_fps.load(kQueueStateOrder)) {
        watermark -= 1;
    }
    if (watermark < 1) {
        watermark = 1;
    }
    if (watermark > fq->max_size) {
        watermark = fq->max_size;
    }
    return watermark;
}
}

bool frame_is_i_frame(const AVFrame* frame) {
    if (!frame) return false;
    return frame->key_frame == 1 || frame->pict_type == AV_PICTURE_TYPE_I;
}

// 折中模式：队列满时每次限量清理，避免一次性清空导致明显大跳。
static constexpr int kDropBudgetNormal = 2;
static constexpr int kDropBudgetIncomingI = 3;

void frame_queue_rebuild_locked(FrameQueue* fq, AVFrame* const* kept, int kept_count) {
    for (int i = 0; i < fq->max_size; ++i) {
        fq->queue[i] = nullptr;
    }

    fq->rindex = 0;
    fq->size.store(kept_count, kQueueStateOrder);
    fq->windex = kept_count % fq->max_size;
    for (int i = 0; i < kept_count; ++i) {
        fq->queue[i] = kept[i];
    }
}

int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    if (!q || !pkt) {
        return -1;
    }

    MyAVPacketList *pkt1;


    pkt1 = (MyAVPacketList *)av_malloc(sizeof(MyAVPacketList));   //分配节点内存
    if (!pkt1)  //内存不足，则放入失败
        return -1;
    // 没有做引用计数，那这里也说明av_read_frame不会释放替用户释放buffer。
    pkt1->pkt = *pkt; //拷贝AVPacket(浅拷贝，AVPacket.data等内存并没有拷贝)
//    printData(&pkt1->pkt);
    pkt1->next = NULL;

    /* 队列操作：如果last_pkt为空，说明队列是空的，新增节点为队头；
     * 否则，队列有数据，则让原队尾的next为新增节点。 最后将队尾指向新增节点
     */
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    //队列属性操作：增加节点数、cache大小、cache总时长, 用来控制队列的大小
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;

    /* XXX: should duplicate packet data in DV case */
    //发出信号，表明当前队列中有数据了，通知等待中的读线程可以取数据了
    q->cond.notify_one();
    return 0;
}



int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    if (!q || !pkt) {
        if (pkt) {
            av_packet_unref(pkt);
        }
        return -1;
    }

    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);
    {
        // std::lock_guard 在作用域结束时自动释放锁
        std::lock_guard<std::mutex> lock(q->mutex);
        ret = packet_queue_put_private(q, pkt1); // 主要实现
    }

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    if (!q) {
        return -1;
    }
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;

    return packet_queue_put(q, pkt);
}

/* packet queue handling */
int packet_queue_init(PacketQueue *q)
{
    if (!q) {
        return -1;
    }
    q->first_pkt = nullptr;
    q->last_pkt = nullptr;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->abort_request = 0;
    av_init_packet(&q->flush_pkt);
    // 每个队列都维护自己的 flush 哨兵，避免多实例共享同一个全局 AVPacket。
    // 这里仍然复用“data 指向自身地址”的老判定方式，但作用域已经收缩到单个 PacketQueue。
    q->flush_pkt.data = reinterpret_cast<uint8_t*>(&q->flush_pkt);
    q->flush_pkt.size = 0;
    q->flush_pkt.stream_index = -1;


    return 0;
}

bool packet_queue_is_flush_packet(const PacketQueue *q, const AVPacket *pkt) {
    if (!q || !pkt) {
        return false;
    }
    return pkt->data == q->flush_pkt.data;
}

void packet_queue_flush(PacketQueue *q)
{
    if (!q) {
        LOGD_PLAYER(-1, "q is null");
        return;
    }
    MyAVPacketList *pkt, *pkt1;

    {
        std::lock_guard<std::mutex> lock(q->mutex);
        for (pkt = q->first_pkt; pkt; pkt = pkt1) {
            pkt1 = pkt->next;
            av_packet_unref(&pkt->pkt);
            av_freep(&pkt);
        }
        q->last_pkt = NULL;
        q->first_pkt = NULL;
        q->nb_packets = 0;
        q->size = 0;
        q->duration = 0;
    }
    LOGD_PLAYER(q->owner_player_id, "packet_queue_flush over");
}

void packet_queue_destroy(PacketQueue *q)
{
    if (!q) {
        LOGD_PLAYER(-1, "q is null");
        return;
    }
    packet_queue_flush(q); //先清除所有的节点
}

void packet_queue_abort(PacketQueue *q)
{
    if (!q) {
        LOGD_PLAYER(-1, "q is null");
        return;
    }
    {
        // unique_lock 或 lock_guard 都可以
        std::lock_guard<std::mutex> lock(q->mutex);
        q->abort_request = 1;       // 请求退出
    } // 离开作用域自动解锁

    q->cond.notify_one();           // 唤醒等待线程
    LOGD_PLAYER(q->owner_player_id, "packet_queue_abort over");
}


void packet_queue_start(PacketQueue *q)
{
    if (!q) {
        return;
    }
    {
        // unique_lock 或 lock_guard 都可以
        std::lock_guard<std::mutex> lock(q->mutex);
        q->abort_request = 0;
        packet_queue_put_private(q, &q->flush_pkt); // 这里放入当前队列自己的 flush 包
    }
}

double packet_queue_cache_duration(PacketQueue *q, AVRational time_base, double packet_duration) {
    if (!q) {
        return 0;
    }
    double pts_duration = 0;    // 按队列头部、尾部的pts时长
    double packets_duration = 0;// 按包累积的时长

    {

        std::lock_guard<std::mutex> lock(q->mutex);
        MyAVPacketList *first_pkt = q->first_pkt;
        MyAVPacketList *last_pkt = q->last_pkt;
        if (!first_pkt || !last_pkt) {
            return 0;
        }
        int64_t temp_pts = last_pkt->pkt.dts - first_pkt->pkt.dts;  // 计算出来差距
        pts_duration = temp_pts * av_q2d(time_base);            // 转换成秒
        packets_duration = packet_duration * q->nb_packets; // packet可能存在多帧音频的情况, 此时这里计算就存在误差

    }

    // 以时间戳为准，然后根据码率预估持续播放的最大时长，比如以时间戳计算出来超过60秒，则是极有可能是有问题的,说明缓存的数据比较多
    if(pts_duration < 60) {
        return pts_duration;
    } else {
        return packets_duration;
    }
}


/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
/**
 * @brief packet_queue_get
 * @param q 队列
 * @param pkt 输出参数，即MyAVPacketList.pkt
 * @param block 调用者是否需要在没节点可取的情况下阻塞等待
 * @param serial 输出参数，即MyAVPacketList.serial
 * @return <0: aborted; =0: no packet; >0: has packet
 */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    if (!q || !pkt) {
        return -1;
    }
    MyAVPacketList *pkt1;
    int ret;

    {
        std::unique_lock<std::mutex> lock(q->mutex);
        for (;;) {
            if (q->abort_request) {
                ret = -1;
                break;
            }

            pkt1 = q->first_pkt;    //MyAVPacketList *pkt1; 从队头拿数据
            if (pkt1) {     //队列中有数据
                q->first_pkt = pkt1->next;  //队头移到第二个节点
                if (!q->first_pkt)
                    q->last_pkt = NULL;
                q->nb_packets--;    //节点数减1
                q->size -= pkt1->pkt.size + sizeof(*pkt1);  //cache大小扣除一个节点
                q->duration -= pkt1->pkt.duration;  //总时长扣除一个节点
                //返回AVPacket，这里发生一次AVPacket结构体拷贝，AVPacket的data只拷贝了指针
                *pkt = pkt1->pkt;
                if(pkt->data == NULL){
                    LOG_QUEUE_PLAYER(q->owner_player_id, "this is a null packet");
                }
                av_free(pkt1);      //释放节点内存,只是释放节点，而不是释放AVPacket
                ret = 1;
                break;
            } else if (!block) {    //队列中没有数据，且非阻塞调用
                ret = 0;
                break;
            } else {    //队列中没有数据，且阻塞调用
                //这里没有break。for循环的另一个作用是在条件变量满足后重复上述代码取出节点
                q->cond.wait(lock);  // 等待条件变量通知
            }
        }
    }
    return ret;
}


int frame_queue_init(FrameQueue* fq, int max_size) {
    if (!fq) {
        return -1;
    }
    for (int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; ++i) {
        fq->queue[i] = nullptr;
    }
    fq->rindex = 0;
    fq->windex = 0;
    fq->size.store(0, kQueueStateOrder);
    fq->max_size = max_size > VIDEO_PICTURE_QUEUE_SIZE ? VIDEO_PICTURE_QUEUE_SIZE : max_size;
    fq->abort_request = 0;
    fq->drop.store(0, kQueueStateOrder);
    fq->late_drop.store(0, kQueueStateOrder);
    fq->rendered.store(0, kQueueStateOrder);
    fq->total.store(0, kQueueStateOrder);
    fq->frame_pool = nullptr;
    fq->adaptive_mid_congestion.store(0, kQueueStateOrder);
    fq->adaptive_high_congestion.store(0, kQueueStateOrder);
    fq->adaptive_high_fps.store(0, kQueueStateOrder);

    return 0;
}


void frame_queue_destroy(FrameQueue* fq) {
    if (!fq) {
        return;
    }
    for(int i = 0; i < fq->max_size; ++i) {
        if(fq->queue[i]) {
            release_frame_to_pool(fq, fq->queue[i]);
            fq->queue[i] = nullptr;
        }
    }
    fq->size.store(0, kQueueStateOrder);
    fq->rindex = 0;
    fq->windex = 0;
    fq->abort_request = 0;
    fq->drop.store(0, kQueueStateOrder);
    fq->late_drop.store(0, kQueueStateOrder);
    fq->rendered.store(0, kQueueStateOrder);
    fq->total.store(0, kQueueStateOrder);
    fq->adaptive_mid_congestion.store(0, kQueueStateOrder);
    fq->adaptive_high_congestion.store(0, kQueueStateOrder);
    fq->adaptive_high_fps.store(0, kQueueStateOrder);
}


int frame_queue_size(FrameQueue* fq) {
    if (!fq) {
        return 0;
    }
    return fq->size.load(kQueueStateOrder);
}

void frame_queue_push(FrameQueue *fq,AVFrame* frame) {
    if (!fq) {
        av_frame_free(&frame);
        return;
    }
    std::unique_lock<std::mutex> lock(fq->frameMutex);
    if (fq->abort_request) {
        release_frame_to_pool(fq, frame);
        return;
    }

    const bool incoming_is_i = frame_is_i_frame(frame);
    const int current_size = fq->size.load(kQueueStateOrder);
    // 提前淘汰旧 P 的触发点。默认比满队列早 4 帧，给 refresh 线程留追帧空间。
    const int early_drop_watermark = compute_congestion_watermark_locked(fq, fq->max_size - 4);
    // 直接拒收新 P 的触发点。默认接近满队列时才启用，策略更激进。
    const int reject_new_p_watermark = compute_congestion_watermark_locked(fq, fq->max_size - 2);

    //最高拥塞时的新 P 准入控制，这个新 P 直接不收，直接丢掉
    if (!incoming_is_i &&
        fq->adaptive_high_congestion.load(kQueueStateOrder) &&
        current_size >= reject_new_p_watermark) {
        const int drop_count = fq->drop.fetch_add(1, kQueueStateOrder) + 1;
        LOG_QUEUE_PLAYER(fq->owner_player_id, "drop incoming P early, size=%d high_congestion=1 high_fps=%d drop=%d",
                  current_size, fq->adaptive_high_fps.load(kQueueStateOrder), drop_count);
        release_frame_to_pool(fq, frame);
        return;
    }
    //当前已经进入中度或高度拥塞且当前队列长度已经达到“提前处理水位”，不是拒绝新帧，而是先清掉一个旧的、价值更低的 P，让队列更新鲜
    const int adaptive_mid_congestion = fq->adaptive_mid_congestion.load(kQueueStateOrder);
    const int adaptive_high_congestion = fq->adaptive_high_congestion.load(kQueueStateOrder);
    if ((adaptive_mid_congestion || adaptive_high_congestion) &&
        current_size >= early_drop_watermark) {
            //在队列里找“最老的非 I 帧”
        const int oldest_p_pos = find_oldest_non_i_logical_pos_locked(fq);
        if (oldest_p_pos >= 0) {
            //找到了，把这个最老的 P 从环形队列里删掉，并把后面的元素往前挪一格，保持 FIFO 顺序
            erase_logical_pos_locked(fq, oldest_p_pos);
            const int drop_count = fq->drop.fetch_add(1, kQueueStateOrder) + 1;
            const int size_after_drop = fq->size.load(kQueueStateOrder);
            LOG_QUEUE_PLAYER(fq->owner_player_id, "proactive drop oldest P, size=%d mid=%d high=%d high_fps=%d drop=%d",
                      size_after_drop,
                      adaptive_mid_congestion, adaptive_high_congestion,
                      fq->adaptive_high_fps.load(kQueueStateOrder), drop_count);
        }
    }

    //保证队列永远不会无控制地超容量
    if (fq->size.load(kQueueStateOrder) >= fq->max_size) {
        const int size = fq->size.load(kQueueStateOrder);
        const int total = fq->total.load(kQueueStateOrder);
        const int drop = fq->drop.load(kQueueStateOrder);
        LOG_QUEUE_PLAYER(fq->owner_player_id, "frame queue full, size = %d, total = %d, drop = %d",
                  size, total, drop);
        // 无 B 帧场景下，队列满时不再做“有序数组提取 + 全量重建”。
        // 这里直接在环形队列上找最老的非 I（即最老 P）并局部压缩删除：
        // 1) 如果队列里有 P，删除最老 P；
        // 2) 如果全是 I 且新帧也是 I，删除最老 I；
        // 3) 如果全是 I 且新帧是 P，直接丢新帧，避免用低价值帧替换关键帧。
        int drop_logical_pos = find_oldest_non_i_logical_pos_locked(fq);

        if (drop_logical_pos < 0) {
            if (!incoming_is_i) {
                const int drop_count = fq->drop.fetch_add(1, kQueueStateOrder) + 1;
                LOG_QUEUE_PLAYER(fq->owner_player_id, "drop incoming non-I, queue has only I frames, drop=%d", drop_count);
                release_frame_to_pool(fq, frame);
                return;
            }
            drop_logical_pos = 0;
        }

        erase_logical_pos_locked(fq, drop_logical_pos);
        const int size_after_erase = fq->size.load(kQueueStateOrder);
        const int tail_idx = (fq->rindex + size_after_erase) % fq->max_size;
        fq->queue[tail_idx] = frame;
        fq->size.store(size_after_erase + 1, kQueueStateOrder);
        fq->windex = (tail_idx + 1) % fq->max_size;
        const int drop_count = fq->drop.fetch_add(1, kQueueStateOrder) + 1;
        LOG_QUEUE_PLAYER(fq->owner_player_id, "drop one frame at logical_pos=%d, total_drop=%d", drop_logical_pos, drop_count);
        fq->total.fetch_add(1, kQueueStateOrder);
        fq->frameCond.notify_one();
        return;
    }

    fq->queue[fq->windex] = frame;
    fq->windex = (fq->windex + 1) % fq->max_size;
    fq->size.fetch_add(1, kQueueStateOrder);
    fq->total.fetch_add(1, kQueueStateOrder);
    fq->frameCond.notify_one(); // 通知可能阻塞的 pop
}

AVFrame* frame_queue_pop(FrameQueue* fq) {
    if (!fq) {
        return nullptr;
    }
    std::unique_lock<std::mutex> lock(fq->frameMutex);

    // 阻塞等待队列有帧或中止
    fq->frameCond.wait(lock, [fq]{ return fq->size.load(kQueueStateOrder) > 0 || fq->abort_request; });

    if (fq->abort_request && fq->size.load(kQueueStateOrder) == 0) {
        return nullptr;
    }

    // 取出队列最前面的帧
    AVFrame* frame = fq->queue[fq->rindex];
    fq->queue[fq->rindex] = nullptr;

    // 更新读索引
    fq->rindex = (fq->rindex + 1) % fq->max_size;
    fq->size.fetch_sub(1, kQueueStateOrder);

    fq->frameCond.notify_one();
    return frame;
}

void frame_queue_abort(FrameQueue *fq)
{
    if (!fq) {
        LOGD_PLAYER(-1, "fq is null");
        return;
    }
    {
        // unique_lock 或 lock_guard 都可以
        std::lock_guard<std::mutex> lock(fq->frameMutex);
        fq->abort_request = 1;       // 请求退出
    } // 离开作用域自动解锁

    fq->frameCond.notify_one();           // 唤醒等待线程
    LOGD_PLAYER(fq->owner_player_id, "frame_queue_abort over");
}

void frame_queue_record_late_drop(FrameQueue *fq)
{
    if (!fq) {
        return;
    }
    fq->late_drop.fetch_add(1, kQueueStateOrder);
}

void frame_queue_record_render(FrameQueue *fq)
{
    if (!fq) {
        return;
    }
    fq->rendered.fetch_add(1, kQueueStateOrder);
}

FrameQueueStats frame_queue_get_stats(FrameQueue *fq)
{
    if (!fq) {
        return FrameQueueStats{};
    }
    FrameQueueStats stats{};
    stats.total = fq->total.load(kQueueStateOrder);
    stats.drop = fq->drop.load(kQueueStateOrder);
    stats.late_drop = fq->late_drop.load(kQueueStateOrder);
    stats.rendered = fq->rendered.load(kQueueStateOrder);
    return stats;
}

// 刷新线程每消费一帧，都会把它判断出来的轻量标志写回这里。
// push 线程下一次入队时，只看中拥塞 / 高拥塞 / 高帧率这 3 个标志。
void frame_queue_update_adaptive_state(FrameQueue *fq,
                                       bool mid_congestion,
                                       bool high_congestion,
                                       bool high_fps)
{
    if (!fq) {
        return;
    }
    fq->adaptive_mid_congestion.store(mid_congestion ? 1 : 0, kQueueStateOrder);
    fq->adaptive_high_congestion.store(high_congestion ? 1 : 0, kQueueStateOrder);
    fq->adaptive_high_fps.store(high_fps ? 1 : 0, kQueueStateOrder);
}

//初始化对象池
int frame_pool_init(FramePool* pool, int capacity) {
    if (!pool || capacity <= 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(pool->mutex);
    pool->abort_request = 0;
    pool->free_frames.clear();
    pool->free_frames.reserve(capacity);
    for (int i = 0; i < capacity; ++i) {
        //新建一个空的AVFrame对象
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            //如果某一次创建失败了。说明内存不够或者分配失败，要全部释放
            for (AVFrame* allocated : pool->free_frames) {
                av_frame_free(&allocated);
            }
            pool->free_frames.clear();
            return -1;
        }
        pool->free_frames.push_back(frame);
    }
    return 0;
}

AVFrame* frame_pool_acquire(FramePool* pool) {
    if (!pool) {
        return nullptr;
    }
    std::unique_lock<std::mutex> lock(pool->mutex);
    //只要当前对象池还没被 abort，并且 free_frames 还是空的，就一直等
    pool->cond.wait(lock, [pool] {
        return pool->abort_request || !pool->free_frames.empty();
    });
    if (pool->abort_request) {
        LOGD_PLAYER(pool->owner_player_id, "pool->abort_request is true");
        return nullptr;
    }
    AVFrame* frame = pool->free_frames.back();
    pool->free_frames.pop_back();
    return frame;
}

//把一个已经用完的 AVFrame 放回对象池，供后面再次复用
void frame_pool_release(FramePool* pool, AVFrame* frame) {
    if (!pool || !frame) {
        return;
    }
    //清掉这帧当前挂着的解码数据引用
    av_frame_unref(frame);
    {
        //放回free_frames
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->free_frames.push_back(frame);
    }
    //告诉可能正在等空闲帧的人，现在有一个可用了
    pool->cond.notify_one();
}

//告诉所有还在等对象池的人：别等了，系统要退出了
void frame_pool_abort(FramePool* pool) {
    if (!pool) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->abort_request = 1;
    }
    //系统状态变了，所有等待的人都别等了。
    pool->cond.notify_all();
}

//销毁对象池
void frame_pool_destroy(FramePool* pool) {
    if (!pool) {
        return;
    }
    std::lock_guard<std::mutex> lock(pool->mutex);
    for (AVFrame* frame : pool->free_frames) {
        av_frame_free(&frame);
    }
    pool->free_frames.clear();
    pool->abort_request = 0;
}





// Created by 软件部- AA on 2026/1/6.
//
