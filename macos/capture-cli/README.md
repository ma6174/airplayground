# capture-cli

macOS 屏幕/窗口捕获命令行工具。使用 `ScreenCaptureKit` 捕获内容，使用 `VideoToolbox` 编码为 H.264；支持 Annex-B 裸流、MP4 文件和向 ESP32 AirPlay 发送端推流（`--tcp`）。

## 构建

需要 macOS 12.3 或更高版本。只需要 Command Line Tools，不需要安装完整 Xcode：

```sh
./build.sh
./capture-cli list
```

默认构建当前机器架构。构建 Intel + Apple Silicon 通用二进制：

```sh
./build.sh capture-cli --universal
```

`Package.swift` 保留用于有完整 SwiftPM 环境时的构建，但日常开发推荐使用 `build.sh`。

首次运行需要在“系统设置 → 隐私与安全性 → 屏幕与系统音频录制”中授权终端或该工具。

## 用法

```sh
# 列举显示器和窗口
capture-cli list

# 推给 ESP32 AirPlay 发送端（设备先烧好本仓 firmware，监听 8090）
capture-cli record --window-id 961 --fps 20 --bitrate 2000000 --tcp 192.168.1.158:8090
capture-cli record --display 0 --width 960 --height 600 --fps 20 --tcp 192.168.1.158:8090

# 声音一起送过去（音频走设备的 8091）
capture-cli record --window-id 961 --fps 20 --audio --tcp 192.168.1.158:8090

# 捕获第一个显示器，录制 10 秒
capture-cli record --display 0 --output screen.h264 --duration 10

# 捕获窗口
capture-cli record --window-id 1234 --output window.h264

# 导出 MP4（--audio 同时录系统音频，混成第二条轨）
capture-cli record --display 0 --output screen.mp4 --audio --duration 10

# 输出到标准输出；日志始终写入 stderr
capture-cli record --display 0 --stdout | ffplay -f h264 -
```

可选参数：

- `--fps N`：帧率，默认 30
- `--bitrate BPS`：平均码率
- `--width N --height N`：输出尺寸，必须为偶数
- `--duration SECONDS`：自动停止；未指定时使用 Ctrl-C
- `--no-cursor`：不捕获鼠标指针
- `--audio`：同时采集系统输出音频（默认关闭；需要 macOS 13+，非窗口专属，
  录的是整个系统的输出声音）；配合 `--tcp` 时音频实时发给设备（见下）
- `--audio-gain FACTOR`：音频增益，默认 `1.0`。AirPlay 的音频通道没有音量控制，
  接收端按自己的增益播，觉得吵就用它压（`0.5` 约 −6 dB）
- `--audio-output PATH`：音频另外写成 WAV 文件；裸流输出时省略则取视频文件同名
  `.wav`，`--tcp` 时省略则只走网络，MP4 不需要（会直接混流）
- `--tcp host:port`：把 H.264 访问单元推给 ESP32 AirPlay 发送端（见下）

`.h264`/`.264` 输出是包含 SPS/PPS 的 Annex-B H.264，便于用 `ffplay`、`ffprobe` 或后续 AirPlay 发送端读取；`.mp4` 输出使用系统 `AVAssetWriter` 封装 H.264，开启 `--audio` 时额外混入一条 AAC 轨。MP4 以外的输出装不下音频：给了 `--audio-output`（或裸流省略时推导出的）WAV 就旁路写成 32-bit float WAV（ScreenCaptureKit 原生 PCM，不经转码，ffmpeg/ffplay 可直接读），`--tcp` 则同时送上设备的音频通道。启动后 stderr 会打印一行 `audio stream connected: ...` 表示音频已接通，结束时打印采集到的 buffer 总数。

## 推流给 ESP32（`--tcp`）

编码后的访问单元不落盘，直接通过 TCP 发给设备侧 `firmware/components/media/src/media_net.c`。
设备只做 AirPlay 加密与转发，不重新编码，所以能投真实桌面。

线协议：一个大端配置包（宽高、帧率、SPS、PPS），随后每帧一个头 + AVCC 访问单元；
非 VCL 的 NAL（SPS/PPS/SEI/AUD）不进帧，参数集只走配置包。
密钥帧每 5 秒一个（文字密集的画面的关键帧上百 KB，设备侧要发 200 ms 以上；
每秒一个就会每秒顿一下）。投放终端/代码这类文字密集窗口时先降分辨率：
`--width 960 --height 664`，关键帧大小大约降到三分之一。

运行时每秒往 stderr 打印一行 `sent N frame(s)`，便于与设备日志对账；
设备端对应的一行是 `frame N sent: ... waited=Xms, sent in Yms`。

### 音频（`--audio`）

设备另有一个音频监听在 `port + 1`（视频 8090 对应音频 8091），`--tcp ... --audio`
会再建一条连接，音频包与视频共用 16 字节头，类型为 3，payload 是一个 ALAC 帧：
未压缩模式，44100 Hz / 16 bit / 2 声道 / 每帧 352 采样（与设备 SETUP 里声明的
`spf=352 sr=44100 ss=16 ch=2` 一致）。所以走网络时不需要 `--audio-output`；
给了它则额外再存一份 WAV。

走到接收端的声音大小由它自己定：链路本身是 1:1 的（实测峰值与源文件一致），
需要调整就在切帧之前用 `--audio-gain` 缩放。

系统音频分四级处理，任何一级慢下来都不会倒推上游的时钟：采集回调只把 PCM 字节
拷进队列就返回（在那里重采样会把 ScreenCaptureKit 的音频线程拖住，丢了 buffer
就是片子里的空洞）；一个线程用 `AVAudioConverter` 重采样成 44100 Hz / 16 bit /
交错立体声；一个线程攒够约 200 ms 后按 352 采样（7.98 ms）的节拍切帧（设备环形
缓冲是 64 帧约 511 ms，客户领先 200 ms 能扛住抖动，又不会把环填满）；最后一个
线程只管写 socket——链路塞住时 `write(2)` 会阻塞几百毫秒，卡在节拍线程上就会
把慢链路变成“又晚又断”的流。

两级队各 0.5 s（待转换的采集数据另算），满了丢最旧的，并计入 `dropped`。音频
连接建不上（设备没在听 8091、或者旧固件）只打印一行警告，视频照常发送；发到
一半断了则停掉音频线程、视频继续。

客户端的音频节拍不从启动那一刻开始算：设备的环形缓冲会在**会话开始的那一刻
清空**（里面是上一场会话、或者根本不属于任何一场的音频），所以设备在音频连接
上回一个字节表示“这条通道现在有人消费了”，客户端收到后把队列里最新的 200 ms
当作前导一次性灌进环形缓冲，再按 7.98 ms 的节拍继续。没有这个握手时，客户端
先启动、用户几秒后才 `/start` 的话，那 200 ms 前导早就被清掉，环形缓冲从空开始
——设备每个调度抖动都要断一次，实测空槽能到 34%，听感就是持续的咔哒和杂音。
设备每开始一场会话都会再发一次这个字节，客户端据此重建前导。会话开始前的采集
只保留最新 200 ms，其余丢弃（这部分“丢”是设计使然，不是跟不上）。旧固件不发
这个字节也不会卡死：等满 15 秒后照旧开始发送。

stderr 每 10 秒打印一行，所有数字都在同一条 44100 Hz 时间线上，所以可以对账：

```
sent 721 audio frame(s) (1714 total), 987 buffer(s) captured, converted 870519 sample(s) = 19739 ms, dropped 267168 sample(s), queues 0 + 0 ms, blocked 8622 ms writing
```

前两个是这 10 秒与累计发出的帧数（稳定时应为 10 s × 125.28 ≈ 1253）；`captured`
是采集侧累计 buffer 数（这个不涨就是 ScreenCaptureKit 那侧的事）；`converted` 是
转出来的采样数；`dropped` 是中途丢掉的采样数；`queues` 是待发送 PCM 与待写
socket 的两级队深；`blocked` 是累计卡在 `write(2)` 里的时间。

对账公式（容差几毫秒的取整）：

```
converted = sent × 7.982 ms + queues + dropped
```

拿上面那行举例：`1714 × 7.982 + 0 + 6059 = 19740 ms`，与 `converted 19739 ms`
对得上。`blocked` 占了墙钟时间的相当一部分，就说明短板在链路上（设备收得慢
或者 WiFi 拥挤），不是在采集；此时 `dropped` 会同步增长，而不是静静卡死。

## 权限与已知问题

- 需要“系统设置 → 隐私与安全性 → 屏幕与系统音频录制”授权。
- **每次重新构建二进制后授权会失效**，需在系统设置里重新勾选（否则 SCStream 报
  `-3805 application connection being interrupted`，或者根本不产帧）。
- 窗口捕获（`--window-id`）在开始前会预热 CoreGraphics（`CGWindowListCopyWindowInfo`），
  否则 `SCContentFilter(desktopIndependentWindow:)` 会因 `CGS_REQUIRE_INIT` 断言崩溃。
- ScreenCaptureKit 只在内容变化时产帧，静止窗口的帧率会低于 `--fps`。
- **窗口被其他应用完全遮挡后就停止出帧**：macOS 会节流、甚至暂停对被完全遮挡窗口的绘制（CoreAnimation 的省电优化），而 ScreenCaptureKit 只是被动读 WindowServer 里已合成的表面，于是没有新帧、下游停在最后一帧。没有公开 API 能绕开（`NSWindow.occlusionState` 只能读，`SCStreamConfiguration` 也没有相关开关），唯一办法是让目标窗口保持可见。注意这与“不需要在前台”不矛盾：不遮挡就照常出帧，遮挡了才停。
