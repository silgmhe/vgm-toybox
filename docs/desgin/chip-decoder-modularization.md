# 芯片解码模块化设计（参考）

- 状态：设计参考，未实施
- 范围：`plugin/Source/backends/libvgm/` 的 MIDI 解码部分
- 目标芯片：NES（2A03 + 扩展）、Sega MD（YM2612 + SN76489）、Neo Geo（YM2610），其余按同一结构追加

## 1. 现状

| 事实 | 位置 |
|---|---|
| `MidiSequenceBuilder.cpp` 1467 行，其余文件都在 400 行以内 | `backends/libvgm/` |
| 一个 TU 里同时做：VGM 命令表、逐芯片寄存器解码、音符提取、MIDI 编码 | `MidiSequenceBuilder.cpp:31`（命令表）、`:314`（命令分发）、`:950-1010`（音符换算）、`:1622`（入口） |
| 10 份芯片状态用 `std::map<deviceId, XState>` 混在同一个 `Builder` 里 | `:1607-1617` |
| 芯片能力判断还有第二处 switch | `LibvgmSongInstance.cpp:41-105` |
| 除 SN76489/YM2610 SSG 外，其余芯片只发 C4 one-shot | 见 `docs/research/2026-10-19-famistudio-vgm-import.md` |

问题不是"没用设计模式"，而是**没有边界**：解析、芯片状态、音乐语义、输出编码四件事共用一份匿名命名空间，新增一颗芯片必须改同一个文件、同一个 switch、同一个状态表。

## 2. 需求与约束

功能：

- 主流芯片先做好：NES 的 2A03（含 VRC6/VRC7/FDS/MMC5/N163 可后置）、MD 的 YM2612 + SN76489、Neo Geo 的 YM2610（FM + SSG + ADPCM）。
- 每颗芯片要给出：音高（含 fine pitch）、起止时间、力度、鼓/one-shot 区分。
- 未支持的芯片保留 one-shot 降级，并在界面上明确标注，不再假装"全支持"。

非功能：

- MIDI 序列在**加载时**一次性预计算，不进音频线程；单曲耗时应远小于 1 秒。
- 新增芯片不改动既有芯片代码（开闭原则的实用版本）。
- 单文件 ≤ 400 行；每颗芯片一个 TU。
- 不引入第三方框架、不用 RTTI 做分发、不新增线程。

约束：

- C++20 + JUCE，`libvgm` 为仓库内 vendored 源码，可打补丁但不作为常规手段。
- VGM/S98/DRO 等格式的解析可以复用现有的 `Builder::parse` 逻辑，但必须搬到独立文件。
- MIDI 输出仍是 16 通道，每轨一个逻辑通道。

## 3. 架构风格

**模块化单体 + 四层**。不引入独立进程/服务，也不做插件化运行时加载。

```
┌──────────────────────────────────────────────────────────────┐
│ playback/            （已有）引擎、混音器、MidiSequence 模型    │
└──────────────────────────────▲───────────────────────────────┘
                               │ MidiSequence
┌──────────────────────────────┴───────────────────────────────┐
│ midi/       MidiEncoder     NoteEvent → MidiEvent（唯一编码器）│
├──────────────────────────────────────────────────────────────┤
│ notes/      NoteExtractor   ChannelState → NoteEvent（芯片无关）│
│             NoteTable       period ↔ note（按芯片/时钟/tuning） │
├──────────────────────────────────────────────────────────────┤
│ chips/      IChipDecoder + 每芯片一个实现                      │
│             （寄存器 → ChannelState，含定时器/包络/扫频）        │
├──────────────────────────────────────────────────────────────┤
│ vgm/        VgmCommandReader 命令流 → RegisterWrite（唯一解析器）│
└──────────────────────────────────────────────────────────────┘
```

关键约束：**只有 `chips/` 知道芯片寄存器**；`notes/` 与 `midi/` 不认识 `DEVID_*`，`chips/` 不认识 VGM 命令字节。

## 4. 目录与文件

```
plugin/Source/chips/
  ChipFamily.h              // 与 libvgm 解耦的枚举 + 能力表（通道数、有无鼓、音高表类型）
  RegisterWrite.h           // { deviceId, channel, reg, value, sampleOffset, isSecondChip }
  ChipState.h               // ChannelState、NoteTable 前置声明
  IChipDecoder.h            // 接口（见 §6）
  ChipDecoderFactory.cpp/.h // DEVID_* + 时钟 → decoder；唯一出现 DEVID_* 的地方
  decoders/
    NesApuDecoder.cpp/.h        // 2A03；VRC6/MMC5/FDS/N163 作为内部子模型
    Ym2612Decoder.cpp/.h        // OPN2 + DAC；OPN/OPNA/OPNB 复用同一个 FM 核心
    Ym2610Decoder.cpp/.h        // OPNB：FM + SSG + ADPCM-A/B
    Sn76489Decoder.cpp/.h       // PSG（MD 的第二颗芯片）
    GbApuDecoder.cpp/.h         // 后续
    OplDecoder.cpp/.h           // 后续（YM3812/3526/2413）
  notes/
    NoteTable.cpp/.h            // 每芯片族一张表，按 clock/PAL/tuning 生成
    NoteExtractor.cpp/.h        // 三态机：Triggered/Released/Stopped
  midi/
    MidiEncoder.cpp/.h          // 从现 Builder 抽出的编码部分
  vgm/
    VgmCommandReader.cpp/.h     // 从现 Builder 抽出的命令表与分发
```

预计行数：`NesApu` 500-700、`Ym2612` 350-450、`Ym2610` 400-500、`Sn76489` 120-180、`NoteTable` 200、`NoteExtractor` 200-250、`MidiEncoder` 200、`VgmCommandReader` 180。总量与今天接近，但每颗芯片可独立修改、独立测试。

## 5. 关键接口

```cpp
namespace toybox::chips
{
	enum class Family { nesApu, vrc6, vrc7, fds, mmc5, n163,
	                    sn76489, ym2612, ym2610, gbApu, opl, unknown };

	struct RegisterWrite
	{
		std::uint32_t deviceId = 0;		// libvgm 的实例 id，仅用于路由，不解释
		std::uint8_t  reg = 0;
		std::uint8_t  value = 0;
		std::int32_t  sampleOffset = 0;	// 相对当前 block，允许为负（跨 block 的写入）
		std::uint8_t  port = 0;			// YM2612 的第二端口等
	};

	struct ChannelState
	{
		bool          present = false;
		bool          gate = false;			// key on
		bool          released = false;
		std::uint32_t period = 0;			// 芯片原生周期/fnum
		int           block = 0;			// FM 八度
		int           volume = 0;			// 0..15，已按包络折算
		int           patch = 0;			// duty / patch / 鼓编号
		std::int16_t  finePitch = 0;		// 表外偏移（音分）
	};

	class IChipDecoder
	{
	public:
		virtual ~IChipDecoder() = default;
		virtual void reset (std::uint32_t clock, double sampleRate) = 0;
		virtual void write (const RegisterWrite& write) = 0;
		virtual void advance (int samples) = 0;		// 帧序列器/定时器；无定时器的芯片留空
		virtual int channelCount() const = 0;
		virtual ChannelState channelState (int channel) const = 0;
		virtual const NoteTable& noteTable (int channel) const = 0;
		virtual Family family (int channel) const = 0;
	};
}
```

`MidiEncoder` 只保留今天已经正确的逻辑：按轨分配 MIDI 通道、mute/solo、master 合并去重、CC123、sampleOffset 排序、one-shot 降级。

## 6. 设计模式取舍

| 模式 | 用不用 | 位置 | 理由 |
|---|---|---|---|
| **Strategy** | 用 | `IChipDecoder` 每芯片一个实现 | 这是唯一必须有的抽象；新增芯片 = 新增文件 |
| **Factory / Registry** | 用 | `ChipDecoderFactory` | 把 `DEVID_* → Family → decoder` 收在一处，替代两处 switch |
| **Template Method** | 用（浅） | 各 decoder 内部的 `decodeCommon()`：寄存器影子、key-on 边沿、块推进 | 复用不靠继承树，只靠一个基类 + protected helper |
| **Adapter** | 用 | `VgmCommandReader` 把 VGM 命令适配成 `RegisterWrite` | 隔离格式与芯片 |
| **Observer** | 已有 | `MidiBlockSink` | 不再新增 |
| Visitor | 不用 | — | 芯片种类固定、每芯片代码独立，visitor 只会增加间接层 |
| Singleton | 不用 | — | 全部由 `SongInstance` 持有，无全局状态 |
| 深继承（每芯片一个基类） | 不用 | — | 最多一层基类；芯片差异用组合（子模型）而非继承 |
| DI 容器 | 不用 | — | 构造顺序在 `LibvgmSongInstance` 里显式写出即可 |

一句话：**模式的收益来自把"哪颗芯片"从"怎么出 MIDI"里拆出去，其余模式都是可选的。**

## 7. 三颗目标芯片的落地细节

### 7.1 NES（2A03，优先）

需要仿真的状态（FamiStudio 的 `VgmFile.cs:2020-2082` 是现成参照）：

| 状态 | 用途 |
|---|---|
| 方波：周期、duty、长度计数器、包络（音量/衰减/循环）、扫频 | 音高 + 力度 + 起止 |
| 三角：线性计数器、周期 | 音高 + 时长 |
| 噪声：周期索引、模式、长度计数器、包络 | 打击乐（鼓键映射） |
| DMC：采样率、剩余字节、循环 | 鼓/采样轨（one-shot） |
| 帧序列器：4/5 步模式、IRQ 抑制 | 包络与长度计数器推进 |

音符来源：方波/三角周期 → `NoteTable`（按 NTSC/PAL 与 tuning 生成），噪声按周期索引映射鼓键。扩展芯片（VRC6/VRC7/FDS/MMC5/N163）作为同一 decoder 内的子模型，按需启用，不引入新文件族。

### 7.2 Sega MD（YM2612 + SN76489）

| 状态 | 用途 |
|---|---|
| 6 个 FM 通道：FNum/Block、key-on/off、算法/反馈（仅音高需要） | 音高与起止 |
| `0x28` key-on 边沿 | note-on/off 的唯一依据（不要用音量推断） |
| DAC（`0x2A` 流） | 只按鼓速率触发，不逐字节发 note（现实现 `writeDac` 已有此处理） |
| SN76489：3 方波 + 噪声 tone 寄存器 | 音高；噪声按周期映射鼓键 |

### 7.3 Neo Geo（YM2610）

| 状态 | 用途 |
|---|---|
| FM 4 通道：FNum/Block、key-on/off | 音高与起止 |
| SSG 3 通道：周期、音量、mixer | 音高（已可用，迁入 provider） |
| ADPCM-A（6 通道）/ ADPCM-B（1 通道）：start/stop 位、音高 | 鼓轨；B 通道需读 pitch 寄存器 |

YM2610 的 SSG 寄存器布局与 YM2149 一致，`Ym2610Decoder` 与未来的 `Ym2149Decoder` 共用 SSG 子模型，避免复制。

## 8. ADR

### ADR-001：两段式（状态层 + 音符提取），而非每芯片直出 MIDI

- **决策**：每芯片只负责寄存器 → `ChannelState`；音高、起止、力度由共享的 `NoteExtractor` 产生。
- **理由**：今天的 one-shot 问题源于"没有状态可读"，不是"MIDI 编码不对"；两段式让新增芯片只需实现状态，音符逻辑自动获得。
- **代价**：多一层数据拷贝（每 block 每通道一个 `ChannelState`），可忽略。

### ADR-002：保留自己的 VGM 解析器，不给 libvgm 打寄存器钩子

- **决策**：`VgmCommandReader` 独立成文件，继续作为唯一解析器；`libvgm` 保持只负责发声。
- **理由**：libvgm 的 `PLR_DEV_INFO` 不暴露设备写回调（`playerbase.hpp:61`），要挂钩子只能改上游；而我们的序列是加载时预计算，不适合复用实时渲染路径。
- **代价**：VGM 命令表与 libvgm 各有一份；用一份合成 VGM 的回归测试保证不漂移。

### ADR-003：one-shot 作为显式降级，并在界面上标注

- **决策**：`midiCapabilityFor` 从 `LibvgmSongInstance.cpp` 移入 `chips/`，能力表由 decoder 自己声明；`oneShot` 仍是合法状态，但 UI 必须显示。
- **理由**：当前的"全芯片支持"是假象；显式降级能让测试与用户都知道边界。

### ADR-004：序列在加载时预计算，不进音频线程

- **决策**：保持 `LibvgmSongInstance::getMidiSequence()` 的时机与所有权。
- **理由**：MIDI 事件与音频采样无关，预计算避免锁与实时性问题；代价是加载时间，但纯计算在毫秒级。

## 9. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 影子状态与 libvgm 实现漂移 | 音高/时长错 | 每芯片 golden 测试；升级 libvgm 时跑全套 |
| 事件量爆炸（DAC、ADPCM 逐字节） | 内存/CPU 浪费 | encoder 侧按时间窗聚合，鼓轨限速（现有 50 ms 窗口保留） |
| 16 通道不够 | 复杂曲目丢轨 | 按优先级分配：鼓 > 旋律；超出时合并或丢弃并告警 |
| 测试材料版权 | 不能进仓库 | 用 `make_song.ps1` 生成合成 VGM，真实曲目只在本地跑 |
| 迁移期双实现分叉 | 新 bug 无人发现 | 新旧事件序列逐条对比，作为迁移的验收条件 |

## 10. 迁移步骤（每步可独立提交、可回滚）

1. **抽 `VgmCommandReader`**：把命令表与 `parse()` 的分发搬出，`Builder` 只接收 `RegisterWrite`。行为不变，用 `[10]` 测试兜底。
2. **抽 `MidiEncoder`**：把 `emit*`、one-shot、`sampleOffset` 排序搬出；事件序列逐条对比。
3. **抽 `NoteExtractor` + `NoteTable`**：先把 SN76489 与 YM2610 SSG 的现有公式搬进去，输出必须完全一致。
4. **`Ym2612Decoder`**：MD 第一批收益；补 key-on 边沿与 FNum/Block 音高。
5. **`NesApuDecoder`**：NES 第一批收益；参照 FamiStudio 的帧序列器实现。
6. **`Ym2610Decoder`**：Neo Geo；SSG 复用第 3 步的子模型，新增 FM 与 ADPCM。
7. **删除旧 `Builder` 的芯片分支**，`MidiSequenceBuilder.cpp` 只留装配代码（目标 < 150 行）。

## 11. 验收

- `vgm_mixer_test [10]` 与 `app.midi` 不回归。
- 每颗芯片一份 golden 测试：合成 VGM → 期望 `NoteEvent` 列表（音符、起止 sample、通道）。
- 每颗芯片至少一首真实曲目人工听感通过（本地，不入库）。
- `MidiSequenceBuilder.cpp` ≤ 150 行；任何单文件 ≤ 400 行；`DEVID_*` 只出现在 `ChipDecoderFactory.cpp` 与 `LibvgmSongInstance` 的枚举代码里。

## 12. 工作量估算

| 项 | 人日（单人） |
|---|---|
| 第 1-3 步（纯重构，行为不变） | 2-3 |
| YM2612 + SN76489（MD 可用） | 3-4 |
| 2A03（NES 可用） | 4-6 |
| YM2610（Neo Geo 可用） | 3-4 |
| 测试与调音（每芯片 1-2 首曲子） | 3-5 |

合计约 15-22 人日；若只做 MD + NES，约 10-13 人日。

## 13. 进度

| 步骤 | 状态 | 落地内容 |
|---|---|---|
| 1. `VgmCommandReader` | 已完成 | `backends/libvgm/VgmCommandReader.{h,cpp}`；`MidiSequenceBuilder` 只看 `VgmCommand` |
| 2. `MidiEncoder` | 未开始 | 仍在 `MidiSequenceBuilder` 内 |
| 3. `NoteExtractor` + `NoteTable` | 已完成（2A03 先行） | `chips/NoteExtractor.{h,cpp}`、`chips/ChipTypes.h` |
| 4. `Ym2612Decoder` | 已完成首版 | `chips/Ym2612Decoder.{h,cpp}`：算法→载波表、总电平（TL）→力度、DAC 接管通道 6 |
| 5. `NesApuDecoder` | 已完成首版 | `chips/NesApuDecoder.{h,cpp}`，帧序列器、长度/线性计数器、包络、扫频、噪声、DMC |
| 6. `Ym2610Decoder` | 未开始 | — |
| 7. 收缩旧 `Builder` | 部分 | 旧 `writeNes` / `NesState`、YM2612 的 `writeFm` 分支已删除（`MidiSequenceBuilder.cpp` 1467 → 1295 行） |

2A03 的验收方式是新的 `vgm_chip_test`（`plugin/Tests/ChipDecoderTest.cpp`）：只链接 `Source/chips`，不链接 libvgm，用寄存器写断言音符与起止采样。期间修正了两处旧实现不可能有、新实现必须有的细节：

- 帧序列器步长要按采样率换算（`7457 * sampleRate / cpuClock`），而不是直接用 CPU 周期数；
- 长度计数器寄存器存的是索引，要查 `kLengthTable`；`$4015` 写 0 才是真正的键控关闭（写 `$4003` 只会重新装载长度）。

YM2612 的验收同样走 `vgm_chip_test`，并补回了旧实现有、两段式重构时差点丢掉的音高曲线：

- 音高来自 `fnum × clock / (144 × 2^(20-block))`，时钟取自 VGM 头，超频曲目不会跑调；
- 是否成音符由算法决定：算法 0-3 只有算子 4 是载波，4 有 2 个，5/6 有 3 个，7 是 4 个之和；取载波里最小的 TL，`0x7F` 视为静音（不发声的音色不再产生空音符）；
- `0x2B` 置 DAC 后通道 6 不再是 FM 通道，避免与 `writeDac` 的鼓标记重复计音符；
- `0x28` 的第二端口位映射到通道 4-6，与通道 1-3 一样独立成轨。

`NoteExtractor` 现在同时返回 `PitchEvent`：同一音符内音高变化（颤音、滑音）不再重新触发，而是写成弯音；起始音符带自己的音分偏移，顺带清掉上一个音符留下的弯音。老实现在每次 `0xA0-0xA6` 写入时都发一次弯音，新实现按 4 音分的死区过滤，事件量从每秒数百条降到实际音高变化的次数。

后续接 YM2610 时，`NoteExtractor` 与 `NesApuDecoder` 都是模板：新的 decoder 只需回答 `channelState()` 与 `noteTable()`。
