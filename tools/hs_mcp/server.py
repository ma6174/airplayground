#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = ["mcp>=1.10,<2", "uvicorn>=0.30", "websockets>=12"]
# ///
"""hs-mcp — 把 hammerspoon 技能封装成 MCP 服务器，让小智等 MCP 客户端操控 macOS。

每个工具内部调用 Hammerspoon 的 `hs` 命令（Hammerspoon.app 持有辅助功能/录屏
授权，且为本地化应用名匹配、Lua 注入转义等坑提供了成熟处理方式）；文字输入复用
仓库里的 tools/typein（剪贴板粘贴模式，支持中文/多行）。

运行方式：

  # 小智模式（一个参数接入：进程内实现 WebSocket 传输的 MCP server，断线自动重连）
  uv run tools/hs_mcp/server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'

  # stdio 模式：给 Claude / aimux 等本地 MCP 客户端
  uv run tools/hs_mcp/server.py

  # HTTP 模式：给小智后台等远程客户端；/sse 与 /mcp 两种端点都提供
  uv run tools/hs_mcp/server.py --http
  # 默认监听 127.0.0.1:8390，可用 HS_MCP_HOST / HS_MCP_PORT 覆盖

前置条件：Hammerspoon.app 正在运行（报 "can't access Hammerspoon message
port" 时先 `open -a Hammerspoon`）。
"""

import base64
import functools
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from contextlib import asynccontextmanager
from concurrent.futures import ThreadPoolExecutor, as_completed
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
# 小智固件的台架接口（8080）。不写死地址：设备重启会换 DHCP 地址，
# 且这个仓库的脚本也会在别的电脑上跑——HS_MCP_DEVICE 显式优先，
# 否则用上次发现的地址，最后才扫本网段。
DEVICE_ENV = os.environ.get("HS_MCP_DEVICE", "").rstrip("/")
DEVICE_CACHE = SHOT_DIR / "device.txt"
# 仓库自带的屏幕/窗口推流工具（macos/capture-cli/build.sh 构建，需要屏幕录制授权）
_CAPTURE_DEFAULT = Path(__file__).resolve().parents[2] / "macos/capture-cli/capture-cli"
CAPTURE_CLI = Path(os.environ.get("HS_MCP_CAPTURE_CLI", _CAPTURE_DEFAULT))
CAST_PID = SHOT_DIR / "cast.pid"
CAST_LOG = SHOT_DIR / "cast.log"
# 拉流时本地 relay 的最小端口（音频用 +1，跟 capture-cli 的约定一致）
RELAY_PORT = int(os.environ.get("HS_MCP_RELAY_PORT", "8090"))
# 当前投流的完整命令，watchdog 靠它把断掉的流拉回来
CAST_STATE = SHOT_DIR / "cast.state"

mcp = FastMCP(
    "hammerspoon",
    instructions=(
        "操作用户 macOS 电脑的工具集。输入文字前先 open_app 打开/聚焦目标应用；"
        "应用名不确定时用 list_running_apps 查（很多应用实际是英文名，如 TextEdit）；"
        "看屏幕内容（截屏/看看屏幕/屏幕上有什么）必须用 screen_look：它返回文字；"
        "screenshot_image 只返回图片，语音对话里看不到；"
        "用户说“把某个应用/屏幕投到电视”时用 cast_to_tv（停止用 stop_cast）。"
    ),
    # 无状态 HTTP：服务重启后旧会话不失效，经公网隧道接入也不需要粘性会话
    stateless_http=True,
)

LOG_FILE = SHOT_DIR / "hs-mcp.log"


def _log(msg: str) -> None:
    try:
        SHOT_DIR.mkdir(parents=True, exist_ok=True)
        with LOG_FILE.open("a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")
    except Exception:
        pass


def tool():
    """注册 MCP 工具，并把每次调用落一行日志（排障：客户端到底调了哪个工具）。"""

    def deco(fn):
        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            args_txt = ", ".join(f"{k}={str(v)[:80]!r}" for k, v in kwargs.items()) \
                or ", ".join(str(a)[:80] for a in args)
            try:
                result = fn(*args, **kwargs)
            except Exception as e:
                _log(f"{fn.__name__}({args_txt}) ERROR {e}")
                raise
            brief = str(getattr(result, "path", result))[:160].replace("\n", " ")
            _log(f"{fn.__name__}({args_txt}) -> {brief}")
            return result

        return mcp.tool()(wrapper)

    return deco


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


@tool()
def list_running_apps() -> str:
    """列出当前正在运行的图形应用（本机本地化名称）。输入/操作某个应用前，用它确认应用名怎么写。"""
    return hs(
        "local t={} for _,a in ipairs(hs.application.runningApplications()) do "
        "if a:kind()>0 and a:title()~='' then t[#t+1]=a:title() end end "
        "return hs.json.encode(t)"
    )


@tool()
def front_app() -> str:
    """查看当前前台应用名和它的窗口标题，用于确认焦点在哪。"""
    return hs(
        "local a=hs.application.frontmostApplication() local w=a:focusedWindow() "
        "return (a:title() or a:name() or '?')..' — '..(w and w:title() or '无窗口')"
    )


_FIND_APP = (
    "for _,a in ipairs(hs.application.runningApplications()) do "
    "if a:kind()>0 then local n=(a:name() or ''):lower() local t=(a:title() or ''):lower() "
    "if n==want or t==want then app=a break end end end "
    "if not app then "
    "for _,a in ipairs(hs.application.runningApplications()) do "
    "if a:kind()>0 then local n=(a:name() or ''):lower() local t=(a:title() or ''):lower() "
    "if (n~='' and n:find(want,1,true)) or (t~='' and t:find(want,1,true)) then app=a break end end end end "
)


def _focus_app(name: str) -> str:
    """把应用带到前台，成功返回其显示名；找不到/失败抛 RuntimeError。"""
    wq = lua_q(name.lower())
    # 事实：get()/launchOrFocus 对已运行应用的匹配不可靠（中英文名不一致），
    # 可靠锚点是遍历 runningApplications 双字段（title 本地化名/ name 进程名）；
    # 某些应用（如 TextEdit）连本地化名都是英文，「中文名」只有 LaunchServices 认识，
    # 所以匹配失败时还要 fallback open -a。
    # 等待循环放 Python 侧：hs CLI 的 IPC 有内部接收超时，单个 Lua 块不能跑秒级轮询。
    hs(f"hs.application.launchOrFocus({wq}) return 'ok'", timeout=5.0)  # 冷启动场景，返回值不代表成败
    find_and_activate = (
        f"local want={wq} local app " + _FIND_APP +
        "if not app then return 'NOAPP' end "
        "if not app:isFrontmost() then app:activate() end "
        "if app:isFrontmost() then return 'FRONT|'..(app:title() or app:name()) end "
        "return 'ACTIVATING'"
    )
    for _ in range(8):
        time.sleep(0.3)
        out = hs(find_and_activate, timeout=5.0)
        if out.startswith("FRONT|"):
            return out.split("|", 1)[1]
        if out != "NOAPP":
            break
    else:
        out = "NOAPP"
    if out == "NOAPP":
        try:
            run(["open", "-a", name], timeout=8.0)  # LaunchServices 认注册名（含部分中文别名）
        except RuntimeError:
            raise RuntimeError(
                f"找不到应用 {name}。可先调 list_running_apps 查看正在运行的应用名再重试。"
            )
    for _ in range(10):
        time.sleep(0.3)
        out = hs(find_and_activate, timeout=5.0)
        if out.startswith("FRONT|"):
            return out.split("|", 1)[1]
        if out == "NOAPP":
            break
    raise RuntimeError(f"已尝试启动 {name}，但未能把它带到前台，可用 front_app 确认当前状态。")


@tool()
def open_app(name: str) -> str:
    """打开或聚焦一个应用（没在运行则启动）。返回聚焦后的前台应用名。

    注意：中文系统上很多应用实际名仍是英文（如 TextEdit、Calculator），
    不确定时先用 list_running_apps 查。找不到中文名时会自动尝试系统 open -a。
    """
    return "已聚焦: " + _focus_app(name)


@tool()
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


@tool()
def press_key(key: str, app: str = "") -> str:
    """按一次组合键，如 'return' 'cmd+s' 'cmd+shift+t'。默认发给当前前台应用；app 给定时先聚焦该应用再按键（推荐，避免打错目标）。"""
    if app:
        _focus_app(app)
    parts = [p for p in key.lower().split("+") if p]
    if not parts or not all(_KEY_RE.match(p) for p in parts):
        raise RuntimeError(f"非法按键: {key}")
    *mods, k = parts
    mod_table = "{" + ",".join(f"'{m}'" for m in mods) + "}"
    return hs(f"hs.eventtap.keyStroke({mod_table},'{k}') return 'ok'")


@tool()
def click_menu(app: str, path: str) -> str:
    """点击应用的菜单栏菜单项，path 用 / 分隔，如 '文件/新建' 或 'File/New Window'。应用必须在运行。"""
    items = ", ".join(lua_q(x) for x in path.split("/") if x)
    return hs(
        f"local a=hs.application.get({lua_q(app)}) if not a then error('应用未运行: {app}') end "
        f"return tostring(a:selectMenuItem({{{items}}})) and 'ok' or '菜单项不存在'"
    )


@tool()
def open_url(url: str) -> str:
    """用系统默认程序打开一个 http(s) 网址（会在浏览器里打开）。"""
    if not url.startswith(("http://", "https://")):
        raise RuntimeError("只支持 http(s) URL")
    return run(["open", url]) or f"已打开 {url}"


@tool()
def screenshot_image(app: str = "") -> Image:
    """截屏并以图片（MCP image content）返回。注意：语音/小智对话**看不到图片内容**，看屏幕请用 screen_look（返回文字）。app 给定时只截该应用的当前窗口，否则截整个屏幕。"""
    jpg = _shot_jpeg(app)
    return Image(path=str(jpg))


def _shot_jpeg(app: str = "") -> Path:
    """截屏（app 可选限定窗口），缩放转 JPEG，返回文件路径。"""
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    png = SHOT_DIR / "screen.png"
    jpg = SHOT_DIR / "screen.jpg"
    png.unlink(missing_ok=True)
    # 5K 屏 snapshot+saveToFile 约 4 秒，超过 hs CLI 的 IPC 接收超时，
    # 所以用 doAfter(0) 做成 fire-and-forget，Python 侧轮询文件落盘
    if app:
        # 先同步确认窗口存在（毫秒级），再异步截图；避免白等
        check = hs(
            f"local want={lua_q(app.lower())} local app " + _FIND_APP +
            "if not app or not app:mainWindow() then return 'NOWIN' end return 'OK'",
            timeout=5.0,
        )
        if check != "OK":
            raise RuntimeError(f"{app} 没有可截取的窗口（应用未运行或无窗口）")
        snap = (
            f"local want={lua_q(app.lower())} local app " + _FIND_APP +
            "if not app then return end local w=app:mainWindow() if not w then return end "
            f"local i=w:snapshot() if i then i:saveToFile('{png}') end"
        )
    else:
        snap = (
            f"local i=hs.screen.mainScreen():snapshot() i:saveToFile('{png}')"
        )
    hs(f"hs.timer.doAfter(0, function() {snap} end) return 'ok'", timeout=5.0)
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
    return jpg


@tool()
def screen_look(question: str, app: str = "") -> str:
    """看电脑屏幕并回答问题：截图交给视觉模型，返回文字描述。

    只要是「截屏」「截图」「看看屏幕」「屏幕上有什么」「写的什么内容」「刚才操作的结果如何」
    都用这个工具（语音/小智场景唯一能看到屏幕的途径，不要用 screenshot_image）。
    question 传具体问题，如“屏幕上显示了什么文字”；
    app 给定时只看该应用的窗口（如 app="TextEdit"），否则看整个屏幕。
    """
    jpg = _shot_jpeg(app)
    raw = _post_vision(jpg.read_bytes(), question)
    # 设备返回 {"success":true,"filename":…,"text":"<答案>"}：只把答案文本交给模型，
    # 否则语音助手会念出整包 JSON，或者把 JSON 当成“看不到”
    try:
        data = json.loads(raw)
    except ValueError:
        return raw
    if isinstance(data, dict):
        if data.get("text"):
            return str(data["text"])
        if data.get("error"):
            raise RuntimeError(f"视觉服务拒绝：{data['error']}")
    return raw


def _post_vision(jpeg: bytes, question: str) -> str:
    """把 JPEG 交给设备的视觉链路，返回原始 JSON 文本。

    设备在重建投屏会话时（capture 源切换）会短暂不收请求，实测能直接送一个
    Broken pipe 回来；用户已经说完话等答案了，这里自己重试一次而不是报错。
    """
    body = json.dumps({
        "question": question,
        "image": base64.b64encode(jpeg).decode(),
    }).encode()
    last: Exception | None = None
    base = _device_base()
    for attempt in (1, 2):
        req = urllib.request.Request(
            f"{base}/vision/explain", data=body,
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=25) as resp:  # 设备上传视觉模型需数秒
                return resp.read().decode("utf-8", "replace")
        except Exception as e:
            last = e
            _log(f"vision explain attempt {attempt} failed: {e}")
            if attempt == 1:
                time.sleep(0.8)
    raise RuntimeError(
        f"视觉请求失败（设备 {base}）：{last}。重试一次仍失败：确认设备在线、固件含 "
        "/vision/explain；设备刚切换投屏源时也会短暂失败，稍等几秒再问")

# ---- 把 Mac 的窗口/屏幕投到电视（capture-cli → 设备 8090 → AirPlay） ----


def _capture_bin() -> str:
    if not CAPTURE_CLI.is_file():
        raise RuntimeError(
            f"找不到 capture-cli（{CAPTURE_CLI}）：先在仓库里跑 macos/capture-cli/build.sh "
            "并在系统设置里给它屏幕录制权限，或用 HS_MCP_CAPTURE_CLI 指定二进制")
    return str(CAPTURE_CLI)


def _device_ok(base: str) -> bool:
    """这个地址是不是小智设备：8080 上得有台架 API 的 /airplay/status。"""
    try:
        with urllib.request.urlopen(f"{base}/airplay/status", timeout=2) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
        return isinstance(data, dict) and "streamed" in data
    except Exception:
        return False


def _lan_prefixes() -> list[str]:
    """本机所有私网 /24 前缀（跳过回环、隧道和 Tailscale 网段）。"""
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True,
                             timeout=5, stdin=subprocess.DEVNULL).stdout
    except Exception:
        return []
    prefixes: list[str] = []
    for m in re.finditer(r"\binet (\d+\.\d+\.\d+)\.\d+", out):
        prefix = m.group(1)
        if prefix.startswith(("127.", "169.254", "100.64", "198.18", "198.19")):
            continue
        if prefix not in prefixes:
            prefixes.append(prefix)
    return prefixes


def _port_open(ip: str, port: int, timeout: float = 0.7) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        return s.connect_ex((ip, port)) == 0
    finally:
        s.close()


def _scan_lan() -> str | None:
    """扫本网段 8080，返回第一台台架 API 认得的设备。

    线程池而不是 asyncio：工具函数可能在 MCP 的事件循环里被调用，asyncio.run 会直接报错。
    """
    hosts = [f"{p}.{i}" for p in _lan_prefixes() for i in range(1, 255)]
    if not hosts:
        return None
    try:
        with ThreadPoolExecutor(max_workers=48) as pool:
            futures = {pool.submit(_port_open, ip, 8080): ip for ip in hosts}
            for fut in as_completed(futures, timeout=12):
                if not fut.result():
                    continue
                base = f"http://{futures[fut]}:8080"
                if _device_ok(base):
                    for f in futures:
                        f.cancel()
                    return base
    except Exception:
        return None
    return None


def _device_base(refresh: bool = False) -> str:
    """设备地址：HS_MCP_DEVICE > 上次发现的 > 扫网段。换电脑/换 IP 都靠这条。"""
    if DEVICE_ENV:
        return DEVICE_ENV
    if not refresh:
        try:
            cached = DEVICE_CACHE.read_text().strip()
        except OSError:
            cached = ""
        if cached and _device_ok(cached):
            return cached
    found = _scan_lan()
    if found is None:
        raise RuntimeError(
            "找不到小智设备：确认它已开机并与这台电脑在同一 Wi-Fi；"
            "或用 HS_MCP_DEVICE=http://<ip>:8080 显式指定")
    try:
        SHOT_DIR.mkdir(parents=True, exist_ok=True)
        DEVICE_CACHE.write_text(found)
    except OSError:
        pass
    _log(f"device found: {found}")
    return found


@tool()
def find_device(refresh: bool = True) -> str:
    """找到小智设备（跑固件的板子）的地址和当前投屏状态。

    看屏幕或投屏前不确定设备地址时用它；换了电脑、设备重启后地址变了也能重新扫出来。
    refresh=True 强制重新扫本网段（默认），False 只看缓存。
    """
    base = _device_base(refresh=refresh)
    try:
        st = _device_get("/airplay/status")
    except RuntimeError as e:
        return f"{base} ({e})"
    where = "not mirroring"
    if st.get("running"):
        where = f"mirroring to {st.get('host') or '?'} (source={st.get('source')})"
    return f"device: {base} — {where}"


def _device_get(path: str, timeout: float = 8.0) -> dict:
    base = _device_base()
    try:
        with urllib.request.urlopen(f"{base}{path}", timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
    except Exception as e:
        raise RuntimeError(f"小智设备 {base} 不可达：{e}")
    try:
        return json.loads(raw)
    except ValueError:
        return {"raw": raw}


def _discover() -> list[dict]:
    """设备看到的接收器列表（阻塞 browse，约几秒）。"""
    out = _device_get("/airplay/discover", timeout=20)
    if isinstance(out, list):
        return out
    return out.get("receivers") or []


def _capture_list() -> tuple[list[dict], list[dict]]:
    """capture-cli list 的显示器与窗口（窗口按面积降序，主窗口在前）。"""
    out = run([_capture_bin(), "list"], timeout=20)
    displays, windows = [], []
    for line in out.splitlines():
        m = re.match(r'\s*\[(\d+)\] id=\d+ pixels=(\d+)x(\d+)', line)
        if m:
            displays.append({"index": int(m[1]), "w": int(m[2]), "h": int(m[3])})
            continue
        m = re.match(r'\s*id=(\d+) app="(.*)" title="(.*)" size=(\d+)x(\d+)', line)
        if m:
            windows.append({"id": int(m[1]), "app": m[2], "title": m[3],
                            "w": int(m[4]), "h": int(m[5])})
    windows.sort(key=lambda w: w["w"] * w["h"], reverse=True)
    return displays, windows


def _even(w: int, h: int) -> tuple[int, int]:
    return max(2, w - w % 2), max(2, h - h % 2)


def _cast_pid() -> int | None:
    """返回还活着的投屏进程 pid，顺手清掉残留的 pid 文件。"""
    try:
        pid = int(CAST_PID.read_text().strip())
    except (OSError, ValueError):
        return None
    try:
        os.kill(pid, 0)
        return pid
    except OSError:
        CAST_PID.unlink(missing_ok=True)
        return None


def _ensure_mirroring(tv: str = "") -> str:
    """让设备投屏到电视（tv 空 = 复用当前/上次的接收器）。返回 "" 或提示文本。"""
    st = _device_get("/airplay/status")
    if st.get("pin_required"):
        return "pin required"
    if st.get("running"):
        if not tv:
            return ""
        # 用户点名了电视：已投到别处就先停（同台重说也只是重连一次）
        _device_get("/airplay/stop")
        for _ in range(20):
            if not _device_get("/airplay/status").get("running"):
                break
            time.sleep(0.3)
        st = _device_get("/airplay/status")

    target = tv or (st.get("host") or "")
    legacy = "1" if st.get("legacy") else "0"
    if not target:
        # 设备重启后不再记住接收器：发现一次，只有一台就直接用，多台让用户选
        recv = _discover()
        if not recv:
            return "no AirPlay receiver on the LAN: turn the TV on and check it is on the same network"
        if len(recv) > 1:
            names = ", ".join(r.get("name", "?") for r in recv[:8])
            return (f"several AirPlay receivers are on the LAN ({names}): ask the user which TV "
                    f"to use, then call cast_to_tv again with tv=<that name>")
        target = recv[0].get("name") or recv[0].get("host", "")
        legacy = "1" if recv[0].get("legacy") else "0"
    _device_get(f"/airplay/start?host={urllib.parse.quote(target)}&legacy={legacy}")
    for _ in range(24):
        st = _device_get("/airplay/status")
        if st.get("pin_required"):
            return "pin required"
        if st.get("running"):
            return ""
        time.sleep(0.5)
    return "the TV did not come up in time"


# ---- 拉流：设备反过来连这台电脑，断线自己重连 ----


def _my_ip() -> str:
    """这台电脑在设备能看到的那个网络上的地址（HS_MCP_CAST_HOST 可覆盖）。"""
    forced = os.environ.get("HS_MCP_CAST_HOST", "").strip()
    if forced:
        return forced
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True,
                             timeout=5, stdin=subprocess.DEVNULL).stdout
    except Exception as e:
        raise RuntimeError(f"读不到本机地址（{e}）；用 HS_MCP_CAST_HOST=<ip> 指定")
    device_ip = urllib.parse.urlsplit(_device_base()).hostname or ""
    same = device_ip.rsplit(".", 1)[0] + "." if device_ip.count(".") == 3 else ""
    fallback = ""
    for m in re.finditer(r"\binet (\d+\.\d+\.\d+\.\d+)", out):
        ip = m.group(1)
        if ip.startswith(("127.", "169.254", "100.64", "198.18", "198.19")):
            continue
        if same and ip.startswith(same):
            return ip
        fallback = fallback or ip
    if not fallback:
        raise RuntimeError("找不到本机的局域网地址；用 HS_MCP_CAST_HOST=<ip> 指定")
    return fallback


def _set_pull(target: str) -> dict:
    """让设备来这台电脑拉流（target 空 = 改回等别人推）。"""
    return _device_get(f"/airplay/pull?host={urllib.parse.quote(target)}")


def _close_quietly(sock) -> None:
    if sock is None:
        return
    try:
        sock.close()
    except OSError:
        pass


class _Relay:
    """把 capture-cli 的推流交给主动来拉的设备。

    capture-cli 不关心谁在收，它只连本机端口；设备拉流时连的是这台机器的网卡地址。
    两边都在这里落脚：来自 127.x 的是推流端，来自局域网地址的是设备。

    设备断开时推流端留着继续读（帧丢掉，它下一个关键帧就把参考链补回来），下一台
    设备连上接的是同一条流——设备重启后自己连回来，没有人需要重新发起投屏。
    """

    def __init__(self, port: int):
        self.port = port
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.listeners: list[socket.socket] = []
        self.capture: dict[int, socket.socket] = {}
        self.device: dict[int, socket.socket] = {}
        # 第一个 configure 包（几何 + SPS/PPS）和设备是否已经收到过数据。
        # capture-cli 只在连接建立时发一次 configure，设备重连时它早已发过了，
        # 没有这个包设备就一直停在“未配置”，画面永远不出现。
        self.configure: dict[int, bytes] = {}
        self.head_buf: dict[int, bytes] = {}
        self.forwarded: dict[int, bool] = {}

    def start(self) -> None:
        for port in (self.port, self.port + 1):
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                srv.bind(("0.0.0.0", port))
                srv.listen(4)
            except OSError as e:
                _close_quietly(srv)
                raise RuntimeError(
                    f"本机端口 {port} 起不来（{e}）；用 HS_MCP_RELAY_PORT 换一个")
            srv.settimeout(0.5)
            self.listeners.append(srv)
            threading.Thread(target=self._accept_loop, args=(srv, port),
                             daemon=True).start()

    def close(self) -> None:
        self.stop_event.set()
        for srv in self.listeners:
            _close_quietly(srv)
        with self.lock:
            socks = list(self.capture.values()) + list(self.device.values())
            self.capture.clear()
            self.device.clear()
        for sock in socks:
            _close_quietly(sock)

    def _accept_loop(self, srv: socket.socket, port: int) -> None:
        while not self.stop_event.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if addr[0].startswith("127."):
                old = self.capture.get(port)
                self.capture[port] = conn
                _close_quietly(old)
                threading.Thread(target=self._pump_capture, args=(conn, port),
                                 daemon=True).start()
                _log(f"relay: capture client on {port}")
            else:
                old = self.device.get(port)
                self.device[port] = conn
                _close_quietly(old)
                # 重连的设备错过了 configure（它已经当帧被丢掉过一次）：先补上
                cfg = self.configure.get(port)
                if cfg and self.forwarded.get(port):
                    try:
                        conn.sendall(cfg)
                        _log(f"relay: replayed the configure packet on {port}")
                    except OSError:
                        _close_quietly(conn)
                        self.device.pop(port, None)
                        return
                threading.Thread(target=self._pump_device, args=(conn, port),
                                 daemon=True).start()
                _log(f"relay: device {addr[0]} on {port}")

    def _track_configure(self, port: int, data: bytes) -> None:
        """记住视频端口的 configure 包，供后面的设备重连时重放。"""
        if port != self.port or port in self.configure:
            return
        buf = self.head_buf.get(port, b"") + data
        if len(buf) < 18:
            self.head_buf[port] = buf
            return
        if buf[:4] != b"APVN" or buf[4] != 1:
            self.head_buf.pop(port, None)  # 首包不是 configure，不必再找
            return
        need = 18 + int.from_bytes(buf[14:16], "big") + int.from_bytes(buf[16:18], "big")
        if len(buf) < need:
            self.head_buf[port] = buf
            return
        self.configure[port] = buf[:need]
        self.head_buf.pop(port, None)
        _log(f"relay: kept the {need}-byte configure packet for reconnects")

    def _pump_capture(self, conn: socket.socket, port: int) -> None:
        while not self.stop_event.is_set():
            try:
                data = conn.recv(65536)
            except OSError:
                break
            if not data:
                break
            self._track_configure(port, data)
            with self.lock:
                dev = self.device.get(port)
            if dev is None:
                continue
            try:
                dev.sendall(data)
                self.forwarded[port] = True
            except OSError:
                with self.lock:
                    if self.device.get(port) is dev:
                        self.device.pop(port, None)
                _close_quietly(dev)
        with self.lock:
            if self.capture.get(port) is conn:
                self.capture.pop(port, None)
        _close_quietly(conn)

    def _pump_device(self, conn: socket.socket, port: int) -> None:
        """设备发回来的只有音频的 arm byte（以及将来可能的控制字节）。"""
        while not self.stop_event.is_set():
            try:
                data = conn.recv(4096)
            except OSError:
                break
            if not data:
                break
            with self.lock:
                cap = self.capture.get(port)
            if cap is None:
                continue
            try:
                cap.sendall(data)
            except OSError:
                pass
        with self.lock:
            if self.device.get(port) is conn:
                self.device.pop(port, None)
        _close_quietly(conn)


_RELAY: _Relay | None = None


def _spawn_capture(args: list[str]) -> subprocess.Popen:
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    with CAST_LOG.open("a", encoding="utf-8") as log:
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=log,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    CAST_PID.write_text(str(proc.pid))
    _log("cast start: " + " ".join(args))
    return proc


def _cast_state_read() -> dict:
    try:
        state = json.loads(CAST_STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return state if isinstance(state, dict) else {}


def _cast_state_write(state: dict) -> None:
    try:
        SHOT_DIR.mkdir(parents=True, exist_ok=True)
        CAST_STATE.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


_watchdog_lock = threading.Lock()
_watchdog_started = False
# 刚重开过的会话给足建立时间：会话建立期间 status 也是 running=false，
# 不等就再开一次，只会把刚接上的会话又敲断
_last_reopen = 0.0


def _start_watchdog() -> None:
    """盯着推流进程：设备重启、电视重开会话、电脑睡眠都能把它弄死。

    拉流模式下设备会自己连回来，但 capture-cli 本身也因连接断开而退出，所以两种
    模式都需要这一层：进程没了就按记录下来的命令重起一路。
    """
    global _watchdog_started
    with _watchdog_lock:
        if _watchdog_started:
            return
        _watchdog_started = True
    threading.Thread(target=_watchdog_loop, daemon=True).start()


def _watchdog_loop() -> None:
    while True:
        time.sleep(5)
        try:
            state = _cast_state_read()
            if not state.get("args"):
                continue
            if _cast_pid() is None:
                _log("watchdog: the capture stream died, starting it again")
                _spawn_capture(state["args"])
                continue
            if state.get("pull"):
                _reopen_pull_session(state)
        except Exception as e:  # 守护线程不能再死一次
            _log(f"watchdog: {e}")


def _reopen_pull_session(state: dict) -> None:
    """设备重启后会话就没了：流还在拉，但没人在投屏。

    把会话接回去是拉流真正的意义——用户只说了一次“投上去”，设备重启、电视断电
    回来都不该再问一遍。停机意图由 stop_cast 表达（它会先把拉流目标清掉）。
    """
    global _last_reopen
    if time.time() - _last_reopen < 20:
        return
    try:
        st = _device_get("/airplay/status")
    except RuntimeError:
        return  # 设备不在线，下一轮再看
    if st.get("running") or st.get("pin_required"):
        return
    _log("watchdog: the device has no session, reopening it")
    _last_reopen = time.time()
    note = _ensure_mirroring(state.get("tv") or "")
    if note:
        _log(f"watchdog: cannot reopen the session ({note})")


@tool()
def cast_to_tv(app: str = "", tv: str = "", audio: bool = True, fps: int = 20,
               mode: str = "push") -> str:
    """把 Mac 上的某个应用窗口（或整个屏幕）投到电视上，电视改显示它而不是设备的界面。

    用户说“把某某投到电视”“让电视显示这个窗口”时用这个工具。
    app 传应用名（如 "TextEdit"、"Google Chrome"），只投该应用的窗口（同名多窗口取最大的）；
    app 留空则投整个屏幕。
    tv 传电视名称（如 "我的Play 5"，取自设备发现列表），空则复用当前/上次的电视；
    局域网里有多台接收器时会报出名字让用户选。
    audio=True 时连 Mac 的系统声音一起送过去（整个系统的输出声音，不是某个应用；默认开）。
    mode 默认 "push"：这台电脑主动推给设备。"pull"：反过来让设备来这台电脑拉，地址记在
    设备里，设备重启/断线后自己连回来（电视自动恢复），也适用于这台电脑在 NAT 后面
    或将来换成云上机器、无法被推的情况。
    停止投回设备界面用 stop_cast。投屏期间助手的声音留在设备扬声器（不占电视的音频通道）。
    """
    global _RELAY
    cli = _capture_bin()
    note = _ensure_mirroring(tv)
    if note == "pin required":
        return ("the TV is waiting for its 4-digit code: ask the user for it, run "
                "self.airplay.enter_pin with those digits, then call cast_to_tv again")
    if note:
        return note

    stop_cast()  # 同一时间只推一路：换窗口/屏幕先停旧的
    # 等设备看到推流断开：它会把会话切回自己的界面。不等的话新连接可能被当作同一个
    # 客户端，会话沿用旧的编码参数（尺寸变了就是花屏）
    for _ in range(15):
        if not _device_get("/airplay/status").get("capture_client"):
            break
        time.sleep(0.2)
    args = [cli, "record"]
    if app:
        _, windows = _capture_list()
        hit = [w for w in windows if app.lower() in w["app"].lower()]
        if not hit:
            names = sorted({w["app"] for w in windows if w["app"]})
            raise RuntimeError(
                f"没找到 {app} 的窗口。当前有窗口的应用：{', '.join(names[:15])}")
        win = hit[0]
        args += ["--window-id", str(win["id"])]
        label = f'{win["app"]} "{win["title"]}"' if win["title"] else win["app"]
        src_w, src_h = win["w"], win["h"]
    else:
        displays, _ = _capture_list()
        if not displays:
            raise RuntimeError("capture-cli 没列出显示器（屏幕录制权限？）")
        args += ["--display", str(displays[0]["index"])]
        label = "the whole screen"
        src_w, src_h = displays[0]["w"], displays[0]["h"]

    # 等比缩到宽 ≤960：文字密集窗口的关键帧能到几百 KB，原尺寸会让电视每秒顿一下
    w, h = _even(src_w, src_h) if src_w <= 960 else _even(960, round(src_h * 960 / src_w))
    if (w, h) != (src_w, src_h):
        args += ["--width", str(w), "--height", str(h)]
    args += ["--fps", str(fps)]
    if audio:
        args.append("--audio")
    pull = mode.strip().lower() == "pull"
    target = ""
    if pull:
        # relay 先就位：设备拉的是它的网卡地址，而 capture-cli 只会连本机端口
        if _RELAY is None:
            _RELAY = _Relay(RELAY_PORT)
            _RELAY.start()
        args += ["--tcp", f"127.0.0.1:{RELAY_PORT}"]
    else:
        dev_host = urllib.parse.urlsplit(_device_base()).hostname or ""
        args += ["--tcp", f"{dev_host}:8090"]
        target = f"{dev_host}:8090"

    proc = _spawn_capture(args)
    base_state = {"args": args, "mode": "pull" if pull else "push", "label": label, "tv": tv}
    _cast_state_write(base_state)
    if pull:
        target = f"{_my_ip()}:{RELAY_PORT}"
        # 设备把这个地址记在 NVS 里，重启后自己连回来，不需要谁再发一次投屏
        _set_pull(target)
        _cast_state_write({**base_state, "pull": target})

    # 设备检测到推流客户端后会自己重开会话切源，所以这里等会话真的跑在 capture 上：
    # source 是会话开始时就定下的，单看它会被上一场会话骗到
    st = {}
    for _ in range(60):
        # 看 pid 文件而不是 proc 对象：watchdog 可能已经把它重起过一次了
        if _cast_pid() is None:
            tail = CAST_LOG.read_text(encoding="utf-8", errors="replace")[-400:].strip()
            raise RuntimeError(f"capture-cli 退出了：{tail}")
        st = _device_get("/airplay/status")
        if st.get("running") and st.get("capture_client") and st.get("source") == "capture":
            size = f"{w}x{h}@{fps}fps" + (" + audio" if audio else "")
            how = (f"the device pulled it from {target}" if pull
                   else f"pushed to {target}")
            return f"casting {label} to the TV ({size}, {how})"
        time.sleep(0.5)
    return (f"started sending {label} to {target}, but the device still reports "
            f"source={st.get('source')!r} capture_client={st.get('capture_client')!r}; "
            f"check {CAST_LOG} and the device log")


@tool()
def stop_cast() -> str:
    """停止把 Mac 窗口/屏幕投到电视，电视回到设备自己的界面。"""
    global _RELAY
    state = _cast_state_read()
    if state.get("pull"):
        # 留着的拉流目标会让设备重连回来，电视就停不下来
        try:
            _set_pull("")
        except RuntimeError:
            pass
    if _RELAY is not None:
        _RELAY.close()
        _RELAY = None
    CAST_STATE.unlink(missing_ok=True)
    pid = _cast_pid()
    if pid is None:
        return "not casting"
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(os.getpgid(pid), sig)
        except OSError:
            break
        for _ in range(20):
            try:
                os.kill(pid, 0)
            except OSError:
                CAST_PID.unlink(missing_ok=True)
                return "stopped: the TV is switching back to the device screen"
            time.sleep(0.25)
    CAST_PID.unlink(missing_ok=True)
    return "stopped: the TV is switching back to the device screen"


@tool()
def clipboard_set(text: str) -> str:
    """把文字写入系统剪贴板。"""
    return hs(f"hs.pasteboard.setContents({lua_q(text)}) return 'ok'")


@tool()
def clipboard_get() -> str:
    """读取系统剪贴板里的文字。"""
    return hs("return hs.pasteboard.getContents() or ''")


@tool()
def say(text: str) -> str:
    """用语音合成（TTS）在电脑上朗读一段文字。"""
    return hs(f"hs.speech.new():speak({lua_q(text)}) return 'ok'")


@tool()
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


def mask_url(url: str) -> str:
    """把 URL 里的 query 掩掉（含 token，不进日志）。"""
    from urllib.parse import urlsplit, urlunsplit
    s = urlsplit(url)
    if s.query:
        s = s._replace(query="token=***")
    return urlunsplit(s)


async def run_xiaozhi(url: str) -> None:
    """小智 MCP 接入点模式：连 wss://api.xiaozhi.me/mcp/?token=…，

    帧协议是裸 JSON-RPC（MCP 消息逐帧收发）。进程内把 WebSocket 帧桥接到
    FastMCP 内部的 lowlevel Server（与 stdio 模式同一套工具逻辑），
    断线自动重连。
    """
    import anyio
    import websockets
    from mcp.shared.session import SessionMessage
    from mcp.types import JSONRPCMessage

    server = mcp._mcp_server  # FastMCP 内部的 lowlevel Server；已 pin mcp<2
    init_opts = server.create_initialization_options()
    backoff = 2

    while True:
        try:
            print(f"[xiaozhi] 连接 {mask_url(url)} …", file=sys.stderr, flush=True)
            async with websockets.connect(url, open_timeout=10) as ws:
                print("[xiaozhi] 已连接，等待云端初始化", file=sys.stderr, flush=True)
                rx_send, rx = anyio.create_memory_object_stream(64)
                tx, tx_recv = anyio.create_memory_object_stream(64)

                async def incoming():
                    try:
                        async for raw in ws:
                            try:
                                # 与 SDK stdio reader 相同：SessionMessage 是 dataclass，
                                # 真正的 pydantic 解析在内层 JSONRPCMessage 上
                                await rx_send.send(SessionMessage(
                                    message=JSONRPCMessage.model_validate_json(raw)))
                            except Exception as e:
                                print(f"[xiaozhi] 丢弃无法解析的消息: {e}",
                                      file=sys.stderr, flush=True)
                    except websockets.ConnectionClosed:
                        pass
                    finally:
                        rx_send.close()

                async def outgoing():
                    try:
                        async for msg in tx_recv:
                            await ws.send(
                                msg.message.model_dump_json(by_alias=True, exclude_none=True))
                    except websockets.ConnectionClosed:
                        pass
                    finally:
                        tx_recv.close()

                async with anyio.create_task_group() as tg:
                    tg.start_soon(incoming)
                    tg.start_soon(outgoing)
                    try:
                        await server.run(rx, tx, init_opts, raise_exceptions=False)
                    except Exception as e:
                        print(f"[xiaozhi] 会话结束: {e!r}", file=sys.stderr, flush=True)
                    tg.cancel_scope.cancel()
            backoff = 2
            print("[xiaozhi] 连接断开，2 秒后重连", file=sys.stderr, flush=True)
        except Exception as e:
            print(f"[xiaozhi] 连接失败: {e}，{backoff} 秒后重试",
                  file=sys.stderr, flush=True)
        await anyio.sleep(backoff)
        backoff = min(backoff * 2, 30)


def main() -> None:
    _start_watchdog()
    argv = sys.argv[1:]
    if "--http" in argv:
        uvicorn.run(
            build_http_app(),
            host=os.environ.get("HS_MCP_HOST", "127.0.0.1"),
            port=int(os.environ.get("HS_MCP_PORT", "8390")),
            log_level="info",
        )
    elif "--xiaozhi" in argv:
        i = argv.index("--xiaozhi")
        if i + 1 >= len(argv):
            print("用法: server.py --xiaozhi 'wss://api.xiaozhi.me/mcp/?token=你的token'",
                  file=sys.stderr)
            sys.exit(2)
        import anyio
        anyio.run(run_xiaozhi, argv[i + 1])
    else:
        mcp.run()  # stdio


if __name__ == "__main__":
    main()
