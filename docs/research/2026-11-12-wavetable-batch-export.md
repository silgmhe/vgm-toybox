# 调研：五颗波表芯片的整曲批量导出

**状态**：调研完成，未改代码
**范围**：HuC6280、SCC / SCC+、X1-010（波表半）、WonderSwan、Game Boy（Wave 通道）
**对照实现**：`collectVsuSamples`、`collectK005289Samples`（已完成的整曲波表导出）

---

## 1. 结论

这五颗的“行”和“拖拽”都已经有波形，缺的是**整曲收集器**：扫一遍写入流，把**每一张真正播放过的表**
各写一个文件。做法与 `collectVsuSamples` 完全同构，每个芯片一个 `collect*Samples`，工作量约
**120–200 行/芯片**，不改解码器、不改 UI。

实现前有一个**必须由你决定的分叉**（§6.1）：这批文件是走现有 **“Export samples”**（和 PCM 样本同一个
列表，按宿主速率零阶保持，32 点的表会变成几十毫秒的阶梯音），还是新增一条 **“整曲波表导出”**
（一个文件就是一个周期，带位深与循环标记，和现在的 “Export wavetables” 按钮一致）。

---

## 2. 现状：两条导出路径

| 路径 | 入口 | 数据来源 | 一个文件是什么 |
|---|---|---|---|
| **A. 整曲样本** | 菜单 “Export samples…” → `exportSamples()` → `writeSamples()` | 后端在 `prepare()` 里用 `collect*Samples` 扫出来的 `DecodedSample` 列表 | 一段录音，**按宿主速率零阶保持**（`heldAt`），因此 32 点的波表会变成 0.5–2 s 的阶梯 |
| **B. 快照波表** | 键盘页 “Export wavetables” → `exportWavetables(snapshot…)` | **当前这一帧**每行的 `ChannelState` | 一个周期（`writeCycleWav`，带位深和 `clm` 循环标记） |

**结论先行：Keyboard 卡片上的“Export wavetables”对五颗芯片已经可用**——它读 `drawnSnapshot()`，把**按下按钮那一刻**每一行的表写成 `<芯片>_<通道>.wav`（一个周期，带位深与 `clm` 标记）。它此前对这几颗芯片是空的，只因为那时行里没有 `wave`；波形做完之后按钮自然就覆盖到了。本文讨论的是**另一件事**：把整首歌用过的表一次性枚举出来。

覆盖表 §9.1 的“整曲导出”一列指的是 **A**：`collectNesDpcmSamples`、`collectC140Samples`、`collectVsuSamples`……
五颗波表芯片这一列是空的，所以它们现在只能在 B 里导出“此刻这一张”。

VSU / ES5503 / K005289 / Mikey / MSM5232 已经走通了 A，因此这五颗是**补齐同一条路径**，不是新机制。

---

## 3. 每颗芯片：表在哪里、速率怎么算

### 3.1 HuC6280（6 通道 × 32 步 5 bit）

| 项 | 值 |
|---|---|
| 写入 | `0xB9`，`reg` = 偏移（`0x06` 是波形数据） |
| 表 | 每通道 32 字节，`reg 0x06` 写入当前 `waveIndex` |
| 索引 | **只有未 key-on 时自增**；DDA 1→0 归零（`c6280_mame.c:219-240`） |
| 速率 | `clock / f`（f 为 12 位分频，0 按 4096） |
| 发射点 | `0x04` bit7 0→1（key-on），以及 keyed 时的频率/波形变化 |
| 格式 | 需要新增 **5 bit 无符号、中心 16**（`huc5`），或展开成 8 位 |
| 命名 | `huc6280_<hash>` |

### 3.2 SCC / SCC+（5 通道 × 32 步 8 bit 有符号）

| 项 | 值 |
|---|---|
| 写入 | `0xD2` 在 loader 里已拆成两条：偶数 `reg` 是 latch（寄存器号），奇数 `reg` 是数据；`port`/2 是块 |
| 表 | 偶发 port 0（SCC）或 port 4（SCC+ 高半区）；地址 `latch 0x00-0x9F` |
| 布局 | SCC：`<0x60` → 通道 `latch>>5`，`≥0x60` 同时写 ch3/ch4；SCC+：`latch>>5`（0–4） |
| 速率 | `2 * clock / (f + 1)`（步进率），f 为 12 位 |
| 发射点 | key mask（port 3）某位 0→1 |
| 格式 | `pcm8Signed`（现成） |
| 命名 | `scc_<hash>` |
| **注意** | SCC+ 标志**不在 IR 里**：loader 发 `0xD2` 时不带 flags，`ChipSlot.flags` 为 0；真值在 `deviceInfo.devCfg->flags`（VGM 时钟 bit31），需要像 `clock` 一样作为参数传进收集器 |

### 3.3 X1-010（波表半：32 张 × 128 字节）

| 项 | 值 |
|---|---|
| 写入 | `0xC8`，`port`/`reg` 是 16 位偏移的高/低字节；波表在 `0x1000-0x1FFF` |
| 表 | 32 张 × 128 字节；通道的波形号是 `reg 0x01 + 8*ch` |
| 速率 | `clock * pitch / 524288`，pitch = `(reg3<<8 | reg2) >> div` |
| 发射点 | `status`（`0x00 + 8*ch`）bit0 0→1 且 bit1 = 波形模式 |
| 格式 | `pcm8Signed`（现成） |
| 命名 | **`x1010wave_<hash>`**，与 PCM 的 `x1010_...` 区分 |

### 3.4 WonderSwan（4 通道 × 32 步 4 bit）

| 项 | 值 |
|---|---|
| 写入 | `0xC6` 内存写（`isMemory`），地址 = `port<<8 | reg`；基址寄存器 `0x8F`（单位 64 字节） |
| 表 | 每通道 16 字节，`base + ch*0x10`；偶数步取**低**半字节 |
| 速率 | `clock / (2048 - (f & 0x7FF))` |
| 发射点 | `0x90` 的对应 bit 0→1（且非噪声/语音） |
| 格式 | **展开成 32 字节后复用 `prom4`**（4 bit、中心 8），或新增 nibble 格式 |
| 命名 | `wonderswan_<hash>` |

### 3.5 Game Boy（Wave 通道 × 32 步 4 bit）

| 项 | 值 |
|---|---|
| 写入 | `0xB3` 偏移 `0x20-0x2F` |
| 表 | 16 字节；偶数步取**高**半字节（与 WS 相反） |
| 速率 | `clock / (2 * (2048 - (f & 0x7FF)))` |
| 发射点 | `0x0E` bit7（trigger） |
| 格式 | 展开成 32 字节后复用 `prom4` |
| 命名 | `gb_<hash>` |

---

## 4. 格式清单

| 现有格式 | 用途 |
|---|---|
| `pcm8Signed` | SCC、X1-010 波表（8 bit 有符号）✓ |
| `prom4` | **每个字节一个 4 bit 电平**（`(b & 0x0F - 8) / 8`）——4 bit 表展开后正好可用 ✓ |
| `vsu6` | 6 bit，不适用 |

需要新增一个：**`huc5`**（5 bit 无符号、中心 16：`((b & 0x1F) - 16) / 16`），仿照 `vsu6` 的写法。

`prom4` 目前是“一字节一电平”，所以 4 bit 表（16 字节）必须先展开成 32 字节；展开时保留各芯片自己的
半字节顺序（WS 低先行、GB 高先行），这样导出的形状与行完全一致。

---

## 5. 发射点与去重（决定文件数）

VSU 的规则：**key-on 边沿发射**，按表内容去重，同名只留一个，速率取最后一次（“last write wins”）。

五颗照抄这条：

- 只统计**播放过**的表，不统计“正在逐字节写入”的中间状态——否则 HuC 一首歌会出几百个文件
  （32 字节的表每写一个字节都会产生一个中间状态）；
- 去重按内容 hash：两个通道用同一张表 → 一个文件；
- 文件数上限 = 芯片的表槽数：SCC 4–5、HuC ≤18、X1-010 ≤32、WS ≤4（但换乐器会重写，实际是“乐器数”）、
  GB = 乐器数。

---

## 6. 需要你确认

### 6.1 走哪条路径（最重要）

**A. 并入 “Export samples”**（推荐，零 UI 改动，与 VSU 一致）

- 文件是 `DecodedSample` → `writeSamples`，默认按宿主速率零阶保持：32 点的表变成几十毫秒的阶梯音，
  没有 `clm` 循环标记；
- 好处：与现有九颗采样芯片同一个列表、同一个菜单，用户不需要学新东西。

**B. 新增 “整曲波表导出”**

- 扫出所有表，每个写一个**周期 WAV**（32/128 点，带位深与 `clm` 标记），放在现有
  “Export wavetables” 按钮的文件夹里；
- 好处：文件就是“一个周期”，拖进 DAW 可以循环；坏处：要新增一个菜单项/按钮语义，并区分
  “当前快照”与“整曲”。

我倾向 **A 先行、B 作为后续**：A 能立刻用，且不改任何 UI；如果 A 导出后你发现“阶梯音不好用”，
再补 B 也不浪费——收集器是共用的。

### 6.2 其余四点

1. **4 bit 表**：展开成 32 字节复用 `prom4`（推荐）还是新增 nibble 格式？
2. **5 bit 表**：新增 `huc5`（推荐）还是展开成 8 位有符号？
3. **SCC+ 标志**：从 `deviceInfo.devCfg->flags` 传参（推荐，IR 里没有）？
4. **命名**：`<chip>_<hash>`（与 `vsu_…` 一致），X1-010 用 `x1010wave_` 区分 PCM —— 可以吗？

---

## 7. 工作量与验证

| 项 | 估计 |
|---|---|
| 收集器 | 5 个文件（.h/.cpp），120–200 行/个 |
| 格式 | `SampleFormat::huc5` + 一个解码分支 |
| 接线 | `LibvgmSongInstance::prepare()` 5 处 + CMake 4 处 |
| 测试 | 5 个脚本（文件数 + 首名 + 一条峰值/长度断言） |
| 文档 | 设计文档 5 节、调研文档各一节 |

验证口径（每颗一首）：

```
samples <n>                      # 与预期文件数一致
sample-names <chip>_<hash 前缀>   # 首名固定
```

文件名是内容 hash，脚本要先跑一次把实际值写进断言——与 `pcm-*` 脚本的做法一致。

---

## 8. 已实现（第一批：菜单与分流，2026-11-12）

按讨论先做**基础设施**，五颗新收集器仍待做：

- `DecodedSample::isCycle`；`SampleCollector::add(..., bool isCycle)`；VSU / ES5503 / K005289 / Mikey /
  MSM5232 五个收集器标记为周期；
- `writeSamples` 跳过周期，`recordsOnly()` / `cyclesOnly()` 提供两条列表；
- 新菜单 **File > Export > All Wavetables…**（`file.export.wavetables`）→ `writeCycleSamples()`：与键盘页
  按钮共用 `writeCycleFile`（2048 点单周期、`<!>` 标记、`kNativeBits` 不二次量化）；
- 两条导出都落在 `所选目录/<歌曲标题>/`；
- 报告新增 `cycles`，overlay 用一组标签选三种文案。

**行为变更**：All PCM Samples 不再包含那五个波表收集器的 32 点文件——它们现在只在 All Wavetables 里出现，
且是一个周期而不是保持到宿主速率的阶梯。`testWavetableBatchExport` 把这条差异钉住。

**下一步**：HuC6280、SCC、X1-010 波表半、WonderSwan、Game Boy 五个收集器（§3–§5 已给出每颗的写入、
速率、发射点与格式）。

**决定（2026-11-12，第二轮）**：两个菜单撤回了。保留 `isCycle` 与两个写出器，但菜单只有一个
（All PCM Samples…），录音与周期一起导出，各自按自己的规则写进 `<歌曲标题>/`。理由是周期只出现在少数芯片上，
单独一个菜单少有人用；而“顺便导出”只要不把周期写成音频就不会骗人。

---

## 9. 已实现（第二批：五颗收集器，2026-11-12）

| 芯片 | 收集器 | 格式 | 速率 |
|---|---|---|---|
| HuC6280 | `collectHuC6280Samples` | `huc5`（新增：5 bit，中心 16） | `clock / f`（f=0 → 4096） |
| SCC / SCC+ | `collectSccSamples` | `pcm8Signed` | `2·clock/(f+1)` |
| X1-010 波表半 | `collectX1010WaveSamples` | `pcm8Signed`（128 字节） | `clock·pitch/524288` |
| WonderSwan | `collectWonderswanSamples` | `prom4`（展开 32 电平，低半字节先行） | `clock/(2048−f)` |
| Game Boy | `collectGameBoySamples` | `prom4`（展开 32 电平，高半字节先行） | `clock/(2·(2048−f))` |

共同规则：key-on 边沿发射、按表内容去重、`isCycle = true`（因此走周期写出器）；SCC 的 SCC+ 布局从
`devCfg->flags` 传入（IR 里没有这一位）。

实测文件数：HuC6280 的 `01 Magical Sound Shower` 13 张、SCC 的 `02 Bio Hazard Snatcher (Part A)` 10 张、
X1-010 的 `02 Stage 1` 4 张（加上 7 个 PCM 共 11）、WonderSwan 的 `01 The Prelude` 2 张、
Game Boy 的 `01 Title` 3 张。

`pcm-wavetables.txt` 与更新后的 `pcm-x1010.txt` 钉住这些数字与首名；53/53 通过。
