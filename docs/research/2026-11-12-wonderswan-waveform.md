# 调研：WonderSwan（FF4 WSC 56 首）的波形可视化

**状态**：调研完成，未改代码
**测试文件**：`test_file/Final_Fantasy_IV_(Bandai_WonderSwan_Color)/` 56 首 + m3u
**相关**：`chips/WswanDecoder.cpp`、`emu/cores/ws_audio.c`、`VgmCommandReader.*`、`VgmSongLoader.cpp`

---

## 1. 结论

WonderSwan 的 4 个通道都是**波表**（32 步 × 4 bit），但这一颗的波表**不在寄存器里**，而在芯片的
16 KB 内部 RAM 中，通过 VGM 的 **`0xC6` 内存写**上传。而我们的链路目前**在三处把 `0xC6` 丢掉了**：

1. `VgmCommandReader` 没有 `0xC6` 分支（4 字节族只处理 `0x50-0x5F`/`0xA0-0xBF` 与几个显式分支）；
2. `VgmCommand::Kind` 没有“内存写”这一种类；
3. `chips::RegisterWrite` 没有空间标志，无法区分“写寄存器 0x80”与“写 RAM 0x0080”。

所以本次不只是解码器加几行，而是**先补一条数据通路**，再画表。工作量约 **200 行**（其中一半是链路），
比 HuC6280/SCC 大，但比 X1-010 小。

---

## 2. 现状（实测）

`01 The Prelude`：**4 行**齐全，Ch2 显示 A1，Ch1/3/4 当前 `-`（静音），每行形如一条键盘条；
左侧的小波形格子是空的（占位），`assert engine.wave.0` 返回 **0 of 4 channels**，
“Export wavetables” 没有内容。

解码器已经正确处理的：频率（11 位，`0x80-0x87`）、音量（`0x88-0x8B` 的 L/R 半字节）、
`sweep`（`0x8C/0x8D` + `0x90` bit6）、噪声（ch3 + `0x90` bit7）、语音（ch1 + `0x90` bit5，这 56 首未用）、
`0xFFFF` 静音与 `isDrum`。**缺的只有波形 RAM 与 `wave`/`sampleRate`**。

---

## 3. 芯片模型（`ws_audio.c`）

### 3.1 寄存器

| 地址 | 含义 |
|---|---|
| `0x80/0x81` … `0x86/0x87` | 通道 0-3 的频率（11 位，`2048 - f` 是分频） |
| `0x88-0x8B` | 通道 0-3 音量（高/低半字节 = 左/右） |
| `0x8C/0x8D` | sweep 步长 / 间隔 |
| `0x8E` | 噪声抽头（ch3） |
| **`0x8F`** | **波形基址：`value << 6`**，ch0..3 依次 +0x10 |
| `0x90` | 低 4 位 = 通道使能，bit5 语音、bit6 sweep、bit7 噪声 |
| `0x91` | 主音量；`0x94` PCM 音量 |

### 3.2 波形取样（`ws_audio_process`）

```
byte = ram[(wave & 0xFFF0) + (pos >> 1)]        // pos: 0..31
nibble = (pos & 1) == 0 ? (byte << 4) & 0xF0    // 偶数步用**低**半字节
                        : byte & 0xF0           // 奇数步用**高**半字节
sample = nibble - 0x80                          // 4 bit 以 8 为中心
```

即：**低半字节先行**，归一化后是 `(nibble - 8) / 8`，范围 −1..+0.875，静音（nibble 8）为 0。

### 3.3 速率

```
delta   = freq * ratemul，ratemul = clock * 65536 / smplrate，smplrate = clock / 128
步进率  = clock / (2048 - (f & 0x7FF))          // 每秒走过多少步
音高    = 步进率 / 32 = clock / (32 * (2048 - f))
```

与现有 `WswanTable::toneHz` 完全一致，所以行与音符不会矛盾。`f == 0xFFFF` 是静音，不是最高音。

### 3.4 RAM 与命令

- RAM 只有 **16 KB** 对声音芯片可见（`ws_internalRam`），整块可通过 `0xC6` 写；
- `0xC6 <addrHi> <addrLo> <data>`（大端 16 位地址，bit15 忽略）；
- 波形表 = `0xC00` 起 4×16 字节（本批测试文件全部如此）。

---

## 4. 测试文件实测

| 项 | 结果 |
|---|---|
| 文件数 | 56 首（`01`–`56`），最长 `44 Epilogue -1-`（225 KB vgz） |
| 波形基址 `0x8F` | 全部 **0x30** → `0xC00` |
| `0xC6` 地址范围 | 全部 `0xC00-0xC3F`（64 字节 = 4 通道 × 16） |
| `0xC6` 写入次数 | 每首 **80–1 264** 次（表会被重写） |
| 每通道不同表（含中间状态） | **9–121** 张（`44` 的 ch1 最多 121） |
| `0x90` 取值 | `0x4F`（四通道 + sweep）、`0xCF`（再加噪声） |
| 语音（bit5） | 56 首均未使用 |

前几首的实际表：`0x00`/`0xFF` 交替的方波，符合 WS 的 4 bit 音色。

---

## 5. 方案

### A. 打通 `0xC6`（必做，三处）

1. **`VgmCommandReader`**：新增 4 字节分支，`port = addrHi`、`addr = addrLo`、`data = value`；
   建议同时新增 `VgmCommand::Kind::memoryWrite`，让 loader 不必猜；
2. **`VgmSongLoader`**：`case 0xC6` → `emit (ChipKind::wonderswan, 0, addrHi, addrLo, value, time, flags, /*isMemory*/ true)`；
3. **`chips::RegisterWrite`**：新增 `bool isMemory = false`（默认值，已有聚合初始化不受影响）。

> 为什么必须加字段：寄存器写是 `reg = 0x80-0x9F, port = 0/1`，RAM 写是 `port = addrHi, reg = addrLo`；
> 当 RAM 地址落在 `0x0000-0x00FF` 时两者无法区分。显式标志是唯一不会出错的做法。

### B. 解码器画表（必做）

- `uint8_t ram[0x4000]`（16 KB，随 `reset` 清空）；
- `write()` 首行：`if (w.isMemory) { ram[((port<<8)|reg) & 0x3FFF] = value; return; }`；
- `channelState()`：表基址 `base = (waveBase << 6) + channel * 0x10`，
  32 个半字节（**低半字节先行**），解码 `(n - 8) / 8`，`tableWave = true`，
  `sampleRate = clock / (2048 - (f & 0x7FF))`；
- 缓存按 32 字节内容 hash：本批单通道最多 121 张不同的表，**上限建议 256**（32 点 float ≈ 128 B/张，总计约 32 KB）。

### C. 不做

- 语音（ch1 + `0x90` bit5）：56 首未用，且它是 PCM 流，需要与 DAC 同类的边界模型；
- 噪声抽头的具体 LFSR：行只画 “这是噪声”（现有 `wsNoise` 已足够）；
- `0x9E` 等未文档化寄存器（core 也不处理）。

---

## 6. 边界与风险

1. **16 KB RAM 每实例**：可接受；若将来多实例也只多一份。
2. **表被频繁重写**：缓存必须按内容，且上限要大于 121（建议 256），否则每帧重解码一次。
3. **`0x8F` 中途改变**：`channelState` 每次按当前基址取表，天然正确；RAM 保留整块，不用迁移。
4. **`0xC6` 也可能写非波形区域**（别的驱动会拷数据）：统一存进 16 KB，无副作用。
5. **MIDI 不受影响**：音符已由 `WswanTable` 给出，本次只加 `wave`/`sampleRate`/`tableWave`。
6. **`VgmCommand::Kind::memoryWrite` 的新种类**要检查所有 `switch (command.kind)` 的地方（loader 与测试脚本解析），
   默认分支必须是 `ignored` 行为，否则会把内存写当成寄存器写。

---

## 7. 验证计划

| 层 | 断言 |
|---|---|
| reader | `0xC6` 产出 `memoryWrite`，`port/addr/data` 正确，`streamOrder` 保留 |
| 解码器 | RAM 写落在 `0xC00`；ch0 表 = 该 16 字节的低半字节先行；`(n-8)/8` 端点 |
| 速率 | `sampleRate = clock/(2048-f)`；`f=0xFFFF` 静音 |
| 行 | harness：`engine.wave.0..3 == 32`（`01 The Prelude`） |
| 导出 | “Export wavetables” 对该 4 行可用（2048 点单周期） |

---

## 8. 需要你确认

1. **`RegisterWrite` 加 `bool isMemory`**（推荐）——可以吗？
2. **`VgmCommand::Kind` 新增 `memoryWrite`**（推荐，明确）还是沿用 `registerWrite` + loader 约定？
3. **缓存上限**：256 张（本批单通道最多 121）——够吗？
4. **未按键通道也显示已装好的表**（推荐，否则没东西可拖）？
5. 语音模式（ch1）本次仍不做——确认？

---

## 9. 已实现（2026-11-12）

按 A + B 落地：

- `VgmCommandReader` 新增 `Kind::memoryWrite` 与 `0xC6` 分支（大端 14 位地址，`port`=高、`addr`=低）；
- `VgmSongLoader` 新增 `case 0xC6`，`emit` 增加 `isMemory` 参数；`chips::RegisterWrite` 增加 `bool isMemory`；
- `WswanDecoder` 保存 16 KB RAM，`0x8F` 记基址，`channelState` 取 16 字节解 32 步（偶数步低半字节），
  `tableWave = true`，`sampleRate = clock/(2048-f)`，按内容 hash 缓存 256 张；
- 语音、噪声抽头仍不建模（前者 56 首未用，后者只影响音色不影响形状）。

验证：`testWswanWaveTable`（32 步、半字节顺序、端点值、`clock/(2048-f)`、+0x10 步长）、
`testWswanMemoryIsNotARegister`（`0x0082` 不改音高）、`engine.wave.0..3 == 32` 在 harness 上通过，
截图 `build/shots/ws-before.png` / `ws-after.png`。
