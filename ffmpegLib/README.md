总结：使用Android jni，通过ffmpeg实现拉rtsp流，并且将解码得到的裸流数据回调给上层，渲染由上层openGL渲染
初始实现方式：目前是双线程，read拉流线程，decode解码线程。裸流数据的回调在解码线程里实现。数据结构使用了一个PacketQueue作为拉流和解码的桥梁，存放拉流获得的AVPacket

第一版优化：新增加一个渲染线程，以及一个FrameQueue(使用了一个循环数组，可覆盖，保证实时性)做桥梁，解码和数据回调给java渲染分开，防止packetQueue堆积过高导致延迟过大

第二版优化：使用状态机，保证推流端退出导致拉流停在最后一帧，不重新启动的情况下，推流端重新推流也可以重新获取到流渲染

2.26:提交了编译ffmpeg使用的脚本以及编译openssl使用的脚本

0403:
1、添加ffmpeg硬解码以及实现硬解码失败自动退回软解码
2、添加启动mediacodec硬解码的编译脚本
3、添加libyuv库，实现NV12到I420的转换

0902:
1、当前切流流程调整为“线程常驻 + 相同参数复用decoder”的模式
2、PacketQueue 和 FrameQueue 改为在 RtspPlayer 创建时初始化一次，在析构时统一销毁；切流过程中不再重复 init/destroy
3、streamComponentOpen 当前的职责变成：
   1) 先调用 ensureDecoderForStream 判断当前 AVCodecContext 是否可复用
   2) 如果还没有 AVCodecContext，则首次创建并绑定到 Decoder
   3) 如果已有 AVCodecContext 且新流参数一致，则直接复用已有 AVCodecContext
   4) decoder_thread 和 refresh_thread 只在首次需要时启动，后续切流不再重复创建线程
4、切流时如果存在 pendingUrl，不再调用 decoder_abort 销毁线程，而是调用 decoder_flush_for_switch：
   1) 清空 packet_queue 和 frame_queue 中残留的旧流数据
   2) flush 当前 AVCodecContext
   3) 重置 renderKeyFrameReady、packet_pending 等解码状态
   4) 重新启动队列的工作状态，让常驻线程继续消费新流数据
5、refresh 线程增加 generation 机制：每次 frame_queue_flush 后递增 generation，refresh 线程检测到 generation 变化后重置 frame_timer、last_frame_pts、session_base_pts、avgFrameIntervalSec 等时间状态，避免新旧流 PTS 不连续导致首帧误判、丢帧或节奏异常
6、与之前流程的主要区别：
   1) 之前每次切流都会走 decoder_init + decoder_start + refresh_start，并在 cleanupPlaybackState 中 decoder_abort + decoder_destroy
   2) 现在切流只关闭旧的 AVFormatContext，保留 Decoder、video_thread、video_refresh_thread 和队列对象本身
   3) 之前切流的主要耗时点之一是反复初始化解码器和线程；现在同参数切流会直接复用 AVCodecContext，只做 flush 和状态重置
   4) stop/destroy 仍然会真正调用 decoder_abort 退出线程；切流场景下则尽量保留常驻线程
7、当前限制：
   1) 只有在新旧流 codec_id、codec_type、分辨率、像素格式、extradata 等参数一致时才复用 AVCodecContext
   2) 如果线程仍在运行且参数不一致，目前不会在线重建 AVCodecContext，而是返回 EAGAIN，避免后台线程访问已释放的解码器上下文
0904:
1、补充“同参数复用 + 不兼容参数重建”的双路径切流策略：
   1) `ensureDecoderForStream()` 先判断当前 `AVCodecContext` 是否可复用；
   2) 新旧流 `codec_id`、`codec_type`、分辨率、`extradata` 等关键参数一致时，直接复用已有 `AVCodecContext`；
   3) 参数不一致时，不再返回 `EAGAIN`，而是调用 `rebuildDecoderForStream()` 执行“停止旧 decoder/refresh 线程 -> 释放旧 `AVCodecContext` -> 重新创建并初始化 decoder”的重建流程；
   4) `streamComponentOpen()` 保持“确保 decoder 就绪后再按需启动线程”的职责，保证异构流切换后仍能继续正常解码和播放。
2、恢复硬解起播门控逻辑：
   1) 当启用 `hevc_mediacodec` 硬解时，送解码前会检测包内 `VPS/SPS/PPS` 与 `IRAP`；
   2) 只有参数集就绪且收到 `IRAP` 后才正式喂给硬解码器，避免切流或重连后硬解直接吃到无效起始包。
0917:
1、添加停止流/继续拉流的接口与 pendingUrl 语义：
   1) 切流过程中收到的新地址保存为 pendingUrl，停止当前流后可从 pending 地址继续拉流；
   2) 真正的 stop/destroy 会清除 pending 并彻底释放资源，不会自动重新拉流。
1107:
1、原始码流录制（RawStreamDumper）：
   1) 新增 startRecord/stopRecord 接口，把收到的原始 ES 落盘为 H.265 裸流（调用方指定文件路径）；
   2) 同步写 CSV 帧级流水（recv_us/pts/size/flags/recv_time），可与 video_diag、logcat 时间轴对齐；
   3) 切流/重连不断流不轮转，无大小上限，仅 stopRecord 或析构结束；
   4) 用途：离线 ffprobe/ffmpeg 解码定界“拉流问题 vs 解码渲染问题”。
2、PacketQueue 高水位受控丢包：
   1) 队列超过 4MB/500 包水位后：丢弃后续非关键帧，关键帧到达则清队从 IDR 干净重启；
   2) 防止解码/渲染停滞时网络包无界堆积导致 OOM。
3、平滑优先渲染：
   1) FrameQueue 12→8 帧、FramePool 14→10；
   2) 显示延迟预算 66ms：健康 IDR 空档（84~148ms，IDR 序列化的固有代价）大部分被存货+预算吸收；
   3) 自适应丢帧水位重标定 6/7、首帧攒帧再出。
4、新增 docs/ 分析文档：TCP卡顿与UDP流畅分析、拉流到显示全链路分析、RTSP抓包分析指南。
1111:
1、CSV 末列新增 recv_time（本地墙钟 YYYY-MM-DD HH:MM:SS.mmm），可与 logcat 时间轴对齐；
2、recv_us 保持单调钟不变，超时保护与统计逻辑零影响；
3、三份分析文档归档至 docs/ 并推送。
