# 那个没人传的变体

> 2026-11-22。YM2413 的键盘行没有音名、没有琴键高亮，而它的 MIDI 一直是对的。
> 修好用了 `a6bdcb5`，但**它真正值得记的是为什么会存在**——同一个坑，这是第二步踩进去。

## 一、现象

用户报的是"YM2413 基本的键盘音符可视化以及 MIDI 输出都没有做"。**前半句对，后半句不对。**

同一首曲子（`test_file/YM2413/05_ChaosTemple.vgm`），三个证据：

```
[ 0] YM2413  1   rms 0.021104   7 note(s)      ← 有声音
track 0 -> channel 1: 2 note-on (first note 52)  ← 有 MIDI
```

而键盘页面上，**九行全部是 `-`，琴键一个都不亮，形状是一条通用正弦**。

**"有声音、有 MIDI、没画面"这个组合本身就是答案的一半**：它说明数据在，只是没进到画面的那条路。

## 二、排查路径（按实际顺序）

**1. 先验证用户的两条结论，发现有一条不成立。**
`vgm_mixer_test` 直接打印每轨音符数和 MIDI 事件——都在。所以"MIDI 没做"排除。

**2. 拿两颗芯片的同一页对比。** OPM（`YM2151`）的行读 `G2`、`E2` 并且琴键标蓝；YM2413 的行读 `-`。
**差异不在"FM 可视化"，在这一行没有状态。**

**3. 顺着画面往下读三层，每层都排除了一个候选：**

| 层 | 结论 |
|---|---|
| `KeyboardRowPaint` | 音名走 `midiNoteName (channel.note)`，`-` 就是 `note < 0`；高亮要求 `active`。**两者都不满足** |
| `ChannelFromChip` | `note` 由 `noteForFrequency (state.frequency)` 来，`frequency` 要 `noteFor (period, block)` |
| `OplDecoder::channelState` | 确认 `period` / `block` / `gate` **全都没被填** |

**4. 找"谁该填它们"，发现那段代码在一个条件后面：**

```424:445:plugin/Source/chips/OplDecoder.cpp
	if (impl->variant == Variant::opll)
	{
		if (reg >= kFnumLowBase && ...) { voice.fnum = ...; return; }
		if (reg >= kBlockBase && ...)   { voice.block = ...; voice.keyed = ...; return; }
```

**OPLL 的音高在 `0x10..0x18` / `0x20..0x28`，而 OPL2 的在 `0xA0` / `0xB0`。变体错了，这些写就落到了别的寄存器上。**

**5. 查"谁设过这个变体"——全仓只有一处：**

```
MidiSequenceBuilder.cpp:494   return chips::OplDecoder::Variant::opll;
```

**那是 MIDI 序列构建器自己 new 的第二个实例。**

**6. 查注册表怎么造实例：**

```129:132:plugin/Source/backends/libvgm/ChipWriteRouter.h
	virtual std::unique_ptr<chips::IChipDecoder> makeDecoder (void) const override
	{
		return std::make_unique<Decoder> ();		// 默认构造 = opl2
	}
```

**根因确定：设备型号从来没被传给解码器。**

## 三、修法

**和 OPN 家族早就用的同一套**：变体走 `ChipConfig::flags`，bit 0 仍是 Y8950 的 Delta-T，变体放 bits 1-2。

```cpp
// OplDecoder::reset —— 位 1-2 有值就用它，为 0 表示"构造函数已经说了"
const uint32_t flaggedVariant = (config.flags >> 1) & 3u;
const Variant variant = (flaggedVariant != 0) ? (Variant) (flaggedVariant - 1) : impl->variant;
```

**顺带修好了 OPL3**：`YMF262` / `YMF278B` 之前也是按 OPL2 解码，**第二组九个通道（9..17）和 rhythm 基准 18 全部丢失**。

**验证**：同一位置（`0:04.27`）修复前后，行 1-6 从 `-` 变成 `E2 G2 C3 E3 C1 C1`，琴键随之高亮。

## 四、为什么会犯这个错

**这是本文的重点。四条，按影响排序。**

### 1. 同一个坑，第二次踩 —— 上一次只补了一处，没有补规则

`VgmReplayer.cpp` 里就有一段注释，写的是**一模一样的事**：

```142:147:plugin/Source/backends/libvgm/VgmReplayer.cpp
			// **An OPN device has to say which OPN it is.** Four devices share `OpnDecoder`, and the
			// buffer makes its instances with the default constructor ... A YM2610 or YM2608
			// therefore reported seven channels where the chip has thirteen
```

**那句话把机制说清楚了："`OpnDecoder` 被四个设备共用，而 buffer 用默认构造。"** 但当时的修复是**在 `switch` 里补了四个 `case`**，**没有把"凡是共享解码器，变体必须由调用方传"变成一条规则**。

**于是 OPL 这一支原封不动地重演了一次**——连"缺通道"的症状都一样（OPL3 丢第二组九个通道、YM2413 丢十三行里的十三行状态）。

### 2. 测试测的是解码器，生产路径造的是另一个解码器

`vgm_chip_test` **早就有 OPLL 的测试**：

```
OK  OPLL FNUM 0x122 block 4 decodes to A4
OK  0x30 bit 5 is the OPLL key-off
```

**但它们自己 new 了带 `opll` 变体的解码器**——测的是"解码器知道 OPLL"，而生产路径造的是 `opl2`。

> **判据**：如果一个测试把生产代码里的某一行配置删掉之后**照样绿**，它守的不是那条路径。

### 3. 两个消费者，没有交叉验证

| 消费者 | 用哪个实例 | 结果 |
|---|---|---|
| 声音 | libvgm 自己的仿真 | 对 |
| **MIDI** | `MidiSequenceBuilder` **自建**、变体正确 | 对 |
| **画面** | 注册表默认构造的 `opl2` | **错** |

**MIDI 一直是对的，它其实是"数据在"的活证据。** 但没有任何一处把两者放在一起比，所以这个矛盾躺了很久。

> **判据**：两个消费者对同一件事说法不同时，第一个问题应该是"**它们是不是同一个对象**"。

### 4. 可视化的正确性没有断言

- `vgm_app_test` 是**截图回归**：它保证画面"没变"，不保证"对"。
- `vgm_mixer_test` 打印的 `note(s)` 来自 **MIDI**，不是 `ChannelState`。

**所以画面这一层从来没有人守。** 补上的第一条断言是 `testOpllVariantReachesTheDecoder`，它 **两半都测**：

```
OK  the flagged decoder has a row for an OPLL channel
OK  and its 0x20 key-on bit opens the gate
OK  an unflagged decoder reads those addresses as OPL2 operator registers   ← 锁住修复前的状态
```

**第二条才是守门人**：它明确写着"没有 flag 时什么都读不到"。**只测"修好了会怎样"的断言，会在下一次实现变化时照样绿。**

## 五、下一颗芯片要注意什么

**接 OPL 家族剩下的成员（OPLL 的真波形、OPL3 的第二组、OPX）时，第一件事是问：**

1. **这个解码器被几个设备共用？** 是的话，变体必须走 `ChipConfig::flags`。
2. **`channelState` 里哪些字段是空的？** `present` / `period` / `block` / `gate` 任意一个缺失，画面上就是"没有音符"。
3. **MIDI 对而画面不对**，先怀疑"两个不同的解码器实例"，而不是解码逻辑。

## 六、仍然开着的

**YM2413 的波形仍是通用正弦。** 这是**另一件事，不是这个 bug**：

- OPLL 没有逐算子的寄存器可抄（`OplDecoder.cpp:581` 那段注释），所以没有 `capturePatch`
- 修好变体后 `fmKnown` 会**诚实地变成 `false`**——之前它是"假装知道一个全零音色"，那本身也是个谎
- 真波形要 `OplDecoder::capturePatch`，内置音色从 ROM 或预置表来，**单独一轮**

**把"没有状态"和"没有音色"分开记，是这次唯一做对的地方**：前者是 bug（一行配置），后者是功能（一张表）。
