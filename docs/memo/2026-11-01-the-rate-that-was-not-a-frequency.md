# 一个「读取速率」被当成了「音高」

> 现象：C352 的每个通道都显示 C9–C10，而 libvgm 播放完全正常。
> 根因：`Pitch.h` 的 `describeSamplePitch` 把字节率直接写进了 `hz`，**少除了一次样本周期**。
> 排查用了六轮，前五轮全部是错的假设。这篇记的是那六轮，因为**它们比结论更有用**。

## 1. 现象

打开 `01 Welcome Racer.vgz`（Namco System 22，C352），Keyboard 页：

```
Ch1  C#9   Ch2  B8   Ch3  D#9  ...  全部八到九个八度
```

**声音是对的**（libvgm 自己播），**行也在**（通道数、音量、形状都对），只有音高离谱。

## 2. 六轮排查

每一轮的结论都是「排除一个」，没有一轮是「找到」。

| 轮 | 假设 | 证据 | 结果 |
|---|---|---|---|
| **1** | 解码器没拿到 ROM，测不出样本周期 | 加 `attachMemory` 后日志 `blocks=12 image=16777216` | 饿，但没修好 |
| **2** | C352 的命令是 `0x5A`，`VgmCommandReader` 没解析它的操作数 | `VgmSongLoader.cpp:141` 写的是 `0xE1`；我加的 `0x5A` 分支和原有的 `0xE1` 分支**重复** | 改错了，删掉 |
| **3** | `0x202`（execute）的 value 被解码器忽略 | libvgm `c352.c:428` 确认 value 本来就不用，KEYON 位在各 voice 的 `flags` | 排除 |
| **4** | 寄存器映射错位 | libvgm `C352RegMap` = `vol_f, vol_r, freq, flags, bank, start, end, loop`，和解码器 `case 0-7` 逐项一致 | 排除 |
| **5** | 所有样本都测不出周期 | 打出 `cycle` 值：**5 个里 2 个成功**（72.5、287.5） | **关键转折** |
| **6** | 测量结果怎么用 | `describeSamplePitch` 里 `pitch.hz = rate` | **找到** |

### 第 5 轮为什么是转折

如果 5 个全失败，故事会是「ROM 内容不对」或「地址算错」，还要继续往下挖。

**但有 2 个成功** —— 说明测量机制是好的，**问题在「测出来的值怎么用」**。

**一个部分成功的信号，比全失败有用得多**：全失败说明这条路不通，部分成功说明路是通的，中间掉了东西。

### 第 6 轮是怎么想到的

到这一步，已经不是「猜」了 —— 是**读那个把结果交出去的函数**：

```cpp
// Pitch.h:252（改前）
pitch.hz = rate;
pitch.transposeCents = transpositionCents (rate, referenceRate);
pitch.rootCents = rootCentsFromCycle (cycleLength, referenceRate);
```

**同一个函数里，`rootCents` 做了除法、`hz` 没做。**

而紧挨着的注释写着：

```
`transpositionCents (rate / cycle, middleC)` equals
`transpositionCents (rate, refRate) + rootCentsFromCycle (cycle, refRate)`
```

**注释里的 `rate / cycle` 和代码里的 `rate` 差了那一项。** 这种「同一个函数里两处不一致」本身就是证据，不需要先知道正确答案。

## 3. 根因

```cpp
// 改前
pitch.hz = rate;
```

`rate` 是**每秒字节数**（`frequency × clock / 288`），**不是频率**。一个样本听起来多高 = **读取速率 ÷ 一个周期的字节数**。

```cpp
// 改后
pitch.hz = (cycleLength > 0.0) ? rate / cycleLength : 0.0;
```

用实测值验算：`cycle = 287.5`、`rate ≈ 40000` 字节/秒 →

```
旧：40000           → C9    （和现象一致）
新：40000 / 287.5   → 139 Hz → C#3
```

**`cycleLength == 0` 时给 0，不回退到 `rate`**：一个「长得像音高、但不属于任何声音」的数，比空白单元格更糟。这一支已经为这个付过一次代价（NES 的 DPCM）。

## 4. 为什么只有 C352 显形

七颗采样芯片共用这个函数，**全都偏高**，只是幅度不同：

| 芯片 | 速率寄存器宽度 | 偏高幅度 | 看起来 |
|---|---|---|---|
| **C352** | 16 位 | 约八个八度 | **C9，直接出界** |
| C140 / ES5503 / RF5C68 / QSound… | 8 位 | 两三个八度 | **像正常音高** |

**同一个 bug，值域大的先撞到边界。**

这也是为什么「C140 和 ES5503 看着正常」这条观察，反而把人往「C352 特殊」的方向带了一程。

## 5. 三条可复用的判断

**① 部分成功比全失败信息量大。**
全失败是「这条路不通」，部分成功是「路通了，中间掉了东西」—— 后者能立刻缩小到「交接处」。

**② 同一个函数里两处做同一件事、一处做了转换一处没做，那就是证据。**
不用先知道答案，先找不一致。

**③ 「声画不一致」时先看单位。**
播放正确而显示错误，说明**播放路径和显示路径读的不是同一个量**。libvgm 播得对是因为它自己算；我们错是因为把「字节/秒」当成了 Hz。

## 6. 涉及的文件

| 文件 | 改动 |
|---|---|
| `plugin/Source/chips/Pitch.h` | `describeSamplePitch` 的 `hz` 补上除法 |
| `plugin/Source/backends/libvgm/ChipMemoryAssembly.h/.cpp` | 从 `Builder` 搬出来的镜像装配（第 1 轮需要它） |
| `plugin/Source/backends/libvgm/VgmReplayer.cpp` | 建解码器实例时 `attachMemory` |
| `plugin/Source/backends/libvgm/VgmCommandReader.cpp` | 删掉一个重复且不可达的 `0xE1` 分支 |

## 7. 没做的事

**`cycleFor` 里 `loopLength > 256` 时不回退** —— 有样本（`loopLength=607`）测不出周期又没有回退，结果是 0。现在那个 voice 会显示空音高，**这是诚实的，但不是完整的**：一个多周期的循环理论上可以用 `estimateCycleLength` 的返回值除以周期数，那需要新的判断，不适合顺手加。

**这条留着，下次遇到「某个 C352 曲子有几行没音高」时从这里开始。**
