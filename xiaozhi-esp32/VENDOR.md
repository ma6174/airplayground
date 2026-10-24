# Vendored upstream: 78/xiaozhi-esp32

本目录是上游小智固件的原样快照，随本仓库一起提交，便于在一个工作区里同时改
AirPlay 发送端（`firmware/`）和小智固件（本目录）。

| 项 | 值 |
|---|---|
| 上游仓库 | <https://github.com/78/xiaozhi-esp32> |
| 版本 | `v2.5.2`（轻量标签） |
| 提交 | `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e` |
| 提交时间 | 2026-10-12T02:38:23+08:00 |
| 提交说明 | `ci: add manual full-matrix firmware release workflow (#2302)` |
| 导入方式 | `git archive v2.5.2 \| tar -x -C xiaozhi-esp32`（不含 `.git`，未含任何 submodule） |
| 导入日期 | 2026-10-24 |

除本文件外，目录内容与上游 v2.5.2 一致。**不要在本目录里改代码**：需要改动的地方
（板级、Kconfig、工具注册）都在 `firmware/` 侧或通过后续补丁进行，这样升级上游时
不会产生冲突。

## 与 `firmware/` 的关系

AirPlay 协议栈（`fairplay` / `hap` / `airplay`）只保留一份，住在
`firmware/components/`；小智工程通过 ESP-IDF 的 `EXTRA_COMPONENT_DIRS` 直接引用
那份源码，不复制副本：

```cmake
# xiaozhi-esp32/CMakeLists.txt，必须在 include(project.cmake) 之前
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../firmware/components")
```

这样协议层的修复、测试、host 测试用例都只有一处。

## 升级步骤

```sh
# 1. 在任意 upstream clone 里取目标版本的树
git fetch --depth=1 origin tag vX.Y.Z

# 2. 用归档替换本目录内容（先清掉旧文件，避免版本间删除的文件残留）
git rm -r --cached xiaozhi-esp32 >/dev/null
find xiaozhi-esp32 -mindepth 1 -maxdepth 1 ! -name VENDOR.md -exec rm -rf {} +
git archive vX.Y.Z | tar -x -C xiaozhi-esp32

# 3. 更新本文件的版本 / 提交 / 时间
# 4. 提交 PR
```
