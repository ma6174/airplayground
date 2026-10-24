# AirPlay 镜像发送端（ESP32-S3 / ESP32-P4）

AirPlay **屏幕镜像发送端**固件：设备自己完成 HAP 配对、FairPlay SAP 密钥交换和 SETUP
协商，然后把 H.264 视频流通过镜像数据通道投到接收端上。画面可以来自三种源：
Mac 屏幕采集（`macos/capture-cli` 推流）、片上软编码的动态测试图案，或预编码静态帧
—— 按此优先级选择，不可用的就退到下一档。

两个目标：

| 目标 | 板子 | 无线 | 片上编码 |
|---|---|---|---|
| `esp32s3` | ESP32-S3-WROOM-1，16 MB flash | 片上 Wi-Fi | OpenH264 软编，320x180@10 是现实上限 |
| `esp32p4` | JC4880P443C_I_W（ESP32-P4 + 板载 ESP32-C6），16 MB flash / 32 MB PSRAM | 经 SDIO 走 ESP-Hosted 到 C6 | 同上（P4 的硬编尚未接入） |

协议栈（`fairplay` / `hap` / `airplay`）与芯片无关，两个目标共用同一份源码。

## 视频源

ESP32-S3 没有硬件 H.264 编码器，官方 `esp_h264` 软编（OpenH264）在 S3 上能跑
320x180@10fps 左右，这就是片上编码的现实上限（P4 有硬编，固件目前仍用同一份软编路径）。
会话层不关心画面从哪来，只认两种字节流形态：

- **live 源**（`airplay_live_source_t`）：实现 `codec_record` + `next_frame` 两个回调，
  会话每次取一帧。已经有两份实现：
  - `media_net`（网络源）：接收 macOS 侧 `capture-cli` 用 ScreenCaptureKit +
    VideoToolbox 编好的访问单元，芯片只做加密和转发，因此可以投 960x682 甚至更高的
    真实屏幕内容；
  - `media_live`（片上编码）：内置动画测试图案 → OpenH264 → Annex-B → AVCC 重封装。
- **静态帧源**（`airplay_frames_t`）：回放编译期用 x264 编好、烧在 flash 里的访问单元
  （`tools/make_frames.py`），720p、字迹清晰，不占运行内存。

选择顺序：有 `capture-cli` 连上来并发了配置 → 网络源；否则片上编码可用 → 测试图案；
否则静态帧。每次 `/start` 都重新选，`/status` 前的日志会打印 `video source: ...`。

每个新会话开始时 live 源重建编码器：接收端拿到的第一帧必定是带新 SPS/PPS 的 IDR，
无论设备之前闲置多久。网络源没有自己的编码器，它在会话开始时跳过队列直到下一个 IDR
（采集端最长 5 秒内会强制一帧），所以接收端永远不会拿一个无法解码的 P 帧开场。

## 目录结构

```
firmware/
├── main/                  应用层：Wi-Fi、身份持久化、启动自检、会话重试
├── components/
│   ├── fairplay/          FairPlay SAP 握手（SAP hash、白盒表、ekey 封装）
│   ├── hap/               HAP 配对（SRP-6a、X25519/Ed25519、ChaCha20-Poly1305 分帧）
│   ├── airplay/           RTSP 控制通道、binary plist、SETUP、视频数据通道
│   └── media/             视频源：网络采集流、片上软编码（esp_h264）、预编码静态帧
├── media-src/             静态帧的源图
├── tools/make_frames.py   源图 → H.264 访问单元 → frames.c
├── tools/pin_bridge.py    常驻串口桥：日志落文件（FIFO 写入仅对真实 UART 板有意义）
├── partitions.csv         3 MB 应用分区
├── sdkconfig.defaults     两个目标共用的配置
├── sdkconfig.defaults.esp32s3 / .esp32p4   目标专属配置（PSRAM、无线、芯片步进）
docs/                      协议与 FairPlay 规格、C 参考实现、运维手册（见下文「文档」）
macos/capture-cli/         Mac 采集端：ScreenCaptureKit + VideoToolbox → TCP 推流给设备
```

## 快速开始

### 1. 配置（不提交到 git）

```sh
cd firmware
cp main/app_config_local.h.example main/app_config_local.h
$EDITOR main/app_config_local.h      # Wi-Fi 账号密码；接收端地址是默认目标，
                                     # 也可不填、用 /targets 查名字后 /start?name= 连
```

`app_config_local.h` 已在 `.gitignore` 里。可选配置项：`APP_DEVICE_NAME`（电视上显示的名字）、
`APP_FRAME_INTERVAL_MS`（静态帧每张停留时间，默认 10 s）、`APP_PAIRING_PIN`（电视要求配对码时）、
`APP_USE_LIVE_VIDEO=0`（关掉片上编码，固定用静态帧）、
`APP_USE_NETWORK_VIDEO=0`（不接受 Mac 采集流）、`APP_VIDEO_INPUT_PORT`（采集流端口，默认 8090）。

### 2. 编译与烧写

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32s3      # 或 esp32p4
idf.py build
PORT=$(../tools/boardport p4)      # 按芯片 MAC 解析端口，口名漂移也不怕
idf.py -p "$PORT" flash
```

**端口不要写死**：两块板可以同时插在一台机器上，USB 描述符一模一样，`/dev/cu.usbmodem*`
名字也会漂。`tools/boardport` 按 USB 序列号（= 芯片 MAC）定位：`tools/boardport list` 看
当前插着哪几块、各自是什么型号，`tools/boardport p4|s3` 只输出对应端口（没插则退出码 1）。
识别规则与交叉核对方式见 [docs/runbook.md](docs/runbook.md#刷写前确认刷的是哪块板)。

S3 用原生 USB 口烧完会停在 download 模式，要借一条 esptool 命令发复位（不用换线、
不用拔插）；P4 烧完自己复位跑起来：

```sh
# 仅 S3：
python -m esptool --port /dev/cu.usbmodem… --after watchdog-reset chip-id
```

`set-target` 会读 `sdkconfig.defaults` 加上 `sdkconfig.defaults.<目标>`；改过这两个文件
之后重新 `set-target`（或删掉 `sdkconfig`）才会生效。

S3 板的两个 USB 口都是 USB-Serial/JTAG，日志两个口都能看；但**这台板的控制台只能读不能写**
（主机写入的字节到不了固件 STDIN），所以配对码不走串口输入，走 HTTP 投递 ——
完整步骤见 [docs/runbook.md](docs/runbook.md)。P4 板同理。

### 3. 运行

上电后固件：FairPlay 自检 → 连 Wi-Fi → 起 mDNS 发现与控制页面，然后**空闲待命，不主动连接**。
投屏由设备自带的 HTTP 接口触发（GET/POST 均可）：

```sh
curl http://<设备IP>:8080/targets   # 列出局域网接收端（mDNS，约 3 秒；老设备带 [legacy] 标记）
curl http://<设备IP>:8080/start    # 连默认接收端（编译配置 APP_AIRPLAY_TARGET_HOST）
curl 'http://<设备IP>:8080/start?name=Living%20Room'  # 按名字连（大小写不敏感，唯一子串也可）
curl http://<设备IP>:8080/start?host=192.168.1.35     # 按地址连，临时覆盖默认目标
curl http://<设备IP>:8080/stop     # 礼貌断开（向接收端发 TEARDOWN）
curl http://<设备IP>:8080/status   # idle / mirroring / 上次会话的结束原因
```

配对码的三条投递通道见 [docs/runbook.md](docs/runbook.md)。

### 4. 投 Mac 的屏幕或某个窗口

`macos/capture-cli` 用 ScreenCaptureKit 抓画面、VideoToolbox 编 H.264，然后把访问单元
推给设备（设备只做加密转发，不重新编码，所以能投真实桌面）：

```sh
cd macos/capture-cli && ./build.sh
./capture-cli list                          # 找显示器索引和窗口 id
./capture-cli record --window-id 961 --fps 20 --bitrate 2000000 \
    --tcp <设备IP>:8090                     # 投单个窗口
./capture-cli record --display 0 --width 960 --height 600 --fps 20 \
    --tcp <设备IP>:8090                     # 投整块屏幕
```

先起采集流，再 `curl http://<设备IP>:8080/start?name=<接收端>`；设备日志会出现
`capture configured: 960x682 @ 20 fps` 和 `video source: capture stream`。
采集端每秒打印一行 `sent N frame(s)`，设备端每秒打印一行
`frame N sent: ... waited=Xms, sent in Yms` —— `sent in` 是加密加发送的耗时
（实测 4–6 ms/帧），`waited` 是等下一帧的时间。

网络源的协议极其简单（一个配置包 + 每帧一个长度前缀包，帧内仍为 AVCC），
写在 `firmware/components/media/src/media_net.c` 顶部；客户端实现见
`macos/capture-cli` 里的 `NetworkSink`。

电视上看到的内容：网络源下就是 Mac 的屏幕/窗口本身；片上编码下是持续变化的彩色条纹动画；
静态帧源下是 `ESP32-S3 / AirPlay mirroring`、`FairPlay OK` 等预编码画面，每 10 秒切换一张。
启动日志会打印实际选择了哪个源（`video source: ...`）。
## 没有 Wi-Fi 密码时：让设备自己开热点

`APP_WIFI_SSID` 留空时，设备不连外部网络，而是起一个热点
（默认 `airplay-esp32` / `airplay1234`，设备地址 `192.168.4.1`）。
主机连上这个热点后，可以在 `192.168.4.2` 跑仓库里的 Go 测试接收端做整链路台架验证：

```sh
go run ./cmd/doubletake-test-receiver -listen 0.0.0.0:7000 -profile modern -stats-interval 5s
```

（在 doubletake 参考实现的检出目录里执行。）然后把 `APP_AIRPLAY_TARGET_HOST` 设为
`192.168.4.2`。这条路径不需要 Apple TV，也不占用你家网络的密码。

## 协议做了什么

一次镜像会话的完整顺序（细节见 `docs/protocol.md`）：

```
TCP → GET /info                      读 features / statusFlags / 显示尺寸
     POST /pair-setup   M1..M6        SRP-6a（3072-bit, SHA-512）；电视要求配对码时走 PIN
     POST /pair-verify  V1..V4        X25519 + Ed25519 → 之后控制通道 ChaCha20-Poly1305 分帧
     POST /fp-setup     m1/m2/m3/m4   FairPlay SAP 白盒交换 + ekey 封装
     SETUP  (session)                 timing + 会话描述
     SETUP  (audio)                   type 96，占位描述（不发音频包）
     SETUP  (video)                   type 110 → 拿到 dataPort，附 shk/shiv
TCP → dataPort                        128 字节头 + AVCC 帧（ChaCha20-Poly1305，AAD=头部）
     每 1 s 心跳 / 每 2 s POST /feedback / 事件通道回 200
```

## FairPlay 自检

`fp_selftest()` 在每次启动时重算所有已发布的测试向量：SAP hash（65 组）、三种 MD5 变异、
4 种消息密码模式、descriptor/exchange、wrapping key、以及完整的 m1→m3→ekey。
串口里看到

```
I (xxx) fairplay: selftest: 474 checks, 0 failures
```

才算 FairPlay 移植正确；任何一条失败都会指明具体用例。这套向量也可以在主机上直接跑：

```sh
cd firmware/components/fairplay/test && make && ./fairplay_test
```

## 已知限制

- **只实现 HAP（TLV8）配对**：最早的那套裸二进制配对（68/96 字节交换、控制通道不加密）和
  AES-CTR 视频加密没有实现。现代 Apple TV 与 macOS 接收器走的都是 HAP。
  注意 `X-Apple-HKP: 3` 不是"裸配对"：它仍是 HAP TLV8，只是配对类型不同
  （4/5/3 的区分见 [docs/protocol.md](docs/protocol.md) 的 Pairing types）。
- **没有音频**：SETUP 里给了音频描述（含 32 字节音频流密钥占位），但不发音频包。
- **接收端地址靠 mDNS 或手动指定**：`/targets` 列出局域网上的 `_airplay._tcp`，
  `/start?name=` / `?host=` 选定目标；也可以把地址写进配置当默认值。
- **真实画面靠 Mac 采集**：片上 OpenH264 只能跑 320x180@10fps 的测试图案，所以投真实
  桌面/窗口由 `macos/capture-cli` 编码后推给设备（网络源）。设备侧实测加密+发送
  4–6 ms/普通帧、约 260 ms/满屏文字的关键帧（镜像通道实际带宽约 4 Mbps），所以帧率只受
  采集端限制：ScreenCaptureKit 对静止画面会主动降帧（静窗口约 12–16 fps，内容持续变化时更高）。
  采集端需要“屏幕与系统音频录制”授权；重建二进制后该授权会失效，需重新勾选。
- **单帧上限 512 KB**：文字密集的窗口（终端里跑 top/htop）关键帧能达到一百多 KB，
  超过上限的帧会被设备丢掉而不是断开连接；采集端同时把关键帧间隔放宽到 5 秒，
  避免每秒一次的大帧顿挫。投这类窗口建议先降分辨率（`--width 960 --height 664`）。
- **一次只接一个采集客户端**：8090 端口接受一个连接，断开后固件自动重新监听；
  没有客户端时回退到片上测试图案。
- **配对要等**：SRP-6a 的 3072 位模幂在 S3 上一轮约 8 秒（期间 CPU 打满，看门狗超时已调到 30 秒）。
  接收端接受过这台设备后，后续会话走 pair-verify，不再重跑 SRP。
- **配对码有三条输入路径**：本机 HTTP 页面（`http://<device>:8080/`）、串口（仅对带真实
  UART 控制台的板子有效，本板是 USB-Serial/JTAG 只读）、以及 `APP_PIN_FETCH_HOST/PORT`
  指向的"设备去主机拉码"钩子。也可以直接把码编译进 `APP_PAIRING_PIN`。
  本板实测可用的是 HTTP 页面与拉码钩子。
- **接收端限流要遵守**：配对失败会被 HomeKit 退避（最长可达 30 分钟），重试过早只会
  延长惩罚；固件会读 `0x08` 重试延迟并等满（`airplay_backoff_ms()`）。
- **要求配对码的接收端要设 `APP_FORCE_PIN_PAIRING=1`**：先试 transient 会失败，而那次
  失败本身会触发限流，把携带正确码的尝试挡在门外。配对成功一次之后接收端会记住这台设备，
  以后每次开机直接 pair-verify（实测 1 秒内开流，不再需要码）。

## 调试

- 日志同时出现在两个 USB 口（都是 USB-Serial/JTAG），`idf.py monitor` 或
  `firmware/tools/pin_bridge.py` 都能读；控制台写入无效（见「快速开始」）。
- S3 用原生 USB 口烧完会停在 download 模式，需要复位才会跑起来。不换线也能复位：

  ```sh
  python -m esptool --port /dev/cu.usbmodem1101 --after watchdog-reset chip-id
  ```

- 没有 Apple TV 时，可以用参考实现的 Go 测试接收端做台架验证（任意 Linux/macOS 主机）：

  ```sh
  go run ./cmd/doubletake-test-receiver -listen 0.0.0.0:7010 -profile modern -stats-interval 5s -debug
  ```

  `-profile modern` 是「HAP 配对 + PTP 时基 + streamConnections 音频描述」的现代接收端形态，
  和本固件选择的路径一致；`video=N/…B` 计数增长说明帧已收到并解密。

更多实操细节（两个 USB 口的分工、配对投码的完整流程、接收端限流的解除、故障速查表）
都在 [docs/runbook.md](docs/runbook.md)。

## 文档

- [docs/runbook.md](docs/runbook.md) — **实操手册**：编译、烧写、复位、看日志、配对投码、限流解除、故障速查
- [docs/protocol.md](docs/protocol.md) — 会话时序与报文走读（实现视角）
- [docs/protocol-spec.md](docs/protocol-spec.md) — 发送端协议规格全文，逐条标注参考实现出处
- [docs/fairplay-spec.md](docs/fairplay-spec.md) — FairPlay SAP 移植规格（算法、常量、向量）
- [docs/fairplay-port-report.md](docs/fairplay-port-report.md) — mbedtls 4.1/PSA 适配验证记录
- [docs/fairplay-reference/](docs/fairplay-reference/) — 独立可编译的 C 参考实现 + 474 组向量
  （`cc -O2 fairplay_ref.c && ./a.out` 应输出 `ALL PASS`）

## 验证过的部分

- 设备上 `fp_selftest()`：474 项 FairPlay 向量，0 失败（每次启动都跑）。
- 与参考接收端（`-profile modern`）完成一整个会话：pair-setup → pair-verify → FairPlay →
  三段 SETUP → 数据通道 → RECORD → 逐帧发送 → 心跳/feedback/事件通道，接收端侧
  `fairplay=44 setup=40 video=…/…B`，零错误。
- 三套 host 测试（fairplay 474 项、hap 133 项、airplay plist/帧头/RTSP 解析）全绿。
- 与 macOS 自带「隔空播放接收器」联调通过：屏幕 4 位码配对（M1–M6）→ pair-verify →
  FairPlay → 三段 SETUP → 数据通道持续发送 720p 静帧，`/feedback` 每 2 秒被 200 应答。
  **关键细节**：macOS 的 features 里 bit 27（legacy pairing）是置位的，这类接收端要求
  `X-Apple-HKP: 3` 且 M5 **不能**带 `com.apple.ScreenCapture` ACL；用 `X-Apple-HKP: 5` + ACL
  会在 M5 收到 **HTTP 470（Connection Authorization Required）**。参考实现也是这么选的。
- macOS 接收器对配对失败会施加很长的退避：**连续失败后 `/pair-setup` 直接回 `RetryDelay`
  1200~2000 秒，连 `/pair-pin-start` 也会 400**；把「隔空播放接收器」关掉再打开**不会**清掉它，
  参考实现（doubletake，同一台机器上跑）同样被挡 —— 说明这是接收端按设备/来源记的策略，
  不是本固件独有。验证这类接收端要一次成功，试错成本是几十分钟。

## 路线图

1. 动态内容：接 `esp_h264` 软编（320×240）或换 ESP32-P4（硬编 1080p）后上实时画面
2. 音频：AAC-ELD / ALAC + 冗余包
3. mDNS 发现、多接收端、SRP 模幂加速（滑动窗口 / 硬件加速）
