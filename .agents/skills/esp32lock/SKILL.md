---
name: esp32lock
description: 多 agent 共享 ESP32 开发板的互斥锁协议。烧录、复位、读串口、真机测试前必须用 esp32lock 排队，否则烧录互打、日志串台。当任务涉及 ESP32 烧录/esptool/idf.py flash/串口日志/真机测试/esp32lock 时激活。
---

# ESP32 板卡共享锁（esp32lock）

一块 ESP32 板被多个 agent 会话共用。所有"上板"操作必须持锁进行，别的 agent 用板时你排队等，锁空闲立刻轮到你。

## 铁律

1. 凡是碰板子的操作——`esptool`、`idf.py flash`、复位、串口读写、拉起 pin_bridge、真机功能测试——**必须**包进锁里。
2. **推荐用法**：把整个测试会话（pkill bridge → 烧录 → 复位 → 抓日志 → curl 验证 → 清理）包在**一条** `exec` 里，锁自动获取/释放，进程被杀也自动释放：

   ```bash
   tools/esp32lock exec 600 -- bash -c '
     pkill -f pin_bridge.py
     cd firmware && idf.py flash
     python -m esptool --port /dev/cu.usbmodem1101 --after watchdog-reset chip-id
     # ...抓日志、验证...
   '
   ```

   脚本在仓库 `tools/esp32lock`（各 worktree 都有副本，锁是共享的）。第一个数字是排队超时秒数（默认 600），超时退出非 0：**不要绕过锁硬上**，用 `status` 看谁占着，稍后重试。
3. 烧录前 `ls /dev/cu.usbmodem*` 现查端口（口名会漂移），这属于会话内部操作。
4. 多步交互式长会话用 `begin`/`end`；`begin` 输出 holder pid，结束时必须 `end <pid>`，否则最长被占 `ESP32_LOCK_MAX_HOLD`（默认 900s）后会被等待方强抢。

## 命令速查

```bash
tools/esp32lock status            # 板子被谁占着（free / BUSY holder=... label=...）
tools/esp32lock wait 120          # 纯等板子空闲（最多 120s）
tools/esp32lock exec 600 -- CMD   # 排队→持锁跑 CMD→自动释放（推荐）
tools/esp32lock begin my-test     # 开始交互会话，返回 holder pid
tools/esp32lock end <holder_pid>  # 结束会话（pid 不符会拒绝，防止误杀别人的会话）
```

排队时每 30s 打一行进度（谁占着、已等多久）；会话超过 MAX_HOLD 未释放会被等待方自动抢走并打 `STEALING` 日志。

## 环境变量

- `ESP32_BOARD`（默认 `board`）：锁名字空间，一板一名。以后有多块板时按板设不同值即可并行。
- `ESP32_LOCK_MAX_HOLD`（默认 900）：会话最长持有秒数。
- `ESP32_LOCK_DIR`（默认 `/tmp/esp32lock`）：锁文件目录，**仓库外**，所有 worktree 共用同一把锁。

## 实现与坑（改脚本前必读）

- 测试在 `tools/esp32lock_test.sh`（18 个场景）。锁用 brew 版 `flock`（`/opt/homebrew/bin/flock`）。
- **flock 挂在 fd 上，fork 的子进程会继承锁**。所以：holder 的 sleep 用 `sleep 60 9>&- &` 显式关 fd；`exec` 的子命令一律 `"$@" 9>&-`。任何新加的子进程若继承持锁 fd，holder/主进程死后会变成无 meta 的"匿名持有者"把板子锁死且无法被抢。
- `status` 以真实 flock 状态为准（`flock -n file true` 探测），meta 文件只是展示信息；锁空闲时顺手清残留 meta。
- holder 的信号处理靠 `wait` 打断 + trap，`kill` 它立即释放；`end` 带 pid 校验和 kill -9 补刀。
- `exec` 里不要留碰板子的孤儿后台进程（如常驻日志 reader）——锁不管它们，下个会话开头顶部 `pkill` 即可（现有工作流）。
