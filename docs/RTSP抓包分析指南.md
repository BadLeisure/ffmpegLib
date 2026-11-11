# Wireshark RTSP 流分析指南（TCP / UDP 模式）

> 适用场景：FPV/图传 RTSP 拉流排查——卡顿、黑屏、花屏、断流、延迟大等问题。
> 本文档中的案例数据来自真实抓包 `capture1.pcap`（遥控器 192.168.144.210 同时拉两路流）。

---

## 1. 抓包方法（分析的前提）

### 1.1 抓哪里

| 场景 | 抓包点 | 工具                                                                |
|---|---|-------------------------------------------------------------------|
| 遥控器/安卓设备本机 | 设备上 tcpdump（需 root） | `adb shell su 0 "tcpdump -i any -s 0 -U -w /sdcard/capture.pcap"` |
| PC 中转链路 | PC 开热点让设备流量经过电脑 | Wireshark 直接抓 Wi-Fi Direct 虚拟网卡                                   |
| UDP 模式流 | 必须抓设备本机或流经过的网卡 | 同上                                                                |

**关键参数说明**：
- `-i any`：抓所有网卡（安卓上产出 SLL 头，Wireshark 正常解析）
- `-s 0`：抓完整包（别截断，否则统计失真）
- `-U`：实时落盘，防止 Ctrl+C 前缓冲区数据丢失
- `adb pull /sdcard/capture.pcap` 拉回电脑分析

### 1.2 抓多久 / 怎么抓

1. **先开始抓，再点 Connect**——必须包含完整 RTSP 握手（OPTIONS/DESCRIBE/SETUP/PLAY），否则丢失会话参数（模式、SDP、Session ID）
2. 正常拉流 30–60 秒
3. 问题如果偶发，抓到问题复现为止；标注问题发生的**大概秒数**（对照 Wireshark 的相对时间轴）
4. 事后对照：Wireshark 默认显示**相对第一包的时间**，编辑问题时刻即可对齐

---

## 2. RTSP 会话结构（分析的知识基础）

```
客户端                          服务器
  │ OPTIONS   ──────────────────> │  探测能力
  │ <────────────── 200 OK        │  Public: 支持的方法列表
  │ DESCRIBE  ──────────────────> │  拿 SDP
  │ <── 200 OK + SDP(m=video...)  │  编解码 H265/H264、sprop 参数
  │ SETUP ──────────────────────> │  协商传输方式
  │ <────────────── 200 OK        │  Transport: TCP交织 或 UDP端口
  │ PLAY      ──────────────────> │  开始推流
  │ <────────────── 200 OK        │
  │ ════════ RTP 媒体数据 ═══════ │  TCP: 交织在554连接里
  │ GET_PARAMETER/OPTIONS 心跳    │  UDP: 独立 RTP/RTCP 端口
  │ （间隔必须 < Session timeout） │
  │ TEARDOWN ───────────────────> │  停止（客户端主动）
```

**两种传输模式的区别**（SETUP 的 Transport 字段区分）：

| | TCP 交织模式 | UDP 模式 |
|---|---|---|
| Transport 写法 | `RTP/AVP/TCP;interleaved=0-1` | `RTP/AVP;unicast;client_port=X-Y` |
| 媒体走哪 | 与信令同一条 TCP 连接（554），`$`开头分块 | 独立 UDP 端口对（视频/RTCP） |
| 丢包的表现 | TCP 重传（自动重传，不丢但可能卡） | 真丢包（花屏/黑屏） |
| Wireshark 解析 | 不自动按 RTP 解析，用 TCP 指标 | 自动按 RTP 解析，统计最丰富 |

---

## 3. 信令层分析（两种模式通用，先做这步）

### 3.1 操作方法

1. Wireshark 过滤器输入 `rtsp`
2. 右键任意 RTSP 包 → **跟随 → TCP 流**
3. 看完整明文会话：**红色 = 客户端请求，蓝色 = 服务器响应**

### 3.2 逐项检查表

| 信令 | 正常 | 异常及含义 |
|---|---|---|
| OPTIONS → | `200 OK` + `Public:` 列表 | 404/501：地址错或服务没起 |
| DESCRIBE → | `200 OK` + SDP | **401**：需要认证；**404**：流不存在 |
| SETUP → | `200 OK` + Transport 协商结果 | 461：不支持的传输方式 |
| PLAY → | `200 OK` | **454**：会话不存在（被踢/超时） |
| 心跳 | OPTIONS/GET_PARAMETER，间隔 < timeout | 间隔 > timeout → 服务器会踢会话（**断流的常见根因**） |
| TEARDOWN | 客户端主动停 + `200 OK` | 无 TEARDOWN 但连接 FIN/RST = 异常断开 |

### 3.3 SDP（DESCRIBE 响应里）关键字段

```
m=video 0 RTP/AVP 96            ← payload type 96（动态类型）
a=rtpmap:96 H265/90000          ← 编解码器 H265，时钟 90kHz
a=fmtp:96 sprop-sps=...;sprop-pps=...   ← 解码所需的参数集
```
SDP 声明的编码和实际推流不一致（比如声明 H264 实发 H265）→ 播放器解码失败 → 黑屏。

### 3.4 快速检索命令

```powershell
# 抽出整条流的信令对话（N = tcp.stream 编号）
tshark -r capture.pcap -q -z follow,tcp,ascii,N

# 在包里直接搜明文（信令是 ASCII，百试百灵）
# 过滤器：frame contains "TEARDOWN"   /   frame contains "454"
```

---

## 4. TCP 模式（交织）媒体层分析

原理：媒体数据和信令复用同一条 TCP 连接，TCP 保证不丢，**丢包表现为重传，卡顿表现为码率低谷**。

### 4.1 丢包/链路健康——TCP 错误计数器

| 过滤器 | 含义 | 阈值参考 |
|---|---|---|
| `tcp.analysis.retransmission` | 重传（网络丢包的直接证据） | 偶发正常；持续多 = 链路差 |
| `tcp.analysis.lost_segment` | 捕获点看到序号空洞 | >0 说明有段没到 |
| `tcp.analysis.duplicate_ack` | 重复 ACK（接收方催发送方） | 少量正常 |
| `tcp.analysis.window_full` | 发送端被接收窗口卡住 | 多 = 接收端慢 |
| `tcp.analysis.zero_window` | 接收端通告窗口=0 | **>0 = 播放器消费太慢，不是网络的锅** |
| `tcp.analysis.flags` | 以上全部的总开关 | 全程≈0 为健康 |

**归因口诀**：重传/丢段多 → 链路的锅；zero_window 多 → 播放器/解码侧的锅。

### 4.2 卡顿——码率曲线

- GUI：**统计 → I/O Graph**，添加图形行，过滤器 `tcp.stream==N`，间隔 1 秒
- 看什么：**有没有跌到接近 0 的秒**。视频码率恒定时（如 CBR），秒级波动应在 ±10% 内
- 命令行：

```powershell
tshark -r capture.pcap -q -z "io,stat,1,tcp.stream==0"
```

### 4.3 RTT（往返时延）估算

发送方数据段 → ACK 回来的时间（`tcp.analysis.ack_rtt`）：

```powershell
tshark -r capture.pcap -Y "tcp.stream==1 && ip.src==<发送方IP> && tcp.len>0 && tcp.analysis.ack_rtt" -T fields -e tcp.analysis.ack_rtt
```

参考量级：本地直连 0.1–1ms；局域网 1–5ms；公网 20–80ms。

### 4.4 断流定位

```
tcp.stream==N && (tcp.flags.reset==1 || tcp.flags.fin==1)
```
出现 RST/FIN 而没有 TEARDOWN → 异常断开；RST 前的最后几个包往往是定位线索。

---

## 5. UDP 模式媒体层分析（RTP）

UDP 不保证送达，**丢包是真实丢包**，Wireshark 的 RTP 统计在这里火力全开。

### 5.1 总量统计：RTP Streams（最常用）

- GUI：**电话(Telephony) → RTP → Streams**——每条流的丢包率、最大抖动、总包数一目了然
- 命令行：

```powershell
tshark -r capture.pcap -q -z rtp,streams
```

### 5.2 判读标准（FPV 场景）

| 指标 | 健康 | 需关注 | 问题 |
|---|---|---|---|
| 丢包率 | < 0.1% | 0.1–1% | > 1%（花屏/黑屏） |
| 最大抖动 | < 10ms | 10–30ms | > 30ms（依赖 jitter buffer） |
| 乱序 | 无 | 偶发 | 频繁（接收端要重排，增延迟） |

### 5.3 常用 RTP 过滤器

```
rtp                          全部 RTP 包
rtp.ssrc == 0x12345678       某一路媒体流（多路时先分清 SSRC）
rtp.p_type == 96             按 payload type 过滤（对照 SDP）
rtp.marker == 1              帧边界包（每视频帧最后一个 RTP 包）
rtp.seq                      序列号——手查丢包看它的跳变
```

### 5.4 定位丢包发生的位置

在客户端抓到丢包后，换**服务器/源端再抓一份**对比：
- 两边丢的数量一致 → 链路中间丢（无线链路质量/拥塞）
- 源端无丢、客户端丢 → 路径某段丢（重点查无线段、NAT）

---

## 6. 典型问题定位手册

### 6.1 卡顿（持续或周期性）

```
先确认模式（SETUP Transport）
├─ TCP 模式
│   ├─ I/O Graph 有码率低谷 + 低谷期重传多 → 链路丢包拥塞
│   ├─ zero_window 多 → 播放器消费慢（解码线程/CPU），不是网络
│   └─ 码率曲线平稳但卡 → 看播放器日志/解码统计，网络层没问题
└─ UDP 模式
    ├─ RTP Streams 丢包率 >1% 或抖动大 → 无线链路问题
    ├─ 只在特定时刻丢（对照时间轴，如电机启动时）→ 图传射频干扰
    └─ 无丢包但卡 → 检查 RTCP/发送节奏、播放器 jitter buffer
```

### 6.2 黑屏（按发生位置分四类）

```
黑屏
├─ ① 连不上（信令层失败）
│   ├─ 无 TCP 三次握手到 554 → 端口不通/防火墙/IP 错
│   │   过滤器：tcp.port==554 && (tcp.flags.syn==1)
│   ├─ DESCRIBE 401 → 要认证；404 → 流路径不存在
│   └─ TCP RST 直接拒绝 → 服务没起或拒绝连接
├─ ② 信令 OK 但无媒体
│   ├─ TCP 模式：PLAY 200 后 I/O 图无字节 → 服务器没推流
│   └─ UDP 模式：信令正常但 client_port 上无 RTP 包
│       → 经典的 NAT/防火墙拦截 UDP！信令通了画面黑
│       过滤器：udp.port==<client_port>（对照 SETUP 里的端口）
├─ ③ 媒体到了但解码失败
│   ├─ SDP sprop 与实际流不匹配、payload type 对不上
│   ├─ UDP 丢了 I 帧（等 IDR 超时）→ 丢包率统计 + rtp.marker 对齐帧
│   └─ 播放器等首帧逻辑超时（查播放器日志）
└─ ④ 服务器本来就没出图
    └─ 包里流正常、码率正常但黑 → 摄像头/编码器侧问题，换端排查
```

### 6.3 花屏

- 传输层丢包（UDP 模式为主）：丢包率统计 + 对照时间轴；花屏时点附近必有丢包/乱序，持续花屏 = 关键参考帧丢失且未刷新 IDR
- ⚠️ **TCP 模式也可能花屏（2026-11-10 实证修正）**：TCP 保证收到的字节 = 服务器发的字节，但不保证"服务器发的码流本身是好的"。链路零丢包仍花屏 → 坏帧来自**源头编码器**：编码器过载丢帧时参考链断裂，产出引用不存在的帧
- **定界方法**：把收到的裸流离线解码 `ffmpeg -v error -i main.h265 -f null -`——有 `Could not find ref with POC N` 错误 = 码流里真有坏帧（源头问题，抓包无丢包佐证 = 编码器过载）；无错误但实时仍花屏 → 查渲染/解码端
- 实测案例：8.94Mbps 录制（图传 8Mbps 超载）→ 100 帧坏帧/10 区间；降到 2.62Mbps → 15 帧/2 区间。坏帧数量与"码率/链路容量比"正相关

### 6.4 延迟大

- 端到端 = 采集+编码 + 传输 + jitter buffer + 解码
- Wireshark 能查传输段：RTT 估算、抖动、乱序、重传
- 全链路各段都正常 → 查播放器缓冲策略和编码端

### 6.5 自动断流（拉一会儿就断）

1. follow TCP 流看最后几条信令：有 TEARDOWN？454？还是直接 RST？
2. 查心跳间隔 vs 服务器 timeout（`Session:` 行或服务器文档）
3. 对照断流时刻的 I/O 图：断流前是否码率先劣化（拥塞踢人）还是突然死亡（进程/链路）

---

## 7. 命令与过滤器速查表

### Wireshark 显示过滤器

```
rtsp                                  RTSP 信令包
tcp.stream==N                         第 N 条 TCP 会话
tcp.port==554                         RTSP 端口
tcp.analysis.retransmission           重传
tcp.analysis.lost_segment             序号空洞
tcp.analysis.zero_window              接收端缓冲满
tcp.analysis.window_full              发送端被卡
tcp.flags.reset==1                    RST（异常断开）
tcp.flags.syn==1                      连接发起
frame contains "TEARDOWN"             按明文搜信令
rtp / rtp.ssrc / rtp.p_type / rtp.marker   RTP（UDP 模式）
ip.addr==x.x.x.x                      按地址
```

### tshark 命令

```powershell
# 协议构成总览
tshark -r f.pcap -q -z io,phs
# TCP 会话排名
tshark -r f.pcap -q -z conv,tcp
# 每秒码率曲线（流 N）
tshark -r f.pcap -q -z "io,stat,1,tcp.stream==N"
# UDP 模式：RTP 丢包/抖动总表
tshark -r f.pcap -q -z rtp,streams
# 抽 RTSP 信令明文（流 N）
tshark -r f.pcap -q -z follow,tcp,ascii,N
# RTT 估算
tshark -r f.pcap -Y "tcp.stream==N && ip.src==<IP> && tcp.len>0 && tcp.analysis.ack_rtt" -T fields -e tcp.analysis.ack_rtt
# 文件基本信息
capinfos f.pcap
```

---

## 8. 实战案例：capture1.pcap 分析记录

**背景**：遥控器（192.168.144.210，安卓，tcpdump -i any 抓取）拉流，抓包 45.7s、26177 包。

**分析过程与结论**：

1. `io,phs` → RTSP over TCP 为主（11.6MB），UDP 无视频 → 确认是 TCP 交织模式
2. `conv,tcp` → 发现**同时拉两路流**：111.204:8554 与 144.10:8554，各 6.5MB
3. TCP 错误计数 → 两路流重传/丢段/重复 ACK/零窗口**全部为 0**
4. `io,stat,1` 每秒码率 → 两路各稳定 137–146 KB/s（≈1.05 Mbps），46 个整秒无一低谷
5. RTT → 到 144.10 平均 0.3ms（本地直连级别）
6. 信令抽取 → 流②有 OPTIONS 心跳（200 OK，LIVE555 客户端）；**流① 45 秒内无任何信令、无心跳** ⚠️

**遗留观察点**：
- 流①（111.204）无心跳——若服务器有会话超时机制，长拉存在被踢风险，需对照 gortsplib 配置
- 两路流码率完全相同（1.05 Mbps）——确认是同一路视频被拉两次还是两个子码流（需抓完整握手看 SDP）

---

## 9. 常见坑

| 坑 | 说明 |
|---|---|
| 抓包没包含握手 | Connect 之后再开始抓，会丢失 SDP/Transport/Session 信息，黑屏类问题无法完整定位 |
| tcpdump 不加 `-U` | 缓冲区没落盘，Ctrl+C 后尾部数据丢失 |
| TCP 模式期待 RTP 统计 | Wireshark 对交织 RTP 不自动按 RTP 解析；TCP 模式用 TCP 指标 |
| UDP 黑屏查信令 | 信令走 TCP、媒体走 UDP，**信令全绿媒体到不了**（NAT/防火墙拦 UDP）是黑屏经典原因 |
| zero_window 当成网络问题 | 它是接收端消费慢的信号，锅在播放器/解码侧 |
| 只抓一端下结论 | 客户端丢包必须对照源端/中间抓包才能定位丢在哪一段 |
| Windows 抓包无接口 | Npcap 没装；接口列表空时先装 Npcap |
| 管理员权限 | Wireshark/Npcap 部分接口需要管理员权限才能抓 |
