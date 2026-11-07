# Player State Management

## 概览

这份文档描述实时播放器的运行期状态流转，包含：

- 启动
- 停止
- 带宽暂停与恢复
- 销毁
- 切流
- 线程协作

当前实现主要围绕 `RtspPlayer::start()`、`requestStopInternal()`、`readThreadLoop()` 以及 native 解码/刷新线程展开。

## 核心状态

`RtspPlayer` 当前使用 3 个主要运行状态：

- `Idle`：播放器当前没有在播放
- `Running`：播放器正在拉流、解码、渲染
- `Stopping`：播放器已经收到停止请求，正在做收尾清理

从概念上说，对象析构后还会进入最终“已销毁”状态，但这个状态不是通过 `state_` 表示，而是由 C++ 对象生命周期本身决定。

## 高层状态流转

```text
                    +------------------+
                    |      Idle        |
                    | not playing      |
                    +------------------+
                              |
                              | start(url)
                              v
                    +------------------+
                    |     Running      |
                    | reading/decoding |
                    | rendering        |
                    +------------------+
                     |       |        |
                     |       |        |
                     |       | start(newUrl)
                     |       | while playing
                     |       v
                     |  +------------------+
stop()/requestStop() +->|    Stopping      |
                     |  | cleaning up      |
                     |  +------------------+
                     |       |        |
                     |       |        |
                     |       | pending URL exists
                     |       | switch to next stream
                     |       v
                     |  +------------------+
                     |  |     Running      |
                     |  | playing new URL  |
                     |  +------------------+
                     |
                     | no pending URL
                     v
                    +------------------+
                    |      Idle        |
                    | fully stopped    |
                    +------------------+
                              |
                              | enableFpv(true)
                              | resume saved URL
                              v
                    +------------------+
                    |     Running      |
                    | resumed stream   |
                    +------------------+
                              |
                              | destructor
                              v
                    +------------------+
                    |    Destroyed     |
                    | resources freed  |
                    +------------------+
```

## 启动流程

一次正常启动通常从下面的调用开始：

```cpp
player->bindCallback(env, callback);
player->start("rtsp://example/live");
```

### `start()` 在做什么

`RtspPlayer::start(const std::string& url)` 并不会同步完成整个播放启动流程。它的职责更像是：

- 校验当前状态是否允许启动
- 记录这次要播放的 URL
- 更新运行标志位
- 拉起后台读流线程

真正的建连、读包、解码准备和播放循环，都是后面的后台线程完成的。

### 启动时序

```text
bindCallback()
    ↓
start(url)
    ↓
verify Java callback is already bound
    ↓
verify player state allows startup
    ↓
store rtspUrl_
set abortRequested_ = false
set threadRunning_ = true
set state_ = Running
reset HEVC gate state
    ↓
launch readThread_
    ↓
readThreadLoop()
    ↓
open input
find stream info
open/reuse/rebuild decoder
start decode/refresh threads if needed
    ↓
begin packet reading and playback
```

### 关键点

- 如果 Java 回调还没有绑定，`start()` 会直接返回，不启动。
- 如果当前播的就是同一个 URL，`start()` 会忽略这次请求。
- 真正耗时的网络建连、FFmpeg 打开流、准备解码器、进入读包循环，都不在调用 `start()` 的线程里做。

## 停止流程

停止通常从下面任一调用开始：

```cpp
player->stop();
```

或者：

```cpp
player->requestStop();
```

### 停止时序

```text
stop()
    ↓
requestStop()
    ↓
requestStopInternal(true)
    ↓
clear pendingRtspUrl_
clear hasPendingRtspUrl_
    ↓
state_: Running -> Stopping
    ↓
set abortRequested_ = true
notify waiting logic
abort queues when needed
    ↓
readThreadLoop() exits playback loop
    ↓
cleanupPlaybackState()
    ↓
if no pending URL:
    decoder_abort()
    releaseOwnedResources()
    state_ = Idle
```

### 关键点

- `requestStopInternal(true)` 表示这是真正的停止，不是切流。
- 调用线程只负责“发停止请求”。
- 真正的收尾清理是在后台 `readThreadLoop()` 里完成的。

## 带宽暂停与恢复流程

当 Flutter 获取日志时，可以通过 `enableFpv(false)` 临时停止所有 RTSP 网络拉流，减少带宽占用。这个流程只暂停播放会话，不销毁 `RtspPlayer` 对象，也不会释放 Java callback 和 `frameBuffer_`。

### 调用关系

```text
Flutter
    ↓ enableFpv(false)
Java/FishEyeCorrectionManager
    ↓ pauseAllRtsp()
Native player registry
    ↓ 遍历所有 RtspPlayer
RtspPlayer::pauseForBandwidth()
    ↓ 保存当前 rtspUrl_
    ↓ requestStopInternal(true)
    ↓ 等待 waitUntilIdle(5000)
所有播放器进入 Idle
    ↓
获取日志
    ↓ enableFpv(true)
Java/FishEyeCorrectionManager
    ↓ resumeAllRtsp()
RtspPlayer::resumeAfterBandwidth()
    ↓ 取出暂停前保存的 URL
    ↓ start(savedUrl)
重新连接、探测流信息、准备解码器
    ↓
等待关键帧和首帧
    ↓
回到 Running
```

### 状态时序

```text
Running
    |
    | enableFpv(false)
    v
Stopping
    |
    | 中断 av_read_frame()
    | 关闭 AVFormatContext
    | 清理 PacketQueue / FrameQueue
    | 完成后台线程收尾
    v
Idle
    |
    | 获取日志期间保持 Idle，不产生 RTSP 网络读取
    |
    | enableFpv(true)
    v
Running
    |
    | 使用保存的 URL 重新连接
    | 等待关键帧并恢复渲染
    v
Running
```

### native 接口职责

```cpp
nativePauseAllRtsp()  // 请求所有播放器暂停，并等待它们进入 Idle
nativeResumeAllRtsp() // 恢复每个播放器暂停前保存的 URL
```

`nativePauseAllRtsp()` 返回 `true` 表示所有播放器都在超时时间内完成停止；返回 `false` 表示至少有一路播放器没有及时进入 `Idle`。Java 层虽然可以保持 `enableFpv(bool)` 无返回值，但应在内部记录这个结果，只有确认暂停完成后才开始读取日志。

### 和普通 stop/start 的区别

```text
普通 stop/start：
Running -> stop() -> Idle -> start(url)

带宽暂停/恢复：
Running -> pauseForBandwidth() -> Idle -> resumeAfterBandwidth()
```

带宽暂停会额外保存暂停前的 URL，并阻止暂停期间普通 `start()` 把流重新拉起。恢复时使用保存的 URL 重新建立 RTSP 连接；由于网络输入已经关闭，不能继续使用旧的 `AVFormatContext`。

### 多路播放器的整体时序

```text
enableFpv(false)
    ↓
暂停 player-1、player-2、player-3
    ↓
等待所有播放器进入 Idle
    ↓
获取日志
    ↓
enableFpv(true)
    ↓
恢复 player-1、player-2、player-3
    ↓
各自重新连接并独立等待首帧
```

Java 层应使用单线程队列串行执行 `enableFpv(false)` 和 `enableFpv(true)`。如果暂停和恢复并发执行，恢复请求可能在播放器仍处于 `Stopping` 时到达，导致无效启动、额外重连或日志获取期间重新产生网络流量。

## 切流流程

切流的触发方式是：当前正在播旧流时，再次调用 `start(newUrl)`。

例如：

```cpp
player->start("rtsp://camera-a/live");
// later
player->start("rtsp://camera-b/live");
```

### 切流时序

```text
currently Running with old URL
    ↓
start(newUrl)
    ↓
detect current state is Running
detect newUrl != rtspUrl_
    ↓
store pendingRtspUrl_ = newUrl
set hasPendingRtspUrl_ = true
    ↓
requestStopInternal(false)
    ↓
old read loop begins shutdown
    ↓
readThreadLoop() finishing
    ↓
hasPendingRtspUrl_ == true
    ↓
this is a switch, not a final stop
    ↓
decoder_flush_for_switch()
rtspUrl_ = pendingRtspUrl_
pendingRtspUrl_.clear()
hasPendingRtspUrl_ = false
abortRequested_ = false
threadRunning_ = true
state_ = Running
reset HEVC gate state
    ↓
continue next open/decode/read cycle
    ↓
new stream begins playback
```

### 关键点

- `requestStopInternal(false)` 不会清掉 `pendingRtspUrl_`。
- 切流路径的目标，是在保证正确性的前提下，尽量避免没必要的完整销毁。
- 解码器这时可能会复用，也可能会因为参数不兼容而重建。

## 销毁流程

销毁发生在 `RtspPlayer::~RtspPlayer()`。

### 销毁时序

```text
~RtspPlayer()
    ↓
requestStop()
    ↓
join readThread_
    ↓
releaseOwnedResources()
reset frameBuffer_
decoder_destroy()
packet_queue_destroy()
frame_queue_destroy()
release Java global refs
    ↓
object lifetime ends
```

### 关键点

- 销毁阶段负责最终资源释放。
- 和切流不同，销毁不会保留任何待播放状态。
- 队列、解码器、帧缓冲、Java 回调引用，都会在这里统一回收。

## 线程模型

这个播放器可以理解为由“调用线程 + 3 条 native 工作线程”协同完成。

### 1. 调用线程

负责：

- `bindCallback()`
- `start()`
- `stop()`

这条线程本身不做重的 FFmpeg 工作，因此可以保持响应。

### 2. `readThread_`

负责：

- 网络建连
- `avformat_open_input`
- `avformat_find_stream_info`
- 读包
- 切流收尾与重新进入下一轮播放
- 最终播放清理

这条线程是一次播放会话的主控制线程。

### 3. `decoder_thread_`

负责：

- 从 `videoQueue_` 取压缩包
- 把包送进 FFmpeg 解码器
- 取出解码后的 `AVFrame`
- 把帧压入 `frameQueue_`

### 4. `video_refresh_thread_`

负责：

- 从 `frameQueue_` 取已解码帧
- 做时序控制
- 按积压情况动态调整渲染节奏
- 调用 `mVideoCallback(frame, pts)`

## 会话行为总结

### 正常启动

```text
Idle
  -> start(url)
  -> Running
  -> read/decode/render
```

### 正常停止

```text
Running
  -> requestStopInternal(true)
  -> Stopping
  -> cleanup
  -> Idle
```

### 切流

```text
Running(oldUrl)
  -> start(newUrl)
  -> requestStopInternal(false)
  -> Stopping
  -> flush/rebind/reopen
  -> Running(newUrl)
```

### 销毁

```text
Idle or Running
  -> destructor
  -> stop/join/destroy resources
  -> Destroyed
```

## 设计说明

这套实现有几个比较关键的设计特征：

- `start()` 是“请求入口”，不是完整的同步启动流程。
- 停止和切流复用了同一部分收尾机制。
- 是否存在 `pendingRtspUrl_`，决定这次收尾是“真正停止”还是“切到下一路流”。
- 当流参数兼容时，解码器可以复用。
- 当复用不安全或不兼容时，通过重建解码器保证播放正确。
- `readThread_` 和解码/刷新线程分开管理，因此切流时可以做得更灵活。

## 实际例子

### 场景 1：第一次播放

```cpp
RtspPlayer player(jvm);
player.bindCallback(env, callback);
player.start("rtsp://cam1/live");
```

结果：

- 播放器从 `Idle` 进入 `Running`
- `readThread_` 启动
- 解码器被初始化或复用
- 画面开始输出

### 场景 2：从一个摄像头切到另一个摄像头

```cpp
player.start("rtsp://cam1/live");
// after some time
player.start("rtsp://cam2/live");
```

结果：

- 当前播放收到停止请求
- 新 URL 被保存到 `pendingRtspUrl_`
- 旧会话先做清理
- 然后沿切流路径继续启动新流
- 最终重新回到 `Running`，但已经是在播新地址

### 场景 3：最终停止

```cpp
player.stop();
```

结果：

- 播放器进入 `Stopping`
- 后台线程完成清理
- 解码、队列、帧相关资源按流程释放
- 状态回到 `Idle`

## Java 调用建议

这一节从 Java 层调用方式来整理推荐时序，重点覆盖两个常见场景：

- 同一个播放器实例内切流
- 先完全停止拉流，等合适时机再重新拉流

### 场景 1：同一个播放器切流

推荐调用方式：

```java
player.start(urlA);

// 需要切到另一条流时，直接再次调用 start
player.start(urlB);
```

推荐时序：

```text
Java:
start(urlA)
    ↓
Native:
进入 Running，开始播放 urlA
    ↓
Java:
start(urlB)
    ↓
Native:
识别为“切流请求”
pendingRtspUrl_ = urlB
requestStopInternal(false)
    ↓
旧流收尾
flush / reopen / reuse or rebuild decoder
    ↓
继续进入 Running，开始播放 urlB
```

这一类场景下，不建议先手动 `stop()` 再 `start(urlB)`，因为那样更像“彻底停播后重启”，而不是同一会话里的平滑切流。

适用场景：

- 同一个页面里切摄像头
- 多个 RTSP 地址轮播
- 用户在同一个播放器窗口里切换不同流

### 场景 2：停止拉流，稍后再恢复

推荐调用方式：

```java
player.start(url);

// 暂时不需要播放
player.stop();

// 合适时机再重新启动
player.start(url);
// 或者 player.start(otherUrl);
```

推荐时序：

```text
Java:
start(url)
    ↓
Native:
进入 Running，开始播放
    ↓
Java:
stop()
    ↓
Native:
requestStopInternal(true)
进入 Stopping
后台完成清理
回到 Idle
    ↓
Java:
start(url) 或 start(otherUrl)
    ↓
Native:
重新进入 Running
启动新一轮拉流/解码/渲染
```

这条路径适合“现在要彻底停流，但播放器对象后面还要继续复用”的场景。

适用场景：

- 页面进入后台，先停流
- 用户暂时离开播放页，但实例还保留
- 网络异常后，等待合适时机再重新拉流

### 场景 3：彻底销毁播放器

如果 Java 层已经确定这个播放器实例后面不会再用了，推荐这样调用：

```java
player.stop();
player.destroy();
player = null;
```

后面如果还要播，需要重新创建新实例：

```java
player = new Player(...);
player.bindCallback(...);
player.start(url);
```

这里的区别是：

- `stop()`：停止当前拉流，但对象还保留，后面还能继续 `start()`
- `destroy()`：彻底释放播放器实例，后面如果再播，需要重新创建

### Java 层简化记法

如果只记最重要的两条，可以直接按下面用：

#### 同一个播放器切流

```java
player.start(urlA);
player.start(urlB);
player.start(urlC);
```

#### 停流后稍后恢复

```java
player.start(url);
player.stop();

// later
player.start(url);
```
