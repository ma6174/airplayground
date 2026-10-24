# capture-cli

macOS 屏幕/窗口捕获命令行工具。使用 `ScreenCaptureKit` 捕获内容，使用 `VideoToolbox` 编码为 H.264；支持 Annex-B 裸流和 MP4 文件。

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

# 捕获第一个显示器，录制 10 秒
capture-cli record --display 0 --output screen.h264 --duration 10

# 捕获窗口
capture-cli record --window-id 1234 --output window.h264

# 导出 MP4
capture-cli record --display 0 --output screen.mp4 --duration 10

# 输出到标准输出；日志始终写入 stderr
capture-cli record --display 0 --stdout | ffplay -f h264 -
```

可选参数：

- `--fps N`：帧率，默认 30
- `--bitrate BPS`：平均码率
- `--width N --height N`：输出尺寸，必须为偶数
- `--duration SECONDS`：自动停止；未指定时使用 Ctrl-C
- `--no-cursor`：不捕获鼠标指针

`.h264`/`.264` 输出是包含 SPS/PPS 的 Annex-B H.264，便于用 `ffplay`、`ffprobe` 或后续 AirPlay 发送端读取；`.mp4` 输出使用系统 `AVAssetWriter` 封装 H.264。当前版本只处理视频，不处理音频，也不接触 ESP32 固件。
