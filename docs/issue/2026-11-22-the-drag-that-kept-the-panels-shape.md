# 拖出去的是面板的形状

> 2026-11-22。`04 Stage 1.vgz` 的 FM6，拖拽导出的音色变成阶梯状，像八位机。
>
> 一句话：**拖拽保留着面板的 8 kHz 波形，因为"重渲染"这一步静默地失败了，而失败与"这一行没有可渲染的音色"用了同一个返回值。**

## 一、两个文件放在一起就说不清了

用户给了两份 wav。**头完全一样**：

| | 旧（对） | 新（阶梯） |
|---|---|---|
| 采样率 | 48000 | 48000 |
| 声道 / 位深 | 1 / 32f | 1 / 32f |
| 数据字节 | 313152 | 313152 |
| 时长 | 1.63 s | 1.63 s |

**连文件大小都一样。** 所以"采样率写错了"这个最自然的假设**当场被排除**——如果头错了，两者不可能一样。

必须比内容：

```
     旧          新
  0.00917     0.00917      ← 相同
  0.12538     0.00917      ← 相同（重复）
  0.90234     0.00917
  0.99451     0.00917
  0.98132     0.00917
  0.99719     0.00917
  0.81161     0.73825      ← 换值
  ...
```

**每 6 个样本重复一次。** `48000 / 8000 = 6`。

**这就是答案：8 kHz 的样本被零阶保持到 48 kHz。** `WaveExport` 的 `heldAt (values, source, target)` 做的正是这件事——它不插值（那条注释解释了为什么：插值会磨掉八位转换器的棱角），所以源速率越低，台阶越宽。

**用户听到的"撕裂"就是这级台阶。**

## 二、为什么是回归：三层，每层都合理

**`00d481a6` 时音色是对的**，因为那时的链条不同：

| 时间 | 面板绘制 | 拖拽导出 | 结果 |
|---|---|---|---|
| `00d481a6` | `noteRateFor`（55 556） | **面板的波形** | 拖出去就是高精度的那份 ✓ |
| 卡顿修复后 | **`kVisualNoteRate`（8 000）** | **面板的波形** | 拖出去是 8 kHz ✗ |
| 我修拖拽（第一次） | 8 000 | 尝试重渲染 | **重渲染失败，退回面板** ✗ |
| 最终 | 8 000 | 重渲染成功 | ✓ |

**所以回归的第一个必要条件是"面板降速率"**——在那之前，面板那份的精度够高，拖拽"直接用面板"是对的。**降速率之后，同一个做法就变成了缺陷。**

**这是那次卡顿修复的副作用里唯一没有同步考虑的一条：它改变了"面板波形"的精度含义，而拖拽依赖了那个含义。**

## 三、根因：一个返回值有两个相反的含义

修拖拽时我加了重渲染：

```cpp
if (auto rendered = engine.renderNoteAt (state.trackIndex, hz,
                                         state.voicePatch.get ());   // ← 当时没有第三个参数
    rendered.wave != nullptr)
{
    state.noteWaveform = std::move (rendered.wave);
    state.noteWaveformRate = (float) rendered.rate;
}
```

`renderNoteAt` 当时是这样找音色的：

```cpp
for (const auto& entry : noteWaves)
{
    if (entry.second.patch != nullptr && trackFor (entry.first) == track)
    {
        patch = entry.second.patch.get ();
        break;
    }
}

if (patch == nullptr) { return {}; }
```

**它用 `noteWaves` 反查，而 `ContentSlot` 手上已经有 `state.voicePatch`。** 两套编号（chip channel 对 mixer row）必须在这里对上，**对不上就返回空**。

而空在调用者眼里有**两个含义**：

| 含义 | 正确反应 |
|---|---|
| 这一行没有可计算的音色（采样行、波表行） | **保留原波形** ✓ |
| backend 找不到（这是 bug） | 也应该保留，**但应该说出来** |

**两种走到同一个分支，于是失败是静默的。** 拖拽沿用面板的 8 kHz 波形，一路到 wav。

**这和 `peak = 0` 那次是同一个病**：一个值同时表示"正常"和"异常"，而区分它们需要第二维信息（那次是 `gate`，这次是"有没有传 patch"）。

## 四、修法：把手上已有的东西传进去

```cpp
virtual chips::RenderedNote renderNoteAt (int track, double hz,
                                          const chips::VoicePatch* patch = nullptr) const;
```

**调用者持有 `ChannelState`，里面就有 `voicePatch`。** 传进来，backend 不再反查。旧的查找保留给"只有一个 track 号"的调用者。

**两条导出路径都改成传 `voicePatch.get ()`。**

**参数注释里写了这一句，因为它是这个 bug 的全部教训**：

> an empty result has two meanings and only one of them is silence

## 五、全 FM 芯片检查

按"会不会出现同一个症状"逐个过。

| 芯片 | `capturePatch` | `noteRateFor` | `renderNoteAt` 传 patch | 有风险吗 |
|---|---|---|---|---|
| **OPN 家族**（`ym2203`/`ym2612`/`ym2608`/`ym2610`） | ✅ | ✅ 4 个分开 | ✅ 通用两处 | **无** |
| **OPM**（`ym2151`） | ✅ 25 条 | ✅ 55 930 | ✅ | **无** |
| **VRC7**（NSF） | ✅ 自有路径 | 用 `Vrc7VoiceDriver::rate()`，`clock/72` | ✅ `NsfPlayer` 已接受 | **无** |
| **OPL 家族**（6 个 `ChipKind`） | ❌ | ✅ `ym3812` 49 716 | 不适用（无 patch） | **无**，见下 |
| **OPX**（`ymf271`） | ❌ | ❌ **缺** | 不适用 | **无**，见下 |

**OPL 与 OPX 为什么不受影响**：它们没有 `capturePatch`，所以 `state.voicePatch` 恒空，`renderNoteAt` 根本不会被调用——它们走的是单周期 `wave` + `sampleRate` 那条路，**与"整段包络的采样率"是两套东西**。

**但它们有两处欠账，记在这里免得下次当成新问题**：

1. **`noteRateFor (ymf271)` 缺失**，落到 `return 7670453/144`（OPN 的数字）。今天无害（没人调），**一旦 OPX 有了 `capturePatch`，这个默认值会让它按 OPN 的速率渲染**，而那是错的。
2. **`fmOutputBits (ymf271)` 也落到默认 16**，同样是无害但错的默认。

**这两处的共同形状**：`switch` 的 `default` 给了一个"看起来合理"的兜底。**兜底值是给 `unknown` 用的，不是给"新芯片还没填"用的。**

## 六、还有一处既有行为，不是回归

**`ym2612` 的 channel 5 是 DAC 行**，而 `OpnDecoder::capturePatch (5)` 会返回一个 FM patch（`5 < layout.fm = 6`）。

**所以拖拽 DAC 行会渲染成 FM 音色，而不是 DAC 的采样形状。** 这一条在 `00d481a6` 时就存在，**不是这次引入的**，但它和刚修的这个 bug 是同一类：**一个行的 `voicePatch` 非空，不代表"用 FM 方式渲染它是对的"。**

**没有改**，因为需要先确认 DAC 行拖出去应该是什么（采样？还是 FM 音色？），那是产品决定。

## 七、留给下一次

**三次同形状的问题，都在这份文档能覆盖的范围内：**

| 事件 | 一个值，两个含义 |
|---|---|
| 键盘面板空白 | `snapshot.channels.size () == 0`：没加载 / 采集没到 |
| `peak = 0` | 没音符 / 有音符但核心没出声 |
| 拖拽的阶梯 | `renderNoteAt` 返回空：没有音色 / 没找到音色 |

**三次都不是算术错，是"正常"与"异常"共用一个返回值。**

**所以判据只有一条：**

> **一个新加的"失败即回退"分支，必须能说出它回退的是哪一种失败。** 说不出来，就会像这次一样——静默地退回一个曾经正确、现在错误的旧值。
