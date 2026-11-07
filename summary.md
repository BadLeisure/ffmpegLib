## 项目描述
负责基于 Android NDK、FFmpeg 和 JNI 的 RTSP 实时播放器研发，完成从网络拉流、解复用、软硬解码、帧队列调度、像素格式转换到 Java/OpenGL 渲染对接的完整播放链路建设。项目重点围绕实时性、稳定性和切流体验展开优化，支持 HEVC SEI 提取、`YUV420P/NV12/NV21` 输出、`DirectByteBuffer` 低拷贝回调、连接/读包超时中断、自动重连、原始码流录制存证，以及“线程常驻 + 同参数复用 decoder”的切流优化方案。

## 项目亮点

### 1. 自研 RTSP 播放核心链路，覆盖 native 播放全流程
- 基于 `AVFormatContext`、`AVCodecContext`、`PacketQueue`、`FrameQueue` 和 JNI 回调，搭建 Android 端 RTSP 播放主链路，完成拉流、解码、帧缓存、I420 输出与 Java/OpenGL 渲染对接。
- 支持 `YUV420P/NV12/NV21` 多种像素格式输出，并结合 `libyuv` 完成 NV12/NV21 到 I420 的统一转换，降低上层渲染适配成本。
- 通过 `DirectByteBuffer` 将 I420 数据直接回调给 Java 层，减少 native 与 Java 间不必要的数据拷贝，优化视频帧传输效率；输出侧采用“前/后双缓冲 + 指针交换”设计，整帧拷贝与像素格式转换在锁外完成并通过缓冲代际校验后才切换前缓冲，避免 Java 读到写了一半的帧。
- 支持 HEVC 码流中的 SEI 提取与回调，满足视频业务场景中元数据透传需求。

### 2. 设计多线程解耦架构，兼顾实时性和渲染稳定性
- 将播放器拆分为 `read thread`、`decoder thread`、`refresh thread` 三段式处理链路，分别负责拉流读包、视频解码和按时间戳渲染调度，降低单线程串行处理带来的阻塞风险。
- 设计 `PacketQueue` 连接拉流线程与解码线程，设计 `FrameQueue` 连接解码线程与刷新线程，通过队列隔离网络抖动、解码耗时和渲染节奏之间的相互影响。
- 为 `PacketQueue` 增加高水位受控丢包保护（字节数达 4MB 或包数达 500 即触发）：解码/渲染停滞导致队列积压超水位时，入队口丢弃后续非关键帧包，关键帧到达则清空队列重新对齐——解码器从新 IDR 干净重启、无引用断裂，防止网络包无界堆积导致 OOM；溢出开始/持续/恢复有全链路日志。
- 为 `FrameQueue` 配套实现 `FramePool` AVFrame 对象池，复用预分配的帧对象、仅通过 `av_frame_ref` 挂接解码数据，避免高频入队出队时反复分配释放。
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
- 在送解码前增加关键帧门控逻辑（起播/切流后丢弃首个关键帧之前的非关键包），并在解码后、入渲染队列前保留一层 I 帧兜底门控，避免直接从无效 P/B 帧起播，提升首帧可用性和播放稳定性。
- 提供“带宽暂停/恢复”播放控制接口：暂停时保留播放器对象与待恢复 URL、中断网络拉流并清理队列，恢复时基于原 URL 重新拉起播放，并支持上层等待播放器完全回到 Idle 状态。
- 保留硬解优先、软解回退的策略，在可用时优先尝试 `hevc_mediacodec`，失败后自动回退到软件解码，兼顾性能与兼容性。

### 5. 建立可观测性能力，提升问题定位效率
- 为切流过程增加 `switch_trace` 链路日志，记录 `request`、`cleanup_done`、`open_input_done`、`find_stream_info_done`、`refresh_thread_start`、`first_key`、`first_frame` 等关键节点耗时。
- 在 refresh 线程中引入 `generation` 机制，每次切流清空 `FrameQueue` 后递增版本号，检测到版本变化时自动重置 `frame_timer`、`last_frame_pts`、`session_base_pts` 等时间状态，避免新旧流 PTS 不连续导致首帧误判、丢帧或节奏异常。
- 通过切流编号、阶段耗时和队列状态日志，提升切流慢、首帧慢、关键帧等待、渲染节奏异常等问题的排障效率。

## 简历表述建议
- 负责基于 `Android NDK + FFmpeg + JNI` 的 RTSP 实时播放器研发，搭建拉流、解复用、软硬解码、帧队列调度、I420 输出与 Java/OpenGL 渲染对接的完整 native 播放链路，支持 HEVC SEI 解析与 `DirectByteBuffer` 低拷贝传输。
- 主导播放器多线程架构与低延迟优化，设计 `PacketQueue + FrameQueue + 解码/刷新线程` 解耦模型，并将切流流程优化为“线程常驻 + 同参数复用 decoder”，减少重复初始化开销，提升实时播放稳定性和切流响应速度。
- 完善播放器鲁棒性与可观测性，支持连接/读包超时中断、自动重连、关键帧门控、硬解优先软解回退、切流耗时链路日志及原始码流录制存证（`startRecord`/`stopRecord`），提升异常流场景下的可恢复性和问题定位效率。
### 2026-09-04 补充亮点
- 在“线程常驻 + 同参数复用 decoder”快路径之外，进一步补齐“参数不兼容时 teardown + rebuild decoder”的慢路径：`ensureDecoderForStream()` 可在异构流切换时安全停掉旧 decoder/refresh 线程、释放旧 `AVCodecContext` 并重建解码器，避免此前返回 `EAGAIN` 导致切流失败，提升复杂流场景下的可恢复性与可用性。

### 2026-11-04 补充亮点：原始码流录制（RawStreamDumper）
- 新增原始码流录制功能并提供 `startRecord(handle, filePath)` / `stopRecord(handle)` 接口，客户端按需开启：把 `av_read_frame` 得到的原始视频包（Annex B ES）录制到调用方指定的完整文件路径，用于区分“拉流数据本身问题”和“解码/渲染问题”。
- ES 文件头自动写入 Annex B 参数集（VPS/SPS/PPS），保证文件可被 ffprobe/ffmpeg 直接离线解码；录制中途切流且新流参数集变化时自动补写新参数集。
- 同步写 CSV sidecar（`<文件路径>.csv`，每包一行 `recv_us,pts,size,flags`），可与 `video_diag`、逐包日志按时间轴对齐，定位异常包的接收时刻、包大小与关键帧标志。
- 录制只受 `startRecord`/`stopRecord` 控制：切流、断线重连、超时重试均不断流不轮转（flush 后继续追加同一对 ES/CSV 文件）、无大小上限；仅 `stopRecord` 或播放器析构结束保存。

### 视频播放诊断汇总
- `video_diag` 每秒汇总一次视频管线指标，避免逐帧日志对播放造成干扰；统计包含每秒收到的 packet 数、解码帧数（`decoded`）、渲染成功帧数（`rendered`）、迟到丢帧数（`late_drop`），以及收包间隔、包 PTS 间隔、包队列等待、解码耗时、帧队列等待/驻留、调度迟到、回调耗时等最大值指标。

- `decoded_pts_interval_ms[min/avg/max]` 基于相邻成功解码帧的 `best_effort_timestamp` 和视频流 time base 计算，只统计正向且不超过 1 秒的间隔；PTS 缺失、重复、倒退和大幅跳变不纳入间隔统计。30 FPS 视频的典型间隔约为 33.3 ms。
- `rendered` 表示 native 视频回调返回成功的帧数，不等同于屏幕实际呈现帧数；`late_drop` 只统计因错过显示时机而丢弃的帧，不包含 FrameQueue 拥塞丢帧。
- 排查时结合解码帧 PTS 间隔、`decoded`、`rendered`、`late_drop`、帧队列驻留和回调耗时判断：PTS 间隔本身偏大优先检查源流/时间戳；PTS 间隔正常但迟到丢帧或队列驻留持续升高，则优先检查本地解码与渲染链路。
