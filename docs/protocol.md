# AirPlay mirroring, sender side

This is the wire-level reference this firmware implements. Everything below is
what the receiver sees on the network; the component READMEs cover how each part
is built.

## Roles and connections

| Connection | Direction | Purpose |
|---|---|---|
| control `host:7000` (TCP) | sender connects | one RTSP/1.0 request at a time, CSeq increments per request |
| event `eventPort` (TCP) | sender connects, says nothing | receiver pushes commands here |
| video data `dataPort` (TCP) | sender connects | 128-byte header + payload per frame |
| timing (UDP) | sender binds, receiver probes | only in NTP mode |

## Session sequence

```
GET  /info                      capabilities: features, statusFlags, displays
POST /pair-setup   M1..M6       HAP, SRP-6a (3072-bit, SHA-512)
POST /pair-verify  V1..V4       X25519 + Ed25519; control channel becomes encrypted here
POST /fp-setup     m1/m3        FairPlay SAP exchange, answers m2/m4
SETUP  session                   timing + session descriptor
SETUP  stream type 96           screen-audio descriptor (no audio packets follow)
SETUP  stream type 110          video descriptor -> dataPort, carries shk/shiv
RECORD                          unless the receiver answered skipRecord=true
data channel                    codec packet (avcC), then IDR access units
every 1 s                       data-channel heartbeat
every 2 s                       POST /feedback
```

## Headers that matter

```
User-Agent: AirPlay/935.7.1          every request
CSeq: <n>                            per connection, starts at 1
Content-Type: <type>                 only when the body is non-empty
Content-Length: <n>                  always
X-Apple-HKP: 4 | 5 | 3               pair-setup/pair-verify: transient | PIN+ACL | PIN
X-Apple-Client-ID: <uuid>            pair-setup/pair-verify
X-Apple-Client-Name: <name>          pair-setup/pair-verify
X-Apple-PD: 1                        pair-verify
X-Apple-ET: 32                       /fp-setup
```

`sessionUUID` and `deviceID` are not headers: they live in the SETUP plist.
`deviceID` is a locally administered MAC (`02:xx:xx:xx:xx:xx`).

## Encrypted control channel

Once pair-verify succeeds every byte on the control connection is framed:

```
[2-byte LE plaintext length][ChaCha20-Poly1305 ciphertext][16-byte tag]
```

- plaintext blocks of at most 1024 bytes, one frame per block
- AAD is the 2-byte length prefix
- nonce is 4 zero bytes followed by the LE64 block counter, one counter per
  direction, both starting at 0

The event channel uses the same framing with the key directions swapped:
our write key is derived with `Events-Read-Encryption-Key`.

## FairPlay

`/fp-setup` carries `FPLY` records: `"FPLY" | 03 01 <type> 00 | BE32 payload length`.

| message | total | content |
|---|---|---|
| m1 | 16 B | `02 00 03 bb` |
| m2 | 142 B | `[12]=2`, `[13]=mode`, `[14:142]`=encrypted receiver SAP |
| m3 | 164 B | `[12]=mode`, `[13:16]=8f 1a 9c`, `[16:144]`=encrypted sender SAP, `[144:164]`=confirmation |
| m4 | 32 B | must echo `m3[144:164]` |

The stream key handed to the receiver as `shk` is
`SHA-512(raw_key || pair-verify shared secret)[:16]`; `shiv` is 16 fresh random
bytes. In an encrypted (HAP) session the legacy `ekey`/`eiv`/`et` session fields
are not sent — the stream descriptor's `shk`/`shiv` are the key reference.

## Pairing types

The `X-Apple-HKP` header selects the pairing type, and the choice is not
cosmetic:

| value | used for | M5 content |
|---|---|---|
| 4 | transient (no code) | identity only |
| 5 | code pairing, Apple TV | identity + `com.apple.ScreenCapture` ACL |
| 3 | code pairing when the receiver advertises feature 27 (legacy pairing) | identity only |

An Apple TV accepts 5; the macOS AirPlay receiver advertises feature 27 and
answers 5 with HTTP 470 at M5, so it needs 3 and no ACL claim.

## SETUP

Body is a binary plist (`application/x-apple-binary-plist`), URI is
`rtsp://host:7000/<streamConnectionID>`.

Session descriptor: `deviceID`, `macAddress`, `sessionUUID`, `sourceVersion`,
`isScreenMirroringSession`, `timingProtocol`, `osBuildVersion`, `model`, `name`,
`updateSessionRequest`, `combinedGetInfoWithControlSetup`, plus either
`timingPort` (NTP) or `timingPeerInfo`/`timingPeerList` (PTP).

Audio stream (`type 96`, placeholder that never carries packets): `ct=2` (ALAC),
`spf=352`, `sr=44100`, `audioFormat=0x40000`, `audioMode=default`,
`usingScreen=true`, `latencyMin=0`, `latencyMax=3748`, `redundantAudio=2`.

Two layouts exist for the transport part of that descriptor:

- **legacy**: `controlPort=<local UDP port>`
- **streamConnections** (receivers advertising feature bit 59): `isMedia=false`,
  `supportsDynamicStreamID=true`, and
  `streamConnections={streamConnectionTypeRTP:{streamConnectionKeyUseStreamEncryptionKey},
  streamConnectionTypeRTCP:{streamConnectionKeyPort}}`. This form rejects a
  top-level `controlPort`, and an encrypted session must also carry a 32-byte
  audio `shk` even though no audio packets are ever sent.

Video stream (`type 110`): `streamConnectionID`, `latencyMs=75`,
`timestampInfo=[SubSu, BePxT, AfPxT, BefEn, EmEnc]`, `shk`, `shiv`.
The response carries `streams[type=110].dataPort`.

## Timing

`sourceVersion` is `980.71.1` for an encrypted session.

**PTP** (used when `/info` advertises feature bit 41): the frame header's
`[40:48]` carries the receiver's `ClockID` from the SETUP reply, and timestamps
come from that timeline: `anchor + (elapsed since the reply + 75 ms)`, where
`anchor` is built from the reply headers `X-Apple-RequestReceivedTimestamp` and
`X-Apple-ProcessingTime` (both decimal milliseconds) when present, otherwise from
our own boot clock.

**NTP** (fallback): we bind a local UDP port, advertise it as `timingPort`,
answer the receiver's probes (`[0]=0x80,[1]=0xd2` in, `0xd3` out), and stamp
frames with our boot-relative NTP fixed point, `timelineID = 0`.

## Video packets

Every packet is a 128-byte little-endian header followed by its payload.

| offset | size | meaning |
|---|---|---|
| 0 | 4 | payload length; for sealed payloads this **includes** the 16-byte tag |
| 4 | 1 | `0x00` encrypted video, `0x01` codec config, `0x02` heartbeat |
| 5 | 1 | `0x10` IDR, `0x00` otherwise |
| 6 | 1 | codec config: `0x16` (avcC); heartbeat: `0x1e`; video: `0x00` |
| 7 | 1 | codec config: `0x01` |
| 8 | 8 | timestamp |
| 16 | 4+4 | codec config: encoded width/height as float32 |
| 40 | 4+4 | codec config: destination rect width/height; video: timeline ID (8 bytes) |
| 56 | 4+4 | codec config: second rect width/height |

Payload is AVCC: each NAL prefixed with its 4-byte big-endian length, VCL NALs
only. The codec packet carries the AVCDecoderConfigurationRecord plus the four
trailing bytes `02 00 00 00`.

Video payloads are sealed with ChaCha20-Poly1305, key
`HKDF-SHA512(shared_secret, "DataStream-Salt<id>", "DataStream-Output-Encryption-Key", 32)`,
nonce = 4 zero bytes + LE64(frame counter), AAD = the complete 128-byte header.
The header must therefore be final before sealing.

## Where this lives in the tree

| Wire piece | Code |
|---|---|
| transport, framing, Digest | `firmware/components/airplay/src/ap_client.c` |
| binary plist | `firmware/components/airplay/src/ap_plist.c` |
| SETUP bodies and reply parsing | `firmware/components/airplay/src/ap_setup.c` |
| 128-byte headers | `firmware/components/airplay/src/ap_frame.c` |
| data channel | `firmware/components/airplay/src/ap_data.c` |
| event channel | `firmware/components/airplay/src/ap_event.c` |
| NTP timing | `firmware/components/airplay/src/ap_ntp.c` |
| session flow | `firmware/components/airplay/src/ap_session.c` |
| pairing | `firmware/components/hap/` |
| FairPlay | `firmware/components/fairplay/` |
