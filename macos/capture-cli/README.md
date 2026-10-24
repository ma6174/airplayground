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
  录的是整个系统的输出声音）
- `--audio-output PATH`：音频单独写成 WAV 文件；仅在裸流/`--stdout`/`--tcp`
  输出时需要（MP4 会直接混流），裸流输出时可省略，默认取视频文件同名 `.wav`
- `--tcp host:port`：把 H.264 访问单元推给 ESP32 AirPlay 发送端（见下）

`.h264`/`.264` 输出是包含 SPS/PPS 的 Annex-B H.264，便于用 `ffplay`、`ffprobe` 或后续 AirPlay 发送端读取；`.mp4` 输出使用系统 `AVAssetWriter` 封装 H.264，开启 `--audio` 时额外混入一条 AAC 轨。裸流/stdout/`--tcp` 装不下音频，`--audio` 时音频旁路写成 32-bit float WAV（ScreenCaptureKit 原生 PCM，不经转码，ffmpeg/ffplay/固件侧可直接读）。启动后 stderr 会打印一行 `audio stream connected: ...` 表示音频已接通，结束时打印采集到的 buffer 总数。

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

## 权限与已知问题

- 需要“系统设置 → 隐私与安全性 → 屏幕与系统音频录制”授权。
- **每次重新构建二进制后授权会失效**，需在系统设置里重新勾选（否则 SCStream 报
  `-3805 application connection being interrupted`，或者根本不产帧）。
- 窗口捕获（`--window-id`）在开始前会预热 CoreGraphics（`CGWindowListCopyWindowInfo`），
  否则 `SCContentFilter(desktopIndependentWindow:)` 会因 `CGS_REQUIRE_INIT` 断言崩溃。
- ScreenCaptureKit 只在内容变化时产帧，静止窗口的帧率会低于 `--fps`。
