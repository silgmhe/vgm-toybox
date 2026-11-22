# FM 可视化的精度与导出保真：调研与方案

> 分支 `feature/fm-export-fidelity`，从 `feature/fm-visualisation` 的 `d402692` 切出。
>
> 起因：YM2151 跑通之后，为了消除音频线程卡顿把渲染降到 8 kHz，随后发现**导出速率也必须跟着降**。两件事被同一个常量绑在一起，而它们的要求其实相反。

## 一、三个要求，互相冲突

| 用途 | 精度要求 | 理由 |
|---|---|---|
| **面板绘制** | **低，固定 8 kHz** | 一行只有约 96 px 宽，1.6 秒的形状；再高的速率画不出更多信息 |
| **拖拽导出** | **必须与芯片一致** | 否则音色不同——高于 4 kHz 的混叠与真实 OPM 的亮度差得能听出来 |
| **波表导出** | 同上 | 单周期来自 `wave`，速率是 `sampleRate`，与本次改动无关 |

**现在的实现只满足第一条**：`OpnVoiceDriver::startCore` 固定用 `chips::kNoteRate`（8000），而面板和导出**共用同一个速率**。

**第二个要求还有一个附带问题：导出在 UI 线程同步跑。** 面板 8 kHz 时它只是"卡一下"，改成芯片速率后 8 个通道 × 89 489 样本会让界面明显停顿——而这段时间里 `captureState` 也在同一线程上，画面会冻住。

## 二、调研结论：架构已经准备好了，只差两个参数

**好消息是"面板与导出分开驱动"这件事已经存在，不需要新建机制。**

| 后端 | 面板实例 | 导出实例 | 现状 |
|---|---|---|---|
| VGM（OPN/OPM） | `voiceDriver` | `exportDriver`（`renderNoteAt` 里懒创建） | **已有两个实例，但速率相同** |
| NSF（VRC7） | `vrc7Driver` | `vrc7ExportDriver` | **已有两个实例** |

`LibvgmTrackPlayer::renderNoteAt` 的注释已经把理由写清楚了：

> **Its own core.** The driver `captureState` uses is being advanced by the audio thread; an export that re-rendered through it would rewrite the registers under that thread's feet.

**所以"两个驱动"这层不用设计，只需要让它们跑不同的速率。**

### 常量与使用面

`kNoteRate = 8000.0` 在 `VoicePatch.h:59`，**全项目只有 6 处引用**：

| 位置 | 用途 |
|---|---|
| `OpnVoiceDriver::startCore` | 启动核心（面板 + 导出共用） |
| `LibvgmTrackPlayer.cpp:1044` | 面板的 `noteWaveformRate` |
| `ContentSlot.cpp:1602` | 拖拽导出的 `noteWaveformRate` |
| `WaveExport.cpp:367` | 兜底速率（`noteWaveformRate` 为 0 时） |
| 两处注释 | — |

**面很小，改名是安全的。**

### 导出为什么在 UI 线程

`ContentSlot::beginExport` 的全部工作都在 `FileChooser::launchAsync` 的回调里：

```1543:1629:plugin/Source/ui/ContentSlot.cpp
waveChooser->launchAsync (..., [this, options] (const juce::FileChooser& chooser)
{
    ...
    for (auto& channel : drawn.channels)          // 逐通道 noteWave::render
    ...
    playback::exportWavetables (drawn, ...);      // 逐通道写 wav
    ...
    onExportReport (report);
});
```

**这是消息线程。** `noteWave::render` 是 CPU 密集的（核心仿真），`exportWavetables` 是磁盘 IO——两者都不该占着 UI。

## 三、方案

### 3.1 常量改名，语义写进名字

```cpp
// VoicePatch.h
/** **The rate a *picture* of a note is sampled at.** Low on purpose: the row is about ninety
    pixels wide ... Export does not use this - see `kExportNoteRate`.
*/
constexpr double kVisualNoteRate = 8000.0;

/** **The rate a note is sampled at when it is written to a file**: the chip's own, because the
    corners of a waveform are what its timbre is made of. */
constexpr double kExportNoteRateFallback = 8000.0;   // only when a patch names no chip
```

**改名不是洁癖**：这次出错的根源就是"`kNoteRate` 听起来像'音符的速率'，于是被两处都用上了"。

### 3.2 驱动接受速率

`OpnVoiceDriver::startCore (model)` → `startCore (model, rate)`，面板传 `kVisualNoteRate`，导出传 `chips::noteRateFor (model)`。

**`load` 里"型号变了要重建核心"的判断要跟着变**：现在是 `loadedModel != patch.model`，**还要比 `loadedRate != rate`**——否则先导出再画面板，核心还是高频的。

`Vrc7VoiceDriver` 同理（它已经有 `makeVrc7VoiceDriver(clock)`，加一个速率参数）。

### 3.3 速率随波形一起返回

`noteWaveformRate` 现在两处都写 `kNoteRate`，这是**对的**（面板），但导出需要芯片速率，而 `renderNoteAt` 只返回 `vector<float>`——**速率丢了**。

**修法**：`renderNoteAt` 返回一个带速率的结构，`ContentSlot` 一并写进 `channel.noteWaveformRate`。

```cpp
struct RenderedNote
{
    std::shared_ptr<const std::vector<float>> wave;
    double rate = 0.0;
};
```

**或者**：`renderNoteAt` 内部就把 `exportDriver->rate()` 记在 `channel` 上。前者更清楚——**速率和样本必须同时返回，否则又是一次"两处各写一份"**。

### 3.4 导出移到后台

`beginExport` 的渲染与写文件装进 `juce::ThreadPoolJob`，完成后 `MessageManager::callAsync` 回来报 `ExportReport`。

**三个前置条件，都已满足：**

| 条件 | 现状 |
|---|---|
| `drawn` 是快照副本 | ✅ `keyboardPanel->drawnSnapshot ()` |
| 渲染用独立驱动 | ✅ `exportDriver`（`mutable`，只在导出用） |
| 结果通过回调交回 | ✅ `onExportReport` |

**要加的是"导出中"标志**：`exportDriver` 是 `mutable` 的，连点两次导出会并发写同一个核心。一个 `std::atomic<bool> exporting` 挡住第二次即可，比加锁简单且不会让 UI 等。

**磁盘 IO 也在后台**：`exportWavetables` 逐通道写文件，放到后台线程才能让"导出 16 个文件"期间界面仍然响应。

## 四、"所有 FM 芯片"的实际范围

**这次改动能覆盖的，是已经有 `capturePatch` 的芯片**：

| 芯片 | `capturePatch` | 本次受益 |
|---|---|---|
| OPN 家族（`ym2203`/`ym2612`/`ym2608`/`ym2610`） | ✅ | ✅ |
| **OPM（`ym2151`）** | ✅（本分支刚加） | ✅ |
| VRC7（NSF） | ✅ | ✅ |
| **OPL 家族 6 个** | ❌ | **❌** |
| **OPX（`ymf271`）** | ❌ | **❌** |

**没有 `capturePatch` 的芯片，`noteWaveform` 恒空**，走的是"单周期"路径（`wave` + `sampleRate`），**与本次改动无关**——它们的精度问题在"能不能画整段"，不是"画多细"。

**所以"所有 FM 芯片"这句话，这次要读作"所有已经能画整段的 FM 芯片"**。OPL/OPX 的 `capturePatch` 是另一件工作（而且 OPL 家族一次覆盖六个 `ChipKind`）。

## 五、风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 改名漏掉一处 | 面板按 55 930 解释 8 000 的样本 | `kNoteRate` 全项目只有 6 处；改完 `grep` 一次 |
| `startCore` 不比较速率 | 先导出再画面板，面板仍用高频核心 | `loadedRate` 参与重建判断 |
| 导出并发 | 两次导出写同一个 `exportDriver` | `exporting` 标志 |
| 后台线程调 `engine` | `renderNoteAt` 是 `const`，但读 `noteWaves` 缓存 | `drawn` 是副本；`noteWaves` 只在渲染时写，导出路径不读它 |
| 89489 × 16 通道的内存 | 约 5.7 MB | 可接受；且只在导出期间 |

## 六、实施顺序

| 步 | 内容 | 验收 |
|---|---|---|
| **1** | `kNoteRate` → `kVisualNoteRate`，新增 `kExportNoteRateFallback` | 编译通过；`grep kNoteRate` 为空 |
| **2** | `startCore(model, rate)` + `loadedRate` 判断 | 面板仍是 8 kHz（`peak` 不变） |
| **3** | `renderNoteAt` 返回速率；导出驱动用芯片速率 | **拖出的文件时长 = sustain + release**，波形比面板细腻 |
| **4** | 导出移到后台 + `exporting` 标志 | 导出期间界面可响应；连点两次只有一次生效 |
| **5** | 断言 | 见下 |

### 断言（三条）

1. **`noteWave::render` 的样本数 = `(sustain + release) * driver.rate()`** —— 速率与长度同源。
2. **导出文件的采样率 = `noteRateFor(model)`** —— 直接读回 wav 头。
3. **面板的 `noteWaveformRate == kVisualNoteRate`** —— 两处（VGM 与 NSF）都要。

**第 2 条是这次真正缺的**：`54/54` 全绿的时候，导出速率是错的，因为**没有任何测试看过写出来的文件的头**。

## 七、不做的事

- **OSL/OPX 的 `capturePatch`**：不在本次。它们没有整段波形可导，先有形状再谈精度。
- **重采样到用户指定的目标速率**：`WaveExport` 已有 `targetRate`，保持"held, not resampled"的现有规则（那条注释解释了为什么插值会磨掉八位转换器的棱角）。
- **导出进度条**：后台化之后界面上是"没有反馈"，但这是下一步的 UI 工作，不是精度问题。
