# 可视化音高与导出音高的分离

> 2026-11-22。回应"可视化固定音高、导出按实际播放音高，用一个 toggle 控制"。
>
> 结论：**方向对，而且可视化那半已经实现了**。但导出有三条路径，其中**拖拽导出仍然是面板的 8 kHz**——这是上一轮没查到的。

## 一、三条路径的现状（实测）

| 路径 | 采样率 | 音高 | 音色是否对 |
|---|---|---|---|
| **面板绘制**（`captureState`） | 8 kHz（`kVisualNoteRate`） | **固定 C4**（`getFmFixedNote`，`defaultFixedNote = 60`） | 形状对，音高不对——**这是想要的** |
| **`Export snapshot`**（批量） | **芯片速率**（上一步刚修） | `fixedNote` 或 `channel.frequency` | ✅ |
| **拖拽导出**（`mouseDrag`） | **8 kHz** ❌ | 面板那份 | ❌ |

**拖拽那一条**：`ContentSlot` 调 `writeChannelShape (state, ...)`，`state` 就是面板的 `ChannelState`——`noteWaveform` 是 8 kHz，`noteWaveformRate` 也是 8 kHz。**和面板一模一样。**

## 二、你的判断为什么成立

**可视化的音高是常量，所以缓存不会因音高失效。** 这一点可以直接从代码确认：

```1015:1025:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
const double hz = playback::noteWave::frequencyOf (store.getFmFixedNote ());
...
if (entry.wave == nullptr || entry.patch != state.voicePatch
    || entry.hz != hz || entry.sustain != sustain || entry.release != release
    || entry.normalise != normalise)
```

**`hz`、`sustain`、`release`、`normalise` 都是设置项，不变就不重建。** 所以面板的重建只由 `state.voicePatch` 驱动——而 OPM 的 TL 已经不再递增版本，**音色真的变了才重建**。

**这就是上一轮卡顿修掉之后的开销水平：一个通道一次重建，之后一直复用。**

**而如果按实际音高渲染，缓存键里的 `hz` 会随音符变化——每换一个音就重建一次，在音频线程上。** 你说的"无法做高精度的频繁更改音高的渲染"正是这个。

## 三、新 toggle 的设计

```cpp
// SettingsDefaults.h, namespace fm
/** **Whether an export renders at the pitch the row is actually playing.**

    On by default: a chip's timbre depends on the pitch, because key scaling sets the envelope's
    rate from the note - so a C4 render of an A2 note is not that note's sound. Off gives every
    export the panel's fixed note, which is what you want when comparing patches across a song.

    This does not affect the panel, which always uses the fixed note: its job is to show a shape,
    and a shape that gets re-rendered on every note change costs the audio thread.
*/
constexpr bool exportAtPlayedPitch = true;
```

**读口：`SettingsStore::getFmExportAtPlayedPitch()` / `set...`。**

**UI：`SettingsOverlay` 的 FM 组，`noteSlider` 旁边一个 toggle。**

**语义清楚：它只影响导出，不影响面板。**

## 四、拖拽导出：一个必须决定的取舍

**拖拽是同步的**——`mouseDrag` 里写文件，然后把路径交给 `performExternalDragDropOfFiles`。**用户松手前必须写完**，没有后台的余地。

**三条路，各有代价：**

| | 做法 | 代价 |
|---|---|---|
| **A** | 拖拽时同步重渲染（芯片速率，8 个通道只渲一个） | 一个通道 89 489 样本 ≈ 几十毫秒。**拖动会顿一下** |
| **B** | 保持 8 kHz（现状） | **同一个文件，拖出来和 File > Export 出来的采样率不同** |
| **C** | 面板侧维护两份缓存：8 kHz 给画，芯片速率给拖 | 内存翻倍（每通道 358 KB × 2），且"什么时候渲染高质量那份"没有好答案 |

**A 其实没有想象中贵**：拖拽一次只渲**一个**通道，不是八个。89 489 样本在 3.5 GHz 上跑 `nukedopm` 大约几十毫秒——**用户拖动的动作本身就有几百毫秒，这一下可能感觉不到**。

**我倾向 A**，理由：**同一个文件的两条导出路径给出不同质量，是那种"用户发现后会觉得被欺骗"的不一致**。而 B 省下的几十毫秒，换来的是永远说不清的一处例外。

**但这一条我拿不准手感**——需要你拖一次试试，或者我做一个 A 的版本让你感觉一下。

## 五、"snapshot" 这个名字已经开始说谎

`Export snapshot` 原本是"导出你现在看到的"。上一轮它开始**重渲染到芯片速率**，这一轮它又要**按实际音高**——**它已经只剩"哪些行"这一个快照语义了。**

两个选择：

| | 做法 |
|---|---|
| **A** | 保留名字，在设置项的描述里说清"快照决定哪些行，渲染决定多细、哪个音高" |
| **B** | 改名 `Export rows…`，把"快照"这个词让给真正字面意义的操作 |

**我倾向 A**——改名的成本比误解高，而那个按钮的位置和图标已经表达了"这些行"。

## 六、顺带发现的一处文档错位

`SettingsDefaults.h` 里 `normalise` 的注释和 `defaultFixedNote` 的注释粘在一起了：

```242:256:plugin/Source/settings/SettingsDefaults.h
/** **The note an export renders at, when the user asks for a fixed one.**
    Off by default: an export of the row that is playing should sound like that row...
*/
/** **Whether a rendered note is scaled to full scale.**
    Off by default, and that is a deliberate answer...
*/
constexpr bool normalise = true;
```

**第一段说"Off by default"，而 `normalise = true`；`defaultFixedNote` 的注释反而被它盖住了。** 顺手修。

## 七、建议的实施顺序

| 步 | 内容 | 验收 |
|---|---|---|
| **1** | `normalise` 的注释归位 | 无行为变化 |
| **2** | toggle + 读口 + 默认 `true` | 面板不变；`Export snapshot` 按实际音高 |
| **3** | 导出路径尊重 toggle：`true` 用 `channel.frequency`，`false` 用 `getFmFixedNote` | 同一行、两个设置，导出的音色可听出差别 |
| **4** | **拖拽**（需要你决定 A/B） | 决定之后再写 |
| **5** | 断言：导出文件的采样率与音高 | 需要先把"选目录"与"导出"拆开 |

**第 2、3 步互不依赖拖拽那条**——可以先做批量导出，拖拽留到最后。

## 八、要你决定的

1. **拖拽导出**：A（同步重渲染，几十毫秒）还是 B（保持 8 kHz）？
2. **`Export snapshot`**：保留名字（A）还是改成 `Export rows…`（B）？
3. **toggle 的位置**：`SettingsOverlay` 的 FM 组里，放 `noteSlider` 旁边可以吗？
