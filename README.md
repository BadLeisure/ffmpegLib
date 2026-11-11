# ffmpegLib
# FFmpeg Android RTSP Player

这是一个基于 Android NDK、FFmpeg 和 JNI 的 RTSP 视频播放器示例工程。播放器在 native 层完成网络拉流、视频解码、帧队列管理和像素格式转换，再通过 Java callback 将视频帧和业务数据回调给上层应用。

## 主要功能

- 支持 RTSP 视频流播放，使用 FFmpeg 完成输入流打开、流信息分析和数据读取。
- 支持同一个播放器实例切换不同的 RTSP 地址。
- 支持网络连接失败和读取失败后的自动重试与退避。
- 支持 FFmpeg 软件解码，并在可用时优先尝试 `hevc_mediacodec` 硬件解码，失败后自动回退到软件解码。
- 支持 H.265/HEVC 视频流的 SEI 数据提取，并通过 Java callback 回调给上层。
- 支持 `YUV420P`、`NV12` 和 `NV21` 视频帧输出。
- 使用 libyuv 将 NV12/NV21 转换为 I420。
- 通过 DirectByteBuffer 将 I420 数据回调给 Java 层，减少不必要的数据拷贝。
- 使用 PacketQueue、FrameQueue 和独立的解码/刷新线程降低播放延迟。
- 使用双缓冲视频输出，避免 Java 读取视频帧时与 native 写入发生冲突。
- 支持连接超时、读取超时和视频包长时间未到达检测。
- 支持原始码流录制：收到的 H.265 ES 可落盘为裸流文件，并同步记录 CSV 帧级流水（recv_us/pts/size/flags/recv_time），用于离线定界"拉流问题 vs 解码渲染问题"。
- 支持 PacketQueue 高水位受控丢包：队列超限时丢弃非关键帧、关键帧到达则清队从 IDR 干净重启，防止解码/渲染停滞时内存无界增长。
- 支持平滑优先渲染：显示延迟预算 + FrameQueue 存货缓冲，吸收图传拥塞期的到达抖动。
- 支持播放器停止、销毁以及 native 资源和 Java 全局引用的统一释放。

## 工作流程

播放器由以下几个主要部分组成：

```text
Java/Kotlin
    |
    | JNI
    v
RtspPlayer
    |
    +-- read thread       打开 RTSP、读取 AVPacket、处理重连和切流
    |
    +-- PacketQueue       连接拉流线程和解码线程
    |
    +-- decoder thread    FFmpeg 解码
    |
    +-- FrameQueue        连接解码线程和刷新线程
    |
    +-- refresh thread    按时间戳调度视频帧并回调 Java
    |
    +-- VideoFrameBuffer  转换并输出 I420 数据
```

### 同一个播放器切流

播放器运行期间再次调用 `startRtsp()` 并传入新的 URL 时，新的地址会先保存为 pending URL。播放器随后异步停止当前流，关闭旧的输入上下文、解码器和队列，再在同一个 `RtspPlayer` 实例中重新连接新的 URL。

如果切流过程中连续收到多个新 URL，最终 pending 的 URL 会作为下一路播放地址。普通 `stop()` 或播放器销毁操作会清除 pending URL，不会自动重新拉流。

## 切流耗时日志

这次新增了切流链路的 `switch_trace` 日志，用来分析一次切流从发起到首帧显示之间的耗时。

日志会记录以下关键阶段：

- `request`：发起切流请求
- `cleanup_done`：旧流清理完成
- `open_input_done`：新流 `avformat_open_input` 完成
- `find_stream_info_done`：`avformat_find_stream_info` 完成
- `refresh_thread_start`：刷新线程启动完成
- `first_key`：收到新流首个关键帧
- `first_frame`：首次回调 Java 画面

每条日志都会带有同一次切流的 `switch_id`，并输出从 `request` 开始计算的各阶段耗时，方便定位切流过程中的耗时环节，例如旧流清理、新流连接、流信息探测、等待关键帧或首帧回调。

## Java 回调

`FFmpegCallback` 当前提供以下回调：

- `onI420Data(ByteBuffer buffer, int width, int height, double pts)`：回调 I420 视频帧。
- `onSEIData(byte[] seiData)`：回调从 HEVC 码流中提取的 SEI 数据。
- `onError(int type)`：回调 FFmpeg 或播放器错误。
- `onProgress(int percent)`：预留进度回调。
- `onComplete()`：预留播放完成回调。

上层可以使用 `ByteBuffer` 数据进行 OpenGL、Surface 或其他视频渲染。

## 工程结构

```text
app/src/main/cpp/
├── RtspPlayer.cpp/.h       播放器状态、拉流、重连和切流
├── ffplay.cpp/.h           解码线程和视频刷新线程
├── ffplay_def.cpp/.h       PacketQueue、FrameQueue 和 FramePool
├── VideoFrameBuffer.cpp/.h 双缓冲和视频格式转换
├── native-lib.cpp          JNI 接口和播放器实例管理
└── native_error.cpp/.h     错误码和状态定义
```

## 构建信息

- Android `minSdk`: 24
- Android `targetSdk`: 36
- NDK ABI: `arm64-v8a`
- CMake: 3.22.1
- Java source/target compatibility: Java 11
- Native 依赖：FFmpeg、libyuv

使用 Android Studio 打开工程并执行 Gradle 构建即可。FFmpeg、libyuv 和相关 native 库位于 `app/src/main/jniLibs` 及 `app/src/main/cpp/include`。

## 当前范围

当前实现主要面向实时视频流播放，支持创建多个相互独立的播放器实例；每个播放器实例负责一路视频流，并独立进行播放、重连和切流。音频播放、单个实例同时播放多路视频、硬件解码器的全平台兼容性以及完整的视频渲染 UI 不属于当前 native 播放器实现范围。

应用层使用openGL做渲染，以及使用RtspPlayerClient做为客户端与native的实例对应。
未来可能兼容：单实例多路
单实例多路：一个 RtspPlayer 里面放多个 session，每个 session 才有自己的 readThread + PacketQueue + Decoder + FrameQueue + VideoFrameBuffer + callback，其实就是把实例从外面提到了里面，只不过需要多包一层管理器

## 0902 流程调整总结

### 当前流程

当前切流流程调整为“线程常驻 + 相同参数复用 decoder”的模式：

- `PacketQueue` 和 `FrameQueue` 在 `RtspPlayer` 创建时初始化一次，在析构时统一销毁；切流过程中不再重复 `init/destroy`
- `streamComponentOpen()` 当前的职责变成：
  - 先调用 `ensureDecoderForStream()` 判断当前 `AVCodecContext` 是否可复用
  - 如果当前还没有 `AVCodecContext`，则首次创建并绑定到 `Decoder`
  - 如果已有 `AVCodecContext` 且新流参数一致，则直接复用已有 `AVCodecContext`
  - `decoder_thread` 和 `refresh_thread` 只在首次需要时启动，后续切流不再重复创建线程
- 切流时如果存在 `pendingUrl`，不再调用 `decoder_abort` 销毁线程，而是调用 `decoder_flush_for_switch()`：
  - 清空 `packet_queue` 和 `frame_queue` 中残留的旧流数据
  - flush 当前 `AVCodecContext`
  - 重置 `renderKeyFrameReady`、`packet_pending` 等解码状态
  - 重新启动队列的工作状态，让常驻线程继续消费新流数据
- `refresh` 线程增加 `generation` 机制：每次 `frame_queue_flush()` 后递增 `generation`，`refresh` 线程检测到 `generation` 变化后会重置 `frame_timer`、`last_frame_pts`、`session_base_pts`、`avgFrameIntervalSec` 等时间状态，避免新旧流 `PTS` 不连续导致首帧误判、丢帧或节奏异常

### 和之前的区别

- 之前每次切流都会走 `decoder_init + decoder_start + refresh_start`，并在 `cleanupPlaybackState` 中执行 `decoder_abort + decoder_destroy`
- 现在切流只关闭旧的 `AVFormatContext`，保留 `Decoder`、`video_thread`、`video_refresh_thread` 和队列对象本身
- 之前切流的主要耗时点之一是反复初始化解码器和线程；现在同参数切流会直接复用 `AVCodecContext`，只做 flush 和状态重置
- `stop/destroy` 仍然会真正调用 `decoder_abort` 退出线程；切流场景下则尽量保留常驻线程

### 当前限制

- 只有在新旧流 `codec_id`、`codec_type`、分辨率、像素格式、`extradata` 等参数一致时才复用 `AVCodecContext`
- 如果线程仍在运行且参数不一致，目前不会在线重建 `AVCodecContext`，而是返回 `EAGAIN`，避免后台线程访问已释放的解码器上下文


未来发展方向：
1、地面端侧自主实时检测，摆脱对机载 SEI 数据的强依赖
2、在 Android 拉流端接入 AI 超分，就能在本地将 720P 实时放大到 1080P 甚至 4K，让用户看到媲美原生高清的画质
