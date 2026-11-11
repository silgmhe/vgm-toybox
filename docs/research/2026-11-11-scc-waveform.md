# 调研：SCC（K051649 / K052539）的波形可视化

**状态**：调研完成，未改代码
**测试文件**：`test_file/SCC（K051649）/` 七首（Bio Hazard、Beyond the Darkness、Squeak!! 等）
**相关**：`chips/K051649Decoder.cpp`、`emu/cores/k051649.c`、`docs/desgin/libvgm-visualisation.md` §9.1

---

## 1. 结论

和 HuC6280 同类：**波表芯片，不是采样芯片**。5 个通道，每通道 32 个 **有符号 8 位**波形点。
要做的事只有三件：

1. 把波形 RAM 从 `0xD2` 的写入里解出来，交给行去画（**现在画的是占位**）；
2. 速率 = `2 × clock / (f + 1)`（波表每秒走多少点）；
3. 分清 **SCC（K051649）** 与 **SCC+（K052539）**——两者的波形 RAM 布局不同，ch4 单独一张表只在 SCC+ 里存在。

工作量约 **100 行** + 断言；不需要动 `SongIr`、不需要动 MIDI。

---

## 2. 现状（实测）

`04 Squeak!!.vgz` 在键盘页：芯片是 **AY-3-8910（3 ch）+ K052539（5 ch）= 8 行**；
SCC 的五行有音符（E0 等）但形状是同一个占位，`assert engine.wave.0..7` 全部为 0；
“Export wavetables” 对 SCC 无内容可导。

解码器本身：寄存器（频率/音量/key mask）、门限（`freq > 8`）、`channelKind = sccWave` 都是对的，
`case 0x00/0x04: break;` 就是缺口。**当前没有任何 SCC 的测试**（`ChipDecoderTest` 里搜不到 `K051649`）。

---

## 3. 芯片模型（以 `k051649.c` 为准）

### 3.1 写协议

VGM `0xD2 <addrByte> <rr> <dd>`：`addrByte` 的 bit7 是芯片号，低 7 位是 **port**；
libvgm 把它变成两次 `write8`：`(port << 1)` 写 `rr`（latch），`(port << 1) | 1` 写 `dd`（数据）。

我们的 loader 已经照这个拆成两条寄存器写（`reg = port << 1` 与 `reg | 1`），decoder 的
`w.reg & 1` / `w.reg >> 1` 正好对应，**这一层不用改**。

| port | 含义 | 备注 |
|---|---|---|
| 0 | 波形 RAM | SCC 与 SCC+ 都用 |
| 1 | 频率（12 位，`rr` 选通道与高低字节） | 现有实现正确 |
| 2 | 音量（4 位，`rr` 选通道） | 现有实现正确 |
| 3 | key mask（`dd` 的每一位一个通道） | 现有实现正确 |
| 4 | 波形 RAM 的 SCC+ 区 | 与 port 0 同一套写入器 |
| 5 | test / deform | 全部七首都没用到 |

### 3.2 波形 RAM

32 个**有符号 8 位**（`int8_t waveram[32]`），不是 5 bit，也不是无符号。

- **SCC 模式**（`mode_plus = 0`）：`offset < 0x60` → 通道 `offset >> 5`（0..2）；
  `offset >= 0x60` → **同时写 ch3 和 ch4**（两者共用同一张表）；
- **SCC+ 模式**（`mode_plus = 1`）：`channel = offset >> 5`（0..4），**ch4 有自己的 0x80-0x9F**，
  不再与 ch3 共享。

七个测试文件正好分成两类（见 §4）。

### 3.3 速率

核心的计数器每输出样本加 `step`，`index = (counter >> 16) & 0x1F`：

```
step   = mclock × 65536 / ((f + 1) × rate/2)      rate = mclock / 16
步进率 = 2 × mclock / (f + 1)                      （每秒走过多少点）
音高   = mclock / (16 × (f + 1))                   （= 步进率 / 32）
```

现有 `SccTable` 用的正是 `clock / (16 × (period + 1))`，与核心一致；
行需要的 `sampleRate` 是**步进率** `2 × clock / (f + 1)`。

`frequency <= 8` 时核心整通道静音（九拍填不满一个 32 点周期），现有 `gate` 已经处理。

### 3.4 SCC+ 标志与 deform

- **SCC+ 由 VGM 时钟字段的 bit31 表示**（`vgmplayer.cpp:1270`：`devCfg.flags = (clock & 0x80000000) >> 31`），
  所以 `PLR_DEV_INFO::devCfg->flags & 1` 就是 `mode_plus`；
- 芯片名（`devDecl->name`）在 SCC+ 时显示 **K052539**，截图里已经如此——说明 libvgm 读到了这个标志，
  **但我们的 decoder 拿不到它**：`VgmReplayer` 的 `slotFlags` 只给 OPN 和 Y8950 设过值；
- deftest/deform 寄存器（port 5）：libvgm 只把字节存进 `test`，**deform 并未被模拟**，音频里也没有；
  所以我们不需要建模，注释写明即可。

---

## 4. 测试文件实测

| 文件 | 芯片 | clock（0x9C） | `0xD2` | block 使用 | SCC+ |
|---|---|---|---|---|---|
| 01 Beyond the Darkness 1 | SCC | 1 789 772 | 18 344 | 0:896 1:6953 2:10416 3:79 | 否 |
| 01 Team Metal Slave | **K052539** | 1 789 773 | 1 450 | 0:320 1:206 2:863 3:29 **4:32** | **是** |
| 02 Beyond the Darkness 2 | SCC | 1 789 772 | 6 221 | 0:1504 1:1936 2:2678 3:103 | 否 |
| 02 Bio Hazard Snatcher A | **K052539** | 1 789 773 | 10 233 | 0:928 1:1138 2:7773 3:106 **4:288** | **是** |
| 03 Battle Ship | SCC | 1 789 772 | 15 341 | 0:4736 1:2542 2:7725 3:338 | 否 |
| 03 Bio Hazard Snatcher B | **K052539** | 1 789 773 | 10 118 | 0:608 1:2805 2:6479 3:66 **4:160** | **是** |
| 04 Squeak!! | **K052539** | 1 789 773 | 7 830 | 0:6144 1:1413 2:161 3:48 **4:64** | **是** |

- 四个 SCC+ 文件的 port 4 写入全部落在 **0x80-0x9F**（ch4 的独立表），与 SCC+ 布局吻合；
- 三个 SCC 文件只有 port 0，ch4 播放 ch3 的表——这是芯片行为，不是缺数据；
- **port 5（deform）一次都没出现**，所以这一版不做的风险为零；
- 所有文件的时钟都是 1.79 MHz（不是解码器默认的 3.58 MHz）；header 给了值，播放器会传进来。

---

## 5. 方案

### A. 解码器暴露波形（必做）

`K051649Decoder::Impl` 增加 `int8_t waveram[5][32]` 与 `bool modePlus`（`reset` 时从 `config.flags & 1` 取）。

`write()` 的 `case 0x00/0x04` 照抄核心：

```
if (modePlus)  waveram[offset >> 5][offset & 0x1F] = value;     // ch0..4，ch4 独立
else if (offset >= 0x60) { waveram[3][..] = value; waveram[4][..] = value; }  // 共享
else           waveram[offset >> 5][offset & 0x1F] = value;     // ch0..2
```

`channelState()`：

```cpp
state.wave      = waveForChannel (channel);   // 32 点，(float) (int8_t) value / 128
state.tableWave = true;
state.sampleRate = (float) (2.0 * clock / ((double) frequency + 1.0));
```

缓存：按 32 字节内容 hash（沿用 HuC6280/K005289 的写法，上限 64 张），`reset()` 清空。
SCC 模式下 ch3/ch4 字节相同，hash 相同 → 共用一次解码，天然一致。

### B. SCC+ 标志（必做）

`VgmReplayer` 的 `slotFlags` 增加：

```cpp
case DEVID_K051649: slotFlags = (device->devCfg != nullptr) ? (device->devCfg->flags & 1) : 0; break;
```

没有这一条，四个 SCC+ 文件的 ch4 会显示成 ch3 的拷贝，而芯片实际让它们独立。

### C. 不做

- **deform / test**（libvgm 未模拟，测试文件也未使用）；
- 噪声、采样导出都不适用（波表行由 “Export wavetables” 走 `tableWave` 那条路）。

---

## 6. 边界与风险

1. **8 行不是 5 行**：这些文件都是 AY + SCC 组合，SCC 行是最后五行；不要按“第 0 行是 SCC”写测试。
2. **SCC 模式下 ch4 = ch3**：这是芯片行为，行显示成相同形状是正确的，不应“修正”。
3. **缓存必须按内容**：SCC 模式下 ch3/ch4 写同一段字节；按通道缓存会存两份，按内容缓存自然共享。
4. **默认时钟**：解码器当前回落到 3 579 545，而 MSX SCC 是 1 789 772；header 有值时不受影响，
   是否修改见 §8。
5. **4 位音量**：现有 `state.volume` 已是 0..15，无需改。
6. **MIDI 不变**：音高已由现有 `SccTable` 给出，本次只动显示与导出用的 `wave`/`sampleRate`。

---

## 7. 验证计划

| 层 | 断言 |
|---|---|
| 解码器 | SCC 模式：`0x60` 同时写 ch3/ch4；ch4 读回与 ch3 相同 |
| 解码器 | SCC+ 模式：`0x60` 只写 ch3、`0x80` 只写 ch4，两者不同 |
| 缩放与速率 | `wave[0] = -1`（0x80）、`wave[16] = 0`、`sampleRate = 2·clock/(f+1)` |
| 行 | harness：七个文件 `engine.wave.3..7 == 32`；截图对比 |
| 导出 | “Export wavetables” 对五行可用，输出 2048 点单周期 |

---

## 8. 需要你确认

1. **SCC 模式的 ch4**：显示成与 ch3 相同的形状（推荐，芯片如此），还是隐藏该行？
2. **deform**：这一版不做（推荐，libvgm 也没模拟）？
3. **默认时钟**：解码器回落的 3 579 545 改成 **1 789 772**（MSX SCC 的实际时钟），还是保持不动？
   （只在 header 缺时钟时才有差别；这七首都不受影响。）
4. **缓存上限**：与 HuC6280 一样用 64 张，可以吗？
5. 行的 `sampleRate` 用**步进率**（`2·clock/(f+1)`，与 HuC6280 的“每秒步数”同一概念）——确认？

---

## 9. 已实现（2026-11-11）

按 A + B 落地：`int8_t waveram[5][32]` 与 `modePlus`（来自 `ChipConfig::flags` bit 0，`VgmReplayer`
把 `devCfg->flags` 传进来）；`0x00/0x04` 按 SCC/SCC+ 两种布局写入，并照抄 test 寄存器的只读语义；
行给 `wave`（`/128`）、`tableWave`、`sampleRate = 2·clock/(f+1)`，按内容 hash 缓存。

deform 仍不建模（libvgm 未模拟，七首也未使用）。实测：七首都能画出各自的表，`assert engine.wave.3..7 == 32`
在 SCC 与 SCC+ 各一首上都通过；“Export wavetables” 随之可用。三条断言钉住缩放与速率、SCC 的共享、
SCC+ 的分离。
