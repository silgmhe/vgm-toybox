# OPM 自己也会重采样，而且不滤波

> 2026-11-22。YM2151 的导出音量偏小、还有滋滋，播放没有。
>
> 前一份文档（`the-hiss-that-was-resampling.md`）的结论是"把重采样去掉"。**这一份是它的反面：对这颗芯片，去掉重采样反而更糟——因为核自己会做，而且做得很差。**

## 一、两个现象，一个原因

| | 播放 | 导出 |
|---|---|---|
| 音量 | 正常 | **偏小** |
| 滋滋 | 没有 | **有** |

**看起来是两个问题。实际是同一个：高频被削掉了。**

**削掉高频的东西，在核里面。**

## 二、第一轮：位深（真的错了，但不够）

**和 YM2612、YM2610 一样先查 `fmOutputBits`：**

```
YM2151 chip  peak 0.249
YM2151 48k   peak 0.249
```

**`0.249` = 满幅的四分之一，和 YM2612 修好前完全一致。** 表里写的是 16 位，实际是 14 位。

**改成 14 后 `peak 0.997`，音量对了。但滋滋还在。**

**这一轮的意义：它排除了位深，把注意力推到"高频还在被什么东西削"上。**

## 三、根因：核自带一个线性插值器

**`nukedopm`（OPM 的核）有一个内部的分数重采样器：**

```2367:2370:libs/libvgm/emu/cores/nukedopm.c
    buf[0] = (oldsamples[0] * (rateratio - samplecnt) + samples[0] * samplecnt) / rateratio;
    buf[1] = (oldsamples[1] * (rateratio - samplecnt) + samples[1] * samplecnt) / rateratio;
```

**`rateratio` 是分数（`RSM_FRAC` 定点），这是**线性插值**。**

**`NOPM_Reset` 里把它设成 `64 * smplRate / clock`：**

```2291:2293:libs/libvgm/emu/cores/nukedopm.c
    chip->rateratio = (uint32_t)((((uint64_t)64 * chip->smplRate) << RSM_FRAC) / chip->clock);
    if (abs(chip->rateratio - (1 << RSM_FRAC)) <= 1)
        chip->rateratio = (1 << RSM_FRAC);
```

**当 `smplRate` 等于 `clock / 64` 时，比值归一为 1.0，插值器什么都不做。** 那是 native。

**一旦要求别的速率——比如文件的 48 kHz——它就变成 0.861，开始插值。**

**线性插值不做低通**：它只能让样本从 A 线性过渡到 B，**降采样时高于新奈奎斯特的内容会原样折叠回来**。这就是那层滋滋，也是高频被削、听感变小的原因。

## 四、为什么上一轮的修法在这里反而更糟

**上一轮为了消除"我们自己的抽取"，让核直接跑文件的速率：**

```cpp
const double wantedRate = exportQuality ? (exportRate > 0 ? exportRate : noteRateFor) : kVisualNoteRate;
```

**对 YM2612 / YM2608 / YM2610 是对的**——它们的核直接按请求的速率生成样本，没有中间环节。

**对 OPM 是错的**——它把"按 48 kHz 生成"理解成"按 native 生成再插值到 48 kHz"，而那个插值器没有滤波。

**播放为什么干净**：`PlayerA` 让核按 native 跑，然后用**它自己的、带滤波的重采样器**转换到 host。

## 五、解法：这一颗芯片走 native + 我们的 FIR

```cpp
// **The OPM keeps its own rate, and the FIR does the conversion.**
//
// `nukedopm` resamples internally when it is asked for a rate other than `clock / 64`: it keeps a
// fractional `rateratio` and interpolates linearly between its own samples, with no filter. Asking
// it for 48 kHz therefore folds the top band in *before* we ever see the samples...
const double wantedRate = (chosen->model == chips::ChipModel::ym2151) ? 0.0 : shape.rate;
```

**`0` 的含义是"用芯片自己的速率"**，于是：

```
核（native 55930，生成真实样本）→ Blackman FIR（26 kHz 衰减到 0.000）→ 抽取到 48 kHz
```

**这是本仓库里唯一一条有滤波器参与的导出路径**，而它恰好是唯一需要滤波器的那条。

## 六、教训

### 1. “同一族”不等于“同一条路径”，而且这个错犯过两次

| 型号 | 核 | 对"跑文件速率"的反应 |
|---|---|---|
| YM2203 / YM2608 | `fmopn` | 直接按请求速率生成 ✓ |
| YM2610 | MAME | 直接按请求速率生成 ✓ |
| YM2612 | `ym3438` | 直接按请求速率生成 ✓ |
| **YM2151** | **`nukedopm`** | **按 native 生成再线性插值 ✗** |

**上一轮 YM2610 已经教过一次"这两颗芯片连核都不共用"**，当时的结论是"位深要各自测"。**这一次是同一条规律在另一个维度上的重演：不只是位深，连"速率怎么处理"都要各自确认。**

### 2. 修好一个地方，可能把另一个地方暴露出来

**位深修好之后音量正常了**，滋滋反而更明显——因为信号从四分之一的满幅回到满幅，**那层噪声跟着放大了**。

**如果我在位深之后就收工，报"音量已修复"，这个滋滋会留到下一次。**

### 3. “播放干净、导出不干净”第二次成为唯一的线索

**上一轮的结论是"我们多了一步重采样"，所以这一轮的第一个假设也是那个方向——结果相反。**

**判据要更细**：不是"有没有重采样"，而是**"谁在做、做得好不好"**。核自己做、且不带滤波，比我们自己做一个带滤波的更糟。

### 4. 两个症状可以是一个原因

**用户报的是"音量小 + 有滋滋"。** 我一开始分两条线查（位深 / 重采样），**实际是同一个滤波器缺失**：它既折叠高频（滋滋），也削掉高频（听感变小）。

**下次遇到"又小又有噪声"，先找那个同时解释两者的机制。**

## 七、当前状态

`fmOutputBits` 的实测表（四个型号，只有 YM2203/2608 是 16）：

| 型号 | 位宽 | 依据 |
|---|---|---|
| YM2612 | 14 | 实测峰值 |
| YM2203 / YM2608 | 16 | 实测峰值 |
| YM2610 | 15 | 听感（MAME 核测不到） |
| **YM2151** | **14** | 实测峰值 |

**导出速率策略，按核分：**

| 核 | 速率 | 转换 |
|---|---|---|
| `fmopn` / `ym3438` / MAME | 文件速率 | 无 |
| `nukedopm` | native | 我们的 FIR |

**还有一颗待接：OPL 家族。** `nukedopl*` 与 `nukedopm` 出自同一系列，**几乎肯定也有那个线性插值器**——接它的时候，这一份文档就是答案。
