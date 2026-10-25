#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = ["mcp>=1.10,<2", "uvicorn>=0.30"]
# ///
"""hs-mcp — 把 hammerspoon 技能封装成 MCP 服务器，让小智等 MCP 客户端操控 macOS。

每个工具内部调用 Hammerspoon 的 `hs` 命令（Hammerspoon.app 持有辅助功能/录屏
授权，且为本地化应用名匹配、Lua 注入转义等坑提供了成熟处理方式）；文字输入复用
仓库里的 tools/typein（剪贴板粘贴模式，支持中文/多行）。

运行方式：

  # stdio 模式：给 Claude / aimux 等本地 MCP 客户端
  uv run tools/hs_mcp/server.py

  # HTTP 模式：给小智后台等远程客户端；/sse 与 /mcp 两种端点都提供
  uv run tools/hs_mcp/server.py --http
  # 默认监听 127.0.0.1:8390，可用 HS_MCP_HOST / HS_MCP_PORT 覆盖

前置条件：Hammerspoon.app 正在运行（报 "can't access Hammerspoon message
port" 时先 `open -a Hammerspoon`）。
"""

import os
import subprocess
import sys
import time
from contextlib import asynccontextmanager
from pathlib import Path

from mcp.server.fastmcp import FastMCP

try:
    from mcp.server.fastmcp.utilities.types import Image
except ImportError:
    from mcp.server.fastmcp import Image

import uvicorn
from starlette.applications import Starlette

HS = os.environ.get("HS_MCP_HS", str(Path.home() / "bin/hs"))
# 默认锚定 server.py 所在仓库的 tools/typein，不依赖启动时的工作目录
_TYPEIN_DEFAULT = Path(__file__).resolve().parent.parent / "typein"
TYPEIN = Path(os.environ.get("HS_MCP_TYPEIN", _TYPEIN_DEFAULT))
SHOT_DIR = Path(os.environ.get("HS_MCP_SHOT_DIR", "/tmp/hs-mcp"))

mcp = FastMCP(
    "hammerspoon",
    instructions=(
        "操作用户 macOS 电脑的工具集。输入文字前先 open_app 打开/聚焦目标应用；"
        "应用名不确定时用 list_running_apps 查（很多应用实际是英文名，如 TextEdit）；"
        "不确定屏幕上发生了什么时调 screenshot 看一眼。"
    ),
    # 无状态 HTTP：服务重启后旧会话不失效，经公网隧道接入也不需要粘性会话
    stateless_http=True,
)


def hs(lua: str, timeout: float = 10.0) -> str:
    """在 Hammerspoon 控制台执行一段 Lua，返回 stdout。"""
    try:
        # stdin 必须接 /dev/null：hs -c 会把 stdin 也当 Lua 代码执行，
        # stdio 模式下继承的 JSON-RPC 流会被它误啃。
        # errors=replace：hs 报错回显会在多字节字符中间截断，产生非法 UTF-8。
        p = subprocess.run([HS, "-c", lua], capture_output=True, text=True,
                           encoding="utf-8", errors="replace",
                           timeout=timeout, stdin=subprocess.DEVNULL)
    except FileNotFoundError:
        raise RuntimeError(f"找不到 hs 命令（{HS}），确认 Hammerspoon 已安装")
    except subprocess.TimeoutExpired:
        raise RuntimeError("hs 命令超时，Hammerspoon 可能正忙，稍后重试")
    if p.returncode != 0 or "can't access Hammerspoon message port" in p.stderr:
        raise RuntimeError(f"hs 执行失败: {(p.stderr or p.stdout).strip()}")
    return p.stdout.strip()


def lua_q(s: str) -> str:
    """把 Python 字符串转义成安全的 Lua 单引号字符串字面量。"""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == "'":
            out.append("\\'")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 32:
            out.append(f"\\{ord(ch)}")
        else:
            out.append(ch)
    return "'" + "".join(out) + "'"


def run(cmd: list[str], timeout: float = 15.0) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
    if p.returncode != 0:
        raise RuntimeError((p.stderr or p.stdout).strip() or f"{cmd[0]} 退出码 {p.returncode}")
    return p.stdout.strip()


@mcp.tool()
def list_running_apps() -> str:
    """列出当前正在运行的图形应用（本机本地化名称）。输入/操作某个应用前，用它确认应用名怎么写。"""
    return hs(
        "local t={} for _,a in ipairs(hs.application.runningApplications()) do "
        "if a:kind()>0 and a:title()~='' then t[#t+1]=a:title() end end "
        "return hs.json.encode(t)"
    )


@mcp.tool()
def front_app() -> str:
    """查看当前前台应用名和它的窗口标题，用于确认焦点在哪。"""
    return hs(
        "local a=hs.application.frontmostApplication() local w=a:focusedWindow() "
        "return (a:title() or a:name() or '?')..' — '..(w and w:title() or '无窗口')"
    )


@mcp.tool()
def open_app(name: str) -> str:
    """打开或聚焦一个应用（没在运行则启动）。返回聚焦后的前台应用名。

    注意：中文系统上很多应用实际名仍是英文（如 TextEdit、Calculator），
    不确定时先用 list_running_apps 查。找不到中文名时会自动尝试系统 open -a。
    """
    want = name.lower()
    wq = lua_q(want)
    # 事实：get()/launchOrFocus 对已运行应用的匹配不可靠（中英文名不一致），
    # 可靠锚点是遍历 runningApplications 双字段（title 本地化名/ name 进程名）；
    # 某些应用（如 TextEdit）连本地化名都是英文，「中文名」只有 LaunchServices 认识，
    # 所以匹配失败时还要 fallback open -a。
    # 等待循环放 Python 侧：hs CLI 的 IPC 有内部接收超时，单个 Lua 块不能跑秒级轮询。
    hs(f"hs.application.launchOrFocus({wq}) return 'ok'", timeout=5.0)  # 冷启动场景，返回值不代表成败
    find_and_activate = (
        f"local want={wq} local app "
        "for _,a in ipairs(hs.application.runningApplications()) do "
        "if a:kind()>0 then local n=(a:name() or ''):lower() local t=(a:title() or ''):lower() "
        "if n==want or t==want then app=a break end end end "
        "if not app then "
        "for _,a in ipairs(hs.application.runningApplications()) do "
        "if a:kind()>0 then local n=(a:name() or ''):lower() local t=(a:title() or ''):lower() "
        "if (n~='' and n:find(want,1,true)) or (t~='' and t:find(want,1,true)) then app=a break end end end end "
        "if not app then return 'NOAPP' end "
        "if not app:isFrontmost() then app:activate() end "
        "if app:isFrontmost() then return 'FRONT|'..(app:title() or app:name()) end "
        "return 'ACTIVATING'"
    )
    for _ in range(8):
        time.sleep(0.3)
        out = hs(find_and_activate, timeout=5.0)
        if out.startswith("FRONT|"):
            return "已聚焦: " + out.split("|", 1)[1]
        if out != "NOAPP":
            break
    else:
        out = "NOAPP"
    if out == "NOAPP":
        try:
            run(["open", "-a", name], timeout=8.0)  # LaunchServices 认注册名（含部分中文别名）
        except RuntimeError:
            return (
                f"找不到应用 {name}。可先调 list_running_apps 查看正在运行的应用名再重试。"
            )
    for _ in range(10):
        time.sleep(0.3)
        out = hs(find_and_activate, timeout=5.0)
        if out.startswith("FRONT|"):
            return "已聚焦: " + out.split("|", 1)[1]
        if out == "NOAPP":
            break
    return f"已尝试启动 {name}，但未能确认它到前台。可用 screenshot 或 front_app 确认实际状态。"


@mcp.tool()
def type_text(app: str, text: str, enter: bool = False) -> str:
    """把文字输入到指定应用当前聚焦的输入框（粘贴方式，支持中文、多行，不受输入法影响）。会先把 app 聚焦；聚焦确认失败会拒绝输入。enter=true 时输入后按一次回车。务必先用 open_app 打开目标应用。"""
    cmd = [str(TYPEIN), "--app", app, "--text", text]
    if enter:
        cmd.append("--enter")
    try:
        return run(cmd) or "已输入"
    except RuntimeError as e:
        raise RuntimeError(f"typein 失败（详见输出）: {e}")


_KEY_RE = __import__("re").compile(r"^[a-z0-9_]+$")


@mcp.tool()
def press_key(key: str) -> str:
    """按一次组合键，发给当前前台应用。格式小写加号连接，如 'return' 'cmd+s' 'cmd+shift+t'。"""
    parts = [p for p in key.lower().split("+") if p]
    if not parts or not all(_KEY_RE.match(p) for p in parts):
        raise RuntimeError(f"非法按键: {key}")
    *mods, k = parts
    mod_table = "{" + ",".join(f"'{m}'" for m in mods) + "}"
    return hs(f"hs.eventtap.keyStroke({mod_table},'{k}') return 'ok'")


@mcp.tool()
def click_menu(app: str, path: str) -> str:
    """点击应用的菜单栏菜单项，path 用 / 分隔，如 '文件/新建' 或 'File/New Window'。应用必须在运行。"""
    items = ", ".join(lua_q(x) for x in path.split("/") if x)
    return hs(
        f"local a=hs.application.get({lua_q(app)}) if not a then error('应用未运行: {app}') end "
        f"return tostring(a:selectMenuItem({{{items}}})) and 'ok' or '菜单项不存在'"
    )


@mcp.tool()
def open_url(url: str) -> str:
    """用系统默认程序打开一个 http(s) 网址（会在浏览器里打开）。"""
    if not url.startswith(("http://", "https://")):
        raise RuntimeError("只支持 http(s) URL")
    return run(["open", url]) or f"已打开 {url}"


@mcp.tool()
def screenshot() -> Image:
    """截取整个屏幕，返回 JPEG 图片。想确认操作结果、看屏幕上有什么时调用。"""
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    png = SHOT_DIR / "screen.png"
    jpg = SHOT_DIR / "screen.jpg"
    png.unlink(missing_ok=True)
    # 5K 屏 snapshot+saveToFile 约 4 秒，超过 hs CLI 的 IPC 接收超时，
    # 所以用 doAfter(0) 做成 fire-and-forget，Python 侧轮询文件落盘
    hs(
        f"hs.timer.doAfter(0, function() local i=hs.screen.mainScreen():snapshot() "
        f"i:saveToFile('{png}') end) return 'ok'",
        timeout=5.0,
    )
    for _ in range(50):
        time.sleep(0.2)
        if png.exists() and png.stat().st_size > 0:
            time.sleep(0.3)  # 等写完
            break
    else:
        raise RuntimeError("截图超时，Hammerspoon 可能正忙")
    # 缩到 1600px 再转 JPEG：全屏原图 JPEG ~1.4MB，base64 后的单行 SSE 响应
    # 会撑爆部分 MCP 客户端；缩图对视觉模型也足够
    run(["sips", "-Z", "1600", "-s", "format", "jpeg", "-s", "formatOptions", "70",
         str(png), "--out", str(jpg)], timeout=15)
    return Image(path=str(jpg))


@mcp.tool()
def clipboard_set(text: str) -> str:
    """把文字写入系统剪贴板。"""
    return hs(f"hs.pasteboard.setContents({lua_q(text)}) return 'ok'")


@mcp.tool()
def clipboard_get() -> str:
    """读取系统剪贴板里的文字。"""
    return hs("return hs.pasteboard.getContents() or ''")


@mcp.tool()
def say(text: str) -> str:
    """用语音合成（TTS）在电脑上朗读一段文字。"""
    return hs(f"hs.speech.new():speak({lua_q(text)}) return 'ok'")


@mcp.tool()
def notify(text: str) -> str:
    """在电脑屏幕上发一条系统通知横幅。"""
    return hs(
        f"hs.notify.new({{title=[[小智]], informativeText={lua_q(text)}}}):send() return 'ok'"
    )


def build_http_app() -> Starlette:
    """把 SSE 和 streamable HTTP 两个端点合并进一个 ASGI 应用（同一个端口）。"""
    sse_app = mcp.sse_app()
    http_app = mcp.streamable_http_app()

    @asynccontextmanager
    async def lifespan(a):
        async with http_app.router.lifespan_context(a):
            async with sse_app.router.lifespan_context(a):
                yield

    return Starlette(routes=[*sse_app.routes, *http_app.routes], lifespan=lifespan)


def main() -> None:
    if "--http" in sys.argv:
        uvicorn.run(
            build_http_app(),
            host=os.environ.get("HS_MCP_HOST", "127.0.0.1"),
            port=int(os.environ.get("HS_MCP_PORT", "8390")),
            log_level="info",
        )
    else:
        mcp.run()  # stdio


if __name__ == "__main__":
    main()
