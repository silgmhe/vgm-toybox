# libvgm 的逐通道可视化：管线已经在，缺的是最后一层

> 调研对象：`plugin/Source/backends/libvgm`、`plugin/Source/chips`（30 个解码器）、`plugin/Source/midi`（MIDI 输出的那套）、`plugin/Source/playback`（快照机制）。
> 目的：`feature/libvgm-visualisation` 开工前，先确认哪些能拿、哪些拿不到、哪些要走新路。
> 结论先行：**VGM 命令流、30 个寄存器解码器、波形合成器三样都在，而且是同一条已经跑了两个功能的管线（MIDI 输出、DPCM 采样）。缺的只有一层——把 `chips::ChannelState` 翻译成 `playback::ChannelState`——以及一个跟随播放位置的寄存器游标。** 不需要重写任何芯片模型，也不需要改 libvgm 源码。

## 0. 一句话

```
  VGM bytes
      │  VgmSongLoader::load()                    ← 现成（MIDI 在用）
      ▼
   SongIr  { chips, writes, romBlocks }           ← 现成（格式中立）
      │  按时间游标喂写成                         ← 缺 ①（新写，约一屏）
      ▼
   IChipDecoder × 30                              ← 现成（MIDI 在用）
      │  channelState(i)
      ▼
   chips::ChannelState { gate, period, hz, patch }← 现成，但**没有波形**
      │  类型转换                                 ← 缺 ②（每个芯片一次映射）
      ▼
   playback::ChannelState { waveKind, duty, wave }← 键盘页已在等
      │  waveform::cycleFor()
      ▼
   键盘页 / 示波器 / 导出
```

**两个"缺"都不是新架构，是这一层本来就没写。**

## 1. 现有的管线，一条一条点出来

### 1.1 `VgmSongLoader`：VGM → `SongIr`

```cpp
// formats/vgm/VgmSongLoader.h:24
bool load (const void* data, std::size_t size, midi::SongIr& out, std::string& error) override;
```

`SongIr` 的注释写着它是"which chips, which register writes, which memory"（`midi/SongIr.h:106`），并且**`deviceId` 只是 `chips` 的下标，从未被解释**（`chips/ChipTypes.h:76`）。

**这正是可视化要的形状**：一个与格式无关的寄存器写列表，带绝对时间。

### 1.2 `VgmCommandReader`：命令流 → `(寄存器写, 时刻)`

```cpp
// backends/libvgm/VgmCommandReader.h:21
struct VgmCommand
{
    Kind kind;          // registerWrite / registerWritePair / wait / end / ignored
    uint8_t command, port, addr, data, data2;
    uint32_t sampleTime;   // 44100 Hz 的绝对位置
};
```

它已经处理掉了 VGM 的所有格式细节：命令长度表、`0x8n` 一个字节代表两次事（写 YM2612 DAC 寄存器 + 等 n 个采样）、`0x67` 数据块、K051649 的双字节写、C140 的 banking。

**这段代码是从"命令长度表凭记忆写过三次、第三次静默失败"里长出来的结论**（`docs/review/2026-10-31-pcm-export-review.md` 的 H-2），已经有断言守着。

### 1.3 30 个解码器：寄存器 → 通道状态

`chips/IChipDecoder.h:69`：

```cpp
/** The channel's current state; index out of range returns a default (present = false). */
virtual ChannelState channelState (int channel) const = 0;
```

覆盖范围（按 `MidiSequenceBuilder.cpp` 的 flush 顺序）：NES、GB、VRC6、VRC7、FDS、MMC5、N163、SAA1099、POKEY、WonderSwan、SN76489、AY8910、RF5C68、K007232、VSU、K005289、MSM5232、MultiPCM、Mikey、OKIM6295、X1-010、C140、C352、K053260、K054539、QSound、GA20、SegaPCM、ES5503、YMF271、YMZ280B、OPM、HuC6280、K051649、OPN、OPL。

**这些是逐芯片的寄存器语义，是整个项目最贵的那部分。** 它们已经能回答"这个通道现在是不是在响、多高、多响"。

### 1.4 波形合成器：UI 侧已经有

```cpp
// playback/WaveShape.h:37
std::vector<float> cycleFor (int waveKind, float duty, int shapeParameter = 0);
```

八种形状：`none / stored / pulse / triangle / noise / delta / saw / fm`。

**含义**：UI 不需要每个芯片一条波形，只需要 `(waveKind, duty, shapeParameter)` 三个数。`stored` 是唯一需要真数组的一类。

### 1.5 快照机制：引擎侧已经有

```cpp
// playback/PlaybackEngine.cpp:1484
if (stateCaptureWanted.load (std::memory_order_relaxed))
{
    std::unique_lock<std::mutex> guard (stateMutex, std::try_to_lock);

    if (guard.owns_lock () && ! trackPlayers.empty () && trackPlayers[0] != nullptr)
    {
        trackPlayers[0]->captureState (stateSnapshot);
    }
}
```

三件事已经定了，**新代码不用再决定**：

1. **只在 UI 要的时候捕获**（`stateCaptureWanted` 是个原子开关）——没打开键盘页时不花 CPU；
2. **`try_lock`**：抢不到就丢一帧，不阻塞音频线程；
3. **只问 `trackPlayers[0]`**：不会因为分轨隔离而问 N 个播放器。

## 2. 缺的第一样：两个 `ChannelState` 不是同一个东西

这是这次调研**最重要的一条**，也是名字最容易骗人的地方。

```cpp
// chips/ChipTypes.h:116  —— 给 MIDI 用的
struct ChannelState
{
    bool present, gate, released;
    uint32_t period; int block;      // FM 的 f-number 与八度
    int volume;                      // 0..15，包络已算过
    int patch;                       // duty / waveform 编号 / drum id
    int16_t finePitch;
    double hz;
    int transposeCents, rootCents;
    PitchSource pitchSource;
    uint32_t keyOns;
};

// playback/ChipState.h:21  —— 给键盘页用的
struct ChannelState
{
    std::string name, chipName;
    bool active, analysing;
    int note; float frequency, sampleRate, volume;   // volume 是 0..1
    std::vector<float> wave;                          // 一个周期，-1..1
    int waveKind; float duty; int shapeParameter;
    int trackIndex;
};
```

**名字一样，没有一个字段同名同义。** 前者是"音高与门控"（MIDI 需要的），后者是"能画出来的东西"（UI 需要的）。

### 2.1 字段对照

| `playback::ChannelState` 要的 | 从哪来 | 难度 |
|---|---|---|
| `name`、`chipName` | `SongIr::chips` + `ChannelKind` 的枚举名 | 照抄一张表 |
| `active` | `chips::ChannelState::gate`（`released` 也要算进去） | 一行 |
| `frequency` | `hz`（或 `period` 经 `noteTable()` 换算） | 一行 |
| `note` | `noteForFrequency(hz)`（`ChipState.h:87` 已有） | 一行 |
| `volume` | `volume / 15.0f` | 一行 |
| **`waveKind`** | **`ChannelKind` 40 个值 → 8 种形状** | **一张表** |
| **`duty`** | **`patch`**（脉冲类的 patch 就是 duty） | **每芯片一行** |
| **`shapeParameter`** | **噪声的 period、DPCM 的样本身份** | **每芯片一行** |
| **`wave`** | **只有 `stored` 类需要** | **见 §3** |
| `sampleRate` | 采样类芯片的播放速率 | 每芯片一行 |
| `trackIndex` | 隔离模式下按设备分轨 | 待定（见 §4） |

**除了 `waveKind` 和 `wave`，其余都是算术。** 这就是为什么 §0 说"缺一层"而不是"缺一套"。

### 2.2 `ChannelKind` → `waveKind` 的映射是自然的

`chips::ChannelKind`（`ChipTypes.h:20`）有 40 个值，但**它们本来就在描述形状**：

| 形状 | `ChannelKind` 里的例子 | `playback::waveform::Kind` |
|---|---|---|
| 脉冲 | `nesPulse`、`gbPulse`、`aySquare`、`sn76489Square`、`saaSquare`、`pokeySquare` | `pulse = 2` |
| 三角 | `nesTriangle` | `triangle = 3` |
| 噪声 | `nesNoise`、`gbNoise`、`ayNoise`、`sn76489Noise`、`wsNoise` | `noise = 4` |
| delta | `nesDmc` | `delta = 5` |
| 锯齿 | `vrc6Saw` | `saw = 6` |
| FM | `fm`、`fmDrum`、`vrc7Fm`、`opnAdpcmB` | `fm = 7` |
| **存储表** | `fdsWave`、`n163Wave`、`c6280Wave`、`sccWave`、`x1Wave`、`gbWave`、`wavetableWave`、`rf5cPcm`… | **`stored = 1`** |
| 采样 | `c140Pcm`、`c352Pcm`、`qsoundPcm`、`okimAdpcm`、`segaPcm`、`ga20Pcm`、`k053260Pcm`、`k054539Pcm` | `stored = 1`（波形来自样本字节） |

**"存储表"和"采样"这两组是唯一需要真波形数据的**，其余六种由 `cycleFor()` 合成。这个划分决定了 §3 的工作量。

## 3. 缺的第二样：哪些芯片真的需要波形数据

`cycleFor()` 能合成六种，`stored` 那种要后端交数组。**交得出来吗？** 逐个看了 `channelState()` 的实现，结论分两类：

### 3.1 交得出来（寄存器里就是数据）

| 芯片 | 依据 |
|---|---|
| **FDS** | NSF 侧已有先例：`NsfPlayer.cpp:1236-1241` 直接读 `info->wave[0..63]` |
| **HuC6280、SCC、X1-010、WonderSwan、VSU** | 都是 32 步（或 16/128 字节）RAM 波表，解码器为了算音高**必然已经读了波形寄存器** |
| **采样类**（C140/C352/QSound/OKIM6295/SegaPCM/GA20/MultiPCM/RF5C68/ES5503/YMF271/YMZ280B） | 数据在 `ChipMemory`（`IChipDecoder.h:35`）里，`attachMemory()` 已经交过一次 |

**注意最后一行**：`attachMemory()` 只在 `flushDecoded()` 时调过一次，**字节的生命周期是"一次 flush"**（注释原文：`bytes` is borrowed）。可视化要长期持有样本，**得改成复制或者重新组织**。这是一个真问题，见 §5。

### 3.2 交不出来，只能画个记号

**OPN/OPM/OPL 的 FM**：`channelState()` 给的是 `period` + `block`，波形由四个算子参数决定，而**这四个数不在 `chips::ChannelState` 里**。

NSF 的 VRC7 已经遇到过并解决了（`NsfPlayer.cpp:1262`）：从模拟器问 `GetFmParameters(multiple, level, waveSelect, feedback)`，然后 `packFm()` 打进 `shapeParameter`。

**libvgm 这边问不到**（解码器不存算子参数），所以有三条路：

| 路 | 做法 | 代价 |
|---|---|---|
| **A** | 给 FM 解码器加四个字段，`channelState()` 填上 | 改 3 个解码器（Opn/Opm/Opl），**它们的寄存器写里本来就有这些数**，只是没留 |
| **B** | 画一条固定正弦 + 一个 "FM" 标记 | 便宜，但对可视化价值打折——FM 是这批芯片的主体 |
| **C** | 用 `patch` 当形状索引，画个示意 | 和 B 差不多，且更含糊 |

**我倾向 A**：算子参数在寄存器里，解码器解析它们算音高时**本来就读过**，只是没存。加四个 int 的成本远小于画一条假波形。

## 4. 缺的第三样：寄存器游标与播放位置怎么对齐

MIDI 输出是**一次性把整首歌解析完**（`buildMidiSequence` 是纯函数，返回一个 `MidiSequence`）。可视化不能这么做：

- **静态预计算**：每秒 60 帧 × 30 通道 × 每帧一个 `ChannelState`（含 vector）≈ 每秒数百 KB，一首 3 分钟的歌几十 MB；
- **而且 seek 之后要重建**，等于把整首歌再解析一遍。

**所以只能增量**：每渲染一个 block，把 `[上一帧位置, 当前帧位置)` 之间的寄存器写喂给解码器，然后取 `channelState()`。

### 4.1 天然对得上的三处

```cpp
// LibvgmTrackPlayer::render() 之后，引擎会调 captureState()
// playback/PlaybackEngine.cpp:1484-1494
```

1. **位置**：`positionSeconds()` 已经是 libvgm 的权威答案，不用自己算；
2. **线程**：`captureState` 在音频线程、`render` 之后、锁内——**和 `VgmCommandReader` 的游标同一线程，不需要锁**；
3. **只在需要时**：`stateCaptureWanted` 关掉时，游标不推进也无所谓（下一帧重新对齐即可）。

### 4.2 三个必须处理的时刻

| 时刻 | 现在 | 要做什么 |
|---|---|---|
| **loop** | `handleEvent` 只理 `PLREVT_END`（`LibvgmTrackPlayer.cpp:399`） | **接 `PLREVT_LOOP`，游标回到 loop offset、解码器 `reset()`** |
| **seek** | `seekSeconds()` 直接跳 | 游标从头（或最近的 loop 点）重放，**解码器 reset** |
| **首帧 / 从暂停恢复** | — | 游标追到当前位置；追不上就空着（快照 `valid = false`，面板画空白）**不要显示错误的旧状态** |

**最后一行是这一支最容易出的 bug**：解码器残留着上一首歌的寄存器值，面板就会画出一个"看起来很像"的假状态。NSF 那边靠 `reset()` 保证，这边必须一样。

### 4.3 多 track 实例怎么办

`trackPlayers[0]` 已经被引擎选定（§1.5）。但分轨隔离模式下：

- **`trackPlayers[0]` 可能是任意一条被隔离的轨道**，它的静音配置会让部分设备 `disable`（注释在 `LibvgmTrackPlayer.h:24-26`：`a suspended device is not emulated at all`）；
- **而在我们这条路上**，静音是在 libvgm **内部**做的，**不影响 `VgmCommandReader` 的解析**——因为它读的是文件，不是播放器。

**所以游标和这个播放器无关**，它只问"现在播放到第几秒"。**分轨隔离不影响可视化**，这是这条路比"hook libvgm 渲染"更好的地方之一。

**但它带来一个反直觉的结论**：可视化看到的通道状态，**是文件的全部通道**，不是"当前 track 的"。要做"只显示这条轨"需要另外按 `trackIndex` 过滤，而这需要 `SongIr::chips` 与引擎 track 列表的对应关系——**这一条我没查清，见 §6 的问题 3**。

## 5. 复用 MIDI 逻辑：边界在哪

你问的这条，答案分三档。

### 5.1 直接复用（一行不用改）

| 东西 | 为什么 |
|---|---|
| `VgmSongLoader::load()` | 输入是文件字节，输出是 `SongIr`，与播放无关 |
| `VgmCommandReader` | 同上；MIDI 用它、DPCM 采集也用它，第三处用不会有问题 |
| `IChipDecoder` 全部 30 个 | `reset()` / `write()` / `advance()` / `channelState()` 是纯寄存器模型，**不知道 MIDI 存在** |
| `NoteTable`（每个解码器的音高表） | 把 `period` 换成 Hz |
| `playback::waveform::cycleFor()` | 纯形状合成 |
| `noteForFrequency()` | `ChipState.h:87` |
| 快照机制（`stateCaptureWanted`、`try_lock`、`trackPlayers[0]`） | 引擎已有 |

### 5.2 复用一半（结构与代码可借，驱动方式不同）

| 东西 | 相同 | 不同 |
|---|---|---|
| **喂写的方式** | `reset()` → 逐条 `write()` + `advance()` | MIDI 是**解析完整首歌**；可视化是**每帧喂一小段，游标常驻** |
| **`flushDecoded()` 的模式** | 都是"配好 clock → 灌 writes → 读状态" | 它在 `MidiSequenceBuilder.cpp` 的私有 `Builder` 里，**是函数不是类型**，要提出一个可复用的驱动 |
| **`chips::ChannelState` → 输出的翻译** | 都要跨过这个类型 | MIDI 走 `NoteExtractor::extractPerformance`（音符事件）；可视化走"当前状态"（每帧一份） |

**`flushDecoded` 值得单独看一眼**：它已经解决了"给一个解码器配 clock、喂它自己的那些设备写、处理双端口和多实例"的问题。可视化要的是同一个东西**减去**"合并成音符"那一段。**把它提成一个可复用的驱动类，是这一支最划算的复用点。**

### 5.3 不能复用

| 东西 | 为什么 |
|---|---|
| `buildMidiSequence()` | 一次解析全曲、产物是文件，与实时播放无关 |
| `NoteExtractor` | 它把状态序列**压缩成音符起止**，而可视化要的是**每一帧的状态**——正好是它丢掉的那部分 |
| `MidiSequence` / `NoteStream` | 同上 |
| libvgm 的事件回调 | 只有 `PLREVT_END` / `LOOP` / `SEEK` / `RATE_CHANGE` 这类宏观事件（`LibvgmTrackPlayer.cpp:397`），**没有逐寄存器写**。要 hook 就得改 `libs/` 下的第三方源码，**不值得**——`VgmCommandReader` 已经把同一件事做得更安全 |

**最后一行是这个调研最重要的判断**：**不要 hook libvgm 的渲染路径**。文件里写着每一次寄存器写，读文件比读播放器内部状态更可靠，而且和已经跑通的两个功能同源。

## 6. 工作量与风险

| 项 | 规模 | 风险 |
|---|---|---|
| `chips::ChannelState` → `playback::ChannelState` 的转换 | **一个新文件**，一张 `ChannelKind → waveKind` 表，每芯片 1-3 行 | 低。纯函数，可测 |
| 寄存器游标（含 loop/seek 重置） | **一个新类型**，约 100 行 | **中**。`PLREVT_LOOP` 要接；seek 的语义要和引擎对齐 |
| 把 `flushDecoded` 提成可复用驱动 | 重构 `MidiSequenceBuilder.cpp` 的一部分 | **中**。`Builder` 是私有的，动它要保证 MIDI 输出 46/46 不变 |
| FM 的算子参数 | 改 3 个解码器，各加 4 个字段 | 低，但要逐芯片核对寄存器含义 |
| `stored` 类的波形数据 | **视芯片而定**，采样类还要解决 `ChipMemory` 是借用的问题 | **高**，这是唯一可能失控的一项 |
| 名称与芯片名 | 一张表 | 低 |

**如果只做前两项**（转换 + 游标），**就能让大部分芯片的通道行亮起来**——脉冲、三角、噪声、锯齿、FM 用合成形状，采样类用 `delta` 画个占位。**这是我认为应该先做的范围**：它让键盘页不再是空的，而"每个采样芯片的真实波形"可以分批补。

## 7. 待确认的问题

**① 范围：先做"能亮起来"，还是先做"波形都对"？**

前者约是后者的三分之一，且能让后面的工作有可见反馈。**我建议前者**，把 `stored` 类逐芯片往后排。

**② FM 要不要改解码器加算子参数？**（§3.2 的方案 A）

改了 FM 才有真的形状；不改就是一条正弦。**看你更在意"快"还是"像"。**

**③ "只显示某条轨"要不要做？**

**已查清，这一条不是问题。** 写本文时以为 track 与通道的对应关系是模糊的，实际不是：

```cpp
// playback/SongBackend.h:77
struct TrackInfo
{
    DeviceId deviceId;
    int deviceIndex = 0;      // devices are numbered in song order
    int channelIndex = -1;    // -1 = the whole device (it cannot be isolated any further)
    ...
};
```

libvgm 后端**已经是"一个通道一条 track"**（`LibvgmSongInstance.cpp:419-431`），联动芯片的通道号接在父芯片之后（同文件 L471）。**所以它和 `IChipDecoder::channelState(i)` 是同一套编号。**

而且 `playback::ChannelState::trackIndex` 的用途不是过滤显示，是**让这一行可以 Mute/Solo**（`ChipState.h:70` 的注释：`A panel needs this to offer mute and solo`）。

**结论：不需要决策。填上 `trackIndex`，键盘页照常显示全部通道，与 NSF 一致；Mixer 静音某条轨时，键盘页那一行跟着暗。**

**同一件事的另一半**：分轨隔离不会让可视化只看到一条轨。静音是 libvgm 在**播放**时做的，而这条读的是**文件**（§4.3）。

**④ 示波器和键盘页用同一份快照吗？**

NSF 那边 `captureState` 同时喂了两个面板（`includeWaves` 控制是否算波形）。libvgm 这边波形要按 §3 现算，**每帧算 30 个通道的 256 点可能就不便宜了**。

**要不要给"波形"单独一个开关**，只在键盘页可见时才算？

**⑤ 一首歌里同一颗芯片有两片（比如两个 YM2612），通道名怎么带？**

`2A03` 与 `2A03 #2`？还是 `YM2612 (1) - FM 3`？**这决定了 `chipName` 的格式**，而它也会出现在拖拽导出的文件名里（`NES APU_Pulse 1_...wav`）。

---

**这份文档的结论是"可以开工"，不是"工作量很小"。** 前两项（转换 + 游标）是确定的，第三项（波形数据）取决于你选哪个范围。
