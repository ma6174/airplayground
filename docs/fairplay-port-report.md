# fairplay 组件验证记录

组件位置：`firmware/components/fairplay`（未改动该目录以外的任何文件，
`include/fairplay.h` 保持原样，mtime 16:55）。

## 1. 生成链（数字全部来自 Go 侧，零手抄）

```
./tools/export_data.sh        # Go 常量      -> tools/fairplay_data.h
python3 tools/gen_tables.py   # 上面的头文件 -> src/fpsap_tables.c + src/fp_data_*.inc
./tools/gen_vectors.sh        # Go 参考实现  -> src/fp_vectors.c
```

`gen_tables.py` 在写文件前把 13 张基表 + 352 个 lookup 三元组展开成 90128 字节并
比对 Go 测试公布的 SHA-256，一致才落盘：

```
tables verified: 90128 bytes, sha256 28d0986abebe30458348dfa2957aa1d52d6f3ad5a9468c5d8a9c4139b7ca2b43
```

`gen_vectors.sh` 生成时会自校验：published-mode3 链的 m3/ekey 必须等于已发布值，
captured ekey 的 [56:72] 必须能被公布的 rawKey 解开，否则测试失败、不写文件。

## 2. Host 测试（macOS clang + CommonCrypto，真跑）

`cd firmware/components/fairplay/test && make`：

```
fairplay component host test (clang + CommonCrypto)
  white-box tables             PASS  90128 bytes, sha256 28d0986abebe30458348dfa2957aa1d52d6f3ad5a9468c5d8a9c4139b7ca2b43
  platform                     PASS  32/32 checks
  sap_hash                     PASS  195/195 checks
  md5                          PASS  9/9 checks
  message                      PASS  32/32 checks
  exchange                     PASS  36/36 checks
  wrapping_key                 PASS  17/17 checks
  chain                        PASS  115/115 checks
  unwrap                       PASS  15/15 checks
  reject                       PASS  23/23 checks
  total                        474 checks, 0 failures
[fairplay] selftest: 474 checks, 0 failures
  public fp_selftest()         PASS  fp_selftest() returned 0
ALL PASS
```

（`make san` = 同一份代码加 ASan + UBSan：同样 ALL PASS，无任何报告。）

编译告警面：`/usr/bin/clang` 与 `/opt/homebrew/bin/clang`，`-std=c99` 与 `-std=c11`，
`-Wall -Wextra -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Werror`
全部零告警。

`src/fp_platform_esp.c` 用 ESP-IDF 6.1 的真实 PSA 头（
`components/mbedtls/port/include` 等 + `-DMBEDTLS_CONFIG_FILE='"mbedtls/esp_config.h"'`）
做过 `-fsyntax-only`，通过。PSA 1.1 里 `psa_mac_compute` 收的是 key id 而不是裸密钥，
所以实现改成 `psa_import_key`（volatile HMAC key）→ `psa_mac_compute` → `psa_destroy_key`。

## 3. 变异测试（证明测试确实会失败）

在副本上逐条注入错误，看是否被抓到：

| 注入的错误 | 结果 |
|---|---|
| `(aux[4] & 0xca) >> 1` 改成 `aux[4] & (0xca >> 1)`（Go/C 优先级坑） | 抓到：sap_hash / exchange / wrapping_key / chain |
| `wide_seed` 在取模前截断到 8 位 | 抓到：sap_hash / exchange / wrapping_key |
| `rotate_or_zero` 换成 `rotl8` | 抓到：sap_hash / exchange / wrapping_key |
| MD5 变异点从 round 31 挪到 round 30 | 抓到：md5 / exchange / wrapping_key / chain |
| 第一网络掩码改到 mix 之前异或 | 抓到：exchange / chain |
| 第二网络表索引从 `[output]` 改成 `[input]` | 抓到：exchange / chain |
| digest32 的变异类型 swap → cycle | 抓到：exchange / chain |
| descriptor 改成"先压缩后加 hash" | 抓到：exchange / chain / unwrap |
| wrapping key 改成"先加 hash 后压缩" | 抓到：wrapping_key / chain / unwrap |
| AES Rcon 去掉 0x1b 归约 | 抓到：chain / unwrap |
| ekey 的 macKey 用成 wrapping key | 抓到：chain |
| m1 能力字节改动 | 抓到：chain |
| m4 比较偏移 144 → 143 | 抓到：chain |
| ekey mask 随机源长度/偏移改错 | 抓到：chain |
| stream key 拼接顺序反过来 | 抓到：platform |
| mode 3 的倒序 CBC 改成正序 | **不报错**——预期，独立缓冲下两者等价 |

## 4. 资源

`clang -fstack-usage -O2`：握手最坏链
`fp_wrap_key` 432 B → `fp_wrapping_key` 544 B → `fp_sap_hash` 560 B ≈ 1.6 KiB；
`fp_selftest()` 最坏 `run_reject` 1536 B → `fp_exchange_m3` 368 B → `fp_exchange`
496 B → `fp_descriptor` 416 B → `fp_sap_hash` 560 B ≈ 3.4 KiB。

Flash 粗估（host arm64 `size`）：表 4.4 KiB、逻辑 ~30 KiB、`fp_vectors.c` +
`fp_selftest.c` ~38 KiB（量产镜像可从 CMakeLists 去掉这两者）。

## 5. 仍未被验证的部分

1. `fp_platform_esp.c` 只做过语法检查，没有在设备上跑过；PSA 的 SHA-1/HMAC/SHA-512
   运行时可用性取决于 sdkconfig（IDF 6.1 默认 `PSA_WANT_ALG_SHA_1`、
   `PSA_WANT_ALG_HMAC`、`PSA_WANT_ALG_SHA_512`、`MBEDTLS_SHA1_C` 都是开）。
2. 没有跑 `idf.py build`（按要求由主会话统一构建），组件 REQUIRES / 链接未验证；
   若 IDF 版本把 PSA 拆成独立 `esp_psa_crypto` 组件，需要补进 REQUIRES。
3. 没有和真实接收端做过 `/fp-setup` 联调。
4. `fp_confirm_m4` 只比对 20 字节 tail（与 Go 参考一致）；真实设备是否回抄别的长度未知。
5. 抓包向量里 ekey[36:56] 的 HMAC 无法复现（那是别的实现算的），tag 路径只由本地
   生成的 5 组链覆盖。
