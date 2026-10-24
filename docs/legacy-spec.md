# Legacy AirPlay Mirroring（老式 AirTunes 镜像）发送端实现规格

> 依据：RPiPlay 接收端源码（`/Users/ma6174/airplay/rpiplay/lib/`，下文引用格式 `RPiPlay lib/文件:行号`）反推发送端必须产生的字节。所有结论凡标「⚠ 推断」者，RPiPlay 源码中无对应证据，来自业界通用实现（iOS 抓包模板/pyatv 特性表），对 RPiPlay 系接收端不作强依赖。映射对象：`firmware/components/airplay|fairplay|hap`。

---

## 1. 会话总流程

接收端为 RTSP 服务器（`raop.c` httpd），路由表见 `RPiPlay lib/raop.c:161-208`。所有响应默认 `RTSP/1.0 200 OK` + 回显 `CSeq` + `Server: AirTunes/220.68`（`RPiPlay lib/raop.c:153-157`）。发送端全程使用**一条 TCP 控制连接**（端口 = mDNS SRV，如极米 7001），顺序：

```
① TCP connect SRV 端口
② OPTIONS * RTSP/1.0                      → 200 + Public 头
③ POST /pair-setup   body 32B             → 200 body 32B
④ POST /pair-verify  V1 body 68B          → 200 body 96B
⑤ POST /pair-verify  V2 body 68B          → 200 body 0B
⑥ POST /fp-setup     m1 body 16B          → 200 body 142B (m2)
⑦ POST /fp-setup     m3 body 164B         → 200 body 32B (m4)
⑧ OPTIONS（iOS 会重复发一次，可选）
⑨ ANNOUNCE rtsp://<ip>/<uuid>  body SDP    → 200 空体
⑩ SETUP  body bplist{eiv,ekey,timingPort} → 200 bplist{eventPort,timingPort}   ← "SETUP 1"
⑪ SETUP  body bplist{streams:[110]}       → 200 bplist{streams:[{dataPort}]}    ← "SETUP 2"
⑫ RECORD                                  → 200（Audio-Latency/Audio-Jack-Status 头）
⑬ [并行] UDP NTP 应答循环 + TCP 镜像数据循环 + POST /feedback 心跳
⑭ TEARDOWN                                → 200；关三条通道
```

- 每个方法路由：`/info`、`/pair-setup`、`/pair-verify`、`/fp-setup`、`OPTIONS`、`SETUP`、`RECORD`、`/feedback` 等（`RPiPlay lib/raop.c:161-180`）。`ANNOUNCE` 无 handler → 落默认 200 空体（`RPiPlay lib/raop.c:152-156`，路由表无 ANNOUNCE 分支）。
- 无处理分支返回的也是 200（如 `GET_PARAMETER` 未知参数只 log，`RPiPlay lib/raop_handlers.h:465-500`）。
- TEARDOWN 后接收端销毁 RTP/mirror 会话（`RPiPlay lib/raop.c:197-208`）。

---

## 2. RTSP 层与配对（legacy pair，非 TLV8）

### 2.1 OPTIONS
请求 `OPTIONS * RTSP/1.0`（或完整 URI）。响应带 `Public: SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER`（`RPiPlay lib/raop_handlers.h:293-296`）。无鉴权检查。

### 2.2 POST /pair-setup（legacy：裸 Ed25519 公钥，非 TLV8）
- 发送端 body = **32 字节本端 Ed25519 公钥**，长度必须恰为 32（`RPiPlay lib/raop_handlers.h:177`）。
- 响应 body = 32 字节接收端 Ed25519 公钥，`Content-Type: application/octet-stream`（`RPiPlay lib/raop_handlers.h:183-190`）。
- 接收端据此把配对状态置为 SETUP（后续 pair-verify 的前置状态检查 `RPiPlay lib/raop_handlers.h:206-208` + `pairing.c:126-143`）。

### 2.3 POST /pair-verify（两步，原始格式）
**V1**：body 68 字节（`RPiPlay lib/raop_handlers.h:213`）：
```
[0]      = 0x01            类型
[1..3]   = 0x00 0x00 0x00  填充
[4..35]  = 发送端 X25519 公钥 (32B, 原始值)
[36..67] = 发送端 Ed25519 公钥 (32B)
```
响应 body 96 字节（`RPiPlay lib/raop_handlers.h:227-231`）：`[0..31]` 接收端 X25519 公钥 + `[32..95]` 64B 签名（已加密，见下）。

**V2**：body 68 字节：`[0]=0x00` + 3 零 + `[4..67]` 发送端 64B 签名（加密态）。响应 200 空 body（`RPiPlay lib/raop_handlers.h:235-249`）。

签名与加密细节（`RPiPlay lib/pairing.c`）：
- 共享密钥 `ecdh_secret = X25519(我私, 收公)`（`pairing.c:152`）。**该值是 legacy 流的唯一跨层密钥材料**（进 §5/§7 派生链）。
- 派生 `key = SHA512("Pair-Verify-AES-Key" || ecdh_secret)[0..16]`、`iv = SHA512("Pair-Verify-AES-IV" || ecdh_secret)[0..16]`（`pairing.c:25-26,41-55,193-194`）。
- 接收端签名内容 = `收X25519公钥 || 发X25519公钥`（`pairing.c:187-188`）→ Ed25519 签名 → AES-128-CTR 从**块 0**（原始 IV）加密后放进 V1 响应（`pairing.c:190-199`）。发送端须用同 key/iv 解密验证。
- 发送端 V2 签名内容 = `发X25519公钥 || 收X25519公钥`（`pairing.c:229-234` 验证序），加密时**先空转一块**（keystream 从块 1 开始，`pairing.c:222-227`「One fake round」）。
- 映射：Ed25519 签名复用 `hap_identity_sign()`；X25519 keygen/shared-secret 目前封装在 `hap_verify_*` TLV8 流程内，需在 hap 组件导出原始接口（小改）。

---

## 3. FairPlay v3 ekey 链（位置与派生）

fp-setup 是独立于 pair-verify 的第二把锁，发生在 `ANNOUNCE` 之前（`RPiPlay lib/raop.c:167-168` 路由；`raop_handlers.h:258-281`）：

**m1（发送端→）**：16 字节 FPLY 记录。接收端只校验 `m1[4]==0x03`（版本），读取 `m1[14]` 为 mode(0..3)（`RPiPlay lib/fairplay_playfair.c:43,48`）；其余字节由我方 `fp_message1()` 构造。响应 m2 = **142 字节固定表值** `reply_message[mode]`（`fairplay_playfair.c:8-11,49`，头 `46 50 4c 59 03 01 02 00 00 00 00 82` = FPLY/3/1/2 + BE 长度 0x82=130）。

**m3（发送端→）**：164 字节。接收端原样保存为 `keymsg`（`fairplay_playfair.c:64`）。
**m4（←发送端）**：32 字节 = `fp_header`（`46 50 4c 59 03 01 04 00 00 00 00 14`，`fairplay_playfair.c:13`）+ `m3[144..164)` 原 20 字节回显（`fairplay_playfair.c:66-68`）。我方 `fp_confirm_m4()` 校验同一区间。

**ekey 的解密位置（接收端）**：SETUP 收到 72B ekey 后调 `fairplay_decrypt(keymsg=m3, ekey) → aeskey[16]`（`RPiPlay lib/raop_handlers.h:364` → `fairplay_playfair.c:75-81` → `playfair/playfair.c:13-31`）：
- `sapKey = SAPHash(default_sap, m3)`，`default_sap` 为固定常量（`playfair.c:21`，`omg_hax.c:522`：`00 03 00 00 … 0x79 填充…`）；
- `chunk1 = ekey[16..32)`，`chunk2 = ekey[56..72)`；`keyOut = cycle(chunk2) ⊕ chunk1` 再两次异或混淆（`playfair.c:15-28`）。
- 我方 `fp_wrap_key()` 产出的 ekey 布局与此严格对位：`[0..11]` FPLY/1/2/1 头+BE 长度 0x3C、`[12..15]` 零、`[16..31]` 随机掩码（=chunk1）、`[32..35]` BE32=16、`[36..55]` HMAC-SHA1、`[56..71]` AES-128 加密块（=chunk2）。**整链复用，0 改动**（已过 474 向量）。

**密钥总派生链（legacy 路径）**：
```
aeskey  = 随机 16B（发送端生成；ekey 包装后送出）          raop_handlers.h:364
aesiv   = 随机 16B（明文进 SETUP1 eiv）                    raop_handlers.h:355-356
ecdh    = legacy pair-verify X25519 secret (§2.3)          pairing.c:152
ekey'   = SHA512(aeskey || ecdh)[0..16]  ←「混入配对密钥」  mirror_buffer.c:44-48
skey    = SHA512("AirPlayStreamKey"+str(connID) || ekey')[0..16]  mirror_buffer.c:52-61
siv     = SHA512("AirPlayStreamIV"+str(connID) || ekey')[0..16]   mirror_buffer.c:53-66
```
   - 「混入」的实现即我方 `fp_stream_key(raw_key, ecdh, 32, mix=true)`（`fpsap.c:478-501`，SHA512(raw||secret) 取前 16）——**legacy 与 HAP 用同一个函数，只换 secret 来源**：HAP 传 HAP pair-verify secret，legacy 传 §2.3 的 X25519 secret。这一级是否执行取决于接收端：RPiPlay 系总是执行（无配对时 secret 为全零），伪装 AppleTV3,2 的贴牌盒子则不混，直接用裸 key（§15.2）；固件用 `legacy_key_mode` 三档覆盖这两种，默认裸 key。
- `str(connID)` 是 streamConnectionID 的**十进制 ASCII**（`mirror_buffer.c:56-57` sprintf `%PRIu64`）。
- 第二级 `skey/siv` 派生不在 fairplay 组件内（它只到 ekey'），需在 airplay 组件新写（~40 行）。

---

## 4. SETUP：二进制 plist 逐字段

同一 handler 处理两种请求，按字段存在与否组合（`RPiPlay lib/raop_handlers.h:317-455`）。Content-Type `application/x-appble-binary-plist`（响应 `raop_handlers.h:453-454`）。

### SETUP 1（密钥 + 时基协商）
请求 root dict 字段（顶层，**不是** streams 内）：
| 字段 | 类型 | 内容 | 证据 |
|---|---|---|---|
| `eiv` | data(16) | 随机 AES-CTR IV（明文） | `raop_handlers.h:339,353-356` |
| `ekey` | data(72) | `fp_wrap_key()` 输出 | `raop_handlers.h:340,358-364` |
| `timingPort` | uint | 发送端 UDP NTP 应答端口 | `raop_handlers.h:370-372` |

响应 root dict：
| 字段 | 类型 | 内容 | 证据 |
|---|---|---|---|
| `eventPort` | uint | 接收端 RTSP 端口本身（events 与控制同口） | `raop_handlers.h:384-385` |
| `timingPort` | uint | 接收端 NTP 客户端源端口 | `raop_handlers.h:377,383` |

接收端此刻启动 NTP 客户端指向我方 timingPort（`raop_handlers.h:376-377`）并初始化 mirror 缓冲（`raop_handlers.h:380`）——**发送端 UDP 应答必须在此之前就绪**。

### SETUP 2（镜像流）
请求：`Transport: RTP/AVP/TCP` 头（`raop_handlers.h:341-346`，决定 use_udp）+ root `streams` 数组，一项：
| 字段 | 类型 | 内容 | 证据 |
|---|---|---|---|
| `type` | uint | **110**（镜像）；96 为音频 | `raop_handlers.h:396,403,428` |
| `streamConnectionID` | uint64 | 发送端随机生成；进 §3 派生串与流标识 | `raop_handlers.h:406-409` |

响应：`streams` 数组，一项 `{dataPort: uint(接收端监听口), type: 110}`（`raop_handlers.h:423-425`）。接收端在该口 `listen(1)`（`RPiPlay lib/raop_rtp_mirror.c:516`），由**发送端发起 TCP connect**（`raop_rtp_mirror.c:208` accept）。

可选头：`DACP-ID`/`Active-Remote`（遥控用，`raop_handlers.h:325-335`），镜像可不发。

---

## 5. ANNOUNCE（SDP）

RPiPlay 对 ANNOUNCE **零解析**：路由表无分支，返回 200 空 body（`RPiPlay lib/raop.c:161-208` 无 ANNOUNCE 项；`raop.c:152-156`）。因此对 RPiPlay 系接收端只需：合法 RTSP 头 + `Content-Type: application/sdp` + CSeq。⚠ 推断（真 AppleTV3,2 会浅校验；iOS 实抓模板，字段按此发送最稳）：
```
v=0
o=iTunes <u32 会话号> 0 IN IP4 <发送端IP>
s=iTunes
c=IN IP4 <接收端IP>
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 mpeg_id=IMG;slice_ctrl=MCST;profile-level-id=4294967295;parameter-sets=<base64(SPS,PPS)>
```
URI 用 `rtsp://<接收端IP>/<uuid>`（RECORD/TEARDOWN 沿用；RPiPlay 不看 URL）。

---

## 6. 镜像数据通道：128 字节头逐字节

单条 TCP（发送端→接收端），记录流 = `128B 头 + payload_size B 载荷`。接收端先收满 128 再按 `payload_size` 收载荷（`RPiPlay lib/raop_rtp_mirror.c:243-249,270-275`）。**头字段全部小端**（`byteutils.c:27-30` 原生 LE 读取）：

| 偏移 | 长度 | 类型 0（视频 VCL） | 类型 1（SPS/PPS） | 证据 |
|---|---|---|---|---|
| 0 | 4 | u32 LE 载荷长度（不含头） | 同左 | `raop_rtp_mirror.c:262` |
| 4 | 2 | u16 LE，低字节 0x00（高字节 iOS 置 0x10 表 IDR，RPiPlay 掩掉） | 低字节 0x01 | `raop_rtp_mirror.c:263`（`& 0xff`） |
| 6 | 2 | u16 LE option，0 | 0 | `raop_rtp_mirror.c:264` |
| 8 | 8 | u64 LE **NTP 32.32 格式、无 1900 纪元**（即启动微秒：高 32=秒，低 32=小数） | 未读 | `raop_rtp_mirror.c:294-300` |
| 16..39 | 24 | 全零 | 全零 | — |
| 40 | 4 | — | float LE 源宽 | `raop_rtp_mirror.c:350` |
| 44 | 4 | — | float LE 源高 | `raop_rtp_mirror.c:351` |
| 48..55 | 8 | 全零 | 全零 | — |
| 56 | 4 | — | float LE 显示宽 | `raop_rtp_mirror.c:352` |
| 60 | 4 | — | float LE 显示高 | `raop_rtp_mirror.c:353` |
| 64..127 | 64 | 全零 | 全零 | — |

**类型 1 载荷（不加密**，`raop_rtp_mirror.c:357`）：avcC 记录 `version,profile,compat,level,[4]=0xFF,[5]=0xE0|sps数,[6..7]BE sps_len,SPS,num_pps,[..2]BE pps_len,PPS`（`raop_rtp_mirror.c:359-375`）。⚠ 接收端 pps_len 解析有缺陷（`raop_rtp_mirror.c:370`），pps 保持小（正常 4–8B）即可。
**类型 0 载荷（整体加密）**：AVCC 长度前缀 NAL 串（4B BE 长度），接收端先解密再按长度切 NAL（`raop_rtp_mirror.c:309-352`）。
**类型 2（心跳等）**：RPiPlay 无分支、静默丢弃（`raop_rtp_mirror.c:287,347` 之外直接 `free`，`:405`）——可发可不发。

---

## 7. AES-128-CTR：连续 keystream 语义（关键正确性点）

- 算法：AES-128-CTR，key=`skey`，counter 初值=`siv`，**整个 TCP 会话共用一个上下文**（`mirror_buffer.c:80` 仅在 SETUP2 时 init 一次；`aes_ctr_start_fresh_block` 只对齐块界、不重置计数器，`crypto.c:133-137`）。
- 接收端解密器用 `og` 残块机制实现**逐字节连续 keystream**：记录 k 的尾部与记录 k+1 的头部共享同一 keystream 块（`mirror_buffer.c:104-131`）。等价的发送端模型：**维护一个跨记录、跨帧连续推进的 CTR keystream；每条载荷按其在流中的字节位置 XOR**，任何记录边界都不重置 IV、不跳块。用 PSA 实现时自管 16B 计数器（keystream 块 n = AES-ECB(skey, siv⊕n)），或维持 multipart cipher。
- ⚠ 接收端角落 bug：若某记录长度 < 上条记录残块余量（`nextDecryptCount`），接收端会过读（`mirror_buffer.c:105-111` 对 `(inputLen-next)` 为负时 `encryptlen=0` 但 head 循环仍按 next 读）。规避法：类型 0 记录始终 ≥ 1KB（整 AU 发送即自然满足），不发明文短记录。
- 与 `ap_frame.c`（ChaCha20/HAP 版）的 diff：
  | 项 | HAP 版（现状） | legacy 版 |
  |---|---|---|
  | 载荷加密 | ChaCha20-Poly1305，per-record nonce | AES-128-CTR，**连续** keystream，无认证标签 |
  | 头 @40..47 | u64 LE timeline_id（`ap_frame.c:56`） | float LE 源宽/高（仅类型 1 用，`raop_rtp_mirror.c:350-351`） |
  | 类型 1 头 [6..7] | 0x16,0x01（`ap_frame.c:39-40`） | option=0；载荷才是 avcC |
  | 时间戳 | 接收端 PTP 锚定时间线（`ap_ntp.c:44-58`） | 本机启动钟，无锚（接收端用 NTP offset 折算，`raop_ntp.c:425-430`） |
  | 密钥来源 | streams[110].shk/shiv + HAP secret（`ap_setup.c:336-338`） | SETUP1 顶层 eiv/ekey + legacy ecdh（§3） |
  头部 u32/u64/float 小端写入函数与 IDR 探测 `ap_avcc_has_idr` 直接复用（`ap_frame.c:9-33,74-83`）。

---

## 8. NTP 时基

- **方向**：接收端每 3 秒向我方 timingPort 发 32B 请求 `{0x80,0xd2,0x00,0x07,0×24 零, 本端发包时刻 BE-NTP@24}`（`RPiPlay lib/raop_ntp.c:246-248,265,324-329`）；发送端是 **NTP 服务器**。
- 应答 32B：`{0x80,0xd3,0x00,0x07,…}`，`t0`@8、`t1`@16、`t2`@24 均为 **BE、自 1900 秒的 NTP 32.32**（`raop_ntp.c:282-286` 解析并减 1900→1970 偏移，`byteutils.c:93-99`）。`t0`=请求离端时刻（可回显请求@24，`raop_ntp.c:265` 写在请求@24）、`t1`=到达时刻、`t2`=发出时刻。
- **我方 `ap_ntp.c:70-112` 已完整实现该应答**（0xd2 判别、回显@8、BE 纪元@16/24）→ 复用，0 改动。
- 视频头时间戳（§6 @8）与此不同：**LE、无 1900 纪元、启动微秒的 32.32 编码**——`ap_ntp_boot_timestamp()` 格式正好（`ap_ntp.c:17-27`）。两条时间线靠接收端 NTP offset 折算（`raop_ntp.c:425-430`）。

## 9. 保活与拆除

- `POST /feedback`（空 body）≈ 每 2s：RPiPlay 仅 log 并回 200（`raop.c:177-178`、`raop_handlers.h:561-566`）。现有 `send_feedback()`（`ap_session.c:684-687`）复用。
- `FLUSH`（带 `RTP-Info: seq=`）仅音频有意义（`raop.c:181-196`）；纯镜像跳过。
- `TEARDOWN`：任意时刻可发，响应 200 后接收端销毁两套 RTP 会话（`raop.c:197-208`）。连接断开同样触发销毁（`raop.c:268-285`）。

## 10. 鉴权

- **Apple-Challenge/Response**：RPiPlay 全库零出现（`grep Apple-Challenge|Apple-Response` 无命中）——RPiPlay 系第三方接收端**永不需要**。真 AppleTV 在部分请求上要求（⚠ 推断，RPiPlay 无法证实；如需对接真机再实现）。本规格目标为第三方投影仪 → 不做。
- **AirPlay 密码 = HTTP Digest**（RFC2069 风格 MD5，`Authorization: Digest username="AirPlay", realm, nonce, uri, response`）：我方 `ap_client.c:390-420,440-452` 已实现 401 挑战重试 → 复用。
- **Basic auth**：AirPlay 生态不用 Basic；RPiPlay 无任何 auth 头处理。忽略。

## 11. 第三方接收器怪癖与判别法（极米实测）

特征组（全部来自实测 mDNS `_airplay._tcp` TXT / SRV）：
1. `model=AppleTV3,2` + `srcvers=220.68`：220.x 是 AirPlay1 时代版本串（RPiPlay 自己也广播 `AirTunes/220.68`，`RPiPlay lib/raop.c:157`，实为模仿 iOS 9 时代）。
2. `features=0x527FFFF7,0x1E`：与 RPiPlay 的 `/info` 广播值 `0x1E<<32|0x5A7FFFF7`（`RPiPlay lib/raop_handlers.h:55`）**高 32 位完全一致、低 32 位仅差 bit27**（0x5A7FFFF7⊕0x527FFFF7=0x08000000）。⚠ bit27 业界命名 LegacyPairing、bit41（高字 0x200）与 HAP/PTP 相关（pyatv/openairplay 特性表；RPiPlay 源只证实广播值，不证实语义；我方 `ap_session.c:107-115` 现按 bit27/41 分路）。**极米无 bit27、无 bit41 却确实走 legacy**——判别不能依赖单一 bit。
3. `_airplay._tcp` **SRV 端口 7001 ≠ 7000**：真实 AppleTV 固定 7000；RPiPlay 系/第三方随意。连接必须用 SRV 端口。
4. `GET /info` 回 200 但 **body 为空**（真 RPiPlay 回 binary plist，`raop_handlers.h:37-140`）。我方 `discover()` 现在把非 plist 判为失败（`ap_session.c:92-97`）→ 必须容忍空 body（小改）。
5. 判别结论：`model=AppleTV3,2` ∨ `srcvers` 220.x ∨ SRV≠7000 ∨ `/info` 空 body → **legacy**。四条任一命中即可，多命中加强。

## 12. 实现任务清单

| 档 | 项 | 量级 |
|---|---|---|
| 复用 | fairplay 整链 `fp_message1/fp_exchange_m3/fp_confirm_m4/fp_wrap_key`（§3） | 0 行 |
| 复用 | `fp_stream_key(mix=true)` 换 legacy secret 入参 | 0 行 |
| 复用 | `ap_plist.c` 二进制 plist 编解码（SETUP/响应） | 0 行 |
| 复用 | `ap_client.c` 请求构造/CSeq/Digest/UA | 0 行 |
| 复用 | `ap_ntp.c` NTP 应答任务（§8 完全对位） | 0 行 |
| 复用 | `ap_frame.c` 小端写入 + `ap_avcc_has_idr`；`hap_identity_sign` | 0 行 |
| 小改 | `ap_client.c` 明文模式（现经 `hap_channel_write`，legacy 直通，`ap_client.c:96-101,427`） | ~15 行 |
| 小改 | hap 组件导出原始 X25519 keygen/shared（现困在 `hap_verify_*` TLV8 内，`hap.h:53-63`） | ~50 行 |
| 小改 | `discover()` 容忍 `/info` 空 body + 产出判别结论（§11） | ~30 行 |
| 小改 | legacy SETUP1/2 plist 构造（顶层 eiv/ekey/timingPort、streams[110]，对照 `ap_setup.c:146-244` 现代版） | ~80 行 |
| 小改 | `ap_frame.c` legacy 头构造（type0 @8 LE 启动钟、@40 不放 timeline；type1 载荷 avcC） | ~30 行 |
| 小改 | media 帧 TLS 层：连续 CTR keystream 加密器（PSA 自管计数器，§7） | ~100 行 |
| 新写 | legacy pair-setup/pair-verify 状态机（§2.2-2.3，非 TLV8） | ~180 行 |
| 新写 | legacy 会话编排（§1 序列 ②-⑭，替换 `session_run` 的 HAP 前段） | ~150 行 |
| 新写 | 二级流密钥派生 skey/siv（§3 末，SHA512×2 + 十进制串接） | ~40 行 |
| 新写 | mDNS TXT 解析 + 双路径判别 | ~70 行 |

新写合计 ~440 行，小改 ~300 行；无新依赖（mbedtls 4.1 PSA 的 SHA-512/AES/X25519/Ed25519 均够用）。

## 13. 双路径共存：mDNS 判别 → HAP 或 legacy

```
browse _airplay._tcp → TXT{deviceid,features,model,srcvers,flags,pi,gid?}, SRV port
   │
   ├─ features 高32位≠0 且 bit41 置位 或 TXT 含 gid/pi + srcvers≥300   → HAP 路径（现状）
   ├─ §11 特征任一命中（model=AppleTV3,2 / srcvers 220.x / port≠7000）→ legacy 路径
   ├─ 模糊 → GET /info：binary plist 且 features 可读 → 按 bit27/bit41 分；空 body → legacy
   └─ 仍不明 → 默认 legacy（伪装者必 legacy）；禁先试 HAP 再降级（pairing backoff 反噬，ap_session.c:130-160）
```
两条路径共享：TCP 客户端、plist、NTP 应答、fairplay 链、帧发送/加密底座；分叉点仅在：配对前端（TLV8 vs 裸对）、SETUP 形态（3×SETUP+shk/shiv vs ANNOUNCE+2×SETUP+eiv/ekey）、加密（ChaCha20 per-record vs AES-CTR 连续）、时钟（PTP 锚 vs 启动钟+NTP）。判别结果缓存进接收端档案（按 deviceid），避免每次探测。

## 14. 验证清单

1. 单测：skey/siv 派生向量（用 RPiPlay `mirror_buffer_init_aes` 在 PC 上生成金标）。
2. 回环：以 RPiPlay 源码编译接收端，全链路走通 §1 十四步，画面出图。
3. 极米实机：连 7001 → `/info` 空 body 容忍 → 出图；断电恢复/TEARDOWN 后重连。
4. 角落：连续 keystream 跨帧一致性（第 1 帧与第 N 帧同ctx）；>16KB 记录分包（`recv` 循环天然支持，`raop_rtp_mirror.c:270-283`）；SPS/PPS 明文记录先行于 IDR。

## 15. 实机结论：伪装成 AppleTV3,2 的贴牌盒子（极米投影仪 220.68）

抓包对象：macOS 27 的屏幕镜像（`X-Apple-ProtocolVersion: 1`，不配对）对同一台盒子。
以下每条都由抓包或离线解密验证。

### 15.1 端口拓扑

| 端口 | 行为 |
| --- | --- |
| 7001 | mDNS SRV 公告的端口，**stub**：任何 RTSP 请求回 `HTTP/1.1 200 OK` + 空 body，`/info` 也空 |
| 7002 | **真正的 AirPlay RTSP 服务**：`fp-setup` 回 142B m2、SETUP 回 binary plist，Apple 的发送端也连这里 |
| 7100 | 与 7001 相同的 stub |
| 8080 | 厂商 Web UI（umi.js SPA） |

因此按 mDNS 端口直连会把整条会话打在 stub 上：SETUP 都「成功」但永远拿不到 dataPort，数据通道也无从建立。遇到 `SETUP 2` 回空 body 的接收端，先把 SETUP 打到「同设备另一个 RTSP 端口」试，通常就是 `公告端口 + 1`。

### 15.2 密钥派生（离线验证，可用于判定接收端是否走标准路径）

用 `playfair_decrypt(m3, ekey)` 从抓包里解出 Apple 会话的 AES key，再比对四种派生：
只有 **`mixed = aeskey`（不混任何配对密钥，直接进 `AirPlayStreamKey/IV` 哈希）** 能把 612 条视频记录全部解成合法访问单元；混零密钥、混真实配对密钥、裸 key+eiv 都解出乱码。

```
eaeskey   = aeskey                                # 无配对时不混 secret
skey      = SHA512("AirPlayStreamKey" + decimal(streamConnectionID) || eaeskey)[0:16]
siv       = SHA512("AirPlayStreamIV"  + decimal(streamConnectionID) || eaeskey)[0:16]
```

codec 记录（type 1）**不参与 keystream**，CTR 只在视频记录之间连续推进；把 codec 记录算进 keystream 会让后续每一帧都错位（曾经踩过）。

### 15.3 128 字节记录头（对着 Apple 抓包逐字节核对）

| 偏移 | 含义 |
| --- | --- |
| 0..3 | 载荷长度，小端 u32 |
| 4 | 记录类型：0 视频 / 1 codec |
| 5 | 0x10 标记同步帧（IDR），其余为 0 |
| 6..7 | codec 记录为 `0x0116`（小端 `16 01`），视频记录为 0 |
| 8..15 | boot 相对的 32.32 定点时间戳，小端（不带 1900 epoch） |
| 16..23 / 40..47 / 56..63 | codec 记录里的宽高 float（三处都写），视频记录留 0 |

发送端实测 30 帧/秒；音频是独立 UDP 通道，视频走分配到的 TCP 端口，帧为裸记录（无 `$` 交织）。

### 15.4 出画条件

按上述规格复刻时，这类盒子会「进入投屏、分配 dataPort、收流不断连」却黑屏。逐项验证后，出画依赖下面四条同时成立：

1. **标签必须完整**：`skey/siv` 的哈希输入是 `"AirPlayStreamKey" + decimal(streamConnectionID)` 再到 16 字节，19 位 id 让标签长 35 字符。用一个装不下的 `char label[32]` 调 `snprintf` 会静默截断成 31 字符，派生出「看着正常」但完全不同的 key，接收端每帧都解出噪声后静默丢弃——界面无任何报错，UxPlay 侧只报 `decryption of video packet failed`。实现里用 `AP_LEGACY_LABEL_MAX`（40）并由 `test_legacy.c` 用 19 位 id 的金标值锁住。
2. **id 取 SETUP 2 plist 里的 `streamConnectionID`**，不是 RTSP URI 末尾那串数：两者不同（Apple 实测 URI `1145513884605957177`，SETUP 2 里是 `-2456313425954716723`）。Apple 发的是有符号 64 位，标签里用它的无符号十进制形式（20 位）。
3. **密钥派生不混 secret**（§15.2 的裸 key 形式）。同一发送端连 RPiPlay/UxPlay 时需要先混一次 32 字节全零（它们总是执行混合步骤，无配对时 secret 为全零），所以固件把这件事做成 `legacy_key_mode` 三档。
4. **每个同步帧带 in-band SPS/PPS**（`legacy_inband_sps`）：这类盒子的解码器不读 codec 记录里的 avcC。

四条齐备后，静态画面在极米投影仪上正常出画，UxPlay 侧的解密错误计数归零。若仍有黑屏，先用 §15.1 的端口拓扑确认会话打的是真 RTSP 端口而不是 stub：stub 对任何请求都回 `HTTP/1.1 200 OK` 且永不流，会话看起来「成功」但没有 dataPort。固件在 legacy 路径上遇到 `HTTP/` 状态行会自动上探一个端口再重来一次。

### 15.5 严格接收端验证（UxPlay）

UxPlay 1.62（RPiPlay 血统）装在局域网 Linux 机器上后，发送端第一次被「会真的检查」的接收端验收：

| 环节 | 结果 |
| --- | --- |
| legacy pair-setup / pair-verify | 通过（修掉 bug 后，见下） |
| FairPlay m1/m2/m3/m4 | 通过，ekey 被正确解开 |
| SETUP 1 / SETUP 2 / RECORD | 通过，拿到 dataPort |
| 视频流解密 + 解码 | 通过（`-vdmp` 的 dump 持续增长） |

由此修掉两个真实 bug：
1. **pair-verify V2 的 keystream 要跳过 64 字节**（一个签名长度的「假轮」），跳 16 字节会让严格接收端直接断连；
2. **FairPlay m1 的 mode 字节**：iOS 用 2、macOS 用 0，本实现原来发 3（不在 Apple 使用范围内）。

### 15.6 iOS 发送端的真实参数（抓包，经 UxPlay 验证可出画）

- 流程：`GET /info`（body 是 `{qualifier:[txtAirPlay]}`）→ pair-setup → pair-verify×2 → fp-setup×2 → SETUP 1 → GET_PARAMETER volume → RECORD → SETUP 2 → `POST /feedback` 每 2 s。
- SETUP 2 里**只有 type 110（视频）**，没有音频流；接收端只回一个 `dataPort`。镜像音频并不经过这条 SETUP。
- 每个请求都带 `DACP-ID`、`Active-Remote`；pair-verify 带 `X-Apple-PD: 1` 和 `X-Apple-AbsoluteTime`。
- SETUP 1 的 plist：`et=32, eiv, ekey, sessionUUID, sessionCorrelationUUID, timingPort, timingProtocol=NTP, isScreenMirroringSession=true, statsCollectionEnabled=false, updateSessionRequest=false, combinedGetInfoWithControlSetup=true, sourceVersion=980.77.1, model=iPhone17,1, name=x, osName="iPhone OS", osVersion, osBuildVersion, deviceID, macAddress`。
- codec 记录里的宽高是**被投设备的竖屏分辨率**（iPhone 17: 498x1080），High profile。
- 密钥派生与是否配对绑定：**配对过就混 pair-verify 共享密钥，没配对就用裸 AES key**（两种都用真实抓包验证过）。

### 15.7 仍未解：贴牌盒子不出画

把上述全部照做（不配对 + 裸 key 派生 + iOS 身份字段 + DACP-ID/Active-Remote + iPhone 真实编码帧 + 1080p/竖屏两种分辨率 + 单流/双流 + 10/33 fps），盒子仍然进镜像态、分配 dataPort、稳定收流、不出画。同一条发送链路对 UxPlay 完全正常，所以问题不在码流、密钥与请求格式，而在这台盒子自己的取流条件上。确定它需要什么的唯一办法是抓一次 iOS → 盒子的会话（ARP 中间人在回程上被盒子忽略；改为 Wi-Fi 监听模式 + 用 PSK 解密 pcap）。

### 15.8 iOS 直接投这台盒子时发了什么（ARP 中间人 + SNAT 抓到的明文）

盒子不接受双向投毒（会按自己的 ARP 缓存直回手机），但**只投手机侧 + 在中间机上 SNAT** 就够：手机以为盒子在中间机 MAC 上，中间机把源地址改写成自己再转发给盒子，回包自然回到中间机。这样两头都不需要配合，RTSP 与视频连接全部明文可见。

抓到的会话（未配对，特征与"配对与否决定派生"一致）：

```
POST /fp-setup   (16B  m1)        POST /fp-setup (164B m3)
SETUP rtsp://<box>/<session-id>   body: et/eiv/ekey/timingPort/isScreenMirroringSession/
                                        sourceVersion/clientInfo… + model/osName/deviceID/sessionCorrelationUUID
GET /info        (无 body)
GET_PARAMETER volume
RECORD
SETUP            body: streams[0] = { type:110, streamConnectionID:<负 int64>,
                                      timestampInfo:[SubSu,BePxT,AfPxT,BefEn,EmEnc], latencyMs:100 }
POST /feedback   每 2 秒
```

要点：
- **没有 pair-setup / pair-verify**，`SETUP 2` 只有一条视频流（与 UxPlay 会话一致）。
- `timestampInfo` 声明帧头里带哪几个时间戳，接收端据此在 128 字节头里找它们；**字段缺失或结构错误时接收端不报错，只是不渲染**。
- 用抓到的 m1/m3/ekey 再走一遍 PlayFair + SHA512 派生，390 条记录、611 KB 全部解成合法 H.264，再次确认派生公式（裸 key + `AirPlayStreamKey/IV` + 十进制 streamConnectionID）。
- 解密后的帧头与实现逐字节一致：视频帧 `+4=0`、`+6..7=0`、IDR 帧 `+5=0x10`、`+8` 小端 32.32 时间戳；codec 记录 `+4=1`、`+6..7=0x0116`、宽高 float 放在 `+16/+20`、`+40/+44`、`+56/+60`。

本仓库的 `ap_legacy_build_setup2` 曾把这个 `timestampInfo` 数组写成嵌套垃圾（`value_slot()` 只分配一个槽却当数组写了五个，后续分配立即覆盖），已修并加单测。
