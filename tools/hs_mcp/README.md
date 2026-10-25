# hs-mcp — Hammerspoon MCP 服务器

把 hammerspoon 技能封装成 MCP 服务器，让小智等 MCP 客户端通过工具调用操控这台 Mac：
打开应用、输入文字（支持中文/多行）、按快捷键、点菜单、看屏幕内容（视觉模型转成文字）、
把某个窗口/整屏投到电视、读写剪贴板、语音播报等。支持限定在某个应用内操作（先聚焦再动作，避免误伤）。

实现方式：每个工具内部调用 Hammerspoon 的 `hs` 命令（Hammerspoon.app 持有辅助功能/
录屏授权，规避了各宿主进程单独授权的问题）；文字输入复用仓库里的 `tools/typein`
（剪贴板粘贴模式，不受输入法影响）。依赖通过 PEP 723 内联元数据声明，`uv run` 直接跑。

## 运行

```bash
# 小智模式（一个参数接入）：进程内连小智的 MCP 接入点，断线自动重连
uv run tools/hs_mcp/server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'

# stdio 模式：给 Claude / aimux 等本地 MCP 客户端
uv run tools/hs_mcp/server.py

# HTTP 模式：/mcp（streamable）与 /sse 两个端点，无状态
uv run tools/hs_mcp/server.py --http    # 默认 127.0.0.1:8390
```

环境变量：`HS_MCP_HOST`/`HS_MCP_PORT`（HTTP 监听，默认 127.0.0.1:8390）、
`HS_MCP_HS`（hs 命令路径，默认 ~/bin/hs）、`HS_MCP_TYPEIN`（默认锚定仓库内 tools/typein）、
`HS_MCP_SHOT_DIR`（截图临时目录，默认 /tmp/hs-mcp）、
`HS_MCP_DEVICE`（显式指定小智固件设备的 8080 地址；不用时自动发现，见下文）、
`HS_MCP_CAPTURE_CLI`（推流工具路径，默认仓库内 `macos/capture-cli/capture-cli`）、
`HS_MCP_CAST_HOST`（拉流时要告诉设备的本机地址，默认从 ifconfig 取同网段地址）、
`HS_MCP_RELAY_PORT`（拉流时本地 relay 的视频端口，默认 8090，音频用 8091）。

前置条件：Hammerspoon.app 正在运行（报 "can't access Hammerspoon message port"
时先 `open -a Hammerspoon`）。`typein` 需要运行本服务的宿主 App 持有「辅助功能」授权
（从终端启动即继承终端的授权；换 launchd 等宿主时需重新授权）。

## 工具清单

| 工具 | 说明 |
|---|---|
| `list_running_apps` | 列出运行中的图形应用（确认应用名怎么写） |
| `front_app` | 当前前台应用与窗口标题 |
| `open_app` | 打开/聚焦应用；名称匹配失败自动 fallback `open -a` |
| `type_text` | 向指定应用的聚焦输入框输入文字（粘贴式，支持中文/多行），可选回车 |
| `press_key` | 组合键如 `cmd+s`；可选 `app` 参数：先聚焦该应用再按键 |
| `click_menu` | 点击菜单栏菜单项，如 `编辑/全选` |
| `open_url` | 用默认程序打开 http(s) 网址 |
| `screenshot_image` | 截屏返回 JPEG（MCP image content）；可选 `app` 参数只截该应用当前窗口，否则全屏。**小智语音链路不消费图片**，看屏幕内容用 `screen_look` |
| `screen_look` | **看屏幕并回答问题（小智场景用这个）**：截图 → 推给小智固件的 `/vision/explain` → 走云端视觉模型 → 返回文字描述。可选 `app` 参数 |
| `cast_to_tv` | **把某个应用窗口或整个屏幕投到电视**。`app` 空=整屏，`tv` 指定电视名，`audio=true` 连 Mac 系统声音，`mode` 选 `push`（默认，本机推给设备）或 `pull`（设备来本机拉，断线自恢复）|
| `find_device` | 找小智设备的地址和当前投屏状态（换了电脑/设备重启换了 IP 也能扫出来） |
| `stop_cast` | 停止投屏，电视回到设备自己的界面 |
| `clipboard_set` / `clipboard_get` | 写/读系统剪贴板 |
| `say` | TTS 朗读 |
| `notify` | 系统通知横幅 |

应用名注意：很多应用实际名是英文（TextEdit、Calculator），不确定先 `list_running_apps`。

### 设备地址不用配

设备没有 mDNS，只有一个 DHCP 主机名，重启后地址可能变；服务也不一定总在这台电脑上跑。
所以设备地址按这个顺序取：`HS_MCP_DEVICE`（显式）→ 上次发现的（`/tmp/hs-mcp/device.txt`，
用前先验证 `/airplay/status` 确实是设备）→ 扫本机所有私网 /24 的 8080（实测 508 个地址约 3 秒）。
扫到的地址会写进缓存，下次直接就命中。

### 为什么小智看不到 screenshot

小智 LLM 不消费 MCP 工具返回的图片（image content 会丢），它的视觉走设备侧链路：
图片 → 云端视觉接口 → **文字回答** → 进对话。所以给小智用的是 `screen_look`：
Mac 截图后推给小智固件设备的 `/vision/explain` 端点（设备地址按上文的发现规则，
也可用 `HS_MCP_DEVICE` 直接指定），设备用自己的 explain URL/token（MCP initialize 时
云端下发的 vision 能力）上传视觉模型，把文字答案原路返回。要求固件包含该端点。
设备在重建投屏会话时会瞬时断连，`screen_look` 因此失败时会自动重试一次。

## 把 Mac 窗口/屏幕投到电视（`cast_to_tv`）

链路：仓库里的 `macos/capture-cli`（ScreenCaptureKit 采集 + VideoToolbox 编码）→ TCP 8090 →
设备的 `media_net` → 现有 AirPlay 加密通道 → 电视。设备只做加密转发、不重新编码，所以电视上是
真实桌面（视频、动画都跟得上）。

一次调用做的事：

1. 解析目标：`capture-cli list` 里按应用名匹配窗口（同名多窗口取面积最大的）；`app` 留空则整屏
2. 确保设备在投屏：已经投着就复用；没投就 `/airplay/start`。设备重启后不记住接收器，会先
   `/airplay/discover`；局域网里有多台接收器时返回名字列表，让小智问用户后带 `tv=` 重新调用
3. 起 `capture-cli record --window-id … --fps 20 --tcp <设备>:8090`；宽度超过 960 等比缩到 960
   （文字密集窗口的关键帧能到几百 KB，原尺寸会让电视每秒顿一下）
4. 等设备 `status.source` 变成 `capture` 才返回；推流进程的日志在 `/tmp/hs-mcp/cast.log`

设备在推流客户端连上/断开时会自己重开会话切换视频源，所以 `stop_cast` 之后电视会自动回到
设备的界面。投屏期间助手的声音留在设备扬声器（电视的音频通道让给画面）。
`audio=true` 则把 Mac 的系统输出声音一起送到电视（整个系统，不是某个应用）。

### push 与 pull

| | `mode="push"`（默认） | `mode="pull"` |
|---|---|---|
| 谁建连接 | 本机连设备 8090 | 设备连本机 8090（地址存在设备 NVS 里）|
| 设备重启后 | 电视停在设备界面，要再说一次投屏 | 设备自己连回来，会话由 watchdog 接回，电视自动恢复 |
| 适用场景 | 同一局域网、临时投一下 | 长期挂着；本机在 NAT 后面；以后换成云上机器 |

被投的窗口要保持在屏幕上可见：macOS 会对被完全遮挡的窗口停止绘制，捕获流随之停帧，
电视上就停在最后一帧（系统机制，没有 API 能绕）。需要时先用 `open_app` 把目标应用带到前台。

### 重启服务不会自己投回去

投屏是用户这一刻要求的一个动作，不是持久配置。hs-mcp 重启（改代码、机器重启）不接管
上一次留下的投屏：落单的 capture-cli 进程直接杀掉、状态文件删掉、设备上的拉流目标清掉，
电视回到设备自己的界面，等用户再说一次。状态文件里记着写出它的进程 pid，所以同一台机器
上另一个 hs-mcp 正在投的屏不会被误伤。

投屏进行中的自愈不受影响：设备重启、电视断电回来时，watchdog 仍会把流和会话接回去。

拉流时本服务进程内起一个 relay（`8090`/`8091`）：capture-cli 连本机端口，设备连本机网卡地址，
两边在这里对接。设备断开时 capture-cli 留在原地继续读（帧丢掉），设备重连后 relay 会补发
它错过的 configure 包（SPS/PPS），画面几秒内自己回来。`stop_cast` 会先清掉设备上的拉流目标，
再停 relay 和推流进程，所以停机是停得住的。

前置条件：跑过一次 `macos/capture-cli/build.sh`（产物不进 git），并给运行本服务的宿主 App
屏幕录制授权；二进制路径可用 `HS_MCP_CAPTURE_CLI` 覆盖。

## 接入小智

在 Mac 上跑一条命令即可（token 在小智后台的 MCP 接入页）：

```bash
uv run tools/hs_mcp/server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'
```

进程内实现 WebSocket 传输的 MCP server（逐帧 JSON-RPC），连上后 15 个工具即注册到
小智，对话里直接说「打开文本编辑器写点内容」就能用；断网/断线每 2 秒起自动重连
（指数退避到 30 秒）。想开机常驻就包一层 launchd/`nohup`，本仓库不内置。

⚠️ **安全**：这组工具等价于电脑的完整控制权。token 泄漏=电脑被别人操控，
接入点 URL 不要外传；后台生成新 token 可随时吊销旧接入。

## 实现注记（踩过的坑）

- `hs -c` 会把 **stdin 也当 Lua 代码执行**：子进程必须接 DEVNULL，否则 stdio
  模式下会误啃 JSON-RPC 流。
- hs CLI 的 IPC 有内部接收超时（秒级），**等待循环一律放 Python 侧**轮询，
  单个 Lua 块保持毫秒级；截屏（~4s）用 `hs.timer.doAfter(0, …)` 做
  fire-and-forget，Python 侧等文件落盘。
- 应用名匹配：`hs.application.get` 对已运行应用的中文本地化名匹配不可靠，
  以遍历 `runningApplications()` 的 `title()`/`name()` 双字段为准；不少应用
  （如 TextEdit）各层级都叫英文名，「中文名」只有部分入口认识，故有 `open -a` 兜底。
- hs 报错回显会在多字节字符中间截断产生非法 UTF-8，读取子进程输出需
  `errors="replace"`。
- 截图返回前缩放到 1600px：5K 原图 JPEG ~1.4MB，base64 后的单行 SSE 响应
  会撑爆部分 MCP 客户端。
- 小智接入点消息是**逐帧裸 JSON-RPC**；mcp SDK 里 `SessionMessage` 是
  dataclass，pydantic 解析在内层 `JSONRPCMessage` 上（与 stdio reader 同款写法）。

## 固件侧：`POST /vision/explain`

台架 8080 端点，body 为 JSON `{"question": "…", "image": "<base64 JPEG>"}`，
返回值是云端视觉服务的答案文本（复用设备持有的 explain URL/token，设备自己完成上传鉴权）。实现：`airplay_http.cc` 里注册 URI、
板级注回调（`AirplayHttpSetVisionExplain` → `AirplayScreenCamera::ExplainJpeg`）。
注意 httpd 在 handler 里会阻塞数秒上传视觉模型，以及 `max_uri_handlers` 已调到 16。
