#pragma once
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <jni.h>
#include <mutex>

extern  "C"{
#include "include/libavcodec/avcodec.h"
}

class VideoFrameBuffer {
public:
    VideoFrameBuffer(int w, int h, JavaVM *vm);
    ~VideoFrameBuffer();

    // 将解码后的 AVFrame 直接写入“后缓冲”的最终 I420 输出区，写完后翻转为“前缓冲”
    bool copyFromFrame(const AVFrame* f);

    // Java 总是读取当前“前缓冲”对应的 DirectByteBuffer
    jobject getDirectByteBuffer(JNIEnv* env);

    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getTotalSize() const { return totalSize; }

    void alloc();
    void resize(int w, int h);

    void fastCopy(uint8_t* dst, const AVFrame* f);
    void slowCopy(uint8_t* dst, const AVFrame* f);
    bool convertSemiPlanarToI420(uint8_t* dst, const AVFrame* f) const;

    bool isFastPath(const AVFrame* f);

    void createJavaBuffer();
    void recreateJavaBuffer();

private:
    // 一次写帧操作在“锁内准备阶段”拿到的上下文快照。
    // 后续整帧 memcpy / 像素格式转换会在锁外进行，所以需要把本次写入依赖的
    // 目标地址、尺寸信息、back buffer 身份以及缓冲代际号先记录下来。
    struct WriteContext {
        uint8_t* dst = nullptr;
        int width = 0;
        int height = 0;
        int ySize = 0;
        int uSize = 0;
        int backIndex = -1;
        uint64_t bufferEpoch = 0;
    };

    // 一个槽位对应一块原始 YUV 内存，以及与之绑定的 Java DirectByteBuffer。
    // 双缓冲模式下始终维护两个槽位：前缓冲给 Java 读，后缓冲给 native 写。
    struct BufferSlot {
        //native 侧真正存放一帧 I420/YUV 数据的原始内存地址，解码线程会把数据写到这里
        uint8_t* buf = nullptr;
        //和这块 buf 绑定的 Java DirectByteBuffer 全局引用/句柄
        jobject cachedBuffer = nullptr;
    };

    void createJavaBuffersLocked();
    void createJavaBufferForSlotLocked(BufferSlot& slot);
    void recreateJavaBufferLocked();
    void resizeLocked(int w, int h);
    void allocLocked();
    void releaseJavaBufferForSlotLocked(JNIEnv* env, BufferSlot& slot);
    void releaseSlotLocked(JNIEnv* env, BufferSlot& slot);
    BufferSlot& frontSlotLocked();
    BufferSlot& backSlotLocked();
    void swapBuffersLocked();
    // 获取本次写入要用的 back buffer 上下文。
    // 只在锁内做轻量操作：等待旧 writer 结束、必要时 resize、记录当前 back buffer 身份。
    bool acquireBackBufferForWriteLocked(const AVFrame* f, WriteContext* ctx);
    // 提交本次写入结果。
    // 只有当 back buffer 身份和缓冲代际都没变化时，才允许 swap 成 front buffer。
    void commitBackBufferWriteLocked(const WriteContext& ctx, bool writeOk);
    // 等待当前锁外 writer 结束。
    // resize 或下一次写入开始前都要先过这里，避免写到一半时旧 buffer 被释放。
    void waitForActiveWriterToFinishLocked(std::unique_lock<std::mutex>& lock);

    int width;
    int height;
    int ySize;
    int uSize;
    int vSize;
    JavaVM *jvm;
    int totalSize;
    std::array<BufferSlot, 2> slots_{};
    // frontIndex_ 指向当前对 Java 可见的缓冲区；
    // backIndex_ 指向当前供 native 写入下一帧的缓冲区。
    int frontIndex_ = 0;
    int backIndex_ = 1;
    std::mutex mtx;  // 线程安全
    // 单实例单写者模型下，这里只需要表示“当前是否有一帧正在锁外写入”。
    // resize 必须等它结束，否则可能把仍在写入的旧内存释放掉。
    std::condition_variable writerCv_;
    bool writerActive_ = false;
    // 每次缓冲区重建（例如分辨率变化）都递增。
    // 写完提交时通过 epoch 校验，避免把旧缓冲上的写入结果错误地 swap 成前缓冲。
    uint64_t bufferEpoch_ = 0;
};
