# AirPlay 镜像发送端（sender）协议规格 — 面向 C 重实现

> 依据：doubletake 参考项目（只读分析），全部结论来自真实代码与字符串常量，
> 出处标注为 `文件:行号`。文中「本实现」= doubletake 的 sender 侧代码。
>
> 阅读前必须知道的三件事：
> 1. **本实现不使用 `stream.xml`**。AirPlay 屏幕镜像的 SETUP 请求体是 **binary plist**
>    （`plist.Marshal(..., plist.BinaryFormat)`，`mirror.go:357-370`），`stream.xml` 是
>    RAOP（AirPort Express 音频）时代的 XML 形态，本项目全文无此字符串。
> 2. **控制通道是 RTSP/1.0 文本封装**（`POST /pair-setup RTSP/1.0` 这样的请求行），一旦
>    pair-verify（HAP）成功，**整条控制连接的字节流被 ChaCha20-Poly1305 加密分帧**
>    （`client.go:1220-1257`）。
> 3. **本实现不发送 `X-Apple-Device-ID`、`Client-Instance`、`Connection:` 三个头**。
>    grep 全仓只有下文表格里列出的那些头（`internal/airplay/*.go` 无 `Client-Instance`
>    字符串）。设备标识通过 SETUP plist 里的 `deviceID`/`macAddress` 字段传递。
>    移植到别的接收端时不要凭记忆添加这两个头 —— 本仓库没有证据。

---

## 0. 角色与端口

| 项 | 值 | 出处 |
|---|---|---|
| 接收端控制端口 | mDNS 发现或 `-target host:port`（默认 7000），TCP | `cmd/doubletake/main.go:167-183` |
| 控制连接 | 一条长 TCP 连接，串行 CSeq | `client.go:501-514` |
| 视频数据通道 | 接收端在 SETUP 响应中给 `dataPort`，**发送端主动 dial**，TCP | `mirror.go:1095-1103` |
| 事件通道 | 接收端在 SETUP 响应中给 `eventPort`，**发送端主动 dial**，TCP | `event_channel.go:69-97` |
| 音频 RTP/RTCP | 发送端本地连续 3 个 UDP 端口（timing / control / data），UDP | `mirror.go:522-527` |
| 音频目标 | 接收端 SETUP 响应 `streams[type=96].dataPort/controlPort`，UDP | `mirror.go:1010-1024` |

控制连接的 dial 超时 10 s（`client.go:504`）；响应读取默认超时 30 s（`client.go:1012-1014`）。

---

## 1. 完整时序（连接 → 画面出现）

顶层编排在 `cmd/doubletake/main.go:190-370`，实际协议在 `client.go` / `mirror.go`。

```
TCP connect(host, port)                         client.go:501
 │
 ├─ GET /info                                   client.go:525-573
 │
 ├─ pair-setup  (+ pair-verify)                 pairing.go:217 / 239 / 415 / 674
 │      └─ 成功后：控制通道整体变为 HAP 加密帧（HAP 路径）
 │
 ├─ POST /fp-setup  ×2  (m1→m2, m3→m4)          fairplay.go:23-125
 │      └─ 得到 fpKey / fpIV / FpEkey(72B) / fpAesKey(16B)
 │
 ├─ SETUP (control)   body=session plist         mirror.go:806-816
 │      └─ 响应可含 info / eventPort / timingPort / timingPeerInfo / skipRecord
 │      └─ 立刻 dial eventPort（若给了）           mirror.go:888-895
 ├─ SETUP (audio)     body={streams:[type 96]}   mirror.go:951-963
 ├─ SETUP (video)     body={streams:[type 110]}  mirror.go:1054-1059
 │      └─ 取 streams[type=110].dataPort
 ├─ TCP connect(dataPort)   TCP_NODELAY, sndbuf 64K   mirror.go:1095-1101
 ├─ RECORD  (除非 skipRecord=true)               mirror.go:681-692, 1110-1113
 ├─ SET_PARAMETER volume（仅当带音频，连发 2 次）  mirror.go:1119-1145
 │
 ├─ 首帧：codec 帧(avcC, 明文) → 首帧 IDR(加密)   mirror.go:1416-1535 / 2019 / 2072
 │
 ├─ 之后并发：                                    mirror.go:1246-1247
 │    • 数据通道心跳 128B / 1s（首帧后启动）        mirror.go:2218-2247
 │    • POST /feedback / 2s（立即发一次）          mirror.go:2253-2295
 │    • 事件通道：被动接收接收端命令并回 200        event_channel.go:299-335
 │
 └─ TEARDOWN（可选，离开时）                      mirror.go:2306-2340
```

### 1.1 各步骤返回值怎么用

| 步骤 | 响应 | 用途 |
|---|---|---|
| `GET /info` | binary plist → `ReceiverInfo` | `features` 决定配对协议/是否 PTP/是否 AAC-ELD；`pk` 是接收端 Ed25519 公钥（raw pair-verify 校验用）；`displays[0].widthPixels/heightPixels(Max)` 决定画布；`statusFlags` bit7=要密码、bit9=要 PIN；`sourceVersion` 决定 PTP 可用性 |
| `POST /pair-setup` M2 | TLV8：`0x02` salt、`0x03` 服务端公钥 B、`0x07` 错误码、`0x08` 退避秒数 | 进 SRP |
| `POST /pair-setup` M4 | TLV8：`0x04` 服务端 proof、`0x07` 错误 | 校验服务端 proof |
| `POST /pair-setup` M6 | TLV8：`0x05` 加密身份、`0x07` 错误 | 只查错误；`SharedSecret = K`（SRP 的 K，随后被 pair-verify 覆盖） |
| `POST /pair-verify` V2 | TLV8：`0x03` 服务端 X25519 公钥、`0x05` 密文 | 算 X25519 shared，派生通道密钥 |
| `POST /pair-verify` V4 | 空 body 或 `0x07` 错误 | 成功标志 |
| `POST /fp-setup` m2 | `FPLY` 记录，142 B | 取 `m2[13]`=mode，`m2[14:142]`=加密的接收端 SAP |
| `POST /fp-setup` m4 | `FPLY` 记录，32 B | `m4[12:32]` 必须等于 `m3[144:164]` |
| `SETUP` control | plist | `info`（会话期 displays）、`eventPort`、`skipRecord`、`timingPort`(NTP)、`timingPeerInfo.ClockID`(PTP)、响应头 `X-Apple-RequestReceivedTimestamp`/`X-Apple-ProcessingTime` |
| `SETUP` audio | plist | `streams[].dataPort/controlPort`（旧式）或 `streamConnections.*.streamConnectionKeyPort` |
| `SETUP` video | plist | `streams[type=110].dataPort` ← **必需**；缺失即报错 `no video data port in SETUP response`（`mirror.go:1085-1087`） |

---

## 2. 必须发送的 HTTP/RTSP 头部（真实值）

### 2.1 通用请求构造（明文控制通道）

`client.go:868-902`（`httpRequestOnce`）与 `client.go:963-1000`（`rtspRequestOnce`）：

```go
fmt.Fprintf(&buf, "%s %s RTSP/1.0\r\n", method, path)
fmt.Fprintf(&buf, "CSeq: %d\r\n", seq)                  // 每条连接从 1 递增，原子计数器
fmt.Fprintf(&buf, "User-Agent: AirPlay/935.7.1\r\n")    // 固定字面量，不拼接
for k, v := range extraHeaders { fmt.Fprintf(&buf, "%s: %s\r\n", k, v) }
if contentType != "" && len(body) > 0 {                 // ← 只有 body 非空才带 Content-Type
    fmt.Fprintf(&buf, "Content-Type: %s\r\n", contentType)
}
fmt.Fprintf(&buf, "Content-Length: %d\r\n", len(body))  // 总是发送，可为 0
buf.WriteString("\r\n")
```

关键点：

- **`User-Agent` 是硬编码 `AirPlay/935.7.1`**（`client.go:874`、`client.go:969`、`mirror.go:421`）。
  不是「拼装规则」，就是这一个常量。iOS/macOS 真机是 `AirPlay/xxx` 形式，但本实现不做版本协商。
- **`X-Apple-Session-ID` 在本实现里从不发送**：代码注释明确说 legacy CSeq/RAOP handler 会拒
  或崩（`client.go:833-836`、`client.go:970-971`）。字段 `sessionID` 仍被生成
  （`client.go:388-396`），但只用于派生设备 MAC（见 2.3），不进头部。
- **`Connection:` 头从不发送**（keep-alive 由 TCP 连接本身隐含）。
- 部分请求带 `X-Apple-ProtocolVersion: 1`，仅 raw 路径（`client.go:916`）。
- 状态行解析兼容 `HTTP/1.1 200` 与 `RTSP/1.0 200` 两种响应（`client.go:1170-1190`）。

### 2.2 按请求列举（全部真实头部）

| 请求 | Content-Type | 额外头（真实值） | 出处 |
|---|---|---|---|
| `GET /info` | 无（body 为空） | 无 | `client.go:529-531` |
| `POST /pair-setup`（raw） | `application/octet-stream` | 无（body=32B Ed25519 公钥） | `pairing.go:330-337` |
| `POST /pair-setup`（HAP） | `application/octet-stream` | `X-Apple-HKP: 4`（transient）或 `5`（PIN/screen-capture）；`X-Apple-Client-Name: <hostname>`；`X-Apple-Client-ID: <PairingID UUID>` | `pairing.go:133-147` |
| `POST /pair-pin-start` | 无 | 同上 + `X-Apple-SupportedPINLengths: 4` | `pairing.go:171-180` |
| `POST /pair-verify`（HAP） | `application/octet-stream` | 同 pair-setup + `X-Apple-PD: 1` | `pairing.go:150-157` |
| `POST /pair-verify`（raw） | `application/octet-stream` | `X-Apple-ProtocolVersion: 1` + 条件性 `X-Apple-PD: 1` | `client.go:915-918`、`pairing.go:905-911` |
| `POST /fp-setup` | `application/octet-stream` | `X-Apple-ET: 32` | `fairplay.go:40-43`、`fairplay.go:62-65` |
| `SETUP` | `application/x-apple-binary-plist` | 无 | `mirror.go:357-370` |
| `RECORD` | 无 | `Session: <sessionUUID>`；`Range: npt=0-`；`RTP-Info: seq=0;rtptime=0` | `mirror.go:683-687` |
| `SET_PARAMETER` | `text/parameters` | 无 | `mirror.go:1129` |
| `POST /feedback` | 无 | 无（body 为空） | `mirror.go:2258` |
| `TEARDOWN` | 无 | 无 | `mirror.go:2315` |
| 401 重试 | — | `Authorization: Digest username="AirPlay", realm="...", nonce="...", uri="...", response="..."` | `digest.go:104-111` |

`X-Apple-HKP` 取值常量：`pairingTypeLegacy=3`、`pairingTypeTransient=4`、`pairingTypeScreenCapture=5`
（`pairing.go:77-79`）。选择逻辑：

```go
// pairing.go:161-169
if c.pairingProtocol == pairingProtocolRaw { return 3 }
if c.info.PrefersLegacyPairing()          { return 3 }   // 无 CoreUtils 特征位
if c.pairType == 0                        { return 5 }
return c.pairType
```

`X-Apple-Client-Name` = 本机 hostname，去空白、必须是可打印 UTF-8，否则回退 `"doubletake device"`
（`pairing.go:112-131`）。

### 2.3 设备 ID / MAC 格式

`deviceID` 由「会话 UUID」压成 MAC：取 UUID 去 `-` 后的前 12 个 hex 字符、**首字节强制改
`02`**（本地管理位）、字母大写冒号分隔（`mirror.go:2545-2562`）：

```go
// uuidToMAC: "3f2a..." → "02:3F:2A:..:..:.."
hex := strings.ReplaceAll(strings.ToLower(id), "-", "")
if len(hex) < 12 { return "02:00:00:00:00:01" }
parts[0] = "02"
return strings.ToUpper(strings.Join(parts, ":"))
```

调用点：`clientDeviceID := uuidToMAC(c.sessionID)`（`mirror.go:468`）。
`sessionUUID` 是另一个随机 v4 UUID（`mirror.go:467`，生成见 `mirror.go:2773-2781`），
出现在 SETUP plist 与 `Session:` 头里。

### 2.4 401 Digest（仅在接收端启用密码时）

挑战解析与响应算法是 **RFC 2069**（无 qop/cnonce/nc，`digest.go:1-13` 注释）：

```
HA1      = MD5(username:realm:password)   // username 固定 "AirPlay"（digest.go:15）
HA2      = MD5(method:uri)                // uri = 请求行里的 path/URI 原样
response = MD5(HA1:nonce:HA2)
```

流程：先带缓存挑战预认证；若仍 401 则**最多重试一次**（`client.go:846-866`）。

---

## 3. pair-setup / pair-verify：HAP 还是 legacy？

**两条协议都存在，运行时协商**（`pairing.go:60-66`）：

- `pairingProtocolHAP`：HomeKit/HAP 风格 TLV8 + SRP-6a + X25519 + Ed25519，**成功后会加密控制通道**。
- `pairingProtocolRaw`：原始 AirPlay 二进制 68/96 字节交换，**控制通道保持明文**。

选择顺序（`performTransientSetupAndVerify`，`pairing.go:239-289`）：

```go
if c.info.usesModernPairing() && c.info.SupportsTransientPairing() {
    HAP pair-setup → 失败且是 400/404/405/415/455/500/501/505 才回退 raw   // pairing.go:300-318
} else {
    raw pair-setup（32B 公钥交换）→ 失败则回退 HAP TLV8
}
```

- `usesModernPairing()` =（feature 38/43/46/48 任一）且（feature 26/51 都无）（`discovery.go:413-423`）。
- `SupportsTransientPairing()` = feature 43 或 48（`discovery.go:395-397`）。
- 有 PIN 需求（`statusFlags` bit9）→ 先 `POST /pair-pin-start`，再用 4 位 PIN 走 SRP
  （`pairing.go:372-395`、`client.go:760-775`）。
- 接收端「Require Password」（bit7）在现代接收端**只用于 HTTP Digest**，不用于 SRP；
  只有 legacy（feature 27）接收端才把该密码用于 SRP（`client.go:146-190`、
  `client.go:760-767`）。

### 3.1 HAP pair-setup（SRP-6a over TLV8）

**参数（真实值，`pairing.go:194-215`）**：

| 项 | 值 |
|---|---|
| N | RFC 5054 **3072-bit** 群，字面量以内联十六进制串给出（`pairing.go:196-212`） |
| g | `5`（`pairing.go:213`） |
| 摘要 | **SHA-512**（全部 SRP 计算都是 SHA-512，不是 SHA-1） |
| 用户名 I | `"Pair-Setup"`（ASCII，`pairing.go:524`） |
| 密码 P | PIN 字符串字节；transient 时为空串 `""`（`pairing.go:524-525`、`pairing.go:363-365`） |
| 填充宽度 | 384 字节（3072 bit，`padTo(..., 384)`，`pairing.go:534-535`） |

**TLV8 类型码**（`pairing.go:47-59`）：`0x00` method、`0x01` identifier、`0x02` salt、
`0x03` publicKey、`0x04` proof、`0x05` encryptedData、`0x06` state、`0x07` error、
`0x08` retryDelay、`0x0A` signature、`0x12` ACL、`0x13` flags。
TLV8 编码规则：`tag, len(≤255), value`，值超过 255 字节自动分段重复同一 tag
（`pairing.go:811-840`）。

**M1**（`pairing.go:415-437` 与 `pairing.go:342-360`）：

```
M1 = TLV{ 0x00: 00,           // method = 0 (pair setup)
          0x06: 01,           // state = 1
          0x13: 10 00 00 00 } // flags = 0x00000010 (LE) —— 仅 transient 带
```

**M2 → M3（SRP 计算，`completeSRPExchange`，`pairing.go:523-650`）**：

```
inner = SHA512("Pair-Setup" || ':' || PIN)
x     = SHA512(salt || inner)                      // 大端整数
k     = SHA512(pad(N,384) || pad(g,384))
a     = random 32 bytes (==0 则置 1)
A     = g^a mod N                                  // 客户端公钥
B     = M2.publicKey                               // 必须 0 < B < N
u     = SHA512(pad(A,384) || pad(B,384))
S     = (B - k*g^x mod N)^(a + u*x) mod N
K     = SHA512(S)                                  // 未填充的自然字节表示
M1proof = SHA512( (SHA512(N) XOR SHA512(g)) || SHA512("Pair-Setup") || salt || A || B || K )

M3 = TLV{ 0x06: 03,
          0x03: pad(A, 384),
          0x04: M1proof }
```

**M4 校验**：`expected = SHA512(A || M1proof || K)`，与 `M4[0x04]` 比对（`pairing.go:634-639`）。

**M5（Ed25519 长期身份，加密发送）**：

```
sessionKey = HKDF-SHA512(ikm=K, salt="Pair-Setup-Encrypt-Salt",
                         info="Pair-Setup-Encrypt-Info", 32)          // pairing.go:619-621
sigKey     = HKDF-SHA512(ikm=K, salt="Pair-Setup-Controller-Sign-Salt",
                         info="Pair-Setup-Controller-Sign-Info", 32)  // pairing.go:623-627
signature  = Ed25519_sign(sigKey || PairingID || Ed25519pub)          // pairing.go:627-628

subTLV = TLV{ 0x01: PairingID(UTF-8),                // X-Apple-Client-ID 同一个 UUID
              0x03: Ed25519 公钥(32B),
              0x0A: signature(64B),
              0x12: ACL }                            // 仅 HKP type 5 追加
nonce  = 4 个 0x00 || "PS-Msg05"                     // 12 字节
M5     = TLV{ 0x05: AEAD_Seal(sessionKey, nonce, subTLV, aad=nil),
              0x06: 05 }
```

ACL 是 **OPACK** 编码的 `{"com.apple.ScreenCapture": true}`，常量字节：
`"\xe1\x57com.apple.ScreenCapture\x01"`（`pairing.go:109-114`）—— 缺它当前接收端会拒绝
screen-capture 身份。

**M6**：只检查 `0x07` error TLV；随后 `c.PairKeys.SharedSecret = K`（`pairing.go:658`）。
注意：这个值随后被 pair-verify 的 X25519 secret **覆盖**，最终用于 FairPlay 混入与事件通道
派生的是后者。

**退避**：M2 返回 `0x07=3`（backoff）时按 `0x08` 的 **LE 倍数**秒等待后**原样重发 M1**，
最多 3 次，单次等待上限 30 s（`pairing.go:442-500`、`pairing.go:65-69`）。

### 3.2 HAP pair-verify（X25519 + Ed25519）

`hapPairVerify`，`pairing.go:674-795`：

```
客户端临时密钥: X25519 (curve25519.ScalarBaseMult)

V1 = TLV{ 0x06: 01, 0x03: X25519_pub(32B) }

V2 = TLV{ 0x03: server_X25519_pub(32B), 0x05: 密文(可选) }
shared = X25519(client_priv, server_pub)
verifyKey = HKDF-SHA512(shared, "Pair-Verify-Encrypt-Salt",
                        "Pair-Verify-Encrypt-Info", 32)
// 若 V2 带 0x05：用 nonce = 0000 || "PV-Msg02" 解密校验

V3: sigInput   = client_X25519_pub(32) || PairingID || server_X25519_pub(32)
    signature  = Ed25519_sign(长期私钥, sigInput)
    subTLV     = TLV{ 0x01: PairingID, 0x0A: signature }
    V3 = TLV{ 0x06: 03,
              0x05: AEAD_Seal(verifyKey, nonce=0000||"PV-Msg03", subTLV, nil) }

V4: 空 body 或 TLV{0x07: err}
```

**成功后立即生效（这一段是 C 实现最容易漏的）**，`pairing.go:783-793`：

```go
c.PairKeys.SharedSecret = shared                                   // ← 后面所有派生的根
c.encWriteKey = HKDF-SHA512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32)
c.encReadKey  = HKDF-SHA512(shared, "Control-Salt", "Control-Read-Encryption-Key",  32)
c.encrypted   = true
c.PairKeys.MixFairPlayKey = true
c.pairingProtocol = pairingProtocolHAP
```

**控制通道 HAP 加密帧格式**（`client.go:1220-1257` 加密、`client.go:1258-1300` 解密）：

```
每个分帧: [2 字节 LE 明文长度][ChaCha20-Poly1305 密文 (明文 + 16B tag)]
- 明文分块：每块最多 1024 字节，逐块独立封帧
- nonce：12 字节，前 4 字节 0，后 8 字节 = LE uint64(块计数器)，计数器从 0 单调 +1，收发各独立
- AAD：该帧的 2 字节 LE 长度前缀
```

即：请求行 + 头部 + body 作为**一个连续的明文流**按 1024 切块加密；响应侧同样解密后再解析
`\r\n\r\n`（`client.go:1055-1130`）。

### 3.3 Raw（legacy）配对

**raw pair-setup**：`POST /pair-setup`，body = 32 字节 Ed25519 公钥，响应必须正好 32 字节
= 接收端 Ed25519 公钥，存入 `info.PK`（`pairing.go:325-340`）。

**raw pair-verify**（`pairing.go:900-1010`，函数头注释即规格）：

```
V1 (68B): 01 00 00 00 | X25519_pub(32) | Ed25519_pub(32)
V2 (96B): server_X25519_pub(32) | AES-CTR(server_sig 64B, offset=0)
V3 (68B): 00 00 00 00 | AES-CTR(client_sig 64B, offset=64)
V4 (0B) : 空 200 OK

key = SHA-512("Pair-Verify-AES-Key" || shared)[:16]
iv  = SHA-512("Pair-Verify-AES-IV"  || shared)[:16]
server_sig 校验对象: server_X25519_pub || client_X25519_pub   (用 /info 的 pk)
client_sig 签名对象: client_X25519_pub || server_X25519_pub
```

raw 路径**不加密控制通道**，但会设置 `MixFairPlayKey = !SupportsLegacyPairing()`（feature 27），
并在 mixing 时给 pair-verify 请求加 `X-Apple-PD: 1`（`pairing.go:901-911`、`pairing.go:1004-1008`）。

---

## 4. FairPlay（/fp-setup，FPSAP）

### 4.1 消息尺寸与头部

`FPLY` 记录头 12 字节（`fpsap.go:283-289`）：

```
"FPLY" | 03 01 <msgType> 00 | BE uint32 payloadLength
```

| 消息 | 总长 | 载荷长度 | 说明 |
|---|---|---|---|
| m1 | **16 B** | 4 (`fpsapM1Payload = 02 00 03 bb`) | `fpsap.go:12-14`、`fpsap.go:71-75` |
| m2 | **142 B** | 130 | `[12]=2`、`[13]=mode(0..3)`、`[14:142]`=加密的接收端 SAP(128B)，`fpsap.go:224-243` |
| m3 | **164 B** | 152 | `[12]=mode`、`[13:16]=8f 1a 9c`、`[16:144]`=加密的发送端 SAP(128B)、`[144:164]`=20B 确认值，`fpsap.go:214-239` |
| m4 | **32 B** | 20 | `[12:32]` 必须逐字节等于 `m3[144:164]`（`fpsap.go:250-262`） |

HTTP 层：`POST /fp-setup`，`Content-Type: application/octet-stream`，`X-Apple-ET: 32`
（`fairplay.go:39-45`、`fairplay.go:61-67`）。接收端缺此头回 400（`receiver_server.go:788-790`）。

本地 SAP 初始化：128 字节，`localSAP[1] = 1`，剩余 126 字节随机
（`fpsap.go:51-60`）。注意 `localSAP[0]` 保持 0。

### 4.2 密钥与 IV

```
IV  : 16 字节随机（fairplay.go:84-87）→ 后续作为 SETUP 的 eiv / shiv
fpAesKey : 16 字节随机（fairplay.go:93-96）
ekey     : 72 字节（wrapFairPlayKey，fairplay_crypto.go:66-103）
```

**ekey 字节布局**（`fairplay_crypto.go:58-64` 注释 + 实现）：

```
[0:16]  "FPLY" 01 02 01 00  00 00 00 3c  00 00 00 00
[16:32] 16 字节随机 mask
[32:36] BE uint32 原始密钥长度 = 16
[36:56] HMAC-SHA1(macKey, ekey[0:36] || rawKey)        // 20 字节
[56:72] AES-128-ECB(wrappingKey, rawKey XOR mask)      // 16 字节
```

其中 `wrappingKey = deriveFairPlayWrappingKey(receiverSAP, m3)`（`fairplay_crypto.go:30-56`），
`macKey = fpsapDescriptorForSAP(senderSAP, receiverSAP)`（`fairplay_crypto.go:96-99`）。
这两套派生依赖 FairPlay 的私有 SAP 哈希 + 改造 MD5（`fairplay_sap.go`、`fairplay_md5.go`、
`fpsap_tables.go` 里的 9 组替换表）。**这部分无法用标准库复现，必须整体移植这些表与
`fairplayMD5Compress` / `fairplaySAPHash`。**

### 4.3 deriveStreamMasterKey 规则（题目问的核心）

`fairplay.go:158-178`：

```go
func deriveStreamMasterKey(rawKey, secret []byte, mixPairVerifySecret bool) []byte {
    if !mixPairVerifySecret || len(secret) == 0 {
        return rawKey                       // 裸 16 字节
    }
    h := sha512.New()
    h.Write(rawKey)
    h.Write(secret)
    return h.Sum(nil)[:16]                  // SHA-512(raw || secret)[:16]
}
```

**条件**是 `PairKeys.MixFairPlayKey`，而这个标志由**实际完成的 pair-verify 形态**决定
（`fairplay.go:110-112`、`pairing.go:791`、`pairing.go:1006`）：

| pair-verify 形态 | MixFairPlayKey | fpKey |
|---|---|---|
| HAP（`hapPairVerify` 成功） | 恒为 `true` | `SHA512(fpAesKey‖X25519_shared)[:16]` |
| raw，接收端**无** feature 27 | `true`（并带 `X-Apple-PD: 1`） | 同上 |
| raw，接收端**有** feature 27 | `false` | 裸 `fpAesKey`（16B） |

并且 `secret` 为空时也退回裸 key —— 所以「没做过 pair-verify 就直接跑 fp-setup」会静默走裸
key 分支（日志 `using raw fpAesKey (legacy receiver or no SharedSecret)`，`fairplay.go:113-118`）。

---

## 5. SETUP 请求（binary plist，非 stream.xml）

三个 SETUP 全部 `Content-Type: application/x-apple-binary-plist`，URI 形如
`rtsp://<host>:<port>/<streamConnectionID>`；control 与 audio **共用** audio 的 URI，
video 用自己的 URI（`mirror.go:594`、`mirror.go:1032`）。

`streamConnectionID` = `time.Now().UnixNano() & 0x7FFFFFFFFFFFFFFF`（`mirror.go:589`、`mirror.go:1031`）。

### 5.1 SETUP #1：control（会话描述）

`mirrorSetupRequest.controlPlist()`，`mirror.go:262-298`：

| 字段 | 类型 | 本实现取值 | 含义 |
|---|---|---|---|
| `deviceID` | string | `uuidToMAC(sessionID)` → `"02:XX:.."` | 发送端标识（MAC 形态） |
| `macAddress` | string | 同上 | 同上 |
| `sessionUUID` | string | 随机 v4 UUID | RTSP `Session` 值 |
| `sourceVersion` | string | 加密(`ChaCha`)时 `"980.71.1"`，明文时 `"280.33"` | 发送端版本（`mirror.go:33-34`、`compatibility.go:173-178`） |
| `isScreenMirroringSession` | bool | `true` | 屏幕镜像标志 |
| `timingProtocol` | string | `"NTP"` 或 `"PTP"` | 时基协议 |
| `osBuildVersion` | string | `"13F69"` | 伪装 iOS build |
| `model` | string | `"Linux"` | 机型 |
| `name` | string | 本机 hostname | 显示名 |
| `timingPort` | int | 本地 UDP 端口（NTP 时必填） | 接收端会向它发 NTP 探测 |
| `timingPeerInfo` / `timingPeerList` | dict / list | `{ID: uuid, SupportsClockPortMatchingOverride: true, DeviceType: 0, Addresses: [localIP]}`（PTP 时） | PTP 对端信息 |
| `updateSessionRequest` | bool | `false` | 仅 control |
| `combinedGetInfoWithControlSetup` | bool | `true` | 仅 control：要求响应里带 `info` |
| `ekey` / `eiv` / `et` | bytes/bytes/int | 仅当「FairPlay 挂在 session 根」且未加密时：`ekey`=72B、`eiv`=16B、`et`=32 | `mirror.go:756-760`、`mirror.go:1300-1310` |

时序协议选择（`compatibility.go:48-96`，`mirror.go:470-486`）：

- 默认 `NTP`。
- 升为 `PTP` 需**同时**满足：`encrypted==true`、`features` bit41、`sourceVersion ≥ 354.54.6`
  且不是 `377.40.x`（`compatibility.go:92-96`、`compatibility.go:126-134`）。

**control-first vs media-first**：本实现先发「只含会话描述、无 streams」的 control SETUP；
若被 400/405/406/415/455/501 显式拒绝，则**只回退一次**到「media-first」——即第一个 SETUP 就
带 audio stream 描述（`mirror.go:855-900`、`mirror.go:333-345`）。

### 5.2 SETUP #2：audio（type 96）

`mirror.go:900-946`：

| 字段 | 取值 | 来源 |
|---|---|---|
| `type` | `96` | 固定 |
| `streamConnectionID` | int64 | 随机 |
| `ct` | `2`（ALAC）或 `8`（AAC-ELD） | `audio.go:31-32`、`audio.go:93-99` |
| `spf` | `352`（ALAC）/ `480`（AAC-ELD） | 同上 |
| `sr` | `44100` | 固定 |
| `audioFormat` | `0x40000`（ALAC）/ `0x1000000`（AAC-ELD） | 同上 |
| `audioMode` | `"default"` | 固定 |
| `usingScreen` | `true` | 屏幕音频 |
| `latencyMin` | **`0`** | 发送端不设最小值（`mirror.go:901-905` 注释解释会重复叠加） |
| `latencyMax` | `samplesFor44k1(audio latency)`，如 85 ms → **3748**（截断） | `latency.go:190-203` |
| `redundantAudio` | `2`（ALAC/AAC-ELD 都带） | `audio.go:52`、`audio.go:80-82`、`mirror.go:920-922` |
| `controlPort` | 本地 UDP 控制端口（legacy 布局） | `mirror.go:1284-1290` |
| `shk` | 32B 随机 ChaCha 音频密钥（仅 ChaCha 布局且长度==32） | `mirror.go:1279-1290` |
| `streamConnections` | `{"streamConnectionTypeRTP":{"streamConnectionKeyUseStreamEncryptionKey":bool},"streamConnectionTypeRTCP":{"streamConnectionKeyPort":int}}` | `mirror.go:1253-1275` |

布局选择：`features` bit59（`featureAudioStreamConnectionSetup`）→ `streamConnections`，
否则 `controlPort`；被显式拒绝则换一种**再试一次**（`mirror.go:943-962`、`compatibility.go:79-86`）。

**只发视频时音频字段是否需要？** 本实现的答案：**仍然发**。`compatibilityForReceiver` 的
注释明确写着「video-only session still creates AirPlay's screen-audio descriptor so legacy
receivers reach the same ready state, but no audio packets follow」（`compatibility.go:56-62`），
音频编码在拿不到可用 mask 时会退化为 ALAC 占位值。`cfg.NoAudio` 只是不发送任何音频 RTP 包，
并且连 `SET_PARAMETER volume` 都跳过（`mirror.go:1117-1145`）。

### 5.3 SETUP #3：video（type 110）

`mirror.go:1034-1052`：

```go
videoStreamDesc := map[string]interface{}{
    "type":               int64(110),
    "streamConnectionID": videoStreamConnectionID,
    "latencyMs":          latencies.video.Milliseconds(),   // 40 / 75 / 100
    "timestampInfo": []interface{}{                         // 顺序固定
        {"name": "SubSu"}, {"name": "BePxT"}, {"name": "AfPxT"},
        {"name": "BefEn"}, {"name": "EmEnc"},
    },
}
if encKey != nil { videoStreamDesc["shk"] = encKey; videoStreamDesc["shiv"] = encIV }
```

- `latencyMs` 来自连接时延画像：低 40 ms / 普通 **75 ms** / 高（无线）100 ms
  （`latency.go:19-26`、`latency.go:159-181`）。
- `shk` = `c.fpKey`（16B），`shiv` = `c.fpIV`（16B）（`mirror.go:498-515`）。
- 无 `latencyMin`、无 `sr`、无 `audioFormat` —— 视频只描述画布与时钟。

### 5.4 SETUP 响应里怎么取端口

```go
// mirror.go:1076-1090
for _, s := range videoResp["streams"] {
    if plistInt(s["type"]) == 110 { dataPort = plistInt(s["dataPort"]) }
}
if dataPort == 0 { return nil, fmt.Errorf("no video data port in SETUP response") }
```

`eventPort` 在**顶层**（任意一个 SETUP 响应都可能给），取法
`if port := plistInt(response["eventPort"]); port > 0 { receiverEventPort = port }`
（`mirror.go:722-726`）。音频端口兼容两种形态（`mirror.go:2386-2404` 的 `plistStreamPorts`）：
旧式 `dataPort`/`controlPort`，新式 `streamConnections.streamConnectionTypeRTP/RTCP.streamConnectionKeyPort`。

`skipRecord=true` 表示接收端自己已启动会话，可跳过 RECORD（`mirror.go:888`、`mirror.go:1107-1113`）。

---

## 6. 数据通道（TCP dataPort）包格式

**TCP 连接由发送端主动建立**，`TCP_NODELAY`，发送缓冲 64 KB（`mirror.go:1095-1101`）。
每帧 = **128 字节头 + payload**，头与长度都是小端。接收端解析见
`receiver_media.go:538-625`（`receiverMirrorHeaderSize = 128`，`receiver_media.go:24`）。

### 6.1 头部字段表（发送端构造：`mirror.go:2019-2065` / `mirror.go:2072-2145`）

| offset | size | 类型 | 含义 / 取值 |
|---|---|---|---|
| 0 | 4 | LE uint32 | payload 字节数。**ChaCha 路径要 +16（Poly1305 tag）** |
| 4 | 1 | u8 | 包类型：`0x00`=加密 VCL 视频；`0x01`=codec 配置（明文）；`0x02`=心跳 |
| 5 | 1 | u8 | `0x10`=IDR，`0x00`=非 IDR；codec/心跳包固定 `0x00` |
| 6 | 1 | u8 | codec 包：`0x16`=H.264 avcC，`0x1e`=HEVC hvcC；心跳包：`0x1e`；VCL 包：`0x00` |
| 7 | 1 | u8 | codec 包 `0x01`；其它 `0x00` |
| 8 | 8 | LE uint64 | NTP 时间戳（秒.32 位小数，**boot 相对**，不是 1970/1900 纪元） |
| 16 | 4 | LE float32 | 编码宽度（codec 包） |
| 20 | 4 | LE float32 | 编码高度（codec 包） |
| 24 | 16 | — | 未使用（全 0） |
| 40 | 4 | LE float32 | 目标矩形宽（codec 包）／VCL 包此处起是 timeline ID |
| 44 | 4 | LE float32 | 目标矩形高（codec 包） |
| 40 | 8 | LE uint64 | **timeline ID**（仅 VCL 包；PTP 时为接收端 `ClockID`，NTP 时 0）→ `mirror.go:2090` |
| 48 | 8 | — | 0 |
| 56 | 4 | LE float32 | 第二组矩形宽（codec 包） |
| 60 | 4 | LE float32 | 第二组矩形高（codec 包） |
| 64..127 | 64 | — | 0 |

`timeline ID` 与 codec 包的 `[40:44]` 共用同一偏移，因为它们是**互斥的包类型**。

### 6.2 payload：AVCC 组帧

- payload 是 **AVCC**（每个 NAL 前加 4 字节大端长度），**不带 Annex-B 起始码**：
  `avccWrap()` = `BE32(len(raw)) || raw`（`mirror.go:1789-1795`）。
- VCL payload **只包含 VCL NAL**（type 1/2/3/4/5）；SPS(7)、PPS(8)、SEI(6)、AUD(9)
  **不进 payload**（`mirror.go:1509-1580`）。
- 一个 access unit 的多 slice 合并成**一个** frame 发送（`vclBuf` 累积后 `flushVCL`）。
- 首帧顺序：**先发 codec 配置包（明文，含 avcC）**，再发 IDR VCL（`mirror.go:1467-1505`）。
- codec 包 payload = AVCDecoderConfigurationRecord，**末尾 4 字节填充 `02 00 00 00`**
  （`buildAVCCConfig`，`mirror.go:1968-1990`）：

```
[0]=0x01, [1]=SPS[1], [2]=SPS[2], [3]=SPS[3],
[4]=0xFF (lengthSizeMinusOne=3), [5]=0xE1,
[6:8]=BE16 len(sps), sps..., [n]=0x01, BE16 len(pps), pps...,
最后 4 字节 = 02 00 00 00
```

### 6.3 视频 payload 加密：两条互斥路径

**(A) AES-128-CTR（明文 pair-verify / legacy，或 `--no-encrypt` 之外的旧路径）**

密钥派生（`deriveVideoKeys`，`mirror.go:2171-2185`）：

```
key = SHA-512("AirPlayStreamKey" || decimal(streamConnectionID) || shk)[:16]
iv  = SHA-512("AirPlayStreamIV"  || decimal(streamConnectionID) || shk)[:16]
```

`streamConnectionID` 用**十进制**字符串拼接（`fmt.Sprintf("AirPlayStreamKey%d", uint64(id))`）。

**CTR 用法不是普通连续流**，而是 16 字节块对齐 + 跨帧 keystream 结转，必须完全照抄
（`client.go:1391-1478` 的 `mirrorCipher.EncryptFrame`，接收端复用同一函数
`receiver_media.go:571`）：

```
状态: blockOffset(当前块已消耗字节), og[16](上帧尾部块的 keystream), nextCryptCount

1) 若 nextCryptCount>0：用 og 的后 nextCryptCount 字节 XOR 本帧前部；
   本帧不够长则把剩余 keystream 保留给下一帧并返回。
2) 若 blockOffset>0：丢弃 (16-blockOffset) 字节 keystream，使 CTR 回到块边界。
3) 完整 16 字节块：直接 XOR CTR keystream。
4) 尾部不足 16 字节：补零到 16、整块 XOR，输出前 restLen 字节，
   把整块 keystream 存入 og，nextCryptCount = 16-restLen。
```

也就是说：**任何一帧都不会跨 16 字节块切割 keystream；尾部块的剩余 keystream 归下一帧使用。**

**(B) ChaCha20-Poly1305（HAP 加密会话，本实现默认走这条）**

`mirror.go:1167-1195`、`deriveChaChaKey` `mirror.go:2203-2216`：

```
IKM  = PairKeys.SharedSecret (pair-verify 的 X25519 shared)   // 无则退化用 fpAesKey
key  = HKDF-SHA512(ikm=IKM,
                   salt = "DataStream-Salt" || decimal(streamConnectionID),
                   info = "DataStream-Output-Encryption-Key", 32)
payload = AEAD_Seal(key, nonce, AVCC明文, aad = 128 字节 header)
```

- **nonce = 12 字节：`[0:4]=0`，`[4:12]=LE64(帧计数器)`**，计数器从 0 起、**每帧 +1**
  （`mirror.go:2096-2112`）。代码注释解释了为什么这样能对齐接收端的
  `chacha20_poly1305_init_64x64`（64 位 counter + 64 位 nonce）。
- **AAD 是完整的 128 字节头**，因此头部必须先定稿再加密。
- `header[0:4]` 的 payload 长度**包含 16 字节 tag**（`mirror.go:2075-2080`）。
- codec 包与心跳包**永不加密**。

### 6.4 心跳包（无 payload）

`dataHeartbeatLoop`，`mirror.go:2218-2247`：

```go
header := make([]byte, 128)
header[4] = 0x02   // 心跳类型
header[6] = 0x1e
// [0:4] = 0（payload 长度 0）→ 只写 128 字节头
ticker := time.NewTicker(1 * time.Second)
```

- **首帧发出后才开始**（等待 `firstFrameSent` 被关闭），周期 **1 s**，写超时 5 s。
- 注释给出参照：「AirMyPC sends these every ~1s: 128-byte header with byte4=0x02,
  bytes6-7=0x1e00, no payload」（`mirror.go:2219-2221`）。

---

## 7. 反馈 / 心跳的必要性

**`POST /feedback`**（`mirror.go:2253-2295`）：

```go
body, headers, err := s.client.rtspRequest("POST", "/feedback", "", nil, nil)   // body 为空
```

- **立刻发一次**（不等 ticker），之后每 **2 s** 一次。
- 响应 body 是 binary plist（本实现只打印，不依赖内容）；**响应头**
  `X-Apple-RequestReceivedTimestamp` / `X-Apple-ProcessingTime` 被用来重锚 PTP 时钟
  （`mirror.go:2264-2267`、`mirror.go:96-145`）。
- 路径必须是**裸 `/feedback`**，不能带 session 前缀的完整 URI（`mirror.go:2291-2292` 注释）。

**不发会怎样**：

- `/feedback`：代码注释与设计说明（`mirror.go:2251-2256`、`mirror.go:1235-1246`）：
  「接收端的 feedback 超时在 SETUP 时就开始计时」，以及「若在同一个串行连接上发
  `GET_PARAMETER` 而它不应答，会把 `/feedback` 堵住足够久，导致接收端**停止渲染视频而音频继续**」。
  即：**不发 / 发迟了 = 画面停但会话还在**。
- 数据通道心跳：本实现只说「以 1 s 周期保活」（与 AirMyPC 一致），未给出不发的直接观测结论；
  保守做法是必须发。
- `GET_PARAMETER` 保活：**故意不发**（同上注释），因为它在同一串行连接上可能永不响应。

---

## 8. 事件通道（reverse 连接）

### 8.1 建立方式（关键：没有请求行，没有 Upgrade 头）

`event_channel.go:69-97`：

```go
address := net.JoinHostPort(c.host, strconv.Itoa(port))
conn, err := net.DialTimeout("tcp", address, 3*time.Second)   // 只 dial，不写任何字节
channel, err := newEventChannel(conn, c.encrypted, sharedSecret)
go serveEventChannel(ctx, channel, clock)                     // 之后纯被动接收
```

- 端口来自 SETUP 响应的**顶层** `eventPort`。
- **发送端 dial 成功后立刻进入读循环，不发送 HTTP 请求行、不发 `Upgrade`、不发 `Connection`**。
  方向是反的：**接收端在这条连接上扮演请求方**。
收到 control SETUP 响应后立刻连（`mirror.go:888-895`），之后每个 SETUP 响应都可能
再次触发（`observeEventPort` + `connectEvent`，`mirror.go:704-760`）。

### 8.2 加密（若控制通道已 HAP 加密）

**密钥方向刻意反转**（`event_channel.go:43-67`）：

```go
readKey  = HKDF-SHA512(SharedSecret, "Events-Salt", "Events-Write-Encryption-Key", 32)
writeKey = HKDF-SHA512(SharedSecret, "Events-Salt", "Events-Read-Encryption-Key",  32)
```

帧格式与 HAP 控制通道相同：`[2B LE 长度][密文+16B tag]`，AAD=长度前缀，
nonce=`0000`+LE64(计数器)，单帧明文上限 **1024** 字节（`event_channel.go:130-175`）。

### 8.3 接收端会发什么 / 发送端必须回什么

接收端发**标准 RTSP 请求**（`receiver_media.go:408-416`）：

```
POST /command RTSP/1.0
CSeq: <n>
Content-Type: application/x-apple-binary-plist
Content-Length: <n>

<binary plist: {"type": "forceKeyFrame"}>
```

本实现已知的命令还有 `updateTimingPeerInfo`（PTP 时更新 `ClockID`，
`event_channel.go:270-297`）。

**发送端必须回**（`event_channel.go:320-334`）：

```
RTSP/1.0 200 OK
CSeq: <原样回填>
Content-Length: 0
```

- 命令解析是**尽力而为**：无法识别的命令也必须回 200（注释明确「Apple receivers expect a
  200 acknowledgement even when a command is irrelevant」）。
- 只读不回的后果：接收端在本仓库里会 `readReceiverEventResponse` 失败并**关闭通道**
  （`receiver_media.go:393-404`）；本实现在通道失败时也只关闭连接、**不影响视频**
  （`event_channel.go:79-86`）。也就是说事件通道**不是画面出现的必要条件**，但 PTP 的
  timing-peer 热切换与 `forceKeyFrame` 会失去通路。

---

## 9. 时序 / 超时 / 重试总表

| 位置 | 值 | 出处 |
|---|---|---|
| 控制连接 dial | 10 s | `client.go:504` |
| 控制响应读超时 | 默认 30 s（GET /info 可传自定义） | `client.go:1012-1014` |
| 数据通道 dial | 5 s | `mirror.go:1096` |
| 事件通道 dial | 3 s | `event_channel.go:74` |
| SETUP 无响应告警 | 3 s 后打日志（不改行为） | `mirror.go:654-675` |
| 失败 SETUP 的 TEARDOWN 预算 | 2 s | `mirror.go:36`、`mirror.go:543` |
| 会话期 GET /info 回退超时 | 3 s | `mirror.go:31` |
| codec 帧写超时 | 1 s | `mirror.go:2054` |
| VCL 帧写超时 | 2 s | `mirror.go:2133` |
| 心跳写超时 | 5 s | `mirror.go:2239` |
| 数据通道心跳周期 | 1 s（首帧后启动） | `mirror.go:2226` |
| /feedback 周期 | 2 s，且启动时立即发 1 次 | `mirror.go:2253-2285` |
| pair-setup 退避重试 | 最多 3 次，单次 ≤30 s，原样重发 M1 | `pairing.go:65-69`、`pairing.go:470-480` |
| Digest 401 重试 | 最多 1 次 | `client.go:846-866` |
| SETUP 顺序回退 | 最多 1 次（control-first → media-first） | `mirror.go:855-900` |
| 音频描述布局回退 | 最多 1 次 | `mirror.go:943-962` |
| NTP 应答读超时 | 3 s 循环 | `mirror.go:2455` |
| 主动 NTP 探测 | 3 个包，间隔 100 ms（仅当响应给了 `timingPort`） | `mirror.go:2509-2545` |
| 帧时间戳单调性 | 新值 ≤ 上一值则强制 `+1` | `mirror.go:2663-2670` |
| 视频时间戳偏置 | `latencyMs`（40/75/100 ms），下限 5 ms | `mirror.go:2610-2618`、`latency.go:19-26` |

**NTP 时基（默认）**：发送端绑定 timing/control/data 三个**连续** UDP 端口，
把 timing 端口写进 SETUP 的 `timingPort`；接收端会向它发 NTP 计时请求，发送端必须应答
（`mirror.go:2446-2520`）：

```
请求: [0]=0x80, [1]=0xd2
应答: 复制前 32 字节, [0]=0x80, [1]=0xd3,
      [8:16]  = 请求的 transmit 时间戳（[24:32] 原样回填）
      [16:24] = now（NTP 格式，含 1900 纪元）
      [24:32] = now
```

注意 `0xd3` 是应答，收到 `0xd3` 必须丢弃否则会互相应答成环（`mirror.go:2469-2471`）。

帧时间戳用 **boot 相对** 的 NTP 定点数（`compactTimestamp`：`(sec<<32)|frac`），
不加纪元（`mirror.go:2689-2697`）。

---

## 10. 最小可行子集：「只把一个静态 H.264 帧显示到 Apple TV」

本仓库里已有现成对照实现：`SendStaticFrames`（`static_stream.go:25-100`），它刻意复用同一
SETUP / 同一加密 / 同一 128 字节头，只改发送节奏。它证明了下面的最小集合。

### 10.1 绝对不能省

| 步骤 | 为什么不能省 | 证据 |
|---|---|---|
| `GET /info` | 需要 `features` / `pk` / `displays` / `statusFlags`；raw 路径的签名校验直接用 `/info` 的 `pk` | `client.go:525`、`pairing.go:963-965` |
| **pair-verify**（含其前的 pair-setup） | 本仓库接收端对「session 请求 + `/fp-setup`」强制要求 pair-verify 已完成，否则 **455** | `receiver_server.go:770-773` |
| `POST /fp-setup` m1/m3 | 接收端在 SETUP 前要求 FairPlay 完成，否则 **455**；且 `shk/shiv` 来自 `fpKey/fpIV` | `receiver_server.go:799-800`、`mirror.go:498-515` |
| control SETUP（带 `isScreenMirroringSession`） | 会话态机的入口；`timingProtocol` 与 `timingPort`/`timingPeerInfo` 成对必需 | `receiver_server.go:1050-1105` |
| video SETUP（type 110） | **唯一**能拿到 `dataPort` 的途径；`streamConnectionID` 参与密钥派生，不能省 | `mirror.go:1076-1090`、`mirror.go:2171-2185` |
| TCP 连到 `dataPort` | 数据面唯一通路，发送端主动 dial | `mirror.go:1095` |
| codec 包（avcC，明文） | 不先给解码器配置，接收端无法解 AVCC 帧 | `mirror.go:1467-1505`、`static_stream.go:44-48` |
| 首帧必须是 **IDR** | 非 IDR 起始会让接收端解不出参考帧/直接断流（本实现 `streamPrimed` 逻辑就是为此） | `mirror.go:1417-1428` |
| 128 字节头字段 0/4/5/8 | 长度、类型、关键帧标志、时间戳 | `mirror.go:2072-2090` |

### 10.2 可以省（对「静态单帧」而言）

| 可省项 | 说明 | 依据 |
|---|---|---|
| 音频 SETUP + 音频 RTP | 没有音频包也能出画面；但本实现仍会发 audio 描述（为旧接收端 ready 状态）。若你的目标接收端接受「只有 video stream 的 SETUP」，可省 | `compatibility.go:56-62`、`mirror.go:1215-1232` |
| `RECORD` | 响应 `skipRecord=true` 时本来就不发；不发 RECORD 画面是否出现取决于接收端实现 | `mirror.go:1108-1113` |
| `SET_PARAMETER volume` | 纯音量，与画面无关；无音频时本实现也跳过 | `mirror.go:1117-1145` |
| 事件通道 | 只是反向命令通道（`forceKeyFrame` / timing 更新），断了不影响出画 | `event_channel.go:299-335` |
| 数据通道心跳 | 非常短的演示可不发；长于数秒建议发（1 s / 次） | `mirror.go:2218-2247` |
| `/feedback` | 演示数秒可省；长会话必须发（2 s / 次），否则接收端可能停渲染 | `mirror.go:2253-2257` |
| NTP UDP 三端口 + 应答 | **只有在用 NTP 时基时必需**；本实现默认 NTP，但若接收端支持 PTP 且你按 PTP 走，则不需要本地 NTP 应答器（改用 `timingPeerInfo` + 接收端 `ClockID`） | `mirror.go:522-541`、`mirror.go:471-486` |
| H.265 | 不需要 | `mirror.go:1279-1285` |

### 10.3 最小报文集合（按字节）

```
1) GET /info  RTSP/1.0                     CSeq:1
2) pair-setup  (HAP: M1→M2→M3→M4→M5→M6)   或 raw: 32B 公钥 → 32B 公钥
3) pair-verify (HAP: V1→V2→V3→V4)         → 之后控制通道加密
4) /fp-setup m1(16B) → m2(142B)           X-Apple-ET: 32
5) /fp-setup m3(164B) → m4(32B)
6) SETUP control  (session plist)         → 记下 eventPort(可忽略) / skipRecord
7) SETUP video    ({streams:[{type:110,...}]}) → 取 dataPort
8) TCP dial dataPort
9) 128B 头[4]=0x01,[6]=0x16,[7]=0x01 + avcC   (明文 codec 包)
10) 128B 头[4]=0x00,[5]=0x10 + 加密的 IDR AVCC
11) (可选) 每 1s：128B 头[4]=0x02,[6]=0x1e
12) (可选) 每 2s：POST /feedback
```

静态帧的时间戳：`static_stream.go:44` 用 `frameTimeAt(time.Now())`，即「现在 + 视频偏置」，
并要求映射成功；C 实现里等价做法 = `boot_now + latencyMs` 转 NTP 定点数。

---

## 11. C 实现落地清单（易错点）

1. **CSeq 每条连接从 1 起、单调 +1**（`client.go:347`、`client.go:869`），且**加密后每个请求
   仍占一个 CSeq**。
2. **`Content-Type` 只在 body 非空时发**，但 `Content-Length` 总是发（`client.go:874-883`）。
3. **pair-verify 成功后整条控制连接立刻进入 HAP 帧模式**：读也要解帧，否则会被
   `\r\n\r\n` 搜索逻辑搞死（`client.go:1055-1130`）。请求也要按 1024 字节切块。
4. **ekey 的 72 字节布局与两套 HMAC/AES 密钥都依赖 FairPlay SAP 私有算法**，
   必须整套移植 `fairplaySAPHash` + `fairplayMD5Compress` + `fpsap_tables.go` 的替换表；
   没有捷径（`fairplay_crypto.go:30-103`）。
5. **`deriveStreamMasterKey` 的条件是「pair-verify 形态」而非「接收端型号」**：
   HAP 或 raw+PD → SHA-512 混合；raw+feature27 → 裸 key（`fairplay.go:158-178`）。
6. **视频加密二选一，别混用**：HAP 会话用 ChaCha20-Poly1305（AAD=128B 头、长度含 tag）；
   明文会话用 AES-CTR 且必须复刻「块对齐 + keystream 结转」（`client.go:1391-1478`）。
7. **`shk`/`shiv` 仍要写进 video 描述**，即使实际用的是 ChaCha（`mirror.go:1049-1052`）。
8. **ChaCha 视频 nonce 是 12 字节 `00000000 || LE64(帧号)`，不是随机、不是 IETF 32 位计数器**
   （`mirror.go:2096-2112`）。
9. **AVCC，不是 Annex-B**；SPS/PPS 只能在 codec 包里（`mirror.go:1789-1795`、
   `mirror.go:1509-1580`）。
10. **时间戳必须单调**（相等则 +1），否则接收端排队逻辑可能丢帧（`mirror.go:2663-2670`）。
11. **`/feedback` 用裸路径**、空 body；`RECORD` 才带 `Session`（`mirror.go:2291`、`mirror.go:683`）。
12. 401 是 **RFC 2069 MD5**，username 固定 `"AirPlay"`，`uri` 用请求行原样路径（`digest.go`）。
13. 事件通道**不写请求行**，拨通就读；收到 `POST /command` 后按 CSeq 回 200（`event_channel.go`）。

---

## 12. 附录：常量速查

| 常量 | 值 | 出处 |
|---|---|---|
| User-Agent | `AirPlay/935.7.1` | `client.go:874` |
| Digest 用户名 | `AirPlay` | `digest.go:15` |
| HAP 帧最大明文块 | 1024 B | `client.go:1226-1230` |
| 镜像帧头长 | 128 B | `mirror.go:2080`、`receiver_media.go:24` |
| 心跳类型/次字节 | `0x02` / `[6]=0x1e` | `mirror.go:2235-2236` |
| codec 包 option | `0x16`(H.264) / `0x1e`(HEVC) | `mirror.go:2029-2033` |
| IDR 标志 | `header[5]=0x10` | `mirror.go:2082-2088` |
| sourceVersion | `280.33`(明文) / `980.71.1`(加密) | `mirror.go:33-34` |
| osBuildVersion | `13F69` | `mirror.go:270` |
| 视频 latencyMs | 40 / 75 / 100 | `latency.go:19-26` |
| 音频 latency | 50 / 85 / 170 ms → 2205 / 3748 / 7497 samples | `latency.go:23-26`、`latency.go:190-203` |
| 音频 ct/spf/format | 2/352/0x40000 (ALAC)；8/480/0x1000000 (AAC-ELD) | `audio.go:31-32`、`audio.go:93-99` |
| redundantAudio | 2 | `audio.go:52` |
| FPSAP m1/m2/m3/m4 | 16 / 142 / 164 / 32 B | `fpsap.go:18`、`fpsap.go:224`、`fpsap.go:214`、`fpsap.go:250` |
| SRP 群 | RFC 5054 3072-bit, g=5, SHA-512 | `pairing.go:194-215` |
| HKDF label（控制） | `Control-Salt` + `Control-Write/Read-Encryption-Key` | `pairing.go:785-786` |
| HKDF label（事件） | `Events-Salt` + `Events-Write/Read-Encryption-Key`（**方向反转**） | `event_channel.go:55-56` |
| HKDF label（视频） | `DataStream-Salt<id>` + `DataStream-Output-Encryption-Key` | `mirror.go:2204-2205` |
| HKDF label（pair-setup） | `Pair-Setup-Encrypt-Salt` / `Pair-Setup-Encrypt-Info` | `pairing.go:619-620` |
| HKDF label（pair-verify） | `Pair-Verify-Encrypt-Salt` / `Pair-Verify-Encrypt-Info` | `pairing.go:731` |
| ChaCha nonce 标签 | `PS-Msg05` / `PV-Msg02` / `PV-Msg03` | `pairing.go:645`、`pairing.go:739`、`pairing.go:758` |
| feature 位 | 14 FairPlay；27 legacy pairing；41 PTP；43/48 transient；59 streamConnections | `discovery.go:344-365`、`compatibility.go:36-46` |
| statusFlags | bit7 需密码；bit9 需 PIN | `client.go:139-142` |
| NTP PT 类型 | 请求 `0xd2` / 应答 `0xd3` | `mirror.go:2490-2510` |
| NTP 纪元常量 | `secondsFrom1900To1970 = 2208988800` | `mirror.go:2763` |
