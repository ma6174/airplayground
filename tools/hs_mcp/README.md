# hs-mcp — Hammerspoon MCP 服务器

把 hammerspoon 技能封装成 MCP 服务器，让小智等 MCP 客户端通过工具调用操控这台 Mac：
打开应用、输入文字（支持中文/多行）、按快捷键、点菜单、截屏看结果、读写剪贴板、语音播报等。

实现方式：每个工具内部调用 Hammerspoon 的 `hs` 命令（Hammerspoon.app 持有辅助功能/
录屏授权，规避了各宿主进程单独授权的问题）；文字输入复用仓库里的 `tools/typein`
（剪贴板粘贴模式，不受输入法影响）。依赖通过 PEP 723 内联元数据声明，`uv run` 直接跑。

## 运行

```bash
# stdio 模式：给 Claude / aimux 等本地 MCP 客户端
uv run tools/hs_mcp/server.py

# HTTP 模式：给小智后台等远程客户端
uv run tools/hs_mcp/server.py --http
# 默认 127.0.0.1:8390；/mcp（streamable HTTP）与 /sse 两个端点都提供，
# 无状态模式（服务重启后客户端会话不失效，经隧道接入无需粘性会话）
```

环境变量：`HS_MCP_HOST`（默认 127.0.0.1）、`HS_MCP_PORT`（默认 8390）、
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
| `press_key` | 组合键，如 `cmd+s`、`cmd+shift+t`（发给前台应用） |
| `click_menu` | 点击菜单栏菜单项，如 `编辑/全选` |
| `open_url` | 用默认程序打开 http(s) 网址 |
| `screenshot` | 全屏截图，返回缩放后的 JPEG（MCP image content） |
| `clipboard_set` / `clipboard_get` | 写/读系统剪贴板 |
| `say` | TTS 朗读 |
| `notify` | 系统通知横幅 |

## 接入小智

1. 让服务对公网可达（小智云端代理调用，必须能从公网访问）：
   自建 frp 反向隧道到自己的 VPS、cloudflared、tailscale funnel 均可；
   服务仍监听 127.0.0.1，隧道只做转发。
2. 在小智后台对应智能体里添加第三方 MCP，填隧道 URL（`…/sse`；
   若后台支持 streamable HTTP 也可用 `…/mcp`）。
3. 对小智说「在电脑上打开记事本写点内容」即可。

⚠️ **安全**：这组工具等价于电脑的完整控制权。不要把端口直接暴露到公网，
隧道层务必加访问控制（frp token / Cloudflare Access / Tailscale 私有网络）。

## 实现注记（踩过的坑）

- `hs -c` 会把 **stdin 也当 Lua 代码执行**：子进程必须接 DEVNULL，否则 stdio
  模式下会误啃 JSON-RPC 流。
- hs CLI 的 IPC 有内部接收超时（秒级），**等待循环一律放 Python 侧**轮询，
  单个 Lua 块保持毫秒级；全屏截图（~4s）用 `hs.timer.doAfter(0, …)` 做
  fire-and-forget，Python 侧等文件落盘。
- 应用名匹配：`hs.application.get` 对已运行应用的中文本地化名匹配不可靠，
  以遍历 `runningApplications()` 的 `title()`/`name()` 双字段为准；不少应用
  （如 TextEdit）各层级都叫英文名，「中文名」只有部分入口认识，故有 `open -a` 兜底。
- hs 报错回显会在多字节字符中间截断产生非法 UTF-8，读取子进程输出需
  `errors="replace"`。
- 截图返回前缩放到 1600px：5K 原图 JPEG ~1.4MB，base64 后的单行 SSE 响应
  会撑爆部分 MCP 客户端。
