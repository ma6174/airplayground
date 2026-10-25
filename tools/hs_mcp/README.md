# hs-mcp — Hammerspoon MCP 服务器

把 hammerspoon 技能封装成 MCP 服务器，让小智等 MCP 客户端通过工具调用操控这台 Mac：
打开应用、输入文字（支持中文/多行）、按快捷键、点菜单、按应用截屏看结果、读写剪贴板、
语音播报等。支持限定在某个应用内操作（先聚焦再动作，避免误伤）。

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
`HS_MCP_SHOT_DIR`（截图临时目录，默认 /tmp/hs-mcp）。

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
| `screenshot` | 截屏返回 JPEG（MCP image content）；可选 `app` 参数只截该应用当前窗口（推荐，更快更聚焦），否则全屏 |
| `clipboard_set` / `clipboard_get` | 写/读系统剪贴板 |
| `say` | TTS 朗读 |
| `notify` | 系统通知横幅 |

应用名注意：很多应用实际名是英文（TextEdit、Calculator），不确定先 `list_running_apps`。

## 接入小智

在 Mac 上跑一条命令即可（token 在小智后台的 MCP 接入页）：

```bash
uv run tools/hs_mcp/server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'
```

进程内实现 WebSocket 传输的 MCP server（逐帧 JSON-RPC），连上后 12 个工具即注册到
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
