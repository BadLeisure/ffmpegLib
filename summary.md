## 项目描述
负责基于 Android NDK、FFmpeg 和 JNI 的 RTSP 实时播放器研发，完成从网络拉流、解复用、软硬解码、帧队列调度、像素格式转换到 Java/OpenGL 渲染对接的完整播放链路建设。项目重点围绕实时性、稳定性和切流体验展开优化，支持 HEVC SEI 提取、`YUV420P/NV12/NV21` 输出、`DirectByteBuffer` 低拷贝回调、连接/读包超时中断、自动重连，以及“线程常驻 + 同参数复用 decoder”的切流优化方案。

## 项目亮点

### 1. 自研 RTSP 播放核心链路，覆盖 native 播放全流程
- 基于 `AVFormatContext`、`AVCodecContext`、`PacketQueue`、`FrameQueue` 和 JNI 回调，搭建 Android 端 RTSP 播放主链路，完成拉流、解码、帧缓存、I420 输出与 Java/OpenGL 渲染对接。
- 支持 `YUV420P/NV12/NV21` 多种像素格式输出，并结合 `libyuv` 完成 NV12/NV21 到 I420 的统一转换，降低上层渲染适配成本。
- 通过 `DirectByteBuffer` 将 I420 数据直接回调给 Java 层，减少 native 与 Java 间不必要的数据拷贝，优化视频帧传输效率。
- 支持 HEVC 码流中的 SEI 提取与回调，满足视频业务场景中元数据透传需求。

### 2. 设计多线程解耦架构，兼顾实时性和渲染稳定性
- 将播放器拆分为 `read thread`、`decoder thread`、`refresh thread` 三段式处理链路，分别负责拉流读包、视频解码和按时间戳渲染调度，降低单线程串行处理带来的阻塞风险。
- 设计 `PacketQueue` 连接拉流线程与解码线程，设计 `FrameQueue` 连接解码线程与刷新线程，通过队列隔离网络抖动、解码耗时和渲染节奏之间的相互影响。
- 在 `FrameQueue` 中引入循环队列、拥塞水位、自适应丢帧和渲染节奏控制策略，降低队列积压导致的延迟放大问题，使播放器更偏向实时流场景。
- 将视频数据回调从解码线程中解耦出来，避免 Java 渲染或上层处理阻塞 native 解码线程，提升播放链路稳定性。

### 3. 主导切流性能优化，减少重复初始化开销
- 将原有“每次切流都重新初始化 decoder、重建解码线程和刷新线程”的方案，重构为“线程常驻 + 同参数复用 decoder”的切流模型。
- 将 `PacketQueue` 和 `FrameQueue` 调整为在 `RtspPlayer` 创建时初始化一次、析构时统一销毁；切流过程中只做 flush 和状态重置，不再反复 `init/destroy`。
- 在 `streamComponentOpen()` 中引入 `ensureDecoderForStream()`，用于判断当前 `AVCodecContext` 是否可复用：
  - 首次播放时创建并绑定 `AVCodecContext`
  - 新旧流参数一致时复用已有 `AVCodecContext`
  - `decoder_thread` 和 `refresh_thread` 仅在首次需要时启动，后续切流保持常驻
- 针对切流场景封装 `decoder_flush_for_switch()`，统一完成旧流残留 packet/frame 清理、decoder flush、关键状态重置和队列重新启动，降低切流路径复杂度。

### 4. 增强实时流稳定性和异常恢复能力
- 增加连接超时、读包超时和长时间无视频包检测，并结合 FFmpeg interrupt callback 主动打断阻塞调用，避免播放器长时间卡死在网络或 IO 层。
- 针对 RTSP 等实时流场景设计自动重连、退避重试和状态机管理机制，提高弱网、推流中断、流端短暂异常等场景下的恢复能力。
- 在送解码前增加关键帧门控逻辑，避免切流或重连后直接从无效 P/B 帧起播，提升首帧可用性和播放稳定性。
- 保留硬解优先、软解回退的策略，在可用时优先尝试 `hevc_mediacodec`，失败后自动回退到软件解码，兼顾性能与兼容性。

### 5. 建立可观测性能力，提升问题定位效率
- 为切流过程增加 `switch_trace` 链路日志，记录 `request`、`cleanup_done`、`open_input_done`、`find_stream_info_done`、`refresh_thread_start`、`first_key`、`first_frame` 等关键节点耗时。
- 在 refresh 线程中引入 `generation` 机制，每次切流清空 `FrameQueue` 后递增版本号，检测到版本变化时自动重置 `frame_timer`、`last_frame_pts`、`session_base_pts` 等时间状态，避免新旧流 PTS 不连续导致首帧误判、丢帧或节奏异常。
- 通过切流编号、阶段耗时和队列状态日志，提升切流慢、首帧慢、关键帧等待、渲染节奏异常等问题的排障效率。

## 简历表述建议
- 负责基于 `Android NDK + FFmpeg + JNI` 的 RTSP 实时播放器研发，搭建拉流、解复用、软硬解码、帧队列调度、I420 输出与 Java/OpenGL 渲染对接的完整 native 播放链路，支持 HEVC SEI 解析与 `DirectByteBuffer` 低拷贝传输。
- 主导播放器多线程架构与低延迟优化，设计 `PacketQueue + FrameQueue + 解码/刷新线程` 解耦模型，并将切流流程优化为“线程常驻 + 同参数复用 decoder”，减少重复初始化开销，提升实时播放稳定性和切流响应速度。
- 完善播放器鲁棒性与可观测性，支持连接/读包超时中断、自动重连、关键帧门控、硬解优先软解回退及切流耗时链路日志，提升异常流场景下的可恢复性和问题定位效率。
