//
// Created by 软件部- AA on 2026/1/20.
//
#include "VideoFrameBuffer.h"

#include <climits>

#include <jni.h>

#define LOG_TAG "VideoFrameBuffer"
#include "native_log.h"

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include "libyuv/convert.h"

VideoFrameBuffer::VideoFrameBuffer(int w, int h, JavaVM *vm)
        : width(w), height(h), jvm(vm) {
    std::lock_guard<std::mutex> lock(mtx);
    // 初始化时一次性准备两块等大的 YUV 内存，并分别绑定对应的 DirectByteBuffer。
    allocLocked();
    createJavaBuffersLocked();
}

VideoFrameBuffer::~VideoFrameBuffer() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (jvm && jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        } else {
            env = nullptr;
        }
    }

    std::lock_guard<std::mutex> lock(mtx);
    releaseSlotLocked(env, slots_[0]);
    releaseSlotLocked(env, slots_[1]);
    if (attached) {
        jvm->DetachCurrentThread();
    }
}

bool VideoFrameBuffer::copyFromFrame(const AVFrame* f) {
    if (!f) return false;

    WriteContext ctx;
    {
        // 第一段短锁：
        // 这里只做“准备工作”，包括等待上一次写入结束、必要时 resize、
        // 以及拿到当前 back buffer 的地址和上下文快照。
        // 真正的大块写入不在锁内做。
        std::unique_lock<std::mutex> lock(mtx);
        if (!acquireBackBufferForWriteLocked(f, &ctx)) {
            return false;
        }
    }

    bool writeOk = false;
    // 第二段无锁：
    // 这里已经拿到了 back buffer 地址，Java 侧读的是 front buffer，
    // 所以整帧拷贝 / NV12(NV21)->I420 转换可以放到锁外执行，减少锁占用时间。
    if (f->format == AV_PIX_FMT_YUV420P) {
        if (!f->data[1] || !f->data[2]) {
            writeOk = false;
        } else {
            if (isFastPath(f)) {
                fastCopy(ctx.dst, f);
            } else {
                slowCopy(ctx.dst, f);
            }
            writeOk = true;
        }
    } else if (f->format == AV_PIX_FMT_NV12 || f->format == AV_PIX_FMT_NV21) {
        if (!f->data[1]) {
            writeOk = false;
        } else {
            writeOk = convertSemiPlanarToI420(ctx.dst, f);
        }
    }
    {
        // 第三段短锁：
        // 写完后只做“提交动作”：
        // 校验这次写入仍然对应当前 back buffer，如果没被 resize/重建打断，
        // 才执行前后缓冲交换。
        std::lock_guard<std::mutex> lock(mtx);
        commitBackBufferWriteLocked(ctx, writeOk);
    }
    return writeOk;
}

bool VideoFrameBuffer::isFastPath(const AVFrame* f) {
    if (f->format != AV_PIX_FMT_YUV420P) {
        return false;
    }
    if ((f->width & 1) || (f->height & 1)) {
        return false;
    }
    if (f->linesize[0] <= 0 || f->linesize[1] <= 0 || f->linesize[2] <= 0) {
        return false;
    }
    if (!f->data[0] || !f->data[1] || !f->data[2]) {
        return false;
    }
    return f->linesize[0] == f->width &&
           f->linesize[1] == f->linesize[0] / 2 &&
           f->linesize[2] == f->linesize[0] / 2;
}

void VideoFrameBuffer::fastCopy(uint8_t* dst, const AVFrame* f) {
    const int yPlaneSize = f->width * f->height;
    const int uvPlaneSize = yPlaneSize / 4;

    memcpy(dst, f->data[0], yPlaneSize);
    memcpy(dst + yPlaneSize, f->data[1], uvPlaneSize);
    memcpy(dst + yPlaneSize + uvPlaneSize, f->data[2], uvPlaneSize);
}

void VideoFrameBuffer::slowCopy(uint8_t* dst, const AVFrame* f) {
    const int w = f->width;
    const int h = f->height;

    for (int i = 0; i < h; i++) {
        memcpy(dst + i * w, f->data[0] + i * f->linesize[0], w);
    }

    const int yPlaneSize = w * h;
    const int chromaWidth = w / 2;
    const int chromaHeight = h / 2;

    for (int i = 0; i < chromaHeight; i++) {
        memcpy(dst + yPlaneSize + i * chromaWidth,
               f->data[1] + i * f->linesize[1],
               chromaWidth);
    }

    for (int i = 0; i < chromaHeight; i++) {
        memcpy(dst + yPlaneSize + yPlaneSize / 4 + i * chromaWidth,
               f->data[2] + i * f->linesize[2],
               chromaWidth);
    }
}

bool VideoFrameBuffer::convertSemiPlanarToI420(uint8_t* dst, const AVFrame* f) const {
    if (!dst || !f) {
        return false;
    }

    const int dstStrideY = f->width;
    const int dstStrideU = f->width / 2;
    const int dstStrideV = f->width / 2;
    uint8_t* dstY = dst;
    uint8_t* dstU = dst + ySize;
    uint8_t* dstV = dst + ySize + uSize;

    int ret = -1;
    if (f->format == AV_PIX_FMT_NV12) {
        ret = libyuv::NV12ToI420(
                f->data[0], f->linesize[0],
                f->data[1], f->linesize[1],
                dstY, dstStrideY,
                dstU, dstStrideU,
                dstV, dstStrideV,
                f->width, f->height);
    } else if (f->format == AV_PIX_FMT_NV21) {
        ret = libyuv::NV21ToI420(
                f->data[0], f->linesize[0],
                f->data[1], f->linesize[1],
                dstY, dstStrideY,
                dstU, dstStrideU,
                dstV, dstStrideV,
                f->width, f->height);
    }

    return ret == 0;
}

void VideoFrameBuffer::createJavaBuffer() {
    std::unique_lock<std::mutex> lock(mtx);
    createJavaBuffersLocked();
}

void VideoFrameBuffer::createJavaBuffersLocked() {
    // 双缓冲的两块原始内存都需要各自绑定一个 DirectByteBuffer，
    // 这样切换前后缓冲时不需要临时创建新的 Java 对象。
    createJavaBufferForSlotLocked(slots_[0]);
    createJavaBufferForSlotLocked(slots_[1]);
}

void VideoFrameBuffer::createJavaBufferForSlotLocked(BufferSlot& slot) {
    // 这个函数的职责是：
    // 1) 为当前槽位 slot.buf 这块 native 内存创建一个 Java DirectByteBuffer；
    // 2) 把这个 ByteBuffer 升级成全局引用，缓存到 slot.cachedBuffer；
    // 3) 这样后续前后缓冲切换时，Java 侧可以直接复用对应对象，不必每帧新建 ByteBuffer。
    if (!slot.buf || totalSize <= 0 || !jvm) {
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    // 这里可能运行在 native 解码线程，不一定已经绑定到 JVM。
    // 所以先尝试拿当前线程的 JNIEnv，拿不到就临时 Attach。
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return;
        }
        attached = true;
    }

    if (!env) {
        if (attached) {
            jvm->DetachCurrentThread();
        }
        return;
    }

    // 先释放当前槽位旧的全局引用，避免分辨率变化或重建时泄漏。
    releaseJavaBufferForSlotLocked(env, slot);

    // 用 slot.buf 这块 native 内存创建 DirectByteBuffer。
    // 这个对象本身不拷贝数据，只是给 Java 一个“访问这块 native 内存”的句柄。
    jobject localBuf = env->NewDirectByteBuffer(slot.buf, static_cast<jlong>(totalSize));
    if (localBuf != nullptr) {
        // 提升为全局引用，避免 local reference 退出当前 JNI 调用后失效。
        slot.cachedBuffer = env->NewGlobalRef(localBuf);
        env->DeleteLocalRef(localBuf);
    }

    // 如果这里是临时 Attach 的线程，结束后把它 Detach 回去。
    if (attached) {
        jvm->DetachCurrentThread();
    }
}

void VideoFrameBuffer::recreateJavaBuffer() {
    std::unique_lock<std::mutex> lock(mtx);
    recreateJavaBufferLocked();
}

void VideoFrameBuffer::recreateJavaBufferLocked() {
    createJavaBuffersLocked();
}

jobject VideoFrameBuffer::getDirectByteBuffer(JNIEnv* env) {
    std::lock_guard<std::mutex> lock(mtx);
    // Java 永远只拿当前前缓冲。这样 Java 侧看到的是一帧已经写完整的数据。
    BufferSlot& slot = frontSlotLocked();
    if (slot.cachedBuffer) {
        return env->NewLocalRef(slot.cachedBuffer);
    }
    return slot.buf ? env->NewDirectByteBuffer(slot.buf, totalSize) : nullptr;
}

void VideoFrameBuffer::resize(int w, int h) {
    std::unique_lock<std::mutex> lock(mtx);
    resizeLocked(w, h);
}

void VideoFrameBuffer::resizeLocked(int w, int h) {
    if (w == width && h == height) return;

    // 这里调用时外层已经持有 mtx。
    // adopt_lock 的意思是：把“当前这把已经加上的锁”交给 unique_lock 接管，
    // 这样 waitForActiveWriterToFinishLocked() 内部就可以安全地 wait/解锁/再加锁。
    std::unique_lock<std::mutex> lock(mtx, std::adopt_lock);
    waitForActiveWriterToFinishLocked(lock);
    lock.release();

    width = w;
    height = h;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (jvm && jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        } else {
            env = nullptr;
        }
    }

    releaseSlotLocked(env, slots_[0]);
    releaseSlotLocked(env, slots_[1]);

    // 分辨率变化时，两块缓冲都必须按新尺寸重建，并重新绑定 Java DirectByteBuffer。
    allocLocked();
    createJavaBuffersLocked();
    frontIndex_ = 0;
    backIndex_ = 1;
    ++bufferEpoch_;
    if (attached) {
        jvm->DetachCurrentThread();
    }
}

void VideoFrameBuffer::alloc() {
    std::unique_lock<std::mutex> lock(mtx);
    allocLocked();
}

void VideoFrameBuffer::allocLocked() {
    if (width <= 0 || height <= 0) {
        width = 0;
        height = 0;
        ySize = uSize = vSize = totalSize = 0;
        slots_[0].buf = nullptr;
        slots_[1].buf = nullptr;
        return;
    }
    //y分量的大小
    long long yCalc = static_cast<long long>(width) * static_cast<long long>(height);
    LOG_BUFFER("allocLocked: width=%d height=%d yCalc=%lld", width, height, yCalc);
    
    if (yCalc <= 0 || yCalc > INT_MAX) {
        ySize = uSize = vSize = totalSize = 0;
        slots_[0].buf = nullptr;
        slots_[1].buf = nullptr;
        return;
    }

    ySize = static_cast<int>(yCalc);
    uSize = vSize = ySize / 4;

    long long totalCalc = static_cast<long long>(ySize) + static_cast<long long>(uSize) * 2LL;
    LOG_BUFFER("allocLocked: ySize=%d uSize=%d vSize=%d totalCalc=%lld",
               ySize, uSize, vSize, totalCalc);
    if (totalCalc <= 0 || totalCalc > INT_MAX) {
        ySize = uSize = vSize = totalSize = 0;
        slots_[0].buf = nullptr;
        slots_[1].buf = nullptr;
        return;
    }

    totalSize = static_cast<int>(totalCalc);
    // 双缓冲：始终为同一分辨率准备两块大小完全相同的 I420 内存。
    slots_[0].buf = new (std::nothrow) uint8_t[totalSize];
    slots_[1].buf = new (std::nothrow) uint8_t[totalSize];

    if (!slots_[0].buf || !slots_[1].buf) {
        delete[] slots_[0].buf;
        delete[] slots_[1].buf;
        slots_[0].buf = nullptr;
        slots_[1].buf = nullptr;
        ySize = uSize = vSize = totalSize = 0;
    }
}

bool VideoFrameBuffer::acquireBackBufferForWriteLocked(const AVFrame* f, WriteContext* ctx) {
    if (!f || !ctx) {
        return false;
    }

    // acquireBackBufferForWriteLocked 进入时，外层已经持有 mtx。
    // 这里先等待上一帧锁外写入完成，避免两个写入阶段交叠到同一个 back buffer。
    std::unique_lock<std::mutex> lock(mtx, std::adopt_lock);
    waitForActiveWriterToFinishLocked(lock);
    lock.release();

    if (f->width > 0 && f->height > 0 && (f->width != width || f->height != height)) {
        resizeLocked(f->width, f->height);
    }

    if (!f->data[0]) {
        return false;
    }

    BufferSlot& dstSlot = backSlotLocked();
    if (!dstSlot.buf) {
        return false;
    }

    // 从这一刻起，表示“有一帧正在锁外写 back buffer”。
    // resize 或下一帧开始前都必须先等这个标志清掉。
    writerActive_ = true;
    ctx->dst = dstSlot.buf;
    ctx->width = width;
    ctx->height = height;
    ctx->ySize = ySize;
    ctx->uSize = uSize;
    ctx->backIndex = backIndex_;
    ctx->bufferEpoch = bufferEpoch_;
    return true;
}

void VideoFrameBuffer::commitBackBufferWriteLocked(const WriteContext& ctx, bool writeOk) {
    // 只有满足以下条件，才说明：
    // 1) 本次写入成功；
    // 2) 中途没有发生 resize 导致缓冲重建；
    // 3) 当前 back buffer 还是本次开始写时拿到的那一块内存。
    //
    // 只有这时才允许 swap，把它变成给 Java 读的 front buffer。
    if (writeOk &&
        writerActive_ &&
        ctx.bufferEpoch == bufferEpoch_ &&
        ctx.backIndex == backIndex_ &&
        ctx.backIndex >= 0 &&
        ctx.backIndex < static_cast<int>(slots_.size()) &&
        slots_[ctx.backIndex].buf == ctx.dst &&
        ctx.width == width &&
        ctx.height == height) {
        // 一帧完整写入后，再交换前后缓冲：
        // - 新写好的后缓冲变成前缓冲，供 Java 读取
        // - 旧前缓冲退回为后缓冲，等待写下一帧
        swapBuffersLocked();
    }

    // 不管成功还是失败，这一帧的“锁外写入阶段”都结束了。
    writerActive_ = false;
    writerCv_.notify_all();
}

void VideoFrameBuffer::waitForActiveWriterToFinishLocked(std::unique_lock<std::mutex>& lock) {
    // 这里等待的不是“谁持有 mtx”，而是“是否仍有一帧在锁外写入 back buffer”。
    // wait 期间会临时释放 mtx，唤醒后再重新加锁。
    writerCv_.wait(lock, [this] { return !writerActive_; });
}

void VideoFrameBuffer::releaseJavaBufferForSlotLocked(JNIEnv* env, BufferSlot& slot) {
    if (env && slot.cachedBuffer) {
        env->DeleteGlobalRef(slot.cachedBuffer);
    }
    slot.cachedBuffer = nullptr;
}

void VideoFrameBuffer::releaseSlotLocked(JNIEnv* env, BufferSlot& slot) {
    releaseJavaBufferForSlotLocked(env, slot);
    delete[] slot.buf;
    slot.buf = nullptr;
}

// Java 侧准备读当前帧 的时候调用
VideoFrameBuffer::BufferSlot& VideoFrameBuffer::frontSlotLocked() {
    return slots_[frontIndex_];
}

//native 写新帧 的时候调用
VideoFrameBuffer::BufferSlot& VideoFrameBuffer::backSlotLocked() {
    return slots_[backIndex_];
}

void VideoFrameBuffer::swapBuffersLocked() {
    // 只交换索引，不搬移内存。
    // 这是双缓冲的关键：用一次轻量级指针/索引切换，替代“边写边读同一块内存”。
    std::swap(frontIndex_, backIndex_);
}
