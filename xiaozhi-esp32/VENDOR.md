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

除下述补丁层与本文件外，目录内容与上游 v2.5.2 一致。**补丁层以外的代码不要改**：
小智侧的功能扩展放在 `main/airplay/`、`main/display/virtual_display.*` 和板级目录里，
对上游文件的改动集中在三个接线点（根 CMakeLists、main/CMakeLists、Kconfig），
这样升级上游时冲突面最小。

## 与 `firmware/` 的关系

AirPlay 协议栈（`fairplay` / `hap` / `airplay`）只保留一份，住在
`firmware/components/`；小智工程通过 ESP-IDF 的 `EXTRA_COMPONENT_DIRS` 直接引用
那份源码，不复制副本：

```cmake
# xiaozhi-esp32/CMakeLists.txt，必须在 include(project.cmake) 之前
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../firmware/components")
```

这样协议层的修复、测试、host 测试用例都只有一处。

除 VENDOR.md 外，本目录在上游基础上带有一层本地补丁（虚拟显示器 + AirPlay 发送端接入），改动文件：

- `CMakeLists.txt`：`EXTRA_COMPONENT_DIRS` 指向 `../firmware/components`；新版 GCC 对某个 vendored LCD 组件的 use-after-free 误报不再当作错误
- `main/CMakeLists.txt`：接入 `display/virtual_display.cc`、`airplay/airplay_service.cc`、`airplay/airplay_http.cc`；新增板型 `jc4880-tv`；`main` 依赖 `airplay` / `media` / `espressif__esp_h264` / `esp_http_server`
- `main/Kconfig.projbuild`：新增板型选项与 `AirPlay display` 菜单（`AIRPLAY_DEFAULT_HOST`、`AIRPLAY_DEVICE_NAME`）
- `main/display/virtual_display.{h,cc}`：无面板的 LVGL 后端，渲染进 PSRAM 全帧缓冲，供 AirPlay 编码
- `main/airplay/airplay_service.{h,cc}`：会话任务、HAP 身份持久化（NVS）、屏幕 → H.264 源（RGB565→I420→软编→AVCC）、MCP 工具
- `main/airplay/airplay_http.{h,cc}`：台架用 HTTP 调试口（8080：status/start/stop/pin）
- `main/boards/jc4880-tv/`：无屏板（JC4880P443C_I_W，ES8311 音频 + 虚拟显示器）
- `sdkconfig.defaults.jc4880tv`：该板的 sdkconfig 覆盖（C6 hosted、pre-rev3 P4）

协议栈本体不在本目录；升级上游前先确认这层补丁仍适用。

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
