# YM2151（OPM）可视化：调研与实施方案

> 2026-11-22。第二次调研。第一次的结论被 harness 的故障带偏过一轮，本文所有结论都建立在**已修好的基准**上（`7ca9054` 修 harness、`9599c30` 加 `shot` 自检）。
>
> 素材：`test_file/YM2151/` 14 首，0.4 – 16.3 KB。

## 一、已确认的现状（实测，不是推断）

`05 Intimidating Spaces (Boss).vgz` 第 6.4 秒，harness 与真实窗口一致：

```
name=Ch 1  kind=fm  active=1  note=55  vol=1.00  wave=0  noteWave=0  patch=0
name=Ch 2  kind=fm  active=1  note=52  vol=1.00  wave=0  noteWave=0  patch=0
...                                                   ← 八行
name=Ch 8  kind=fm  active=0  note=33  vol=1.00  wave=0  noteWave=0  patch=0
```

| 项 | 状态 | 说明 |
|---|---|---|
| 八行 | ✅ | `ChipPlans { 8, 0, 0 }` 已在 |
| `active` | ✅ | key 解析正确 |
| `note` | ✅ | **KC/KF → 半音的换算是对的**（G3/E3/E4/E4/B3/B2/E2/A1） |
| 单周期波形 | ✅ | 走 `ChannelState::fmKnown` 那条降级路径 |
| `name` | ⚠️ | `Ch 1`–`Ch 8`，不是 `FM 1` |
| `volume` | ⚠️ | 恒 `1.00`（`OpmDecoder` 写死 `15`） |
| `noteWaveform` | ❌ | 空 |
| `voicePatch` | ❌ | 空 |

**所以 OPM 的起点比第一次调研判断的高得多**：不用接线，直接做"整段包络 + 音量 + 名字"。

## 二、基础设施已就绪的六处

调研中意外的是，规划层几乎都接好了：

| 位置 | 内容 |
|---|---|
| `ChipPlans.cpp:37` | `/* ym2151 */ { 8, 0, 0 }` |
| `VoicePatch.h:118` | `noteRateFor = 3579545.0 / 64.0` → **55 930 Hz** |
| `VoicePatch.h:90` | `fmOutputBits(ym2151) = 16` |
| `ChipModel` | `ym2151` 已存在 |
| `OpnVoiceDriver::deviceIdFor` | `case ym2151: return DEVID_YM2151` |
| `OpnVoiceDriver::clockFor` | `3579545`，注释写着 "OPM 64" |

`load` 的注释甚至已经写了「a backend with an OPN and an OPM rebuilds when the patch names a different chip」——**预留了路径，只是没人走完。**

## 三、`OpmDecoder` 的现状（153 行）

**已经对的：**

- **KC/KF 解析**，note code 表正确（`code - code >> 2` 跳过 3/7/11/15）；
- **`period = (kc << 6) | kf`**，`OpmNoteTable` 直接反算半音，不经过 f-number；
- **operator 编址 `0x40 + op*8 + ch`**（与 OPN 的 `+ op*4` 不同，注释已写明）；
- **key 判定**：`reg == 0x08`、`channel = value & 0x07`、`keyed = (value & 0x78) != 0`。

**缺的：**

- `Impl::Channel` 只存 `multiple`/`totalLevel`/`algorithm`/`feedback` 四组；
- **缺 `0x80`（KS/AR）、`0xA0`（AME/D1R）、`0xC0`（DT2/D2R）、`0xE0`（D1L/RR）四个 bank**；
- 没有 `capturePatch`；
- `channelState` 里 `state.volume = 15` 是字面量。

**注释要改的一处**：`OpmDecoder.h` 写「The key register 0x08 carries the channel in bits 2..0」——容易被读成"地址的低三位也是通道"，而代码是 `reg == 0x08` 精确匹配。**14 首文件实测全部如此**（`0x78|ch` key on、`0x00|ch` key off），所以**语义对，注释要澄清"整颗芯片只有一个 key 寄存器"**。

## 四、最大的未知：`nukedopm.c` 的写协议

**这是必须最先验的一件事，它决定后面所有工作是否可能。**

`OpnVoiceDriver` 通过 `SndEmu_Start(DEVID_YM2151)` 启动核心，再用 `devDef->Write` 写寄存器。而 `nukedopm.c` 的入口是：

```2374:2377:libs/libvgm/emu/cores/nukedopm.c
static void nukedopm_write(void *chip, uint8_t a, uint8_t v)
{
    OPM_WriteBuffered((opm_t *)chip, a, v);
}
```

```2303:2319:libs/libvgm/emu/cores/nukedopm.c
if (chip->writebuf[chip->writebuf_last].port & 0x02)
{
    NOPM_Write(chip, chip->writebuf[chip->writebuf_last].port & 0x01, ...);
}
...
chip->writebuf[chip->writebuf_last].port = (port & 0x01) | 0x02;
```

```2204:2211:libs/libvgm/emu/cores/nukedopm.c
if (port & 0x01) { chip->write_d = 1; }
else             { chip->write_a = 1; }
```

**`port` 只用到 bit 0——它是"地址/数据选择"，不是寄存器地址。** 而 `devFunc` 声明的是 `DEVRW_A8D8`（8 位地址 + 8 位数据）。

**两种可能：**

1. **libvgm 在中间把 `0x54 aa dd` 拆成了两次写**（`a=0` 发地址、`a=1` 发数据）——那 `OpnVoiceDriver` 的 `core->write(0, 0x28, v)` 就**不工作**，必须改成两次调用；
2. **`a` 的高位在别处被使用**，bit 0 只是副作用——那现在的单次写是对的。

**代码读不出答案**（`OPM_WriteBuffered` 把 `port` 直接截成 `& 0x01`，没有任何地方保留高位）。**只能实测。**

### 验证方法（15 分钟）

在 `OpnVoiceDriver` 里加一个临时的 `load` 分支，只写四个 OPM 寄存器，然后 `render` 一秒，看 `peak`：

```
0x20 | ch = 0xC0      RL 全开（bit 6 L、bit 7 R），CON = 0（4 op 串联）
0x28 | ch = 0x4A      KC = block 4, code 10 → A4
0x30 | ch = 0x00      KF = 0
0x08      = 0x78      key on 全部四个 operator（ch 0）
```

**判据：**

| `peak` | 结论 |
|---|---|
| `> 0.4` | 单次写是对的，方案 A 可行 |
| `≈ 0` 且 `left`/`right` 都是 0 | 写协议不匹配（可能性 1），需要两次写 |

**这一步做完之前不要动任何其他代码**——否则一个 0 分不清是"寄存器写错了"还是"协议错了"。

## 五、方案：四个阶段，每阶段独立可验

### 阶段 0：验写协议（见第四节）

**改动**：临时分支，验完删除。
**验收**：`peak > 0.4`，或确认需要两次写并记录。

### 阶段 1：名字与音量（最小，先落地）

**1a. 名字 `Ch N` → `FM N`**

`describeSong` 里给 `ChipKind::ym2151` 一条命名分支。需要先找到现在的兜底在哪个函数（`LibvgmSongInstance.cpp` 的轨道命名处，本轮未读完）。

**1b. `volume` 由 TL 推出**

```cpp
// OpmDecoder::channelState
state.volume = 15 - (loudestCarrier (voice) * 15) / 127;
```

`loudestCarrier` 照 `OpnDecoder` 的形状写，但**载波表不同**：

| | OPN | OPM |
|---|---|---|
| operator 编号 | 1-4，寄存器 `+ op*4` | M1/M2/C1/C2，`+ op*8` |
| 载波表 | `kCarriers[8]` | 见下 |

**OPM 的载波掩码可以从 `nukedopm.c` 的 `fm_algorithm[4][6][8]` 读，但不要照抄**——它的 `[op]` 索引顺序与寄存器里的 operator 编号不是一回事（这一节读了两遍才确认）。**更稳的做法：查 YM2151 datasheet 的 CON 表，用固定的 8 个掩码，并在注释里写明出处。**

**注意**：`volume` 只影响**音量条显示**，不影响波形——`capturePatch` 把 TL 原样回写，渲染时由核心自己算算法。**所以这一项可以先用近似，不必阻塞。**

**验收**：八行的音量条随音乐变化；同一通道在不同算法下不再恒满。

### 阶段 2：patch（核心）

**2a. `Impl::Channel` 扩成六个 bank 的原始值**

```cpp
struct Channel
{
    uint8_t op[4][6];    // [operator][bank]，bank 依次 0x40/0x60/0x80/0xA0/0xC0/0xE0
    uint8_t control;     // 0x20-0x27 原值（RL/FB/CON）
    uint8_t kc, kf;
    bool keyed, havePitch;
};
```

**存原值而不是解析后的字段**，因为 `VoicePatch` 的设计就是"回写寄存器"——解析成 `multiple`/`totalLevel` 后渲染前还要再编码回去，那是第二份权威（`code-quality.md` §13）。

**代价**：`channelState` 里现在读的 `multiple`/`totalLevel` 要改成从 `op[..]` 取，`fmKnown` 那条降级路径跟着改。

**2b. `capturePatch`**

```cpp
bool OpmDecoder::capturePatch (int channel, VoicePatch& out) const
{
    out = VoicePatch {};
    out.model = ChipModel::ym2151;

    for (int op = 0; op < 4; ++op)
        for (uint8_t bank = 0x40; bank <= 0xE0; bank += 0x20)
            out.regs.push_back ({ (uint16_t) (bank + op * 8 + channel),
                                  op[op][(bank - 0x40) / 0x20] });

    out.regs.push_back ({ (uint16_t) (0x20 + channel), control });
    return true;
}
```

**25 条**：4 op × 6 bank + 1 条通道级。

**`+ op * 8` 不是 OPN 的 `+ op * 4`** —— 这是两颗芯片最容易混的一处，`capturePatch` 里要再写一次，因为它是**生成**地址而不是解析地址，读者会以为能复用 OPN 的模板。

**验收**：`capturePatch` 返回 25 条；offset 全在 `0x20-0xFF`；对同一通道两次调用结果相同；**把 `op*8` 改成 `op*4` 会让断言失败**。

### 阶段 3：渲染（整段包络）

`OpnVoiceDriver` 的 OPM 分支，四处：

| 位置 | OPN | OPM |
|---|---|---|
| `channelInPort` | `first & 0x03` | `first & 0x07` |
| `load` 的初始化 | `0x22`/`0x27`/`0x2B`/`0x29` | **都不写**；写 `0x20\|ch` 开 RL |
| `noteOn` | f-number + block | **KC/KF** |
| `noteOff` | `0x28` 的 key 位 | `0x08 \| ch = 0` |
| `divisor` | 144 / 288 | **64** |

**`render` 与 `rate` 完全通用**（只调 `devDef->Update`、读 `core->rate`），不动。

**`noteOn` 的反算是本轮唯一的新数学：**

```cpp
// 正向（OpmNoteTable::noteFor）：semitone = 13 + 12*block + (code - code>>2) + kf/64
const double x = semitones - 13.0;
const int block = std::clamp ((int) std::floor (x / 12.0), 0, 7);
const double r = x - 12.0 * block;                       // 0..12
const int delta = std::clamp ((int) std::floor (r), 0, 11);
const int kf = (int) std::lround ((r - delta) * 64.0);

// delta → code：code - (code >> 2) == delta 的最小 code
static constexpr int kCodeForDelta[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
```

**`render` 的 mixdown 是 `(L + R) * 0.5`** —— **RL 位为 0 时两个声道都是 0，`peak` 就是 0。** 这是"OPM 静音"的头号原因，`load` 里必须写 `0x20`。

**验收**：`05 Intimidating Spaces` 的八行各自有整段包络；`noteWave != 0`；`peak > 0.5`；六个不同音色的通道波形互不相同。

## 六、断言（三条，都要能失败）

| # | 断言 | 失败时看到 |
|---|---|---|
| 1 | `OpmNoteTable` 往返：MIDI 24–96 每个音，`noteFor(encode(n))` 回到 `n`（±1 cent） | 某个八度整体偏移，或 block 边界跳音 |
| 2 | `capturePatch` 的 25 条、offset 范围、`op*8` | 通道 4–7 的 patch 落到别人身上 |
| 3 | `OpnCoreOracleTest` 风格的渲染：`noteOn` 后 `peak > 0.5` | 写协议、RL 位、KC/KF 任一处错 |

**第 1 条是这次唯一"新写的数学"，必须验往返**——`OpmNoteTable::noteFor` 是既有的、被 14 首曲子的音高证明过的；`encode` 是新写的，**只有往返能证明它和 `noteFor` 是同一个映射的两端**。

## 七、风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **写协议不匹配**（第四节） | 阶段 3 完全不可行 | **阶段 0 先验，15 分钟** |
| **载波表照抄 `fm_algorithm` 会错** | 只影响音量条 | `volume` 是显示，不阻塞 |
| OPM 的 `0x08` 若某文件用 `0x09` 写通道 1 | 该通道不亮 | 14 首实测全是 `0x08`；**注释写清，并加一条"若出现 `0x08-0x0F` 的地址也要处理"的兜底** |
| `load` 的 OPN 寄存器写进 OPM | 可能锁死或静音 | 分支里**一个 OPN 寄存器都不写** |

## 八、顺序与工作量

| 阶段 | 内容 | 估计 | 依赖 |
|---|---|---|---|
| **0** | 验写协议 | 15 分钟 | 无 |
| **1** | 名字 + 音量 | 40 分钟 | 无 |
| **2** | 六个 bank + `capturePatch` | 1.5 小时 | 无 |
| **3** | 驱动四处分支 + `noteOn` 反算 | 2 小时 | 阶段 0 结论 |

**阶段 3 必须等阶段 0**；1 和 2 可以并行。

**每阶段的验证都用修好的 harness**：`tick` → `shot`，现在它会因为"有歌无行"而失败，**这正是它该做的事**。

## 九、不做的事

- **CSM 模式**（`0x0F` 的 CSM 位）：OPM 的 ch 3 可以独立触发四个 operator。手上没有用 CSM 的曲子，不验不做。
- **LFO**（`0x18`/`0x19`/`0x1A`/`0x1B`）：波形里不会有 vibrato/tremolo。**这是取舍**——与 OPN 的 `0x22 = 0` 同一条理由：让波形可复现。
- **噪声模式**（`0x0F` bit 7）：同上，没有素材。
- **`0x14-0x17` 的时钟分频**：只影响实际输出的音高，而 `noteRateFor` 已经用固定 64；若某文件改了它，音高会偏——**目前 14 首没有**。
