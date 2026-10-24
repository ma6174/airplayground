# 运维手册

日常操作的全部实操细节：编译、烧写、看日志、配对，以及出问题之后怎么救。
协议本身见 [protocol.md](protocol.md)（走读）与 [protocol-spec.md](protocol-spec.md)（规格全文）。

## 硬件与串口

两块板共用同一套固件，烧写/看日志的方式不同：

**ESP32-S3-WROOM-1**（`esp32s3`，16 MB flash）

- 两个 USB 口都是 **USB-Serial/JTAG**（设备名形如 `/dev/cu.usbmodem…`，每台主机、每个口
  都不一样，用 `ls /dev/cu.usbmodem*` 现查）。flash 和日志走哪个都行。
- 烧完会停在 download 模式，要借 esptool 发一次复位 app 才跑（见下文）。

**JC4880P443C_I_W**（`esp32p4`，16 MB flash / 32 MB PSRAM，板载 ESP32-C6）

- 烧写/日志口是 **USB2** 口（板丝印 USB1/USB2 分开），USB1 是 HS OTG，不是控制台。
- 日志口和烧写口是同一个（USB-Serial/JTAG），烧完自己复位跑起来，不需要额外复位命令。
- 芯片是 **pre-rev3**（revision v1.3），依赖 `sdkconfig.defaults.esp32p4` 里的
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` / `CONFIG_ESP32P4_REV_MIN_100`；配错不会起机。
- 这块板没有 P4 的射频：Wi-Fi 全走 SDIO 到 C6（ESP-Hosted），细节见下文「P4 的无线」。

两块板共用同一个仓库，但**上板操作要分开排队**：`ESP32_BOARD=board`（默认，S3）与
`ESP32_BOARD=p4` 是两把独立的锁（`tools/esp32lock`）。

两块板可以同时插在一台机器上，所以每次刷写前先确定目标（见下一节）。

- **控制台只能读不能写**：USB-Serial/JTAG 口上主机写入的字节到不了固件的 `STDIN`
  （设备侧 `read()` 一直 `EAGAIN`；UART0 口同理）。所以配对码不要走串口输入，
  用下面的 HTTP 投递通道。
- 烧写时会独占串口：常驻的日志桥接要先停掉，烧完再起新的（见下文）。
- 烧写/日志/配对是三种不同的线序需求，换线前先看本文对应小节，不要凭感觉插拔。

## 刷写前：确认刷的是哪块板

两块板的串口描述符一模一样（都是 `USB JTAG/serial debug unit`，VID 都是 `0x303a`），
`/dev/cu.usbmodem*` 的名字也会漂移（见过 `1101` / `11101` / `11201`），所以**不能看口名、
也不能看 USB 产品名**。唯一稳定的标识是 **USB 序列号 = 芯片 MAC**：

| 板子 | 芯片 | USB 序列号（= 芯片 MAC） | 目标名 |
|---|---|---|---|
| JC4880P443C_I_W | ESP32-P4 | `80:F1:B2:D3:54:66` | `p4` |
| ESP32-S3-WROOM-1 | ESP32-S3 | `24:58:7C:CE:38:7C` | `s3` |

按序列号解析端口（非破坏性，不打开串口、不复位）：

```sh
tools/boardport list     # 列出当前插着的全部 Espressif 设备：板名 / 端口 / 序列号
tools/boardport p4       # 只打印 P4 的端口；没插就退出码 1，方便脚本直接 abort
tools/boardport s3
```

刷写时把端口求值给 `idf.py`，不要写死：

```sh
PORT=$(tools/boardport p4) && idf.py -p "$PORT" flash
```

### 如果 firmware 还是未知的（刚拿到的板、烧坏了的板）

上面的序列号表是本仓库两块板的映射；遇到陌生设备，用下面两条交叉确认，**都在板锁里做**
（`chip-id` 会复位板子，抢占正在跑的任务）：

```sh
# 1) 它是哪颗芯片（最硬的证据）
ESP32_BOARD=p4 tools/esp32lock exec 120 -- \
  python -m esptool --port "$(tools/boardport p4)" --before default-reset chip-id

# 2) 烧完的启动横幅也会自报型号与 MAC
#    I app: ==== AirPlay sender on esp32p4 ====
#    I app: mac=80:f1:b2:d3:54:66
```

注意 `--before no-reset chip-id` 对**正在跑 app** 的板子是连不上的（不是故障，是 app 占了
USB-JTAG）；要读芯片就得允许它复位。日志里的 `==== AirPlay sender on <目标> ====`
与 `mac=` 一行是判断“刚才刷的是哪块板”的最后一道核对。


## 编译

```sh
cd firmware && . ~/esp/esp-idf/export.sh && idf.py build
```

- 两个目标：`esp32s3`（默认那块 S3 板）和 `esp32p4`（JC4880P443C_I_W）。
- 用 `idf.py set-target <目标>` 切换；公共配置在 `sdkconfig.defaults`，目标专属的
  在 `sdkconfig.defaults.<目标>`（PSRAM 模式/速率、无线、芯片步进）。改过它们后要
  重新 `set-target`（或删掉 `sdkconfig`），否则旧 `sdkconfig` 会把新值挡掉。
- ESP-IDF 6.1（mbedtls 4.1，纯 PSA API）。网络芯片相关的两个托管组件只在 P4 上拉：
  `espressif/esp_hosted`（见 `main/idf_component.yml` 的 target 规则）。
- host 测试不需要 IDF，改了组件先跑这个再上板：

```sh
cd firmware/components/fairplay/test && make
cd firmware/components/hap/test && make
cd firmware/components/airplay/test && ./run.sh
cd firmware/components/media/test && ./run.sh
```

## 烧写、复位、看日志

```sh
idf.py -p /dev/cu.usbmodem… flash
# 仅 S3：烧完停在 download 模式，app 不会自己跑，复位一下：
python -m esptool --port /dev/cu.usbmodem… --after watchdog-reset chip-id
```

P4 烧完会自己复位进 app，不需要第二条；它的 bootloader 只剩约 300 字节余量，改分区表或
再开 bootloader 选项前先看 `idf.py build` 结尾的 `Bootloader binary size` 警告。

`chip-id` 只是借这条命令发复位，不用换线、不用拔插。注意先停掉占着串口的日志桥接，
复位成功后再启动（桥接开着 esptool 打不开串口）。

看日志用 `idf.py -p /dev/cu.usbmodem… monitor`，或常驻的桥接脚本
（日志落文件，方便 grep；FIFO 入参用于带真实 UART 控制台的板子，本板用不上）：

```sh
~/.espressif/python_env/*/bin/python firmware/tools/pin_bridge.py /dev/cu.usbmodem… /tmp/pin_fifo > /tmp/bridge.log 2>&1 &
tail -f /tmp/bridge.log        # 日志里偶有二进制字节，grep 记得加 -a
```

**每次启动必须看到** `[fairplay] selftest: 474 checks, 0 failures`。
这是 FairPlay 移植是否正确的唯一快速判据，看不到它别往下走。

## 配置

Wi-Fi、目标接收端等凭据放在 `firmware/main/app_config_local.h`
（已 gitignore，模板见 `main/app_config_local.h.example`）：

| 宏 | 说明 |
|---|---|
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | Wi-Fi；留空则 fallback 到设备热点（见 README） |
| `APP_AIRPLAY_TARGET_HOST` / `APP_AIRPLAY_TARGET_PORT` | 默认接收端地址（macOS 7000，参考接收端可自定义）；运行时可用 `/start?name=` 或 `?host=` 临时换 |
| `APP_FORCE_PIN_PAIRING` | **macOS 接收器必须置 1**，见下文 |
| `APP_PAIRING_PIN` | 编译期写死配对码（一般不用） |
| `APP_PIN_FETCH_HOST` / `APP_PIN_FETCH_PORT` | 设备主动来拉的 HTTP 服务（投递配对码用） |

设备身份（deviceID/UUID）存在 NVS，第一次开机生成；`idf.py erase-flash` 会清掉，
清掉后接收端会把它当新设备，需要重新配对一次。

## 首次配对（要求配对码的接收端）

前置：接收端已打开（macOS：系统设置 → 通用 → 隔空播放接收器 → 允许任何人），
主机与设备在同一网段（网络有隔离时让设备开热点、主机直连）。

1. 确认 `APP_FORCE_PIN_PAIRING=1`。macOS 接收器用 transient（HKP 4）会先失败一次，
   而那次失败会触发惩罚（见下一节），所以一开始就走 PIN。
2. 起配对码投递服务：

   ```sh
   mkdir -p /tmp/pin_serve
   python3 -m http.server 7001 --bind <主机IP> --directory /tmp/pin_serve
   ```

3. 复位设备后调 `curl http://<设备IP>:8080/start` 触发连接（固件平时空闲，不会自己连），
   等日志出现 `waiting for the pairing code`，同时接收端屏幕会显示 4 位码。
4. **一分钟内**把码投进去（设备每 2 秒来拉一次）：

   ```sh
   printf '2574' > /tmp/pin_serve/pin
   ```

5. 等日志走到 `pair-setup complete` → FairPlay 握手 → 三段 SETUP → `frame 0 sent`。
6. 成功一次之后，接收端记住了这台设备：以后每次只需 `curl /start`，
   pair-verify 直接过（实测 1 秒开流），**不再要码**。

## 「AirPlay server 限制」：失败惩罚与解除

- **症状**：`/pair-setup` 响应里带 `RetryDelay`（实测 1200~2000 秒）；
  `/pair-pin-start` 直接回 400；换正确的码也没用。
- **原因**：HomeKit 对连续配对失败有惩罚计数。
- **解除**：`killall ControlCenter`。关开「隔空播放接收器」开关**无效**。
- **纪律**：一次配对一次对。投递通道先起好、看到屏幕码再写、写完盯日志；
  不确定码对不对就等下一轮，不要连续重试把惩罚拉满。

## 会话控制接口

设备平时空闲；投屏会话由 HTTP 接口控制（设备 `8080` 端口，GET/POST 均可）：

| 接口 | 作用 |
|---|---|
| `GET /targets` | 列出已知接收端（`_airplay._tcp` + `_raop._tcp`，即时返回；后台每 30 s 刷新；AirTunes 老设备带 `[legacy]` 标记） |
| `GET /start` | 开始一次投屏会话；正在投则回 `already mirroring`。可选 query 参数：`?name=` 按名字选接收端（大小写不敏感精确匹配，其次唯一子串匹配，含空格用 `%20`），`?host=`/`?port=` 按地址选；都不带则用编译配置的默认接收端 |
| `GET /forget?name=` | 忘掉指定名字的接收端（含 NVS 里的持久记录） |
| `GET /stop` | 礼貌结束会话（TEARDOWN）；空闲时回 `not mirroring` |
| `GET /status` | `mirroring` 或最近一次会话的结束原因 |
| `GET /` | 配对码输入页 |
| `POST /pin` | 提交互配对码（`code=1234`） |

名字没匹配到时，响应里会带上网内实际发现的接收端名单，方便直接复制重试。

### 只广播型老设备（极米这类）

实测极米：**不理任何查询**（组播/单播 QM/QU 都不应答），但每 ~4 秒向全网持续广播全套记录（`_airplay._tcp` + `_raop._tcp`，含 PTR/SRV/TXT/A/NSEC）。当前实测：**它的广播包到不了 ESP32**（Mac 能听到；把原始包从 Mac 重放后 ESP32 完整解析入库）——纯 AP/路由器组播转发问题（疑 IGMP snooping/双频隔离），不是固件问题：

- 路由器侧修复：关「IGMP Snooping/组播过滤」或开「组播转单播（IGMP Enhancement）」，或把 ESP32 与投影仪放同一频段/AP；
- 固件侧兜底：`/start?host=&port=` 手动指定；接收到广播后 legacy 条目永久保存（NVS 断电不丢），名字变了或设备报废用 `/forget` 清掉；地址变了自动跟随（条目按名字为键）；
- `[legacy]` 标记来自 TXT（`model=AppleTV3,2` 或 `srcvers=220.x`），legacy 会话路径据此路由；
- Wi-Fi 省电已关（`WIFI_PS_NONE`）：省电模式会丢组播，这是另一类常见原因。

配对等待、SETUP 阶段、送帧循环里都会响应 `/stop`，停止时向接收端发 TEARDOWN。

## 配对类型速查（X-Apple-HKP）

| 值 | 场景 | M5 内容 |
|---|---|---|
| 4 | transient，无码配对 | 仅身份 |
| 5 | 码配对，Apple TV | 身份 + `com.apple.ScreenCapture` ACL |
| 3 | 码配对，接收端 features bit27（legacy pairing）置位 | 仅身份，**不带 ACL** |

macOS 的「隔空播放接收器」bit27 置位，必须用 3；用 5+ACL 会在 M5 收到
**HTTP 470**。固件按 `/info` 的 features 自动选，不需要手工配置。

## 换投的图

默认画面是片上软编码的动画测试图案（`esp_h264`/OpenH264，320x180@10fps），
不需要任何工具。两种情况下才会用到静态帧：

- 板子内存不够起编码器（启动日志会有 `live video unavailable`），固件自动回退；
- 想要清晰的大分辨率画面（720p 文字）：用 `APP_USE_LIVE_VIDEO=0` 切回静态帧。

```sh
python3 firmware/tools/make_frames.py 图1.png 图2.png 图3.png
```

生成 720p H.264 Baseline IDR 帧（每帧 18~27 KB），替换 `firmware/components/media/src/frames.c`
后重编。设备端只回放字节，编码在主机完成。

## P4 的无线（ESP-Hosted）

P4 没有射频，`esp_wifi_*` 在 IDF 6.1 里由 `esp_wifi/remote` 转成 RPC，经 SDIO 发给
板载 C6（后端就是 `espressif/esp_hosted`）。应用代码照旧调 `esp_wifi_*`，`wifi_net.c`
两个目标一模一样；只有 kconfig 名字不同（P4 上是 `CONFIG_WIFI_RMT_*`）。

- SDIO 用 slot 1 的默认引脚：CLK 18 / CMD 19 / D0-D3 14-17，从机复位 GPIO 54。
- `esp_wifi_set_ps(WIFI_PS_NONE)` 通过 RPC 作用在 C6 上，所以 mDNS 组播在两个目标上
  行为一致。
- C6 从机固件可能比主机侧旧。启动日志里会看到：

  ```
  esp-hosted fw versions: host=3.0.10 coprocessor=0.0.0
  E: major version mismatch - OTA coprocessor from host
  ```

  实测 Wi-Fi 照常工作（连接、DHCP、mDNS 都正常）。要让两边版本对齐，需要给 C6 烧同一
  版本的 slave 固件；出厂包在开发资料 `8-Bin Burn operation/Burn files/JC4880P443_C6.bin`。
- Hosted 的建链日志（`eh_sdio` / `eh_init_evt`）是判断无线是否起来的根据：看到
  `slave chip id: 0x0d (esp32c6)` 和 `sta ip: …` 就说明 SDIO 和 RPC 都通了。

## 会话任务的栈深度

openh264 的 slice / 码率控制路径嵌套较深：`session` 任务 8 KB 栈会在首帧编码时触发
`Stack protection fault`（`Detected in task "session"`），在 RISC-V 上尤其明显。
现在是 16 KB。以后要把编码器搬到单独任务里，栈不能低于这个量级。

## 故障速查

| 症状 | 原因 | 处理 |
|---|---|---|
| M5 回 HTTP 470 | 配对类型/ACL 不对 | 确认走 HKP 3 且不带 ACL（固件自动选，检查有没有手改 `hkp`） |
| `/pair-pin-start` 回 400 | 被惩罚限流 | `killall ControlCenter`，按纪律重来 |
| M4 认证失败 | 码过期（约 1 分钟）或输错 | 重开一轮，出码后立刻投 |
| 日志一直 `fetch stage: response carried no 4-digit code` | pin 文件没写 / HTTP 服务不对 | 检查 `http.server` 与 `/tmp/pin_serve/pin` |
| 烧完没日志 | 停在 download 模式 | `esptool --after watchdog-reset chip-id` 复位 |
| 启动没有 `selftest: 474 checks, 0 failures` | FairPlay 移植被改坏 | 先跑 `components/fairplay/test`，对照 [fairplay-reference](fairplay-reference/) |
| 配对中看门狗复位 | SRP-6a 一轮约 8 秒，饿死 idle | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`（已设，别改小） |
| 阻塞网络调用永远不返回 | `SO_RCVTIMEO` 未生效 | `CONFIG_LWIP_SO_RCVTIMEO=y`（已设）；阻塞调用放独立任务 |
| `LoadProhibited` panic | 请求头带了 NULL value 之类 | 看 panic 行；`ap_client_build` 现在会跳过空 header |

## 文档索引

- [protocol.md](protocol.md) — 会话时序走读（实现视角）
- [protocol-spec.md](protocol-spec.md) — 发送端协议规格全文，逐条标注参考实现出处
- [fairplay-spec.md](fairplay-spec.md) — FairPlay SAP 移植规格（算法、常量、向量）
- [fairplay-port-report.md](fairplay-port-report.md) — mbedtls 4.1/PSA 适配验证记录
- [fairplay-reference/](fairplay-reference/) — 独立可编译的 C 参考实现 + 474 组向量（`cc -O2 && ./a.out` 应输出 `ALL PASS`）
