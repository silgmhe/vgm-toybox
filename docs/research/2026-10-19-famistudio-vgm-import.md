# 调研：FamiStudio 的 VGM/VGZ 导入与 MIDI 导出

- 调研对象：`reference/FamiStudio`（本地源码，MIT，Copyright (c) 2019 BleuBleu）
- 主要位置：`FamiStudio/Source/IO/VgmFile.cs`（2296 行）、`IO/MidiFile.cs`（1455 行）、`IO/NsfFile.cs`、`Player/NesApu.cs`、`Project/Note.cs`、`Project/Channel.cs`
- 目的：为 VGMToybox 的 MIDI 输出找架构参考，而不是再补一颗芯片的解码

## 1. 结论

FamiStudio 的导入效果好，不是因为"寄存器翻译成 MIDI"更聪明，而是因为**先把寄存器重放成芯片状态模型，再从模型里读音符**：

```
VGM 命令流
  │ 逐帧推进（wait 累计采样，凑满一帧处理一次）
  ▼
芯片影子状态（寄存器 + 长度计数器 + 包络 + 扫频 + DMC 计数）
  │ 每帧查询，归一化为 ChannelState { period, volume, trigger, patch }
  ▼
音符判定（音高表查表 + 三态状态机 + 乐器识别）
  │ 写到工程行网格（一行 = 一帧）
  ▼
Project（音符级模型） ──► MIDI 导出（一条通道一条 MTrk）
```

关键：**MIDI 质量 = 模型质量**，导出器只做搬运（`MidiFile.cs:1470-1620`）。要借鉴的是前四层。

对照我们：`MidiSequenceBuilder.cpp` 把"读寄存器、判断发声、算音高、发 MIDI"压在同一层，每芯片一个 `case`，没有状态机、音高表、音量。于是除 SN76489、YM2610 外只能发 C4 one-shot——这正是"基本不能用"的根因。

## 2. 导入管线

入口 `VgmFile.Load`（`VgmFile.cs:1669`）：

| 步骤 | 位置 | 说明 |
|---|---|---|
| 读文件、解 `.vgz` | 1671-1673 | 自带解压 |
| 校验 `"Vgm "` | 1675-1679 | 只收 VGM |
| 建 Project / 通道 | 1685-1700 | 通道数由芯片位与扩展掩码决定 |
| 定帧长 | 1718-1719 | `framePacing ? 44100/(FreqNtsc/29780.5) : 735`，起点半帧偏移 |
| 判 PAL/NTSC | 1721-1750 | 头部 rate/时钟，必要时扫 wait 计数 |
| 主循环 | 1760-2450 | 逐命令解析 |
| 帧处理 | 2020-2082 | 先跑帧序列器，再对每通道调 `UpdateChannel` |
| 收尾 | 2413-2444 | 打印命令数、时钟倍率、帧数、GD3 |

wait 是唯一节拍源：

```csharp
samples += waitSamples;
if (samples >= samplesPerFrame) {
    samples -= samplesPerFrame;
    frame++;
    // 包络、长度计数器、扫频、DMC
    ...
    for (int c = 0; c < song.Channels.Length; c++)
        UpdateChannel(p, n, song.Channels[c], channelStates[c]);   // 2079-2081
}
```

`frameSkip` 可每 N 帧采样一次（`2077`），用来降低网格密度。

## 3. 关键机制

### 3.1 状态是仿真出来的

NES 部分在导入器里维护完整 APU 影子状态：

| 状态 | 位置 |
|---|---|
| `apuRegister[0x18]` | 写入口 2124-2200 |
| 方波/三角/噪声长度计数器 | 2020-2024 |
| 方波扫频（含 negate 的 +1 差异） | 2025-2049 |
| DMC 字节速率/剩余/循环 | 2053-2075 |
| 4/5 步帧序列器 | 2104-2121 |

`UpdateChannel` 拿到的不是"某寄存器刚被写"，而是"这一帧末通道的周期与音量"。

### 3.2 `GetState`：归一化通道状态

`GetState(channelType, stateId, index)`（865 起）对外只暴露语义量：

- `STATE_PERIOD` / `STATE_VOLUME` / `STATE_DUTYCYCLE`（NES）
- `STATE_VRCPATCH` / `STATE_FMPATCHREG`（VRC7、EPSM FM）
- `STATE_S5BMIXER` / `STATE_S5BENVENABLED` / `STATE_S5BENVSHAPE`
- `STATE_FDSWAVETABLE` / `STATE_FDSMODULATION*`
- `STATE_FMTRIGGER` / `STATE_FMSUSTAIN`

这套 ID 与 NSF 导入共用（`NsfFile.cs:1000+` 用同一批常量），说明"状态查询"被当成独立于容器格式的接口。

> 教训：两条导入路径的转换代码是复制粘贴的（`VgmFile.cs:1152-1631` 与 `NsfFile.cs` 同名逻辑几乎逐行相同）。我们重构时状态提取与音符判定都必须共享。

### 3.3 音符判定：状态机 + 音高表

`UpdateChannel`（1152-1631）每帧：

1. 取 period、volume、duty/patch、trigger。
2. 状态机（1360-1370）：
   ```csharp
   var newState = volume != 0 && (type == Noise || period != 0)
                  ? ChannelState.Triggered : ChannelState.Stopped;
   if (newState != state.state) { stop = ...; force = true; state.state = newState; }
   ```
   另有 `attack` / `release` 两个方向位，对应起音、释放、停止——即 MIDI 的 note-on、弱化、note-off。
3. 音高查表（1515）：
   ```csharp
   var noteTable = NesApu.GetNoteTableForChannelType(type, pal, n163Channels, tuning);
   var note = release ? Note.NoteRelease : (stop ? Note.NoteStop : state.note);
   ```
   - 表按芯片类型分别生成（`NesApu.cs:799`），按 PAL/NTSC、N163 通道数、tuning 参数化；
   - FM 用 `GetBestMatchingNote`（582-600）取最近邻并算 `finePitch`；
   - 噪声直接 `note = (period ^ 0x0f) + 32`（1521-1522）；
   - S5B 比 2A03 表偏移 -1，显式补偿（1526-1527）。
4. 八度折叠：FM 的 block/fnum 先归一化进表，再调八度（1529-1541）。
5. 落盘：仅 period/八度/乐器变化或 `force` 时写 note；音量单独比较（1373-1377）。

### 3.4 乐器识别

| 芯片 | 依据 | 位置 |
|---|---|---|
| 2A03/VRC6/MMC5 方波 | duty | `GetDutyInstrument` 602-617 |
| FDS | 64 字节波表 + 32 字节调制表 + 主音量 | 1386-1400、619 |
| VRC7/EPSM FM | patch 号 + 8/31 个 patch 寄存器 + sustain | 1417-1431、1449-1459 |
| S5B/EPSM 方波 | mixer、包络开关/形状、噪声频率 | 1432-1448、1460-1486 |

匹配到已有乐器就复用，否则新建。对 MIDI 的价值是**音色分组**：同 duty 的音符落在同一逻辑轨/同一 program 上。

### 3.5 时间网格

`p` 是 pattern 下标，`n` 是行号，由帧号推出；同一帧内的变化合并为一行。好处是天然对齐 60.0988/50.007 Hz；代价是丢失亚帧时序。我们按 sample 渲染，应保留 sample 精度，只在导出侧按需量化。

### 3.6 支持范围

解析的命令：`0x31 0x4F 0x50 0x51 0x52 0x53 0x55 0x56 0x57 0x58 0x59 0x61-0x63 0x66-0x68 0x93-0x95 0xA0 0xB4 0xE0`。但不是每命令一套解码，而是折叠进自己的通道模型：

| 命令 | 真实芯片 | 折叠到 |
|---|---|---|
| 0xB4 | NES APU | Square1/2、Triangle、Noise、DPCM |
| 0x51 | YM2413 | VRC7 FM ×6 |
| 0x52/0x55/0x56/0x58 | YM2612/YM2203/YM2608/YM2610B | EPSM 低区 |
| 0x57 | YM2610 | EPSM 高区 |
| 0x53/0x59 | YM2151/YM3526 | EPSM 高区 |
| 0xA0 | AY8910 | S5B ×3 |

即**只保留自己引擎能表达的部分**（3 路 SSG + 6 路 4-op FM + 6 路固定鼓），映射不到的直接丢弃。UI 选项（`App/Common/Dialogs/VgmImportDialog.cs:33-39`）：tuning、adjustClock、reverseDpcm、preserveDpcmPad、importDmcVals、ym2149AsEpsm、framePacing。

导入一次性：遇 `0x66` 结束（1899），不用头部 loop offset/samples；循环只在导出 VGM 时写入（`VgmFile.Save` 195-254）。

## 4. MIDI 导出

`MidiFileWriter.Save`（`MidiFile.cs:1624`）：

```csharp
Save (Project project, string filename, int songId, int instrumentMode,
      int[] instrumentMapping, bool volumeAsVelocity, bool slideToPitchWheel, int pitchWheelRange)
```

| 步骤 | 位置 | 说明 |
|---|---|---|
| 拒绝 FamiTracker tempo 工程 | 1626-1630 | 只支持自己的 tempo 模型 |
| 深拷贝、删无关 song | 1636-1638 | |
| `ConvertToSimpleNotes()` | 1638 | 先把包络/效果降维成普通音符——**关键** |
| 控制轨 tempo/拍号/groove | 1648、1368 | 按 pattern 长度换算 |
| 每条有内容的通道一条 MTrk | 1650-1660 | 顺序分配 MIDI 通道 1-16，超出丢弃并告警 |
| 单声部：新音前先关旧音 | 1544-1551 | 与 chiptune 通道一一对应 |
| volume → velocity | 1538-1539 | 可选，`volume/15*127` |
| 滑音 → pitch wheel | 1522-1533、1555-1560 | 可选，带 bend range |

导出器没有任何芯片知识。**要抄的是音符模型，不是导出器。**

## 5. 为什么它好、我们差

| 维度 | FamiStudio | 我们 |
|---|---|---|
| 芯片知识位置 | 影子寄存器 + `GetState` | 每芯片一个 `case`，混在 1467 行里 |
| 发声判定 | 三态状态机 + 长度计数器 + 包络 | 只看寄存器被写 |
| 音高 | 每芯片音高表 + 最近邻 + finePitch | 公式直算，时钟/分频错一处就整体跑调 |
| 音量/力度 | 每帧读包络 → note.Volume | 不建模，固定力度 |
| 时长 | 状态机给 release/stop | 多数芯片只有 one-shot |
| 时间 | 帧网格 | sample offset（这一层我们更强） |
| 乐器 | 反推 duty/patch/波表 | 无 |
| 范围 | 只做自己能表达的，做不到就丢 | 承诺全芯片，做不到发 C4 标记 |

最后一行是根本问题：**没有状态模型却承诺全芯片覆盖**，只能用 one-shot 掩盖缺失。

但也要看到：FamiStudio 的芯片全属 NES 家族（方波/三角/噪声/波表/2-op、4-op FM），音高关系简单、文档完备；它并没有解决 YM2612 的 CSM、DAC 流、SSG-EG，也没有解决 YM2610 的 ADPCM 音高。所以"它效果好"不能直接推成"照抄就全好"。

## 6. 可借鉴的架构

### 6.1 分层

在 `plugin/Source/backends/libvgm/` 与 `playback/` 之间加 `chipstate/`：

```
libvgm 核 ──writeRegister(reg, value, sampleOffset)──► ChipStateProvider（每芯片一个）
                                                          │ 影子寄存器 + 计数器/包络/扫频
                                                          ▼
                                                 ChannelState（周期、音量、gate、patch）
                                                          │
                                                 NoteExtractor（芯片无关）
                                                          │ 音高表 + 状态机
                                                          ▼
                                                 NoteEvent（start/end/note/velocity/finePitch）
                                                          │
                                                 MidiEncoder（现 MidiSequenceBuilder 收缩）
                                                          ▼
                                                 MidiBlockEvent → 宿主
```

### 6.2 接口草案

```cpp
class ChipStateProvider
{
public:
    virtual ~ChipStateProvider() = default;

    virtual void reset (std::uint32_t clock, double sampleRate) = 0;
    virtual void writeRegister (std::uint32_t reg, std::uint8_t value, int sampleOffset) = 0;
    virtual void advance (int samples) = 0;          // 帧序列器/定时器，按需

    virtual int channelCount() const = 0;
    virtual ChannelState channelState (int channel) const = 0;
    virtual const NoteTable& noteTable (int channel) const = 0;
};

struct ChannelState
{
    bool          present = false;
    bool          gate = false;          // key on
    bool          released = false;
    std::uint32_t period = 0;            // 芯片原生周期/fnum
    int           block = 0;             // FM 八度
    int           volume = 0;            // 归一化 0..15
    int           patch = 0;             // duty / patch / 鼓编号
    std::int16_t  finePitch = 0;
};

struct NoteEvent
{
    std::uint32_t startSample = 0;
    std::uint32_t endSample = 0;
    std::uint8_t  note = 0;
    std::uint8_t  velocity = 0;
    std::uint8_t  channel = 0;           // 逻辑轨；MIDI 通道由混音器决定
    std::int16_t  finePitch = 0;
    bool          isDrum = false;
};
```

`MidiEncoder` 只保留今天已经正确的部分：按轨分配通道、mute/solo、master 合并去重、CC123、sampleOffset 排序，one-shot 作为降级路径保留。

### 6.3 每芯片解码器（按收益排序）

| 优先级 | 芯片 | 需要的状态 | 难度 |
|---|---|---|---|
| P0 | SN76489（已可用） | 3 方波 + 噪声，tone 寄存器 | 低，作为新架构样板 |
| P0 | YM2610 SSG（已可用） | 同上 | 低 |
| P1 | YM2610 FM | FNum/Block、key-on/off | 中 |
| P1 | 2A03/2A07 | 周期、长度计数器、扫频、包络 | 中（可对照 FamiStudio） |
| P1 | Game Boy | 4 通道频率、wave RAM、包络 | 中 |
| P2 | YM2612/YM2608/YM2203 | FNum/Block、key-on/off、DAC 流（作鼓） | 中高 |
| P2 | YM3812/YM3526/YM2413 | FNum/Block、key-on/off、节奏通道 | 中 |
| P3 | MSM6295/MSM6258/RF5C/QSound | 只做鼓轨（映射 GM 鼓键） | 低（只需触发点） |

### 6.4 测试策略

FamiStudio 没有导入的自动化测试（只有手工 QA），我们可以更强：

1. **每芯片 golden 测试**：合成 VGM → 期望 `NoteEvent` 列表（音符、起止 sample、通道）。先写期望再写 provider。
2. **迁移回归**：新旧实现同一 VGM 的事件序列逐条一致（SN76489/YM2610 已可用，正好当基线）。
3. **可听性检查**：MIDI 渲染成 WAV 与 VGM 做 onset 对齐 + 音高直方图，脚本化"音符数/音高分布"偏差阈值。

## 7. 不建议照搬

| 不抄 | 原因 |
|---|---|
| 工程/乐器/包络模型 | 目标是 MIDI，不是第二个 tracker |
| 一行 = 一帧的量化 | 我们按 sample 渲染，精度更有价值；量化放导出侧 |
| 一次性导入、丢弃 loop | 播放器支持 seek/loop，MIDI 也要跟随 |
| 把 YM2612/YM2610 折进 EPSM | 它为了"能用自己的芯片回放"，我们只需准确音高 |
| 复制粘贴两条导入路径 | 它的 NSF/VGM 转换各写一遍，正是要避免的 |

## 8. 实施顺序

1. **抽层、行为不变**：新增 `chipstate/`，把 SN76489 与 YM2610 SSG 的状态提取搬过去，`NoteExtractor` 产出与今天完全相同的事件，`MidiSequenceBuilder` 收缩为编码器；用现有 `[10]` 测试与 `app.midi` 保证不回归。
2. **补 P1 芯片**：每颗芯片一个 provider + 一份 golden 测试；先做 2A03（素材最多、可对照 FamiStudio）。
3. **音量/时长**：`ChannelState.volume` → velocity，`endSample` → note-off；鼓轨单独走 GM 鼓键。
4. **质量门槛**：建立"每芯片至少一首曲目听感通过"的清单，替换今天"全芯片 one-shot"的假覆盖。

## 9. 参考位置

| 内容 | 位置 |
|---|---|
| 导入入口与主循环 | `VgmFile.cs:1669`、`1760-2450` |
| 帧序列器与逐帧处理 | `VgmFile.cs:2020-2082` |
| 状态查询 | `VgmFile.cs:865` 起 |
| 音符判定 | `VgmFile.cs:1152-1631` |
| 音高表 | `NesApu.cs:799`、`VgmFile.cs:582` |
| 命令→芯片折叠 | `VgmFile.cs:2238-2380` |
| MIDI 导出 | `MidiFile.cs:1470-1665` |
| 通道类型 | `Channel.cs:1598-1698` |
| 导入选项 UI | `App/Common/Dialogs/VgmImportDialog.cs:33-39` |
| 我们的现状 | `plugin/Source/backends/libvgm/MidiSequenceBuilder.cpp`、`docs/desgin/midi-output-design.md` |
