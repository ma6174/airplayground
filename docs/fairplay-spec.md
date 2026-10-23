# FairPlay SAP 握手：C（mbedtls）移植规格

**来源**：doubletake 参考项目（commit `5e2f1cb`），`internal/airplay/` 包。
**目标读者**：在 C / mbedtls 环境逐字节重写 AirPlay v3 FairPlay SAP 握手的实现者。
**验证方式**：本文所有算法、常量和向量均已用一份独立 C 参考实现（`fairplay_ref.c`，见附录 C）跑通；该实现覆盖 SAP hash 65 组向量、MD5 三种变异、4 种消息密码模式、7 组 descriptor/exchange 向量、完整 m1→m3→ekey 链路、4 组 wrapping key、以及上游抓包解包向量，全部 0 失败。

本文只描述**发送端（client / initiator）**必须实现的部分；`receiver_fairplay.go` 是同一套原语的镜像实现，用于交叉验算。

---

## 0. 术语与总体结构

| 名称 | 含义 | 长度 |
|---|---|---|
| SAP | FairPlay 会话的 128 字节随机块（`localSAP` 发送端 / `remoteSAP` 接收端） | 128 B |
| m1 / m2 / m3 / m4 | `/fp-setup` 的 4 条 FPLY 记录 | 16 / 142 / 164 / 32 B |
| descriptor | 由两个 SAP 派生的 20 字节白盒种子 | 20 B |
| masks | descriptor 派生的 9 个 16 字节轮掩码 | 9×16 B |
| tail | 由两个 SAP 派生的 20 字节"交换确认值" | 20 B |
| wrapping key | 由 m3 + receiverSAP 派生的 AES-128 密钥 | 16 B |
| ekey | 72 字节封装后的原始音频密钥记录 | 72 B |

调用顺序（`fairplay.go:23-125`）：

```
newFPSAPSession(entropy)                  // localSAP[128] = 00 01 || 126 随机字节
m1 = session.message1()
m2 = POST /fp-setup (m1)                  // X-Apple-ET: 32
m3 = session.exchangeM3(m2)               // 解析 m2、加密 localSAP、算 tail
m4 = POST /fp-setup (m3)
session.confirmM4(m4)                     // m4[12:32] 必须等于 m3[144:164]
iv   = 16 随机字节                          // 作为 SETUP 的 eiv
rawKey = 16 随机字节                        // 真正的音频流密钥
ekey = session.wrapKey(rawKey)            // 72 字节，作为 SETUP 的 ekey
fpKey = mixPairVerify ? SHA-512(rawKey || ecdh)[:16] : rawKey
```

m1/m3 以 `Content-Type: application/octet-stream` 直接 POST 到 `/fp-setup`（`fairplay.go:47-48`、`69-70`）；`ekey`/`eiv` 之后放进 SETUP 的 plist 根字段，同时 `et=32`（`mirror.go:1291-1302`）。

---

## 1. m1 / m2 / m3 / m4 的字节布局

### 1.1 通用记录头（`fpsap.go:283-289`）

```
offset  size  value
0       4     "FPLY"                          magic
4       1     3                               version
5       1     1                               protocol/kind
6       1     messageType (1/2/3/4)
7       1     0
8       4     payloadLength, big-endian
12      N     payload
```

`validateFPSAPRecord`（`fpsap.go:266-281`）要求：总长 == `12+payloadLength`、magic == `FPLY`、`record[4:8] == {3,1,type,0}`、`BE_u32(record[8:12]) == payloadLength`。

### 1.2 m1（16 字节，`fpsap.go:14`、`60-64`）

| offset | size | value | 含义 |
|---|---|---|---|
| 0 | 12 | 头，type=1，len=4 | |
| 12 | 1 | `0x02` | payload marker |
| 13 | 1 | `0x00` | 保留 |
| 14 | 1 | `0x03` | **能力位掩码**（不是 mode，见 `fpsap.go:10-12`） |
| 15 | 1 | `0xbb` | 固定 |

完整常量：`46 50 4c 59 03 01 01 00 00 00 00 04 02 00 03 bb`
（`fpsap_test.go:114` 以该 hex 断言。）

### 1.3 m2（142 字节，接收端生成，`fpsap.go:206-216`；镜像实现 `receiver_fairplay.go:50-66`）

| offset | size | 含义 |
|---|---|---|
| 0 | 12 | 头，type=2，len=130 |
| 12 | 1 | payload marker，**必须 == 2**（`fpsap.go:210-212`） |
| 13 | 1 | **mode，0..3**（接收端选择；镜像实现默认 3，`receiver_fairplay.go:27`） |
| 14 | 128 | 接收端 SAP 的密文（SAS-CBC，见 §4.5） |

**m2 中被解析用于派生 m3 的字段只有三个**：`m2[12]`（marker 校验）、`m2[13]`（mode，写入 m3[12] 并决定加解密模式）、`m2[14:142]`（128 字节密文，解密后成为 `remoteSAP`）。头部的 magic/type/长度只做校验，不参与派生。

> 实现提示：`decryptFPSAPBody`（`fpsap.go:66-76`）在 Go 里构造了一个 144 字节临时缓冲：`message[12]=mode; copy(message[16:], payload)`，因为块解密函数从 `message[12]` 取 mode、从 `message[16:144]` 取密文。C 侧不必构造这个"伪记录"，直接把 `(mode, cipher+0, plain+0)` 传给块解密即可；`message[13:16]` 在 Go 里恒为 0、不参与运算。

### 1.4 m3（164 字节，`fpsap.go:218-235`）

| offset | size | 含义 |
|---|---|---|
| 0 | 12 | 头，type=3，len=152 |
| 12 | 1 | mode，**回抄 m2[13]** |
| 13 | 3 | `8f 1a 9c` 固定标签（`fpsap.go:16`；接收端校验 `receiver_fairplay.go:75-77`） |
| 16 | 128 | `localSAP` 的密文（同一 mode 的 SAS-CBC） |
| 144 | 20 | **tail** = `fpsapExchangeForSAP(localSAP, remoteSAP)` |

### 1.5 m4（32 字节，接收端生成，`receiver_fairplay.go:88-91`）

| offset | size | 含义 |
|---|---|---|
| 0 | 12 | 头，type=4，len=20 |
| 12 | 20 | 必须逐字节等于 m3[144:164]（`fpsap.go:253-264`） |

---

## 2. FPSAP 内部状态与数据流

`fpsapSession`（`fpsap.go:41-46`）只有一个实例贯穿 m1 → m3 → ekey：

| 字段 | C 类型 | 长度 | 写入点 | 读取点 |
|---|---|---|---|---|
| `localSAP` | `uint8_t[128]` | 128 | `newFPSAPSession`：`[0]=0`、`[1]=1`、`[2..127]` 取 126 字节 CSPRNG（`fpsap.go:51-58`） | m3 加密（`fpsap.go:221`）、tail 计算（`fpsap.go:231`） |
| `remoteSAP` | `uint8_t[128]` | 128 | `exchangeM3`：m2 密文解密结果（`fpsap.go:225-233`） | ekey wrapping key（`fairplay_crypto.go:84`）、MAC key（`fairplay_crypto.go:97`） |
| `m3` | `uint8_t[164]` | 164 | `exchangeM3` 末尾 `copy`（`fpsap.go:234`） | `wrapKey` 的 KDF 输入（`fairplay_crypto.go:66-72`） |
| `hasM3` | `bool` | — | 同上 | 前置条件检查（`fpsap.go:240`、`247`） |

传给 `wrapFairPlayKey` 的两个参数即 `session.remoteSAP` 与 `session.m3`（`fpsap.go:246-251`），所以 C 里需要一个结构体持有这三块 buffer，生命周期覆盖两次 `/fp-setup` 请求和 SETUP 阶段。

---

## 3. 派生链总览

```
                 localSAP(128)  remoteSAP(128)
                        \          /
                  descriptorForSAP  ──► seed(20)
                        │
                   masks(9×16) ──► firstNetwork ──► intermediate(16)
                                                        │
                                     digest32(intermediate, fixedBlock) ──► left(16)
                                                        │
                                  secondNetwork(left, masks) ──► white(16)
                                                        │
                                        digest32(left, white) ──► digest(16)
   tail(20) = white[0:4] || digest

   m3 body  = SAS-CBC(mode, IV[mode], localSAP)
   m4       = m3[144:164]

   wrappingKey(16) = KDF(receiverSAP, m3)        // 需要先把 m3 body 解密成 senderSAP
   macKey(20)      = descriptorForSAP(senderSAP, receiverSAP)
   ekey            = 头 || mask || 长度 || HMAC-SHA1(macKey, ...) || AES-128-ECB(wrappingKey, rawKey^mask)
```

---

## 4. 算法规格

### 4.1 SAP hash（`fairplay_sap.go:51-94`，核心电路 `101-213`）

> **注意**：SAP hash **不调用 AES**，也不调用标准 MD5。它由三段组成：字节乱序 + 840 轮字节搅动、一趟约 100 条语句的直线型非线性电路、一趟 256 步反向扩散。

签名：`void sap_hash(const uint8_t in[64], uint8_t out[16])`。

状态（全部 `uint8_t`，初值固定）：`hash[20]`、`matrix[35]`、`aux[10]`（全 0）、`work[210]`（先被填满）。

**Step 1 — 装载（`fairplay_sap.go:55-57`）**

```
for i in 0..209:  work[i] = in[(i & 63) ^ 3]
```
即每个 4 字节组内字节反序（0↔3, 1↔2），64 字节重复 3 遍再补 18 字节。

**Step 2 — 840 轮搅动（`fairplay_sap.go:60-63`）**

```
for (uint32_t i = 0; i < 840; i++) {
    x = work[(i - 155u) % 210u];
    y = work[(i -  57u) % 210u];
    z = work[(i -  13u) % 210u];
    w = work[i % 210u];
    work[i % 210u] = rotl8(y,5) + (rotl8(z,3) ^ w) - rotl8(x,7);   // 全部 mod 256
}
```
**关键点**：`i` 必须是 `uint32_t`。`i<155` 时 `i-155u` 下溢成约 4.29e9，再 `%210`（因为 2^32 mod 210 = 46），结果与"有符号负数取模"**不同**。用 `int` 写会得到不同的 `work`，整个 hash 全错。

**Step 3 — 非线性电路（`fairplay_sap.go:101-213`）**

约 100 条语句（含局部变量声明与续行），严格顺序执行，每条都会读写 `hash`/`matrix`/`aux`/`work`。取数助手（必须逐字保留语义）：

| Go | C 等价 |
|---|---|
| `hi(i)` | `hash[i % 20]`（`i` 为 byte） |
| `si(i)` | `sap_seed[i % 21]` |
| `h(k)` | `hash[work[k] % 20]` |
| `m(k)` | `matrix[work[k] % 35]` |
| `s(k)` | `sap_seed[work[k] % 21]` |
| `ma(k)` | `matrix[aux[k] % 35]` |

以及 6 个纯函数（`fairplay_sap.go:7-24`，下文用 `uint8_t` 写法，附录 A 用等价的 `u8`）：

```c
static uint8_t majority(uint8_t a, uint8_t b, uint8_t c) { return a ^ ((a^b) & (a^c)); }
static uint8_t select_bits(uint8_t mask, uint8_t s, uint8_t c) { return c ^ ((s^c) & mask); }
static uint8_t square(uint8_t v) { return v*v; }
static uint8_t cube(uint8_t v) { return v*v*v; }
static uint8_t rotate_or_zero(uint8_t v, uint8_t n) { return n == 0 ? 0 : rotl8(v, n); }  // n==0 返回 0！
static uint8_t wide_seed(uint8_t in, uint8_t n) {
    if (n == 0) return sap_seed[0];
    int r = ((int)in << n) | ((int)in >> (8 - n));   // int 运算，不截断到 8 位
    return sap_seed[r % 21];
}
```

因为电路无法压缩描述，**逐语句的 C 转写见附录 A**（已用 65 组向量验证），移植时直接对照 `fairplay_sap.go:101-213` 校核即可。

**Step 4 — 折叠输出（`fairplay_sap.go:79-93`）**

```
memset(out,0,16);
out[0..2]  = aux[0..2];      // 3 字节
out[4..10] = aux[3..9];      // 7 字节   → out[3], out[11..15] 此时仍为 0
for i in 0..15: out[i] += 0xe1;
out[3] = 0x3d; out[11] = 0x3c;         // 赋值，不是异或
out[10] ^= aux[3] ^ 133;
for i in 0..209:                       // 210 次，按 i&15 归并
    v = work[i];
    if (i < 35) v ^= matrix[i];
    if (i < 20) v ^= hash[i];
    out[i & 15] ^= v;
for (int i = 0; i < 256; i++)          // 反向扩散，256 轮
    out[i & 15] ^= rotl8(out[(i-7)&15],1) ^ rotl8(out[(i-5)&15],6) ^ rotl8(out[(i-1)&15],5);
```

`(i-7)&15` 在 Go 里是 `int`，负数按补码与 15 相与，结果仍是 0..15；C 用 `int` 写表达式即可（`&15` 保证非负）。

### 4.2 变异 MD5 压缩（`fairplay_md5.go:45-76`）

```
void md5_compress(uint32_t st[4], const uint8_t block[64], int mutation);
```

与标准 MD5 的差异有两处：

1. **消息字按大端读取**（`fairplay_md5.go:47-49`）：`m[i] = BE_u32(block + 4i)`。
2. **`round == 31` 那一轮结束时改写消息调度表**（`fairplay_md5.go:70-72`）。

轮函数本身与标准 MD5 完全一致（F 与下标见 `fairplay_md5.go:55-64`、常量 `26-43`、移位 `19-24`）：

```
a,b,c,d = st[0..3]
for round in 0..63:
    f, word = 标准 MD5 的 F 与下标
    tmp = d; d = c; c = b; b = b + rotl32(a + f + K[round] + m[word], shift[round]); a = tmp
    if (round == 31) mutate(m, a, b, c, d, mutation)   // 用更新后的 a,b,c,d
st[i] += {a,b,c,d}
```

`mutate`（`fairplay_md5.go:78-106`）先算 8 个下标：

```
idx[0..3] = a&15, b&15, c&15, d&15
idx[4..7] = (a>>4)&15, (b>>4)&15, (c>>4)&15, (d>>4)&15
```

然后三种变异：

- **swap**（`fpsapSwapMutation`，`fairplay_md5.go:81-88`）：`for i in 0..7: swap(m[i], m[idx[i]])` —— **顺序执行**，后一次 swap 可能搬动前一次刚换过的字。
- **cycle**（`fpsapCycleMutation`，`fairplay_md5.go:89-97`）：`first = m[idx[0]]; m[idx[i]] = m[idx[i+1]] for i=0..6; m[idx[7]] = first`。下标可能重复，必须保持这个顺序语义。
- **kdf**（`fairplayKDFMutation`，`fairplay_md5.go:98-105`）：`swap(m[a&15], m[b&15]); swap(m[c&15], m[d&15]); for shift in {4,8,12}: swap(m[(a>>shift)&15], m[(b>>shift)&15])`。

字 ↔ 字节（`fairplay_md5.go:108-120`）：`words_from_le` 把 16 字节按**小端**装入 4 个字（初值 `fairplayInitialSessionKey` 用这个方向）；`words_be` 把 4 个字按**大端**输出 16 字节。两者方向不同，容易写反。

### 4.3 白盒两段网络（`fpsap.go:138-187` + `fpsap_tables.go`）

#### 表结构

`fpsap_tables.go` 不直接存 256 字节表，而是"13 个 256 字节置换基表"+"逻辑表的 (base, in_xor, out_xor) 三元组"：

```c
typedef struct { uint8_t table, in_xor, out_xor; } lookup;
uint8_t substitute(const lookup *l, uint8_t v) { return subst_bases[l->table][v ^ l->in_xor] ^ l->out_xor; }
uint8_t mix       (const lookup *l, uint8_t v) { return mix_bases [l->table][v ^ l->in_xor] ^ l->out_xor; }
```

- `subst_bases[6][256]` = 1536 B（`fpsap_tables.go:34-113`），6 个都是 0..255 的置换。
- `mix_bases[7][256]` = 1792 B（`fpsap_tables.go:115-208`），7 个也都是置换；**不是** GF 乘 2/3/9/11/13/14 表，不要尝试用 AES 的 xtime 生成。
- 两个网络的逻辑表：`round[9][16]`、`mix[4][4]`、`final[16]`，各 176 个 `lookup` 三元组，共 2×176×3 = 1056 B（`fpsap_tables.go:209-249`、`250-289`）。
- 基表归属（从三元组的 `table` 字段即可读出）：第一网络 round0 用 base 0、round1..8 用 base 1、final 用 base 2、mix 各列用 base {0,1,1,2}；第二网络 round0..7 用 base 3、round8 用 base 4、final 用 base 5、mix 各列用 base {3,4,5,6}。
- 另有 `firstInputMask[16]`（`fpsap_tables.go:25-27`）与 `secondOutputMask[16]`（`28-30`）。

**C 侧自检**：把三元组展开成完整表，按 `fpsap_test.go:11-57` 的顺序串联并算 SHA-256，应得

```
长度 = 90128 字节
sha256 = 28d0986abebe30458348dfa2957aa1d52d6f3ad5a9468c5d8a9c4139b7ca2b43
```
串联顺序：`firstInputMask[16]` → 对两个网络各做 `round[0..8][0..15]`（每个展开 256 字节）→ 每个 `mix` 输入列展开 `value*4 + outputByte` 共 1024 字节 × 4 → `final[0..15]`（每个 256 字节）。

#### 混合层（`fpsap.go:175-187`）

```c
// state 按 4 个 4 字节字处理；mix 索引是 [输入字节][输出字节]
for word in 0..3:
  off = word*4
  for ob in 0..3:
    m = 0
    for ib in 0..3: m ^= mix(mixcol[ib][ob], substituted[off+ib])
    state[off+ob] = m
```

#### 第一网络（`fpsap.go:138-158`）

```
state = fixedBlock ^ firstInputMask                         // fpsap.go:28-32, 25-27
for bank in 0..8:
    for o in 0..15:                                          // 注意：表索引 = 状态索引 = map[o]
        i = firstPositionMap[o]                              // fpsap.go:33-35
        substituted[o] = substitute(round[bank][i], state[i])
    mix(firstTables, state, substituted)
    for i in 0..15: state[i] ^= masks[bank][i]
for o in 0..15:
    i = firstPositionMap[o]
    out[o] = substitute(final[i], state[i])
```

#### 第二网络（`fpsap.go:160-173`）

```
state = in[16]
for bank in 8 down to 0:
    for o in 0..15:                                          // 表索引 = o，状态索引 = map2[o]
        i = secondPositionMap[o]                             // fpsap.go:37-39
        substituted[o] = substitute(round[bank][o], state[i]) ^ masks[bank][o]
    mix(secondTables, state, substituted)
for o in 0..15:
    i = secondPositionMap[o]
    out[o] = substitute(final[o], state[i]) ^ secondOutputMask[o]
```

**两段网络的索引方式不对称**：第一段表索引取 `map[o]`，第二段表索引取 `o`；且掩码在第一段是"混合之后整块异或"，在第二段是"每个 substitute 之后逐字节异或"。这是最容易写错的地方之一。

### 4.4 descriptor / masks / digest32 / exchange（`fpsap.go:79-204`）

#### descriptor（`fpsap.go:79-107`）

```
uint8_t padded[320] = {0};
off = 0;
memcpy(padded+off, descriptorPrefix, 17);  off += 17;   // fpsap.go:18-22
memcpy(padded+off, m3SAP, 128);            off += 128;  // 发送端 = localSAP
memcpy(padded+off, m2SAP, 128);            off += 128;  // 发送端 = remoteSAP
memcpy(padded+off, descriptorSuffix, 17);  off += 17;   // fpsap.go:23-27
padded[off] = 0x80;                                     // off == 290
wr_le64(padded+312, (uint64_t)off * 8);                 // = 2320，注意是 290 字节的比特数
```

状态初值：`st[i] = LE_u32(fairplayInitialSessionKey + 4i)`（`fairplay_crypto.go:12-15`）。

```
for each of 5 blocks (o = 0,64,128,192,256):
    add = sap_hash(padded + o)                       // 16 字节
    for w in 0..3: st[w] += LE_u32(add + 4w)         // ① 先把 hash 加进 state
    st = md5_compress(st, padded+o, FPSAP_CYCLE)     // ② 用已加上 hash 的 state 做压缩
    if (o == 256) {                                  // 末块：连压两次
        firstFinal = st;                             //    第一次的结果，只取 [0] 这个字
        st = md5_compress(st, padded+o, FPSAP_CYCLE)
    }
out[0..3]  = BE_u32(firstFinal[0]);
out[4..19] = BE(state[0..3]);                        // 16 字节
```

注意"末块两次压缩"：`firstFinal` 是第一次压缩后的状态，输出只取它的第 0 个字；其余 16 字节来自第二次压缩后的状态（`fpsap.go:97-105`）。

#### masks（`fpsap.go:109-126`）

```
st0 = {0x1d4a4587, 0x92f39fcc, 0x1d87d836, 0xcdc86697}   // 每次循环都从 st0 重新开始
suffix[15] = 57 d8 ee cb de fb cf 59 1c 27 a2 cf be b0 89
for i in 0..8:
    block[64] = {0}
    block[0..19] = seed
    block[20]    = i
    block[21..35]= suffix
    block[36]    = 0x80
    wr_le32(block+56, 0x320)      // 只写 4 字节，block[60..63] 保持 0
    st = st0
    md5_compress(st, block, FPSAP_SWAP)
    masks[i] = BE(st[0..3])       // 16 字节
```

#### digest32（`fpsap.go:128-136`）

```
block[64] = 0; block[0..15] = left; block[16..31] = right;
block[32] = 0x80; wr_le32(block+56, 0x100);          // 256 bit = 32 字节
st = {0xb9f3dcdc, 0xfbdc740b, 0x60f77f86, 0x51907216};
md5_compress(st, block, FPSAP_SWAP);
return BE(st[0..3]);
```

#### exchange（`fpsap.go:189-204`）

```
masks        = masks(seed)
intermediate = first_network(masks)
left         = digest32(intermediate, fixedBlock)
white        = second_network(left, masks)
digest       = digest32(left, white)
out[0..3]    = white[0..3];
out[4..19]   = digest;
```

`exchange_for_sap(m3SAP, m2SAP) = exchange_seed(descriptor(m3SAP, m2SAP))`。参数顺序恒为"发起方 SAP 在前、响应方 SAP 在后"（`fpsap.go:189`、`fairplay_crypto.go:97`、`receiver_fairplay.go:81`）。

### 4.5 SAS-CBC 消息密码（`fairplay_message.go`）

**与标准 AES-CBC 的差异**

| 维度 | 本算法 | 标准 AES-128-CBC |
|---|---|---|
| 轮密钥 | 直接给出 11 个 16 字节轮密钥：`roundKey0`、`middleKeys[mode][0..8]`、`roundKey10`（`fairplay_message.go:160-213`） | 由 16 字节主密钥经密钥扩展生成 |
| 数据长度 | 固定 128 字节 = 8 块，**无 padding** | 任意 16 倍数 + padding |
| IV | 按 mode 选 1 个固定 IV（`153-158`） | 由调用方给出，可每消息随机 |
| 解密顺序 | mode 3 时块**倒序遍历**（`fairplay_message.go:12-18`）；因链值取前一块密文，结果与正序相同 | 正序 |
| 轮结构 | 与 AES-128 完全一致 | 同 |

即：**它就是一个 AES-128 轮结构 + 外部注入轮密钥的 CBC**。因此 C 侧不能用 `mbedtls_aes_setkey_*`（无法注入自定义轮密钥）。

单块加密（`fairplay_message.go:74-89`）：

```
add_round_key(key0); sub_bytes(fwd); shift_rows;
for r in 0..8: mix_columns; add_round_key(middle[mode][r]); sub_bytes; shift_rows;
add_round_key(key10);
```

单块解密（`fairplay_message.go:32-50`）：

```
add_round_key(key10);
for r = 9 down to 1: inv_shift_rows; sub_bytes(inv); add_round_key(middle[mode][r-1]); inv_mix_columns;
inv_shift_rows; sub_bytes(inv); add_round_key(key0);
```

状态布局：`state[4*col + row]`（列优先，`fairplay_message.go:97-113`）。`shift_rows`：`s[4c+r] = p[4((c+r)&3)+r]`；`inv_shift_rows`：`s[4c+r] = p[4((c-r+4)&3)+r]`。`mix_columns` 与 `inv_mix_columns` 作用于每 4 个连续字节（`126-135`、`115-124`），GF 乘见 `137-151`（0x1b 多项式）。

CBC 加密（`fairplay_message.go:52-72`）：

```
chain = IV[mode]
for b in 0..7: tmp = plain[b*16..] ^ chain; encrypt_block(tmp, mode); out[b*16..] = tmp; chain = out[b*16..]
```

CBC 解密（`fairplay_message.go:8-30`）：

```
for step in 0..7:
    b = (mode == 3) ? 7-step : step
    decrypt_block(cipher[b*16..], mode) -> tmp
    chain = (b > 0) ? cipher[(b-1)*16..] : IV[mode]
    plain[b*16..] = tmp ^ chain
```

C 侧一律使用**独立输入/输出缓冲**，不要原地操作（Go 的 mode 3 倒序技巧正是为原地安全而写）。

### 4.6 wrapping key（`fairplay_crypto.go:27-51`）

```
void wrapping_key(const uint8_t receiverSAP[128], const uint8_t m3[164], uint8_t out[16]);

mode = m3[12];
sas_cbc_decrypt(mode, m3+16, decrypted /*128*/);

material[320] = {0};
off  = 17; memcpy(material,     kdfPrefix, 17);          // fairplay_crypto.go:17-21
off += 128; memcpy(material+17, decrypted, 128);
off += 128; memcpy(material+145, receiverSAP, 128);
off += 17;  memcpy(material+273, kdfSuffix, 17);         // fairplay_crypto.go:22-26
material[290] = 0x80;
wr_le64(material+312, 290*8);                            // 2320

st[i] = LE_u32(fairplayInitialSessionKey + 4i);
for o in 0,64,128,192,256:
    mod = md5_compress(st, material+o, FAIRPLAY_KDF);    // 先压缩
    h   = sap_hash(material+o);
    for w in 0..3: st[w] = mod[w] + LE_u32(h + 4w);      // 用"压缩结果 + hash"整体替换
out = BE(st[0..3]);                                      // 16 字节 → AES-128 密钥
```

**与 descriptor 的组合方式不同**：descriptor 是"先加 hash，再压缩"，wrapping key 是"先压缩，再把压缩结果与 hash 相加"。两者的块数与 padding 完全相同（290 字节数据 → 320 字节、5 块）。

### 4.7 ekey 72 字节（`fairplay_crypto.go:66-103`）

| offset | size | 内容 | 生成方式 |
|---|---|---|---|
| 0 | 16 | `46 50 4c 59 01 02 01 00 00 00 00 3c 00 00 00 00` | 常量（`fairplay_crypto.go:76-79`；接收端严格比对 `receiver_fairplay.go:110-116`） |
| 16 | 16 | 掩码 mask | CSPRNG |
| 32 | 4 | `00 00 00 10`（BE 16 = 原始密钥长度） | `BE_u32(len(rawKey))` |
| 36 | 20 | HMAC-SHA1 | `HMAC-SHA1(macKey, ekey[0:36] || rawKey)` |
| 56 | 16 | `AES-128-ECB(wrappingKey, rawKey ^ mask)` | 单块加密 |

其中 `macKey = descriptor_for_sap(senderSAP, receiverSAP)`，`senderSAP` 由 **m3 body 解密** 得到（`fairplay_crypto.go:95-97`；接收端做同样计算 `receiver_fairplay.go:126-131`）。

`wrappingKey` 与 `macKey` 的差异：前者输入是 `(receiverSAP, m3)`，后者输入是 `(senderSAP, receiverSAP)`。两者不可混用。

### 4.8 流密钥派生（`fairplay.go:170-177`）

```
if (!mixPairVerify || ecdhLen == 0) fpKey = rawKey;                       // 16 字节
else { SHA-512(rawKey || ecdhSecret) → 取前 16 字节 }
```
`mixPairVerify` 由配对流程协商决定（HAP 选中混合分支；物理 AppleTV3 走 raw 分支，见 `fairplay.go:108-117` 与 `fairplay_streamkey_test.go` 的注释）。

---

## 5. 依赖的密码学原语清单

| # | 原语 | 用途 | mbedtls 可用性 |
|---|---|---|---|
| 1 | AES-128 ECB，单块加密 | ekey[56:72]（§4.7） | ✅ `mbedtls_aes_setkey_enc` + `mbedtls_aes_crypt_ecb` |
| 2 | AES-128 轮结构 + 注入式轮密钥 | SAS-CBC（§4.5），4 种 mode | ❌ 公共 API 不支持自定义轮密钥。两条路：(a) 自己实现 4 个原语 `sub_bytes/shift_rows/mix_columns/inv_*`（约 100 行，需要正向 + 逆向 S-box，逆向 S-box 见 `fairplay_message.go:215-232`，正向由逆向反查得到 `234-239`）；(b) 直接填 `mbedtls_aes_context.rk`（注意 3.x 的 `rk_offset` 与 `MBEDTLS_AESNI_C`/`MBEDTLS_PADLOCK_C` 会改用不同存储）为 11 组轮密钥，再调 `mbedtls_internal_aes_encrypt`——依赖内部布局，不推荐。**推荐 (a)** |
| 3 | HMAC-SHA1 | ekey[36:56] | ✅ `mbedtls_md_hmac`（密钥 20 字节） |
| 4 | SHA-512 | 流主密钥混合（§4.8） | ✅ `mbedtls_sha512` |
| 5 | MD5 压缩轮（64 轮 + 常量 + 移位） | descriptor / masks / digest32 / wrapping key | ❌ **不能用** `mbedtls_md5`：本算法按大端读字、且第 31 轮后有消息调度变异（§4.2）。必须自写，但只需压缩函数，不需要 `init/update/final` |
| 6 | 字节置换表（13×256） | 白盒网络 | ❌ 数据表，随代码编译进去 |
| 7 | SHA-256 | **仅测试**：表展开校验、SAP hash corpus | 可用 mbedtls 或自写 |

**不需要** AES-192 / AES-256 / CBC（对外）、GCM、ChaCha20（ChaCha 属于后续数据面，不在本握手内）。

随机数需求：localSAP 126 字节（`fpsap.go:55`）、m3 后 16 字节 IV（`fairplay.go:80-84`）、rawKey 16 字节（`fairplay.go:91-93`）、ekey mask 16 字节（`fairplay_crypto.go:80-82`）。全部可直接用 `mbedtls_ctr_drbg_random`。

---

## 6. 测试向量

> 表中"层级"列：**最终值** = 对外可见的协议字节；**中间值** = 某个派生阶段的输出，用于定位错误。
> "来源"列：*项目测试* = 代码里写死的断言；*参考实现 dump* = 由本项目代码运行后打印的派生值（用同一份 go 源码，未改动逻辑），C 侧可据此逐段比对。

### 6.1 SAP hash

| 用例 | 输入 | 期望 | 层级 | 来源 |
|---|---|---|---|---|
| 单向量 | `block[i] = i*3+1`（64 B） | `75498a4e218773030e9cdf04f0c49367` | 最终值 | 项目测试 `fairplay_vectors_test.go:30-33` |
| corpus | 64 个 xorshift 块（见下） | 64 个 16 字节摘要串联后 `sha256 = 36ad2a7920076af59452d9f0c91e3b7d1aebc53f9143bd6819e39119d4535c92` | 最终值（聚合） | 项目测试 `fairplay_vectors_test.go:36-53` |

corpus 生成器（`fairplay_vectors_test.go:38-52`）：

```
uint64_t state = 0x6a09e667f3bcc909;
for (k = 0; k < 64; k++) {
    for (i = 0; i < 64; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        block[i] = (uint8_t)state;
    }
    digest = sap_hash(block); sha256_update(&ctx, digest, 16);
}
```

### 6.2 变异 MD5

固定输入：`block[i] = i*3+1`，初值 `st = LE_u32(00 01 .. 0f)`。

| 变异 | 期望（按 4 字的大端字节序） | 期望（按 4 字的小端序列化，项目测试写法） | 来源 |
|---|---|---|---|
| KDF | `cb28f7f6b697435a924f6675aa59b891` | `f6f728cb5a4397b675664f9291b859aa` | 项目测试 `fairplay_vectors_test.go:26-33`（LE 写法） |
| SWAP | `e5c410068489d23a0b9ed242c0f0a487` | `0610c4e53ad2898442d29e0b87a4f0c0` | 参考实现 dump（由 §6.3 的 exchange 向量间接覆盖） |
| CYCLE | `290d7c25132dd69555d47bca3156b0ce` | `257c0d2995d62d13ca7bd455ceb05631` | 同上 |

### 6.3 descriptor + exchange（7 组，全部为最终值，`fpsap_test.go:55-97`）

固定 `localSAP = fpsapReferenceLocalSAP()`（`fpsap_test.go:305-313`）：

```
0001e4e3dd688293e6fa66b95ba41768e587c65f750218ff1be21543d573cefb
087bd36e0c6363c3c8242f4abcfa6d660b801032015405eb4ab04dda7aeff38f
fb36f4cfa48f0b5d92ae363f68b45925bbe6413ab6bdc4968f548d21e67d20f1
912b6820e53f1013cde29df7350a9b9fa7c51320aea62d2949786c87642e34ba
```

每组先给出 m2 body 密文（128 B），测试用 mode 3 解密后再算 descriptor 与 exchange：

| # | m2 body 密文 | 解密所得 m2SAP | descriptor(20 B) | exchange 输出(20 B) |
|---|---|---|---|---|
| 1 all-zeros | 全 0 | `00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0` | `7e38958ffe4ed433743919fe7eb16376afa4eb9e` | `6f627565f3e77f5b5ede91beee7baf92e4241e0b` |
| 2 all-ff | 全 `0xff` | `34c4dc74e4e1e7c0e4282a0a271c041fec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792ed` | `0e521732cc8f7712c4330b73dc6168078af62d47` | `dc2cc74f2ed55484f59f95b96082f0f5c017dd17` |
| 3 captured-m2 | 见下方 captured m2 的 `[14:142]` | `00018b2d9ac45a1d0134ccc1d9f09168da5298d19f066ca3355634076b7ff73635bae69475e68ccc45e9800df9e3f4ebe38d9068d3488cfde5140d146ad834d87d81b55e54989e5b5b329bfc9be1807ef1ab9107e91cf8725d6b2af6dd5ab02e76b28cbc05cd82c950ffd5f213d52ff45dee7e842b897d43face99c853ead538` | `88f54b0f07b660c8a81b1029e777fccce590540e` | `4b911e48af23d8406368aeafbb61bfcd569e3e55` |
| 4 0x42@0 | `42 00…` | `bec2a74439b984a5000fa240b9a4d15e6542e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0` | `638cf32c301873bea451169f6bd3a3e8c96cf875` | `9bfb9556b8659c2ac94b7ef9e587d71e159ea624` |
| 5 0x42@63 | 第 63 字节 `0x42` | `00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a00d4c7110dc712e93ffd3b47ab9a2e8342742e5a3c2b8e42d442793c7a44409e22742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0` | `1de544d6a75911d3836071200306f49268d6559a` | `150d9fa4eb456e73ba48de5779c5c996b16b3b23` |
| 6 0x42@64 | 第 64 字节 `0x42` | `00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a099c52c651a8f9adfdc92a951eaf0b8536542e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0` | `1c74ef083db6ad37300d4b1aab7facdc9ea4b956` | `a167db30424ff8890d085c0f1c92b2c5cc06fc45` |
| 7 0x42@127 | 第 127 字节 `0x42` | `00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a00d4c7110dc712e93ffd3b47ab9a2e834` | `e3bac81ee204258c1248ee8b23669615c5ac7120` | `d246ec5e7adc8118994b8df77146529486ac7caf` |

第 3 组用到的真实抓包 m2（142 B，`fpsap_test.go:60`）：

```
46504c59030102000000008202034a114c26b77d4e2eec2c8f89fdb653b5b32d3576bc176816d110
a14c3f53c08dbb936183bfdfe0a4f3c12e85216003b46f738c40c54da6c436d29d1b342d63c7b314
309ae79a33bb1787709ef077cbfe4190117a3423e270fd1a2eac44da1a7934f59dc681d1b70783f2
28c4d077c2d495f5285c3bf8df586fc2ebfe17fb5b65
```

另有 descriptor 专项 2 例（`fpsap_test.go:80-97`）：全 0 密文 → `7e38958f…`（同上第 1 组）；密文 `01 00…`（首字节 1）→ `ea46797d726c6a9be43ffa72385ff97ce1c54f1b`。

### 6.4 完整 m1 → m3 → ekey 链（中间值 + 最终值，参考实现 dump，源自 `fpsap_test.go:99-181` 的场景）

确定性熵：`localSAP = 00 01 01 02 03 … 7e`（即 `[0]=0,[1]=1,[2+i]=i+1`，共 128 B）。

| 项 | 值 |
|---|---|
| m1 | `46504c590301010000000004020003bb`（项目测试 `fpsap_test.go:114`） |
| 接收端 SAP（固定） | `0001cc342a5e5b1a6773c20e21b8224df862481864ef810aae2e3703c8819c23539de5f5d749bc5b7a266c496283ce7f03937ae1f616de0c15ff338ccaffb09eaabbe40f5d5f558fb97f1731f8f7da60a0ec6579c33ea98312c3b67135a6694ff82305d9ba5c615fa254d2b1834583cee42d4426c835a7a5f6c8421c0da3f1c7`（`fairplay_key_test.go:158-167`） |
| m2（mode 3） | `46504c59030102000000008202039001e1727e0f57f9f5880db104a6257a23f5cfff1abbe1e93045251afb97eb9fc0011ebe0f3a81df5b691d76acb2f7a5c708e3d328f56bb39dbde5f29c8a17f481487e3ae863c678325422e6f78e166d18aa7fd636258bce28726f661f738893ce44311e4be6c0535193e5ef72e8686233729c227d820c999445d89246c8c359` |
| m3（164 B，最终值） | `46504c590301030000000098038f1a9c7ddd506f27dfcaf90b08b73940966603a44b2ddb4df5ea46b9846ae655124558231ace6d9cc1419392a7d2e7852139b94301f227ab0427b748961c0fb9e2a27a8929618638e29a99a6d5743bb03273f5f98932495b3abfad59110341f892728cee7447a7e3eea1e7a29825da08be9dd4cfe45b5a3e925ad6d4169a217f7fb0672ff6f0d976a8edafa9013d99cc3efe6ee0bd522f` |
| m3 tail（= m4 body） | `2ff6f0d976a8edafa9013d99cc3efe6ee0bd522f` |
| m3 body 解密 | 等于 localSAP（`fpsap_test.go:129-136`） |
| m4 | `46504c590301040000000014` + 上面的 tail |
| ekey（mask 全 `5a`，rawKey = `00 01 … 0f`） | `46504c59010201000000003c000000005a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a00000010806e886eba85f78947fe5cd52d8af2f0873833b39a11b854e4a681872bf5f46226c182fc` |
| 该 ekey 的 macKey(20 B) | `95db1c9222269adb677097e6ac4175f6d431e7be` |

模式覆盖测试（`fpsap_test.go:183-241`）：接收端 SAP 固定为 `[1]=1, [i]=i*7+3`，验证 4 种 mode 下 `m3[12] == mode`、body 解密等于 localSAP、tail 等于 `exchange(localSAP, receiverSAP)`（tail 与 mode 无关，本例恒为 `2b579e0e026e67f1f50b242c076c536a0a2fec47`）。

### 6.5 消息密码（4 组，最终值，`fairplay_vectors_test.go:55-90`）

输入消息 body：`cipher[i] = (i+16)*5+7`（i=0..127），`message[12] = mode`。

| mode | 解密结果（128 B） | 再加密应还原输入 | 由该消息导出的 wrapping key |
|---|---|---|---|
| 0 | `b66a3295ffa6b56e02ed1b3d67fef74b90fe148570de65e6773669126a4905d8405644cae0b2f5ed6109c099c7aea7398dac8d623fbd69b87242b374d98f89502bb5a63e29c46a8ed0e98466966191ec1e6c8675087fde21337db1c8fab4c21db824026335f6fc37e2e5b6f53357d06994bd383d6029a0aff654fb1521bcdde4` | 是 | `f7dd1ccb9e745f7951a6e325d73a1f5f` |
| 1 | `0f95c6ddc8987eda18577da2db074e7c04715af8b3914a73be1b3d6c111953017ee0a39dfcab3e0d57f2f9fbd59c5e18101788c2ab8e3cbb403bcb48b53f3e5bf74f949e79fa5ca679df4bfcb33a69b1442675d03f948fe5bd0c5ffb64b73a5ab58f46d6baae097b599624147c2487991163ecffc4d966240f9526346a10fdb0` | 是 | `b44ad891396f097aa309bc132f5b8889` |
| 2 | `40f18751b44d733e0aa0416401a7d3f40375fad3ce56900602578bca14660909820e6ef3a5e943cafef5370f72c52177d9b82278b414811201a3d99202bedcca26a4d1ad08bc2669f4bae6ca54b8a120d0425edb6082f51f5aecdb547bfdb319099c9ea2729ae6a1c4480827ce9991e273843cf1c7d74ebbebc2657659bcea9f` | 是 | `d38cd8efecdb20f333273c4312d9b236` |
| 3 | `70a3c30edf0e1dfa1785ce4336ed547062672a47f714a0c1f89a83d95691103dfe5cf653d4cb8299793faf33fd0d4482ef5333b41ab094a90e1baf996bcf4989783f6918397fbacddaf00a2b97556dd8099841578bc5eb1444912b47298eaf356fdd6701bb3f64e725a80eb4c6f3556195de35c93e7cc703bdd24351468e9847` | 是 | `769e2fe4c5ad7fbe6fd6772d00f529f4` |

wrapping key 列使用固定 receiverSAP = `fairPlayReferenceReceiverSAP()`（同上 §6.4 表内那个 SAP）。mode 3 的记录额外验证了"原地解密"（`fairplay_vectors_test.go:82-86`）。

### 6.6 ekey 与解包

| 用例 | 输入 | 期望 | 层级 | 来源 |
|---|---|---|---|---|
| 封装往返（4 种 mode） | `testFairPlayM3(mode)`（`fairplay_key_test.go:136-146`）+ `testFairPlayReceiverSAP(mode)`（`148-154`）+ mask `01..10` | ekey 头 `46504c59010201000000003c00000000`；`[32:36] == 16`；`[16:32] == mask`；`[36:56] == HMAC-SHA1(macKey, ekey[0:36]\|\|rawKey)`；解包回原 key | 最终值 | 项目测试 `fairplay_key_test.go:13-55` |
| 上游 MAC 向量 | m2 = `46504c5903010200000000820201cf32a2…688a`，m3 = `46504c590301030000000098018f1a9c5b9228…2ee4`，rawKey `000102…0f`，mask `c853e777b9b65e7652d768d97c974f15` | m2 解密 = reference receiver SAP；ekey 前 56 字节 = `46504c59010201000000003c00000000c853e777b9b65e7652d768d97c974f15000000102a53a0008888fe26bfb1e1f825f38f50d6730059`；macKey = `2fd95dc2c23122bc77c57b983a9188c4760db322` | 最终值 + 中间值 | 项目测试 `fairplay_key_test.go:74-105` |
| 抓包解包 | m3 = `46504c590301030000000098018f1a9c7d0af2…067f2a`，ekey = `46504c59010201000000003c0000000088e4f82c…eee18` | rawKey = `8e1214398d46d72e7b1b8e32f80c8bf0` | 最终值 | 项目测试 `fairplay_key_test.go:107-120` |
| 同一向量的中间值 | 同上 | wrapping key = `bc94d7985601318b0ed5d9d33af125e8`；senderSAP = `0001e023dda8b803…370ca5`；macKey = `291c49a06396bd0efe996926c7ca5cf52dc2d2cf` | 中间值 | 参考实现 dump |

### 6.7 结构/参数校验（C 侧也应实现这些拒绝路径）

| 检查 | 出处 |
|---|---|
| m2 长度必须是 142、magic/版本/类型匹配、声明长度 130 | `fpsap.go:207`、`266-281` |
| m2[12] 必须为 2；m2[13] 必须 < 4 | `fpsap.go:210-216` |
| m4 必须 32 字节且 `m4[12:32] == m3[144:164]` | `fpsap.go:253-264` |
| m3 的 mode 必须 < 4；m3 长度 164 | `fairplay_crypto.go:68-74` |
| ekey 长度 72、头 16 字节精确匹配、`[32:36] == 16` | `receiver_fairplay.go:106-122` |
| m1 的 4 字节 payload 必须精确等于 `02 00 03 bb` | `receiver_fairplay.go:55-58` |

---

## 7. 可移植性风险点

### 7.1 Go 运算符优先级（**实际踩到的坑**）

Go 里 `*  /  %  <<  >>  &  &^` **同级、左结合**；C 里 `<< >>` 高于 `&`，`&` 高于 `^` 高于 `|`。两处真实差异：

| 代码 | Go 语义 | C 里若照抄 | 后果 |
|---|---|---|---|
| `aux[4]&0xca>>1`（`fairplay_sap.go:147`） | `(aux[4] & 0xca) >> 1` | `aux[4] & (0xca>>1)` | `expandedSelector` 错 → `matrix[18]` 起全错 → SAP hash 错 |
| `0x12 \| selectBits(...) & (...)`（`fairplay_sap.go:196-197`） | `0x12 \| (sel & m)` | 若写成 `(0x12 \| sel) & m` | `work[143]` 错 → SAP hash 错 |

**规则**：转写每条语句时，对每个二元运算显式加括号，宁可多写括号，也不要依赖优先级。附录 A 的转写就是这么做的。

### 7.2 uint32 下溢（`fairplay_sap.go:60-63`）

`(i-155)%210` 在 Go 中是 `uint32` 运算；C 必须用 `uint32_t` 和 `155u`/`210u` 字面量。用 `int` 会得到 `(-109)%210 == -109`，索引直接越界或语义全变。同理 `for i := range uint32(840)` 的循环变量类型就是 `uint32`。

### 7.3 字节算术的截断点

Go 的 `byte` 运算每一步都 mod 256。C 里 `uint8_t` 参与运算会先提升为 `int`，只在**赋值时**截断。对本算法用到的 `+ - * << & | ^`，最后截断与每步截断结果相同（模 256 同态），因此可以直接写；但下面几类**不能**这样：

1. **除法 / 右移**必须作用在已截断的 8 位值上。例如 `work[99]/3`、`aux[9]/3`、`s(57)*231`（先乘后再 `/5` 的 `digestIndex`）。
2. 下面这些位置 Go 显式转成 `int`，**不能**用 `uint8_t` 算：
   - `byte((int(work[177])+int(s(79)))>>1)` 与 `byte(3*int(work[148])/5)`（`fairplay_sap.go:129`）
   - `byte(int(224|s(92)&27) * int(m(41)) / 3)`（`:122`）
   - `byte((int(selectBits(...))+int(selectBits(...)))/5)`（`:166`）
   - `byte(wide * wide * wide >> 1)` 与 `byte(wide * wide / 5)`（`:153`、`:192`），此处 `wide` 是 `int`（0..255）
   - `wideSeed` 内部的 `(int)in<<n | (int)in>>(8-n)`：**不截断到 8 位**再 `%21`
3. **一元负号**：`-s(190)&7`、`-(h(61)/5)&7`、`-m(17)*(m(17)&1)`、`-square(s(45))&7`。Go 对 byte 取负等于 `(256-x) mod 256`；C 里写成 `(-(int)x) & 7` 或 `(uint8_t)(-(int)x) * (m&1)` 都能得到相同结果（补码下低 3 位一致），但**不要**写成 `7 - (x&7)`（x≡0 时不同）。

### 7.4 两个"rotate"助手不是一回事

- `rotl8(v,n)` = `bits.RotateLeft8(v,n)`：`n==0` 返回 `v`。
- `rotateOrZero(v,n)`：`n==0` 返回 **0**（`fairplay_sap.go:7-12`）。

电路里两次用到 `rotateOrZero`（`fairplay_sap.go:107`、`:147`、`:190`、`:199`），都用 `&7` 掩过，n 可能为 0，语义差别是致命的。用同一个宏实现两者必然出错。

### 7.5 白盒网络的两处非对称

- 表索引：第一网络用 `map[o]`，第二网络用 `o`。
- 掩码位置：第一网络在 `mix` 之后整块异或；第二网络在每次 `substitute` 之后逐字节异或，最终输出再异或一次 `secondOutputMask`。
- 循环方向：第一网络 bank 0→8，第二网络 bank 8→0。

### 7.6 缓冲区/别名语义

- `decryptFairPlayMessage(message, plaintext)` 允许"原地"（`mode==3` 时倒序块处理正是为此，`fairplay_message.go:12-18`）。C 侧统一用**独立缓冲**，避免依赖这一点。
- Go 的 `copy(out[:], aux[:3])` / `copy(out[4:], aux[3:])` 只写了 10 字节，`out[3]`、`out[11..15]` 保持 0（`fairplay_sap.go:80-81`）；随后还有 `+=0xe1` 和两次赋值。C 侧务必先 `memset(out,0,16)`。
- `aux`、`work` 初值为全 0（Go 结构体零值）；C 侧要显式 `memset`。
- 各处长度都是固定常量：SAP 128、body 128、m3 164、ekey 72。Go 里有 `len()` 校验，C 侧用 `static_assert`/断言即可。

### 7.7 字节序混用（最容易造成"只差几个字节"的错误）

| 位置 | 方向 |
|---|---|
| 协议长度字段（FPLY 头 `[8:12]`、ekey `[32:36]`） | **大端** |
| MD5 padding 长度（`block[56:60]` 或 `[312:320]`） | **小端** |
| MD5 消息字读入 | **大端**（`fairplay_md5.go:47-49`） |
| MD5 状态初始化 | **小端**（`fairplay_md5.go:108-113`） |
| MD5 摘要序列化 | **大端**（`fairplay_md5.go:115-120`） |
| SAP hash 输出喂给 MD5 状态时 | **小端**（`fpsap.go:94`、`fairplay_crypto.go:49`） |

项目测试里 `f6f728cb…` 与 `cb28f7f6…` 的差异就来自这里：前者按小端逐字打印，后者是 `wordsBigEndian` 的结果，两者是同一组 4 个字。

### 7.8 其他 Go 特性

- **map 迭代顺序**：本算法未使用 map，无风险。
- **大整数**：未使用 `math/big`；只有 `uint32`/`uint64` 的模运算，C 的 `uint32_t` 天然一致。
- **slice 别名/越界**：`copy(material[offset:], ...)` 依赖 `offset` 累加值恰好 17/145/273/290；C 侧用计算出的常量并加断言。
- **`for i := 0; i < 256; i++ { out[(i-7)&15] }`**：Go 里 `i` 是 `int`，`&15` 保证非负；C 用 `int` 同写即可。若把 `i` 写成 `unsigned`，`(i-7)&15` 依旧是 0..15（模 16），结果相同，但不要改成 `%`（负数取模在 C 里是 -7%16=-7）。
- **测试里的 `string(rune('0'+mode))`**（`fairplay_key_test.go:14`）等仅用于子测试命名，与算法无关。

### 7.9 mbedtls 相关

- 不要用 `mbedtls_aes_*` 实现 SAS-CBC（自定义轮密钥）。若确实想复用，可只在 `MBEDTLS_AESNI_C` 关闭时填 `ctx->rk`，但这依赖私有布局，升级会断。
- `MBEDTLS_MD5_C` 不能复用（大端字 + 消息调度变异）。
- HMAC 的 key 是 20 字节（descriptor 输出），不是 16。
- 所有 buffer 都是定长，无需动态分配；单个最大缓冲为 320 字节（descriptor / KDF 的 padding 缓冲）。

---

## 8. 建议的实现顺序与自检清单

1. **MD5 压缩 + 三种变异** → 对 §6.2 三条向量。先过 kdf，再过 swap/cycle（后两者会被 §6.3 覆盖）。
2. **SAP hash** → 先对 §6.1 单向量，再过 64 组 corpus 聚合；失败时按附录 A 的语句顺序定位（可以仿照本文做法：在 Go 侧插入状态打印，与 C 侧逐语句比对 `hash/matrix/aux/work`）。
3. **SAS-CBC** → 对 §6.5 四组（解密 + 再加密 + 原地）。
4. **white-box 表展开** → 对 90128 字节 SHA-256。
5. **descriptor / masks / digest32 / networks** → 对 §6.3 七组（先 descriptor，再 exchange）。
6. **m1/m2/m3/m4 组装与校验** → 对 §6.4 的 m3/m4。
7. **wrapping key + ekey** → 对 §6.5 的 wrapping key、§6.4 的 ekey、§6.6 的解包向量。
8. **流主密钥** → SHA-512 分支。

---

## 附录 A：SAP hash 非线性电路的逐语句 C 转写（已用 65 组向量验证）

下文可直接编译：`sap_seed` / `sap_initial_hash` / `sap_initial_matrix` 由附录 B 的常量提供，其余依赖见函数前声明。`u32` 只用于两处 `wide` 的三次方/平方截断，换成 `uint32_t` 等价。调用约定：`hash[20]`、`matrix[35]` 为初值，`aux[10]` 全 0，`work[210]` 已完成 §4.1 的 Step 1/2；本函数只做 Step 3，返回后仍需 Step 4 的折叠。

```c
typedef uint8_t u8;
typedef uint32_t u32;

static u8 rotl8(u8 v, unsigned n) { n &= 7; return n ? (u8)((v << n) | (v >> (8 - n))) : v; }
static u8 majority(u8 a, u8 b, u8 c)                     { return (u8)(a ^ ((a ^ b) & (a ^ c))); }
static u8 select_bits(u8 mask, u8 s, u8 c)               { return (u8)(c ^ ((s ^ c) & mask)); }
static u8 square(u8 v)                                   { return (u8)(v * v); }
static u8 cube(u8 v)                                     { return (u8)(v * v * v); }
static u8 rotate_or_zero(u8 v, u8 n)                     { return n == 0 ? 0 : rotl8(v, n); }
static u8 wide_seed(u8 in, u8 n)                         /* sap_seed[] 见附录 B */
{
    if (n == 0) return sap_seed[0];
    int r = ((int)in << n) | ((int)in >> (8 - n));
    return sap_seed[r % 21];
}

#define H_(i)  hash[work[(i)] % 20]
#define S_(i)  sap_seed[work[(i)] % 21]
#define M_(i)  matrix[work[(i)] % 35]
#define MA_(i) matrix[aux[(i)] % 35]
#define HI_(i) hash[(i) % 20]
#define SI_(i) sap_seed[(i) % 21]

/* 调用前：hash[20]/matrix[35] 已拷贝初值，aux[10] 全 0，work[210] 已由 Step 1/2 填好。
   返回后仍需执行 Step 4 的折叠。 */
static void sap_nonlinear_circuit(u8 hash[20], u8 matrix[35], u8 aux[10], u8 work[210])
{
matrix[12] = (u8)(0x14 + (select_bits(92, work[64], (u8)(work[99] / 3)) & wide_seed(S_(206), 4)));
work[4] = (u8)(2 * square((u8)(work[99] / 5)));
work[153] ^= (u8)(square(M_(203)) * work[190]);
hash[3] = (u8)(0x13 ^ (((S_(205) >> 1) & 0x10)));
work[33] = (u8)(work[33] - (u8)(S_(36) & (u8)~9));
aux[5] = (u8)((((M_(67) & (u8)~2) | 1 | ((H_(181) >> 6) & 2) | (hash[3] & 0x10))) - 15);
matrix[12] = 0x07;
work[2] = (u8)(work[2] - 64);
hash[19] = S_(58);
aux[4] = (u8)(92 - M_(32));
aux[9] = (u8)(M_(15) + 0x9e);
work[34] = (u8)(work[34] + (u8)(SI_(aux[9]) / 5));
hash[19] = (u8)(hash[19] + (u8)(0xe6 ^ ((HI_(aux[9]) >> 1) & 0x66)));
work[15] ^= (u8)(3 * rotate_or_zero(work[72], (u8)((-S_(190)) & 7)) - 9 * S_(126));
hash[15] ^= cube(M_(181));
matrix[4] ^= (u8)(work[202] / 3);
matrix[1] = (u8)(matrix[1] + cube(majority((u8)(92 - HI_(aux[4])), (u8)~work[105], 0xc6)));
hash[19] ^= (u8)(((int)(224 | (S_(92) & 27)) * (int)M_(41)) / 3);
work[140] = (u8)(work[140] + rotate_or_zero(92, (u8)((-work[5]) & 7)));
matrix[12] = (u8)(matrix[12] + majority((u8)(~work[4] ^ M_(12)), work[182], 192));
work[36] = (u8)(work[36] + 125);
work[124] = rotl8(majority(majority(work[138], hash[15], 74), H_(43), 95), 4);
{
    u8 aux_hash = HI_(aux[9]);
    aux[1] = (u8)(0x4c & (u8)~(aux_hash & (u8)(S_(68) << 1)));
    aux[2] = (u8)(222 - majority((u8)(((int)work[177] + (int)S_(79)) >> 1), (u8)((3 * (int)work[148]) / 5), matrix[1]));
    matrix[16] = (u8)(matrix[16] + (u8)((((MA_(4) & (u8)~0x60) | aux_hash | 8)) - (u8)((rotl8(work[33], 2) | 128))));
    hash[14] ^= MA_(2);
    work[19] = (u8)(work[19] + majority(rotate_or_zero(SI_(H_(201)), (u8)((M_(112) << 1) & 6)),
                                       (u8)((((H_(208) & (u8)~0x7c)) | (H_(164) & 0x7c)) / 5), 37));
    matrix[8] = (u8)(rotate_or_zero(140, (u8)((-square(S_(45))) & 7)) ^ aux[4]);
    work[190] = 56;
    work[53] = (u8)~(((u8)(H_(83) | 204)) / 5);
    hash[13] = (u8)(hash[13] + H_(41));
    hash[10] = (u8)(majority(MA_(4), work[2], aux[2]) / 15);
    aux[3] = (u8)(92 - square((u8)(0x28 | (MA_(1) & (u8)(0x12 | (S_(2) & 4))))));
}
{
    u8 seed_bits = SI_(aux[4]);
    matrix[13] ^= seed_bits;
    aux[6] = (u8)(92 + square(majority((u8)(M_(179) - 38), aux[2], 177)));
    u8 expansion_bits = majority((u8)(aux[3] + (aux[4] & 74)), (u8)~seed_bits, 121);
    work[47] ^= (u8)(M_(89) + majority((u8)(expansion_bits ^ 0xa6), aux[4], 4));
    aux[7] = (u8)((u8)(seed_bits / 3) - MA_(9) -
                  (u8)(0x14 | (work[151] & (u8)((aux[4] & 0x88) | 0x62)) | (aux[4] & 0x22)));
    u8 expanded_selector = (u8)(expansion_bits ^ (u8)((aux[4] & 0xca) >> 1) ^ 75);
    aux[9] = (u8)(aux[9] + (u8)(0x80 | (majority(aux[7], work[151], 0x20) & 0x64) |
                               (seed_bits & 0x44) | (MA_(9) & 0x1b)));
    matrix[33] ^= work[26];
    matrix[30] = (u8)((u8)((aux[9] / 3) - (u8)((aux[4] & (u8)~8) | 0x13)) ^ H_(122));
    work[22] = (u8)((M_(90) & 0x1b) | 0x44);
    {
        int wide = (int)select_bits(71, matrix[expanded_selector % 35], SI_(aux[5]));
        matrix[18] = (u8)(matrix[18] + (u8)((u32)(wide * wide * wide) >> 1));
    }
    matrix[5] = (u8)(matrix[5] - S_(92));
    matrix[18] ^= (u8)(select_bits(aux[3], MA_(3), select_bits(16, M_(183), work[41])) *
                       select_bits(expanded_selector, H_(59), work[17]));
    matrix[22] = (u8)(majority(select_bits((u8)(hash[14] | 28), (u8)((work[7] & 28) | 0x82), H_(93)),
                               rotate_or_zero(MA_(4), (u8)(rotate_or_zero(work[11], (u8)((-M_(28)) & 7)) & 7)),
                               matrix[33]) + 74);
    hash[15] = (u8)(hash[15] - majority(majority(aux[3], aux[4], 214), SI_((u8)(H_(39) ^ 217)), aux[6]));
    {
        u8 hash9 = HI_(aux[9]);
        u8 indexed_hash = HI_((u8)((u8)((aux[4] / 3) - (u8)(aux[9] | work[22])) ^ aux[6] ^
                                   (u8)((((M_(57) | hash9) & (u8)(0x52 | (aux[9] & 0x0d))) |
                                         (((u8)((M_(57) & hash9) | aux[9])) & 0x20)))));
        aux[6] = (u8)(square(square(H_(99))) | MA_(9));
        aux[1] = (u8)(aux[1] +
                      (u8)(rotate_or_zero((u8)(H_(151) | S_(202)), (u8)(H_(50) & 7)) +
                           majority(H_(4),
                                    (u8)(((int)select_bits(matrix[16], indexed_hash, M_(138)) +
                                          (int)select_bits(17, work[33], S_(39))) / 5),
                                    147)));
        aux[0] = select_bits((u8)(hash[10] & 7), (u8)(MA_(6) & H_(209)),
                             select_bits(0x47, rotate_or_zero(S_(127), (u8)(MA_(6) & 7)),
                                         (u8)(SI_(MA_(5)) << 1)));
        u8 selected_square = select_bits(198, square(M_(14)), (u8)(H_(145) ^ aux[0]));
        u8 seed9 = SI_(aux[9]);
        u8 hash3 = HI_(aux[3]);
        matrix[2] = (u8)(matrix[2] + (u8)(((u8)(hash3 << 1) & (u8)((work[25] & 0x96) | (seed9 & 8))) | (seed9 & 0x40)));
        matrix[14] = (u8)(matrix[14] - select_bits(34, work[97], (u8)(MA_(3) & (aux[0] ^ M_(100)))));
        work[23] ^= (u8)(majority(majority(S_(17), hash3, aux[0]), (u8)(work[50] / 3), 0x76) << 1);
        hash[17] = 115;
        hash[13] = (u8)(((majority(HI_(aux[7]), work[10], 82) >> 1) & 0x68) | (H_(39) & 0x17));
        matrix[33] = (u8)(matrix[33] - (u8)(work[113] & 9));
        matrix[28] = (u8)(matrix[28] - (u8)((aux[3] & (u8)~0x20) | ((work[110] >> 1) & 0x20)));
        work[95] = SI_(aux[3]);
        hash[15] = (u8)(majority((u8)(work[95] - 48), (u8)~work[184], 189) &
                        cube(majority(aux[7], SI_(aux[1]), 0xaa)));
        matrix[22] = (u8)(matrix[22] + work[183]);
        aux[4] ^= (u8)(3 * S_(1));
        aux[5] = (u8)(aux[5] + (u8)((u8)(198 * majority(S_(178), MA_(1), 209)) * H_(13) * (u8)(S_(26) >> 1)));
        aux[8] = select_bits(10, MA_(3), MA_(9));
        matrix[18] = (u8)(matrix[18] - select_bits(hash[15], (u8)(aux[5] / 15), cube((u8)(HI_(aux[6]) | 81))));
        aux[1] = (u8)(aux[1] + (u8)((u8)(SI_(HI_(aux[1])) / 3) - H_(160)));
        hash[16] = (u8)(147 - majority(aux[0],
                                       majority(S_(69), work[172], (u8)(aux[2] - selected_square + 77)),
                                       (u8)(0xc2 | (aux[0] & 5))));
        hash[3] = (u8)(hash[3] - wide_seed(majority(S_(155), work[105], 141),
                                          (u8)(majority(S_(168), H_(29), 6) & 7)));
        work[5] = (u8)(rotate_or_zero(0x38, (u8)((-(u8)(H_(61) / 5)) & 7)) ^ (u8)(((u8)~MA_(8)) / 5));
        work[198] = (u8)(work[198] + work[3]);
        {
            int wide = (int)(162 | MA_(9));
            work[164] = (u8)(work[164] + (u8)((u32)(wide * wide) / 5));
        }
        aux[2] = (u8)(majority(rotate_or_zero(139, (u8)((-aux[5]) & 6)), HI_(aux[3]), 12) |
                      select_bits(95, cube(seed9), HI_(aux[7])));
        matrix[12] = (u8)(matrix[12] + (u8)((16 | ((work[103] | 60) & (aux[2] | (work[103] & 32)))) / 3));
        work[143] = (u8)(work[143] -
                         (u8)(0x12 | (select_bits(aux[9], select_bits(matrix[8], work[35], aux[7]), (u8)(aux[8] / 3)) &
                                      (u8)(0x4d | ((work[172] >> 1) & 0x20)))));
        matrix[29] = 162;
        hash[15] = (u8)(hash[15] + majority((u8)(M_(149) ^ square(work[43])),
                                            (u8)(select_bits(95, H_(125), SI_(aux[1])) >> 1), 115));
        aux[9] = (u8)(aux[9] - HI_(aux[7]));
        hash[7] = (u8)(hash[7] - square(rotate_or_zero(MA_(5), (u8)((-M_(17)) * (M_(17) & 1)))));
        matrix[8] = (u8)(matrix[8] + cube(S_(202)) - work[184]);
        hash[16] = (u8)((M_(102) << 1) & 0x84);
        aux[6] ^= (u8)(SI_(aux[7]) >> 1);
        hash[7] = (u8)(hash[7] - (u8)(H_(191) - select_bits(177, SI_(SI_(aux[1])), (u8)(S_(80) << 1))));
        hash[6] = H_(119);
        hash[12] = (u8)((HI_(aux[8]) ^ (u8)(M_(71) + M_(15))) &
                        majority((u8)((work[118] & (u8)~0x2c) | 2), square(HI_(aux[9])), 27));
        {
            u8 digest_index = (u8)(select_bits(0xa9, (u8)(S_(57) * 231), majority(work[32], MA_(1), 23)) / 5);
            u8 seed_sample = SI_(aux[6]);
            aux[5] = (u8)(majority((u8)((seed_sample & 0x1c) | (H_(82) & 0xa2) | (SI_(digest_index) & 0x41)),
                                   majority(cube(HI_(aux[7])), work[82], 92), 192) ^ digest_index);
            matrix[25] ^= (u8)((u8)(2 * HI_(aux[9]) * work[5]) -
                               (u8)(rotate_or_zero(aux[4], (u8)(seed_sample & 7)) & (u8)(aux[3] + 110)));
        }
    }
}
}

#undef H_
#undef S_
#undef M_
#undef MA_
#undef HI_
#undef SI_
```

对应 Go 语句顺序：`fairplay_sap.go:101-213`（一一对应，不可调换，不可合并）。

## 附录 B：常量清单（C 直接照抄，不在此重复全文）

| 常量 | 位置 | 大小 |
|---|---|---|
| `sapInitialState.hash` | `fairplay_sap.go:27-30` | 20 B |
| `sapInitialState.matrix` | `fairplay_sap.go:31-37` | 35 B |
| `sapSeed` | `fairplay_sap.go:39-42` | 21 B |
| `md5Shift` / `md5Constant` | `fairplay_md5.go:19-24` / `26-43` | 64 / 256 B |
| `fairplayInitialSessionKey` | `fairplay_crypto.go:12-15` | 16 B |
| `kdfPrefix` / `kdfSuffix` | `fairplay_crypto.go:17-21` / `22-26` | 17 / 17 B |
| `messageIV[4][16]` | `fairplay_message.go:153-158` | 64 B |
| `messageRoundKey0` / `RoundKey10` | `fairplay_message.go:160-162` / `164-166` | 16 / 16 B |
| `middleKeys[4][9][16]` | `fairplay_message.go:168-213` | 576 B |
| `inverseAESSBox[256]` | `fairplay_message.go:215-232` | 256 B（正向由反查得到） |
| `m1Payload` / `m3Label` | `fpsap.go:14` / `16` | 4 / 3 B |
| `descriptorPrefix` / `Suffix` | `fpsap.go:18-22` / `23-27` | 17 / 17 B |
| `fixedBlock` | `fpsap.go:28-32` | 16 B |
| `firstPositionMap` / `secondPositionMap` | `fpsap.go:33-35` / `37-39` | 16 / 16 B |
| `firstInputMask` / `secondOutputMask` | `fpsap_tables.go:25-27` / `28-30` | 16 / 16 B |
| `substitutionBases[6][256]` | `fpsap_tables.go:34-113` | 1536 B |
| `mixBases[7][256]` | `fpsap_tables.go:115-208` | 1792 B |
| `firstTables` / `secondTables` 三元组 | `fpsap_tables.go:209-249` / `250-289` | 2×176×3 B |

## 附录 C：配套 C 参考实现

本次提取同时生成了一份可编译、可自测的 C 参考实现（与项目源码零依赖）：

- `docs/fairplay-reference/fairplay_ref.c` — 全部原语 + 自测 main（`cc -O2 -o fairplay_ref fairplay_ref.c && ./fairplay_ref`，输出 `ALL PASS`）
- `docs/fairplay-reference/fairplay_data.h` — 由 Go 参考实现直接导出的常量与表（避免手抄表出错）
- `docs/fairplay-reference/sap_corpus.h` — 64 组 SAP hash 块/摘要
- `refimpl/` — 从 doubletake 项目复制的 6 个源文件 + 导出/打印用测试，用于生成上述数据（分析期产物，未随本仓库分发）

该实现自带标准 AES-128 密钥扩展、单块加密与全部 FairPlay 私有原语，不链接任何第三方库；唯一省略的是 ekey[36:56] 的 HMAC-SHA1（C 里留空占位，生产实现由 `mbedtls_md_hmac` 填充，其输入 `macKey` 已在该自测中校验）。
