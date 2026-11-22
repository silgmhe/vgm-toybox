# 藏在芯片里的鼓

> YM2608 的六个 ADPCM-A 行有信号、没波形；修好之后有波形、音色不对。
>
> 两次都不是"算法错了"。第一次是**数据不在文件里**，第二次是**我把一个中间变量当成了单位换算**。
>
> 这一类芯片——OPN、OPL、OPM——都同时有 FM 段落和采样段落，而两者的**数据来源**与**地址来源**完全不同。
> 这份记录的重点是最后两节：一张四层诊断表，和一张给下一颗芯片的检查表。

## 一、两个症状

`10 Hey [Myrtle].vgz`（YM2608），键盘页十六行：

| 阶段 | 现象 |
|---|---|
| **一** | `ADPCM-A 1..6` 有电平（mixer 在响），**没有波形**；同曲的 `ADPCM-B` 正常 |
| **二** | 修出波形之后，**音色不对**——形状在动，但不是那个鼓 |

**同一个文件、同一颗芯片，`ADPCM-B` 一直是对的。** 这是第一个有用的线索：不是"采样段落整体坏了"。

## 二、采样行和 FM 行差在哪里

FM 行需要的东西，采样行一样都不缺：

```
patch（音色）  →  core->write  →  render  →  peak
```

采样行不需要 patch，它需要**另外两样**：

| 需要 | 从哪来 |
|---|---|
| **采样数据**（字节） | 歌曲文件里的 ROM 块，**或芯片内部的 ROM** |
| **这段采样的起止** | 寄存器，**或一张硬件表** |

**"或"字后面那两种，就是这次两次失败的地方。**

## 三、四层诊断

采样行没有波形，有四个可能的位置，**必须按顺序排掉**：

| 层 | 量 | 为假时说明 |
|---|---|---|
| **1. `rom`** | `impl->adpcmARom.size()` | 数据根本没进来——块号错、或数据不在文件里 |
| **2. `latch`** | `regs.latchStart..latchEnd` | 起止是 0（或相等）——没有 key on 时的地址计算 |
| **3. `gate`** | `state.gate` | 这一帧没在响——正常，等下一帧 |
| **4. `wave`** | `state.wave->size()` | 前三层都过而这里空，才是切片的代码问题 |

**这次打印的顺序是 `latch → gate → rom`，于是前两轮都在看 `latch=0..0`，而真正空的是 `rom`。**

**按 1→2→3→4 打，第一轮就能落到 `rom` 上。**

### 一条能立刻定性的打印

```cpp
[adpcmA] slot=%zu latch=%u..%u gate=%d rom=%zu
```

**四个数一起看：**

```
rom=0            → 数据没进来，别再看别的
rom>0 latch=0..0 → 没有地址，去看 key on 那段
rom>0 latch 对 gate=0 → 这一帧没响，正常
rom>0 latch 对 gate=1 而 wave 空 → 切片代码
```

## 四、根因一：数据不在文件里

### `kRequirements` 声明的块号是另一颗芯片的

```cpp
static constexpr uint8_t kAdpcmABlock = 0x82;
static constexpr ChipRequirements kRequirements { 8000000u, true, kAdpcmABlock, ... };
```

**`0x82` 是 VGM 里 "YM2610 ADPCM ROM" 的块号。**

**YM2608 的文件里没有它**——那个芯片只有一个 `0x81`（Delta-T ROM）。于是：

```cpp
const std::vector<uint8_t> rom = memory::rom (ir, type, ...);   // 空
if (! rom.empty ()) { attachMemory (...); }                     // 没执行
```

`adpcmARom` 一直是空。

### 而 YM2608 的鼓根本不在文件里

```c
/* fmopn.c */
static const unsigned char YM2608_ADPCM_ROM[0x2000] = { ... };   // 8 KB，芯片内部
```

**这是硬件的内部 ROM，没有哪个 VGM 块携带它。** 所以即便块号改对，文件里也不会有。

**三颗芯片的采样数据来源完全不同：**

| 芯片 | ADPCM 数据 | 文件块 |
|---|---|---|
| **YM2608** | 六个鼓在**芯片内部** 8 KB ROM | 只有 Delta-T（`0x81`） |
| **YM2610** | 由**歌曲提供** | `0x82`（ADPCM）+ `0x83`（Delta-T） |
| YM2612 | 无 | — |

**同一个 `OpnDecoder`，两套数据来源。** `kRequirements` 只有一个 `memoryType`，写死的是 OPNB 那一套。

### 修法：由调用方注入，而不是解码器去 include

```cpp
/* VgmReplayer.cpp */
void attachRhythmRom (toybox::chips::IChipDecoder& decoder, uint8_t slotFlags)
{
    if (slotFlags != 2 || typeid (decoder) != typeid (toybox::chips::OpnDecoder))
        return;

    static const std::vector<uint8_t> rhythm (YM2608_ADPCM_ROM,
                                              YM2608_ADPCM_ROM + sizeof (YM2608_ADPCM_ROM));
    toybox::chips::ChipMemory memory;
    memory.bytes = &rhythm;
    decoder.attachMemory (memory);
}
```

**为什么不在 `OpnDecoder.cpp` 里 include `fmopn_2608rom.h`**：`vgm_chip_test` 编译这个文件时**没有 libvgm 的头路径**（那是 P0-4 修过的东西）。拉进来会重新踩那个坑。

**这条是一个可复用的约束**：解码器属于 `chips/`，它不认识 libvgm；需要核心内部数据的，由 `backends/libvgm/` 注入。

**注入点有两处**，因为 `reset` 会 `*impl = Impl()` 把一切都清掉：

- `adoptNewBuffers`：设备第一次出现
- `rewind`：复位之后

**漏掉第二处，从头播放时鼓就没有了。** 这类"复位清掉注入"的坑，在这个文件里已经用注释标过两次（`adpcmBRom` 和采样图像）。

## 五、根因二：地址不在寄存器里

### OPNA 的 rhythm 没有可写的起止

```c
/* fmopn.c, FM_ADPCMAWrite */
switch (r) {
case 0x00:  /* key on */
case 0x01:  /* TL */
case 0x08:  /* start low */
case 0x10:  /* end low */
case 0x18:  /* start high */
case 0x20:  /* end high */
}
```

**而 `ym2608_write` 只把 `0x10-0x1F` 交给它，且 `r = addr - 0x10`：**

- `0x10` → `r=0` → key on
- `0x18` → `r=8` → **start low**（能到）
- `0x20` → `r=0x10` → end low —— **但 `0x20` 在 `ym2608_write` 里是 `case 0x20`（Mode 寄存器），根本不会进 `FM_ADPCMAWrite`**

**所以 `0x18-0x1D` 那条 start low 写进去也没人用，`end` 完全不可达。** 硬件用的是表：

```c
static const unsigned int YM2608_ADPCM_ROM_addr[2*6] = {
    0x0000, 0x01bf, /* bass drum  */
    0x01c0, 0x043f, /* snare drum */
    0x0440, 0x1b7f, /* top cymbal */
    0x1b80, 0x1cff, /* high hat   */
    0x1d00, 0x1f7f, /* tom tom    */
    0x1f80, 0x1fff  /* rim shot   */
    };
```

**而解码器只调了 `keyAdpcmA`，它只记一个 gate：**

```cpp
void keyAdpcmA (uint32_t time, uint8_t data)   // 没有 start，没有 end
```

于是 `latchStart == latchEnd == 0`，而切片的条件是 `latchEnd > latchStart` —— **永远为假。**

**修法**：key on 时按表填。

## 六、根因三：单位（"音色不对"那一次）

第一版修法写成了：

```cpp
regs.latchStart = kOpnaRhythmAddress[channel * 2] << 1;
regs.latchEnd = (kOpnaRhythmAddress[channel * 2 + 1] << 1) + 1;
```

**依据是核心里的这一行：**

```c
adpcm[c].now_addr = adpcm[c].start << 1;    /* fmopn.c */
```

**看起来"核心自己都在左移，说明表是小单位"。错了。**

`now_addr` 是 **nibble 游标**——每个 ADPCM 步半个字节，所以从字节起点换算到 nibble 才需要 `<< 1`。
而 `latchStart` 索引的是 `adpcmARom[index]`，**按字节**。

**翻倍的后果，以及为什么它表现为"音色不对"而不是"没有波形"：**

```
high hat 的真实起点  0x1B80 = 7040
左移之后              14080
& adpcmAMask (0x1FFF) 5888        ← 落进别的一段
```

**地址超出 ROM，被掩码回绕，于是切到了另一个鼓。** 长度也跟着从 384 字节变成 767。

| | latch | 字节 | 样本 |
|---|---|---|---|
| 左移 | 14080..14847 | 767 | 1534 |
| 正确 | 7040..7424 | **384** | **768** |

**384 = `0x1CFF - 0x1B80 + 1`**，正好是表里 high hat 那一段。

**判据**：`kOpnaRhythmAddress` 覆盖 `0x0000-0x1FFF`，而 ROM 是 `0x2000` 字节——**表的最大值等于 ROM 的最大索引**。任何左移都会越界，所以这个表只能是字节。

## 七、教训

### 7.1 采样段落要比 FM 段落多问两个问题

**FM：音色在寄存器里，音高在寄存器里，数据不缺。**

**PCM：数据在哪？地址在哪？**

这两问没问，就会写出"能编译、能运行、有一半正确"的代码——因为同族的另一颗芯片（YM2610）恰好两者都在文件里，`0x82` 也恰好是对的。

**"同族"只保证寄存器大体相似，不保证数据来源相同。**

### 7.2 块号是"哪颗芯片的哪份数据"，不是"这类芯片"

`kRequirements` 是 `static constexpr`，对整类设备只有一份。**一颗芯片的采样在文件里、另一颗在芯片里，这个结构就装不下。**

将来再遇到（YM2151 没有 PCM、YMF271 有、OPL4 的 wavetable 又是另一套），**先确认 `kRequirements` 能不能表达**，表达不了就要么注入、要么按变体分叉，而不是把块号换成一个"大概对"的值。

### 7.3 照抄核心的中间变量之前，先问它是给谁用的

`now_addr = start << 1` 是一个**内部游标**的换算，不是公开的单位定义。**中间变量带着它自己的坐标系。**

**安全的做法**：找到一个**端点**去对齐。这次是"表的最大值 = ROM 的最大索引"，一句话就能判定单位，成本是一行注释。

### 7.4 复位会清掉注入

`*impl = Impl()` 之后，之前 `attachMemory` 的东西全没了。**所有"从外面塞进来的状态"，都必须有第二次注入点**（`rewind`）。

这个文件里已经有两条注释在说同一件事（采样图像、Delta-T ROM），**这是第三条**。

### 7.5 打印的顺序决定了要查几轮

这次的打印是 `latch → gate → rom`，于是前两轮都在看 `latch=0..0`，而真正空的是 `rom`。

**按依赖顺序打**：`rom`（有没有数据）→ `latch`（有没有地址）→ `gate`（这一帧响不响）→ `wave`（切没切出来）。

**前一层为空，后面几层没有意义。** 打印顺序错了，人会顺着第一个非零值往下猜——而那个值可能是另一层的结果。

### 7.6 "音色不对"通常意味着地址对了、位置错了

**没有波形**和**波形不对**是两个不同层的故障：

| 现象 | 层 |
|---|---|
| 没有波形 | 数据 / 地址 / 门控 |
| 有波形但音色错 | **切片位置**（地址算错、单位错、掩码回绕） |

**第二次的 `latch=14080` 一眼就能看出来是错的**——如果当时把 `latch` 和"表里的值"并排打出来。**打印要带上期望值，否则异常的数字看起来只是"另一个数字"。**

## 八、给下一颗"FM + PCM"芯片的检查表

实现前把这几行填掉，比事后查三轮便宜。

| # | 问题 | YM2608 的答案 |
|---|---|---|
| 1 | **PCM 数据从哪来？** 文件块 / 芯片内部 / 两者 | **芯片内部 8 KB** |
| 2 | **文件块号是什么？** 按变体确认 | 只有 Delta-T `0x81`；`0x82` 是 OPNB 的 |
| 3 | **`kRequirements` 表达得了吗？** | **表达不了**，需要注入 |
| 4 | **地址从哪来？** 寄存器 / 硬件表 / 混合 | **硬件表**，寄存器不可达 |
| 5 | **表的单位是什么？** 字节 / word / nibble / 页 | **字节**（最大值 = ROM 长度） |
| 6 | **解码器按什么索引？** 字节还是 nibble | 字节（`adpcmARom[index]`） |
| 7 | **门控窗口多长？** key on 之后多久 | 80 ms（`kAdpcmAGate`） |
| 8 | **复位后要重新注入吗？** | **要**，两处 |
| 9 | **诊断四层都打了吗？** `rom/latch/gate/wave` | 顺序不能反 |

### 一个能提前发现 5、6 两项的自检

**把表的最大值和 ROM 的长度并排写出来：**

```cpp
static_assert (kOpnaRhythmAddress[2 * 6 - 1] < 0x2000,
               "the table is byte offsets into YM2608_ADPCM_ROM");
```

**一条 `static_assert` 就能挡住"左移一位"这个错误**，而且它在写代码的时候生效，不是在听曲子的时候。

## 九、留下的

**`MidiSequenceBuilder` 的扫描实例没有这份 ROM。** 它只用 `gate`/`note`/`volume`，不读 `wave`，所以现在无害。

**但 `WaveExport` 如果哪天要走那条路径，这里需要一起接。** 这是同一个"数据在芯片里"的问题在另一条路径上的第二次出现——**写下来的意思是，下次遇到它时不必重新分析一遍。**
