# AirPlayground

**给一块没有屏幕的 ESP32 装上 AirPlay 发送端，于是客厅里的电视、投影仪、Mac 都成了 AI 的显示器。**

AirPlay 是苹果生态里最难「借」的一件东西：它不是开放标准，而是一套由 HAP 配对、FairPlay
SAP 白盒密钥交换、RTSP 会话协商、加密镜像数据通道拼起来的私有协议栈。常规做法只有两条
——买一颗 MFi 认证芯片，或者放弃。

这个仓库走了第三条路：在几十元的 MCU 上把**发送端**从头实现一遍，再把这份能力接到语音
助手（小智 / 灵矽）身上。今天它已经能把语音助手自己的界面、文字、图片、网页、一台电脑的
桌面、乃至它的声音，一起送到大屏上；也让助手反过来操作那台电脑。

---

## 实机演示

<p align="center">
  <video src="docs/assets/1024-airplayground.mp4" controls muted playsinline width="720"></video>
</p>

---

## 一、为什么值得做

### 1. 协议自主：发送端比接收端稀缺得多

市面上能见到的「AirPlay 盒子」几乎都是接收端——把手机画面收下来。发送端是另一件事：
它必须自己扮演 Apple 设备，完成配对、拿到合法会话密钥、按接收端的时钟与格式把码流喂过去。
任何一步错了，接收端不会报错，只会安静地显示黑屏。

本仓库把这套东西完整落地，且**不依赖任何苹果授权硬件**：FairPlay SAP 的白盒表与密钥派生、
HAP 的 SRP-6a/X25519/Ed25519/ChaCha20-Poly1305 分帧、现代与 legacy 两条镜像路径，全部是
纯软件实现，并且带 474 组金标向量在每次开机自检。

### 2. 设备价值：无屏设备获得了「显示器」

语音助手类硬件的天花板一直是那块一两寸的小屏。接上这里的能力之后，设备把整台电视当成自己的
主显示器——聊天界面、一段文字、一张图、一个网页、一台电脑的桌面，都能上大屏；助手说的话
（TTS）、放的歌、Mac 里的声音，也走同一条通道出去。设备硬件成本不变，人机界面的面积放大了
两个数量级。

### 3. 交互价值：语音 + 大屏，才谈得上「可玩性」

只有声音的助手是音箱，只有屏幕的电视是显示器。两者连起来之后才有这些场景：说一句「把屏幕
上这段话放到电视上」、说一句「画一只柴犬」（云端生成，十秒后出图）、说一句「把 B 站投到
电视上」、说一句「帮我在编辑器里把这段代码补完，然后把窗口投上去」。屏幕让助手的输出有了
空间，语音让大屏不再需要遥控器。

---

## 二、能力全景

| 能力 | 形态 | 落地位置 |
|---|---|---|
| **发送端：现代路径（AirPlay 2）** | HAP 配对 + FairPlay SAP + ChaCha20-Poly1305 镜像通道；对接 macOS 隔空播放接收器、Apple TV | `firmware/components/{fairplay,hap,airplay}` |
| **发送端：legacy 路径（AirPlay 1 / AirTunes）** | 裸 pairing + ekey 链 + AES-CTR 记录；对接极米等贴牌盒子、UxPlay | 同上（`ap_legacy.c`） |
| **视频源：本机界面** | 无屏板把小智的 LVGL 界面渲染进 PSRAM 全帧缓冲，等同于接了一块面板 | `main/display/virtual_display.*` |
| **视频源：文字 / 图片** | 整屏文字（大小自动、分页）、网图与 AI 生图（4:4:4 JPEG 硬解、等比黑边） | `airplay_service.cc`、`self.display.*` |
| **视频源：电脑桌面（推）** | macOS ScreenCaptureKit 采集 + VideoToolbox 编码 → 设备只加密转发，真实桌面 20 fps | `macos/capture-cli` |
| **视频源：任意机器（拉）** | 设备主动连过去拉帧流，重启后自己接回，适合 NAT 后/云上机器 | `media_net_pull`、`self.screen.cast_server` |
| **视频源：静态帧** | 编译期 x264 编码的 720p 静帧，烧在 flash 里，零内存成本 | `media_frames` |
| **音频（设备 → 大屏）** | 助手 TTS / 音乐 / 提示音，或 Mac 系统声音；24 kHz → 44.1 kHz 重采样，未压缩 ALAC 每包 352 样本 | `airplay_audio.cc` |
| **语音控制大屏** | 12 个投屏/显示 MCP 工具：发现接收端、开停投屏、进配对码、放文字/图片、网页、远程机器、照片协议 | `boards/jc4880-tv` |
| **设备「看」屏幕** | 无屏板实现了 Camera 接口：截自己的帧 → JPEG → 云端视觉 → 文字答案 | `airplay_screen_camera.*` |
| **操作电脑** | hs-mcp：16 个工具，开应用、输入中文、按快捷键、点菜单、看屏幕、投窗口、剪贴板、播报 | `tools/hs_mcp` |
| **台架 API** | 8080 端口：状态、发现、开关、配对码、截图、单帧 H.264、投图、视觉问答 | `airplay_http.cc` |

术语：下文把跑着小智固件、被语音唤醒的助手简称为「助手」（小智 / 灵矽）。

---

## 三、架构设计

### 3.1 全景

```
┌────────────────────────── 大屏（接收端）───────────────────────────┐
│ macOS「隔空播放接收器」 · Apple TV · 投影仪（AirTunes）· UxPlay    │
└──────────────────────────────▲─────────────────────────────────────┘
                               │ 控制：RTSP / HTTP（配对、SETUP、心跳）
                               │ 数据：镜像通道（视频 AVCC + 音频 ALAC）
┌──────────────────────────────┴─────────────────────────────────────┐
│ firmware/components  协议层：一套源码，两条路径                    │
│   fairplay  白盒交换、ekey 封装、474 组自检向量                    │
│   hap       SRP-6a / X25519 / Ed25519 / TLV8 / 加密分帧            │
│   airplay   mDNS 发现、会话编排、SETUP、镜像数据通道、legacy 记录头│
├────────────────────────────────────────────────────────────────────┤
│ firmware/components/media  内容层：会话只认 AVCC 访问单元          │
│   静态帧 · 片上编码图案/文字 · 网络推流(8090) · 拉流(pull)         │
├────────────────────────────────────────────────────────────────────┤
│ xiaozhi-esp32/main  设备整合层                                     │
│   VirtualDisplay 内存显示后端   AirplayService 会话与源互斥        │
│   AirplayAudioSink ALAC 回灌    AirplayScreenCamera 拍屏           │
│   MCP 工具 12 个                 台架 HTTP API                     │
├────────────────────────────────────────────────────────────────────┤
│ 主机侧                                                             │
│   capture-cli 采集编码   hs-mcp 16 个工具   file_stream 文件投屏   │
│   typein 文字输入   boardport 认板   esp32lock 板卡互斥锁          │
└────────────────────────────────────────────────────────────────────┘
```

两个硬件目标共用同一份协议源码：

| 目标 | 板子 | 无线 | 编码 |
|---|---|---|---|
| `esp32s3` | ESP32-S3-WROOM-1，16 MB flash | 片上 Wi-Fi | OpenH264 软编，320x180@10 为现实上限 |
| `esp32p4` | JC4880P443C_I_W（ESP32-P4 + 板载 C6），16 MB flash / 32 MB PSRAM | SDIO → ESP-Hosted | 软编同款；硬编与 JPEG 硬解已接入 |

### 3.2 协议层：同一个会话，两类接收端

接收端分成两个世代，报文格式完全不同。命名按**接收端世代**而不是按宣传口径：现代路径对接现行
的 Apple 接收端（通常所说的 AirPlay 2：HAP 配对 + FairPlay SAP + 加密镜像），legacy 路径对接
AirPlay 1 / AirTunes 时代的盒子（裸配对 + ekey 链 + AES-CTR 记录）。分流依据是
`airplay_config_t.legacy`：

| | 现代路径 | legacy 路径 |
|---|---|---|
| 配对 | HAP TLV8：SRP-6a（3072-bit）、X25519、Ed25519、4 位码 | 裸交换（32/68/96 字节），可选 |
| 密钥 | FairPlay SAP 白盒 + ekey 封装 | FairPlay ekey 链 + 三种密钥混合模式 |
| 会话 | RTSP：`/info` → SETUP(session/timing) → SETUP(audio) → SETUP(video, type 110) | RTSP + ANNOUNCE + 两段 SETUP，`Server: AirTunes/220.x` |
| 数据 | 128 字节头 + AVCC，ChaCha20-Poly1305（AAD=头部） | 128 字节记录头 + AVCC，AES-128-CTR |
| 音频 | 通道已声明，未发流 | type 96 流，ALAC spf=352，每 RTP 包一帧 |

分路不是猜的：mDNS TXT 的 `model`/`srcvers`、`/info` 的 features 位、以及端口拓扑
（有的盒子在被广播的端口上放一个只会回 `200 OK` 的 HTTP stub，真 RTSP 在下一个端口）都要
参与判断。`docs/legacy-spec.md` 把这些判别法逐条落在 RPiPlay 源码行号上。

### 3.3 内容层：会话不关心画面从哪来

协议层只接受两样东西：**一份 AVCC 解码配置记录**和**一串 AVCC 访问单元**。内容源因此可以
自由插拔，同一个会话任务喂什么就投什么：

```
                       ┌── 静态帧（flash 里的 x264 产物）
airplay_live_source_t ─┼── 片上编码：图案 / 文字 → OpenH264 → AVCC
                       ├── 网络推流：Mac 编码好 → 设备只加密转发
                       └── 拉流：设备主动连过去取
```

这条边界带来两个直接收益：一是**真实桌面不需要 MCU 编码**（Mac 编、设备转发，960x682@20fps
稳定），二是新内容源（网页帧流、摄像头、文件流）只需实现两个回调就能接进来。

内容源在同一时刻只允许一个，切换源会重建会话：接收端在 SETUP 那一刻就定下了流的形状，
运行中换源必须重新协商。这条规则由 `CaptureWatchTask` 统一执行，而不是每个入口各自小心。

### 3.4 设备整合层：无屏板如何变成「显示器」

- **`VirtualDisplay`**：继承上游的 `LvglDisplay`，把整屏 UI 渲染进 PSRAM 里的 RGB565
  全帧缓冲，不推任何面板。上游界面代码一行不改，`self.display.text` / `image` 只是在上面
  多盖一层内容。
- **`AirplayService`**：一个会话任务 + 一个网络任务 + 一个源看护任务。它持有 HAP 身份
  （NVS 持久化）、选择内容源、把「配对码待输入」「会话失败原因」翻译成助手能转述的短语。
- **`AirplayAudioSink`**：音频服务每播放一块 PCM 就先递给它。它重采样到 44.1 kHz、打包成
  ALAC、进约 128 ms 的环；环满时反压解码线程，节奏跟接收端的时钟而不是内存容量。环空就
  不发包（接收端自然静音），所以句子之间的停顿不花任何代价。
- **`AirplayScreenCamera`**：实现 Camera 接口，截自己的帧 → JPEG → 走云端视觉链路。
  助手的「看屏幕」因此不需要额外协议，复用上游现成的工具与鉴权。

文字与图片在两侧各有一套渲染器：独立固件走 `media_text`（固定字格、单行、超宽跑马灯，字库
编译进 flash 零 RAM 成本，可打印 ASCII + GB2312 一级字共 3944 字形）；小智整合走 LVGL 的
`VirtualDisplay`（比例字宽、自动换行与分页、可选文字色与背景色、图片按比例黑边）。

### 3.5 主机侧：让设备变「薄」的那一半

| 工具 | 作用 | 为什么放在主机 |
|---|---|---|
| `macos/capture-cli` | ScreenCaptureKit 采集 + VideoToolbox 编码 → TCP 8090 | MCU 编不动真实桌面，也不需要编 |
| `tools/hs_mcp` | 16 个 MCP 工具，把 Mac 交给助手操作 | 权限、IME、窗口管理都是操作系统的事 |
| `tools/file_stream.py` | 把一个视频文件（H.264 + ALAC）投上电视 | 任何有 python + ffmpeg 的机器都能当发送端 |
| `tools/typein` | 向指定应用输入文字（含中文、多行） | 免输入法干扰，走剪贴板粘贴 |
| `tools/boardport` | 按 USB 序列号（= 芯片 MAC）认板 | `/dev/cu.usbmodem*` 名字会漂 |
| `tools/esp32lock` | 多 agent 共享一块板的互斥锁 | 并发烧录会串台、坏镜像 |

### 3.6 几个关键决策

| 决策 | 理由 |
|---|---|
| 协议层只保留一份源码，小智工程用 `EXTRA_COMPONENT_DIRS` 引用 | 修复、host 测试、规格文档只有一处，升级上游时冲突面最小 |
| 会话只认 AVCC | 内容源可插拔，协议层与「画面从哪来」彻底解耦 |
| 现代/legacy 双路径由接收端指纹决定，不让用户选 | 用户只知道「投那台电视」，技术选型属于固件 |
| 无屏板用内存帧缓冲替代面板 | 上游 UI、工具、状态机全部复用，只换后端 |
| 一次一个画面源，切源重建会话 | SETUP 定流形状；同时喂两路只会两路都错 |
| 图片、投屏一律丢后台任务 | 云端一个语音回合只有十几秒，同步等生成结果必然超时 |
| 推流与拉流都支持 | 临时投一下用推；长期挂机、机器在 NAT 后或云上用拉 |
| MCP 工具的**描述**写清「哪种说法用哪个工具」 | 三块屏幕（设备自己 / 这台 Mac / 别的机器）语义相近，只能靠描述分流 |

---

## 四、成果

### 4.1 互操作矩阵

| 接收端 | 路径 | 结果 |
|---|---|---|
| macOS 系统自带「隔空播放接收器」 | 现代 | 4 位码配对 → pair-verify → FairPlay → 三段 SETUP → 持续出画，`/feedback` 每 2 秒被 200 应答 |
| Apple TV | 现代 | 共用同一条路径，配对用 HKP 5 + `com.apple.ScreenCapture` ACL |
| 极米投影仪（伪装 `AppleTV3,2`） | legacy | 出画 + 出声，断电恢复后重连正常 |
| UxPlay 1.62（Linux 主机） | legacy | 作为「会真的检查」的接收端验收：解密失败计数归零 |
| doubletake 参考接收端（Go） | 现代 | 整链路台架：pair-setup → 数据通道 → 逐帧解密计数增长 |

端到端验收主要在 macOS 接收器与极米投影仪上完成；Apple TV 与 macOS 共用同一条路径，差别只在
配对类型。

反方向也能跑通：`tools/file_stream.py` 把一个 mp4 拆成访问单元喂给设备，画面与声音一起上电视。
所以「谁能投屏」不限于 macOS——发送侧可以是 macOS（原生采集）或 Linux/任意主机（文件流、
可被拉流的帧源），接收侧可以是 macOS、Apple TV、Linux 上的 UxPlay 或贴牌投影仪。

### 4.2 实测数据

| 指标 | 值 |
|---|---|
| 加密 + 发送耗时 | 4–6 ms/普通帧（960x682@20fps，镜像通道约 4 Mbps） |
| 音频链路 | 44.1 kHz / 352 样本每包 / 约 128 ms 缓冲，实测 175 秒连续播放零停滞 |
| 拍照到文字回答 | 1.3 s（259 KB 帧 → 9 KB JPEG → 云端视觉） |
| 大屏出图 | 工具 0.87 s 应答，图片 10–40 s 后上屏（生成式图源的生成时间） |
| 固件体积 | 应用镜像约 4.15 MB，单个 OTA 槽 4.18 MB（16 MB flash：双 OTA + assets） |
| 配对首连 | SRP-6a 3072 位模幂在 S3 上约 8 秒；之后走 pair-verify，1 秒内开流 |

### 4.3 工程产出

| 项 | 数量 |
|---|---|
| 自研代码（不含 vendored 小智上游） | 约 3.7 万行（协议栈 2.0 万 / 内容层 6.6 千 / 设备整合 4.8 千 / 主机侧 3.7 千） |
| 规格与运维文档 | 4.3 千行，含 `protocol-spec`、`legacy-spec`、`fairplay-spec`、`runbook` |
| 已合并 PR | 28 个（第 1 至第 29 号，其中一个是依赖升级被关闭） |
| 提交 | 37 次，跨度 2026-10-23 16:18 → 2026-10-25 22:54 |
| 自动化检查 | FairPlay 474 项（开机自检 + host 测试）、HAP 133 项、airplay plist/帧头/客户端/legacy 四组、media 五组 |

---

## 五、AI：这个项目为什么能在三天里长出来

从第一行代码到「双协议栈 + 音频 + 语音助手全链路」，实际跨度不到三天（约 55 小时）。这不是
「AI 写代码比较快」那么简单，值得拆开看。

### 5.1 AI 创造的技术价值

AirPlay 发送端真正的难点不是代码量，而是**没有文档**：协议是私有的，行为只在别人的实现和
抓包里。这个项目把研究过程变成了三样东西，AI 才能接力：

1. **可执行规格**：`docs/protocol-spec.md`、`docs/legacy-spec.md`、`docs/fairplay-spec.md`
   逐条标注参考实现出处（连 RPiPlay 的行号都在），推断与实证分开标。规格本身就是任务分解。
2. **金标向量**：474 组 FairPlay 向量 + HAP 的 host 测试向量，全部由 `reference.py` 独立
   复算。谁写实现都能立刻知道自己错在哪一条，而不是对着黑屏猜。
3. **设备自检**：每次开机跑 `fp_selftest()`，474 项 0 失败才算移植正确。硬件在环的自检把
   「到底是我错了还是电视挑食」这个二义性消掉了。

有了这三样，「读懂一个陌生协议」这种过去以周计的工作，被压缩成了几天内的多次小步验证。

> 一个具体例子：极米投影仪长期黑屏而 UxPlay 完全正常。最后定位到 `char label[32]` 装不下
> 19 位 stream id 的标签（需要 35 字符），`snprintf` 静默截断 → 派生出的密钥「看着正常」
> 但每帧都解出噪声，接收端静默丢弃、界面无任何报错，只在 UxPlay 侧留下
> `decryption of video packet failed`。**用 UxPlay 当 oracle、数解密失败计数的增量**才把
> 密钥模型（裸 key / 混 pair-verify secret / 混全零）从三种候选里唯一确定下来——这类「只有
> 增量指标、没有报错」的问题，正适合 AI 反复试。

### 5.2 AI 工程效率：把人的注意力从重复劳动里挪走

大量的真实故障不是算法问题，而是**并发与硬件**问题。这些位置被工程化之后，AI 才能安全地
长时间跑：

- **`tools/esp32lock`——板卡互斥锁**。多个 agent 共享一块开发板时，并发烧录/复位会互相
  打断：镜像写坏、串口日志串台、上一个 agent 刚复位下一个就开始写。锁用 `flock`，生命周期
  严格等于持有它的进程（`exec` 模式下进程退出即释放，子命令不继承 fd，不可能留死锁），
  按板名分命名空间，超时未释放会被等待方按 `ESP32_LOCK_MAX_HOLD` 抢走。**这是「并行提效」
  的前提**：不是让多个 agent 抢同一块板，而是让它们排队而不互相污染。
- **`tools/boardport`——按 MAC 认板**。USB 描述符里带芯片 MAC，`/dev/cu.usbmodem*` 的名字
  会漂。写死端口的脚本迟早把固件刷到隔壁那块板上。
- **台架 HTTP API——让 agent 自证**。状态、发现、截图、单帧 H.264、投图、视觉问答都在
  8080 上。agent 不需要人帮忙「看一眼电视」，它能自己取回屏幕像素、自己判断这次改动是否
  生效。这是自动化闭环里最关键的一块。
- **无 IDF 的 host 测试**。协议层能在任何机器上 `make && ./run.sh` 跑完四组测试，不必等
  一次编译烧录；反馈周期从分钟级降到秒级。
- **一次一个 PR 的验收闭环**。每个改动都带「怎么证明它对」的方式（host 测试、台架端点、
  真机日志），评审时先看证据再看代码。
- **工具描述即提示词**。`self.airplay.start` / `self.screen.cast_server` / 那台 Mac 的
  `cast_mac_to_tv` 三块屏幕语义极近，模型只能靠工具描述里的「用户说的是哪一个」来分流；
  描述里也写清图片生成用哪个 URL、长文本要整段发（固件会分页）。提示词工程在这里落在固件的
  工具定义里，而不是聊天窗口里。
- **异步与超时**。语音回合有硬时限：生成一张图要 15–40 秒，同步等必然被云端判超时并关会话。
  所以图片工具「先说一句、后台出图」，投屏工具等到 `source` 真的切换才回答。这类约束一旦
  违反，症状是「用户说什么都没反应」，最难查。

### 5.3 人仍然在哪

AI 负责实现与验证，人负责**定义正确**：哪块屏幕该由哪个工具投、什么算「真的出画了」、
音频该让给画面还是留给助手、flash 只剩几十 KB 时砍哪个功能（例如把编译优化从 `-O2` 换回
`-Os`，实测能省下近 400 KB，代价是热路径变慢）。这些判断没法从代码里推出来，靠的是对场景
的理解。

---

## 六、快速开始

### 6.1 形态一：独立发送端固件（`firmware/`）

```sh
cd firmware
cp main/app_config_local.h.example main/app_config_local.h   # 填 Wi-Fi；接收端地址可留空
$EDITOR main/app_config_local.h

. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4          # 或 esp32s3
idf.py build
PORT=$(../tools/boardport p4)      # 按芯片 MAC 解析端口，口名漂移也不怕
idf.py -p "$PORT" flash
```

S3 用原生 USB 口烧完会停在 download 模式，补一条复位（P4 会自己复位）：

```sh
python -m esptool --port /dev/cu.usbmodem… --after watchdog-reset chip-id
```

上电后固件自检 FairPlay、连 Wi-Fi、起 mDNS 与控制页面，然后**待命，不主动连接**。投屏由
HTTP 触发：

```sh
curl http://<设备IP>:8080/targets                    # 列出局域网接收端（约 3 秒）
curl 'http://<设备IP>:8080/start?name=Living%20Room' # 按名字连；?host= 直接给地址
curl 'http://<设备IP>:8080/text?msg=你好世界'         # 整屏文字
curl http://<设备IP>:8080/status                     # idle / mirroring / 上次结束原因
curl http://<设备IP>:8080/stop                       # 礼貌断开（TEARDOWN）
```

投一台 Mac 的桌面或某个窗口：

```sh
cd macos/capture-cli && ./build.sh
./capture-cli list
./capture-cli record --window-id 961 --fps 20 --bitrate 2000000 --tcp <设备IP>:8090
```

先起采集流，再 `/start`；设备日志会出现 `video source: capture stream`。

`main/app_config_local.h` 里可调：设备名、静态帧停留时间、配对码、是否启用片上编码/网络
视频、采集端口。Wi-Fi 账号留空时设备自建热点（`airplay-esp32` / `airplay1234`），可以在
`192.168.4.2` 上用参考接收端做整链路台架，不需要 Apple TV。

### 6.2 形态二：小智固件整合（`xiaozhi-esp32/`）

```sh
cd xiaozhi-esp32
idf.py -B build-tv -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.jc4880tv" build
```

改动 `sdkconfig.defaults*` 或 `main/Kconfig.projbuild` 的默认值之后，**先删掉工程根目录的
`sdkconfig` 再编译**（IDF 6 的配置文件不在 build 目录里），否则新值不会生效。

烧好后对设备说话即可：

- 「把你自己投到电视上」→ `self.airplay.start`
- 「在电视上写：会议三点开始」→ `self.display.text`
- 「显示一只猫」/「画一只柴犬」→ `self.display.image`（云端生成，十几秒后出图）
- 「把小智的声音放到电视上」→ `self.airplay.audio`

电脑一侧（助手操作 Mac）：

```sh
uv run tools/hs_mcp/server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'
```

之后「打开编辑器写段笔记」→ `open_app` + `type_text`；「帮我查一下这个网址」→ `open_url`；
「把这台电脑的浏览器投到电视上」→ `cast_mac_to_tv`；「看看我现在屏幕上是什么」→ `screen_look`
（截图走设备视觉链路，返回文字）。

### 6.3 上板纪律

同一台机器上可能插着多块板、跑着多个 agent。上板操作一律包锁：

```sh
PORT=$(tools/boardport s3)
tools/esp32lock exec 600 -- sh -c "cd firmware && idf.py -p $PORT flash"

PORT=$(tools/boardport p4)
ESP32_BOARD=p4 tools/esp32lock exec 600 -- sh -c "cd firmware && idf.py -p $PORT flash"
```

`esp32lock status` 看板子被谁占着，`esp32lock wait` 排队。常驻命令（如 `idf.py monitor`）
跑完要显式释放，否则后面的编译烧录一直在排队。

---

## 七、目录结构

```
firmware/                    独立发送端固件（S3 / P4）
├── components/
│   ├── fairplay/            FairPlay SAP：白盒表、ekey、474 组向量 + host 测试
│   ├── hap/                 HAP 配对：SRP-6a、X25519/Ed25519、TLV8、加密分帧（133 项）
│   ├── airplay/             RTSP 会话、binary plist、SETUP、镜像数据通道、legacy 记录头
│   └── media/               内容源：静态帧、片上编码、文字、网络推流/拉流
├── main/                    Wi-Fi、身份持久化、配对码投递、启动自检
└── media-src/               静态帧源图

xiaozhi-esp32/               小智固件（vendored 上游 v2.5.2 + 本地补丁层，见 VENDOR.md）
└── main/
    ├── airplay/             AirplayService、音频 sink、ALAC 帧、台架 HTTP、webcast 拉流
    ├── display/             含 VirtualDisplay 内存显示后端
    └── boards/jc4880-tv/    无屏板：MCP 工具、屏幕摄像头

macos/capture-cli/           Mac 采集端：ScreenCaptureKit + VideoToolbox → TCP
tools/
├── hs_mcp/                  Hammerspoon MCP 服务器（16 个工具，可直连小智）
├── file_stream.py           视频文件投屏（H.264 直通 + ALAC）
├── typein                   向指定应用输入文字
├── boardport                按芯片 MAC 认板
├── esp32lock                板卡互斥锁
├── make_font.py             字库生成
└── mitm_capture.py          ARP 中间人抓包（协议取证用）
docs/                        协议规格、legacy 规格、运维手册、FairPlay 参考实现、演示视频
```

---

## 八、文档

- [docs/runbook.md](docs/runbook.md) — **实操手册**：编译、烧写、复位、看日志、配对投码、限流解除、故障速查
- [docs/protocol.md](docs/protocol.md) — 现代路径会话时序与报文走读
- [docs/protocol-spec.md](docs/protocol-spec.md) — 发送端协议规格全文，逐条标注参考实现出处
- [docs/legacy-spec.md](docs/legacy-spec.md) — legacy AirTunes 镜像规格：端口拓扑、密钥模型、贴牌盒子怪癖
- [docs/fairplay-spec.md](docs/fairplay-spec.md) — FairPlay SAP 移植规格（算法、常量、向量）
- [docs/fairplay-port-report.md](docs/fairplay-port-report.md) — mbedtls 4.1 / PSA 适配验证记录
- [docs/fairplay-reference/](docs/fairplay-reference/) — 独立可编译的 C 参考实现（`cc -O2 fairplay_ref.c && ./a.out` 输出 `ALL PASS`）
- [tools/hs_mcp/README.md](tools/hs_mcp/README.md) — Mac 操控工具清单与踩坑
- [macos/capture-cli/README.md](macos/capture-cli/README.md) — 采集端用法
- [xiaozhi-esp32/VENDOR.md](xiaozhi-esp32/VENDOR.md) — vendored 上游版本与补丁层边界、升级步骤

---

## 九、已知限制

- **音频只在 legacy 会话上**：现代路径的音频通道已声明（`spf=352`、`ct=2`），但还没发流。
- **真实桌面靠主机编码**：片上软编只有 320x180@10，投桌面/窗口要 `capture-cli` 或
  `file_stream.py`。设备只做加密转发，所以帧率上限在采集端。
- **单帧上限 512 KB**：文字密集窗口（终端里跑 `top`）的关键帧可能超限，超限帧被丢弃而不是
  断连；这类窗口建议先降到 960 宽。
- **一次一个接收端、一个采集端**：8090 端口一次只接受一个连接，断开后自动重新监听。
- **首配要等**：SRP-6a 3072 位模幂在 S3 上约 8 秒（看门狗已放宽）；接收端记住设备后每会话
  pair-verify，1 秒内开流。配对失败会被 HomeKit 退避（最长 30 分钟），固件会读 `0x08` 等满。
- **flash 预算紧张**：应用分区只剩几十 KB 余量。加功能前先看 `idf.py size`，必要时换 `-Os`
  或砍字体档位。
- **设备没有 mDNS**：只有 DHCP 主机名（`xiaozhi-XXYY`），发现靠环境变量、缓存或扫 8080。
- **网页拉流需要帧源服务端**：设备侧的拉流与工具已就绪（协议见
  `xiaozhi-esp32/main/airplay/webcast_client.h`），渲染服务端不在本仓库。
- **被完全遮挡的 macOS 窗口会停帧**：系统对不可见窗口停止绘制，没有 API 能绕，需要先把
  目标应用带到前台。
- **助手的视觉链路只回文字**：云端不消费 MCP 返回的图片，所以「看图」必须是
  截图 → `/vision/explain` → 文字答案 这条路径。

---

## 十、路线图

1. **硬编接入**：P4 硬件 H.264 编码跑 1080p，摆脱对主机编码的依赖
2. **现代路径音频**：把 ALAC 通道接到 HAP 会话，音频不再受接收端世代限制
3. **会话加速**：SRP 模幂滑动窗口 / 硬件加速，把首配从 8 秒压到 1 秒内
4. **多接收端**：同屏投多处（客厅 + 书房），以及逐接收端的源记忆
5. **自动化测试台**：把「真机验收」进一步脚本化——台架 API + 像素比对 + 解密错误计数
