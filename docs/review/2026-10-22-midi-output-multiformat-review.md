# MIDI 输出：可维护性与多格式扩展评审

| 项目 | 内容 |
|---|---|
| 评审日期 | 2026-10-22 |
| 评审对象 | `feature/midi-output`，commit `5d538c1` |
| 范围 | `plugin/Source/chips/`（31 个解码器）、`plugin/Source/backends/libvgm/`（装配层）、`plugin/Source/playback/`（引擎接口）；不评审 UI 细节与 libvgm 本身 |
| 方法 | 静态阅读接口与实现、统计规模与重复度、核对文档声明、以真实曲目运行验证现行行为 |
| 结论 | **功能正确、芯片层已解耦；装配层仍与 VGM 强耦合**。新增芯片很便宜，新增**格式**很贵。3 个结构性阻断项、4 个高优先级、3 个中、1 个低 |
| 状态 | 仅评审，未修改代码 |

## 1. 总体结论

这个 feature 的核心设计是成立的，且有三处做得比一般实现好：

- **芯片层与格式无关**。`Family` 枚举刻意不含 libvgm 的 `DEVID_*`（`ChipTypes.h`），`IChipDecoder` 只有 `reset/write/advance/channelState/noteTable/family` 六个抽象，`NoteExtractor` 只通过 `channelState()` 观察世界。这是整套体系最有价值的部分，也是后续支持新格式的**唯一支点**。
- **音高模型是数据，不是代码**。L1 移调（`transposeCents`）与 L2 根音（`rootCents`）随 `NoteEvent` 一起传递，`pitch.useSampleRoot` 在编码期才决定是否相加；改设置不需要重解析。
- **可验证性是一等的**。单测直接构造 `RegisterWrite` 向量；`--pitch-report` 把「寄存器精确 / 带根音 / one-shot / 根音种类数」变成可断言的事实；每颗芯片都用真实 rips 验收。

但从「下一颗芯片」走到「下一种格式」，当前装配层需要重写，而不是复用。

### 建议决策

| 事项 | 建议 | 理由 |
|---|---|---|
| `R0` 接口机械改造（`reset(ChipConfig)`、`RegisterWrite.value` 扩到 16 位） | **立即做** | 越晚做越贵：现在 31 处，格式层落地后是 31 处 × 每个 loader |
| `R1` 抽出 `SongIr` + `ISongLoader` | **本分支做** | 它决定了 NSF/FMT 是「写一个 loader」还是「写一个平行 Builder」 |
| `R2` `DecoderRegistry` 替换 31 个成员 | 本分支做 | 与 R1 同批最省事，避免两次触碰同一片代码 |
| `R3` 30 个 `NoteTable` 合并 | 排后续 | 纯减债，不影响扩展性 |
| `R4` `LibvgmSongInstance` 拆分 | 随 NSF 一起 | 没有第二种格式时拆它的收益不明显 |
| `R5` NSF 适配 | 单独立项 | libvgm **不支持** NSF，需要引入独立 CPU 核心，属新依赖 |

## 2. 实测规模

| 区域 | 体量 | 判断 |
|---|---|---|
| `chips/` | 9 075 行 / 31 个解码器 / 30 个 `NoteTable` 派生类 | 结构健康，重复度偏高 |
| `backends/libvgm/` | 2 456 行 | 耦合集中区 |
| `MidiSequenceBuilder.cpp` | 1 098 行，其中 `Builder::parse` **462 行**（91–553） | 瓶颈 |
| `Builder` 内 `ChipBuffer<T>` 成员 | **31 个** | 扩展成本点 |
| `parse` 内 `case 0x` | **57 个** | VGM 命令硬编码 |
| `LibvgmSongInstance.cpp` | 517 行，承担五类职责 | 拆分候选 |

## 3. 问题清单

| ID | 级别 | 问题 |
|---|---|---|
| C-1 | 阻断 | `Builder::parse` 把「VGM 命令解析」与「MIDI 装配」放在同一函数（462 行 / 57 个 case） |
| C-2 | 阻断 | 设备路由靠 31 个 `ChipBuffer<T>` 成员 + `deviceFor(DEVID_*)`，无法跨格式复用 |
| C-3 | 阻断 | `IChipDecoder::reset(clock, sampleRate)` 的契约写着「来自 VGM 头部」，芯片层泄漏格式概念 |
| H-1 | 高 | `RegisterWrite.value2` 为 C352 单颗芯片而加，格式细节进入共享 IR |
| H-2 | 高 | `kVgmRate = 44100.0` 是全局常量，时间栅格无处传递 |
| H-3 | 高 | 30 个 `NoteTable` 各写一遍 `noteFor`，仅 13 处复用 `quantisePlaybackRate` |
| H-4 | 高 | `LibvgmSongInstance` 同时做容器解包、libvgm 生命周期、轨枚举、能力表、序列构建 |
| M-1 | 中 | `ChannelState` 同时携带查表派与速率派两组字段，新芯片作者需自行判断该填哪套 |
| M-2 | 中 | `Family` 混装「芯片族」与「声道种类」（`aySquare` 与 `fm` 同级） |
| M-3 | 中 | 测试栅格固定 44 100，跨格式的时间基契约未文档化 |
| L-1 | 低 | `RegisterWrite.deviceId == 0` 兼作「未指定」哨兵，与合法实例号 0 无法区分 |

## 4. 阻断项

### C-1 `Builder::parse` 必须拆成「翻译」与「装配」

```cpp
// MidiSequenceBuilder.cpp:91
std::unique_ptr<playback::MidiSequence> parse (const void* data, std::size_t size)
{
    VgmCommandReader reader (data, size);
    ...
    switch (command.command) { /* 57 个 case，到 553 行为止 */ }
}
```

NSF 的输入是 CPU 对 APU 的写入，没有 `0x50/0x52` 这类命令号。按现状只能再写一个 460 行函数，两个函数里「建轨、noteOn/noteOff、bend、flush」的逻辑会各自演化，这是最难维护的一种重复。

建议引入格式中立的中间表示，让 Builder 只消费它：

```cpp
struct ChipSlot  { DeviceKind kind; int instance; std::uint32_t clockHz; };

struct SongIr
{
    double tickRate;                     // 44100(VGM) / 1.79e6(NTSC CPU) / ...
    std::vector<ChipSlot> chips;
    std::vector<chips::RegisterWrite> writes;   // tick 以 tickRate 为单位
};

class ISongLoader
{
public:
    virtual ~ISongLoader() = default;
    virtual bool load (const playback::SongMemory& image, SongIr& out, juce::String& error) = 0;
};
```

- `VgmSongLoader`：`0x50`–`0xC7` → `writes`（现有 case 全部搬过去，逐条一一对应）；
- `NsfSongLoader`（未来）：CPU 周期 → `writes`；
- `Builder`：只负责「writes + 设备表 + 轨表 → `MidiSequence`」，预计从 462 行降到约 60 行。

### C-2 设备路由要用注册表，不要用成员变量

```cpp
ChipBuffer<chips::Sn76489Decoder> sn;
ChipBuffer<chips::OplDecoder>     opl;      // ... 共 31 个
```

每增加一种格式，这套成员与 `deviceFor` 调用都要复制一遍。建议：

```cpp
using Factory = std::function<std::unique_ptr<chips::IChipDecoder> (const chips::ChipConfig&)>;

class DecoderRegistry
{
public:
    void registerKind (DeviceKind kind, Factory factory);
    std::unique_ptr<chips::IChipDecoder> create (DeviceKind kind, const chips::ChipConfig&);
};
```

注册一次，任何格式复用。`ChipConfig` 也顺手解决 C-3。

### C-3 芯片层不能提到 VGM

```cpp
/** @param clock master clock from the VGM header (Hz), 0 when unknown
    @param sampleRate the rate the stream is expressed in (normally 44100) */
virtual void reset (std::uint32_t clock, double sampleRate) = 0;
```

两个问题：注释把格式写进了芯片抽象；`sampleRate` 实际是**时间栅格**，与「音频输出率」混为一谈（NSF 的栅格是 1.79 MHz，而不是 44.1 kHz）。

```cpp
struct ChipConfig
{
    std::uint32_t clockHz;      // 0 = 未知，解码器取默认值
    double        timeBaseHz;   // 写入时间戳的单位
    std::uint32_t flags;        // 板卡/变体（C140 已用到）
};

virtual void reset (const ChipConfig&) = 0;
```

改动机械但覆盖面广（31 个实现），**必须在上层格式化之前完成**。

## 5. 高优先级

| ID | 位置 | 问题 | 建议 |
|---|---|---|---|
| H-1 | `ChipTypes.h` `RegisterWrite` | `value2` 的注释写着「C352 是唯一按字写入的芯片」——为单颗芯片加了字段 | `value` 改 `uint16_t`，删 `value2`；`port` 保留，语义写成「子地址」 |
| H-2 | `MidiSequenceBuilder.cpp:49` | `kVgmRate` 是全局常量，`--pitch-report` 与 `loop at A of B` 都隐含它 | 速率进 `SongIr::tickRate`，只在展示层换算 |
| H-3 | `chips/*.cpp` | 30 个 `final : public NoteTable`，只有 13 处用 `quantisePlaybackRate` | 抽 `RateNoteTable(rateFn, refRate)`，多数芯片退化成 3 行构造；单测逐个兜底 |
| H-4 | `LibvgmSongInstance.cpp` | 五类职责同处（解包 / 生命周期 / 轨枚举 / 能力表 / 序列构建） | 抽 `ISongInstance`，为 `NsfSongInstance` 让位 |

## 6. 中/低

- **M-1** `ChannelState` 同时有 `period/block/finePitch`（查表派）与 `hz/transposeCents/rootCents`（速率派）。建议在 `pitchSource` 上注明「哪组字段有效」，或拆成两个子结构，避免新芯片作者选错。
- **M-2** `Family` 把芯片族（`aySquare`）与声道种类（`fm`、`fmDrum`）放在同一枚举。多格式后建议拆 `ChipFamily` + `ChannelKind`；这一步会触及编码器的 `patch` 语义，宜与 H-3 同批。
- **M-3** 测试栅格固定 44 100（`RegisterWrite.sampleTime`）。跨格式后需要明确：loader 负责换算到统一栅格，还是解码器接受任意 `timeBaseHz`。建议前者——解码器不该关心时间基。
- **L-1** `deviceId == 0` 兼作「未指定」。合法实例号可能就是 0（`DEVID_*` 的第一片），建议 `kNoDevice = UINT32_MAX` 或 `std::optional`。

## 7. 目标结构

```
Source/
  chips/          现有 31 个解码器；只换 reset 签名，其余不动
  midi/           MidiSequenceBuilder / NoteExtractor / Pitch —— 从 backends/libvgm 移出
  formats/
    vgm/          VgmCommandReader -> VgmSongLoader -> SongIr
    nsf/          (CPU 核心)      -> NsfSongLoader -> SongIr
  playback/       PlaybackEngine / ITrackPlayer（不变）
```

判据只有两条：

1. `midi/` 不认识任何格式，只认 `SongIr`；
2. `formats/*` 只做翻译，不认识 MIDI。

## 8. 迁移路径

| 阶段 | 内容 | 成本 | 风险 | 验收 |
|---|---|---|---|---|
| R0 | `ChipConfig` + `RegisterWrite.value` 扩 16 位 | 0.5 天 | 低 | `ctest` 34/34，golden 输出不变 |
| R1 | `SongIr` + `ISongLoader`，`parse` 换成 `VgmSongLoader` | 2 天 | 中 | 95 首 rips 的 `[13]` 结果逐首不变 |
| R2 | `DecoderRegistry` 替换 31 个成员 | 1 天 | 低 | 同上；`--pitch-report` 输出不变 |
| R3 | 30 个 `NoteTable` 合并为 `RateNoteTable` | 2–3 天 | 低 | 每个解码器的单测断言不变 |
| R4 | `LibvgmSongInstance` 拆容器/轨/序列 | 2 天 | 中 | 轨枚举与能力表快照不变 |
| R5 | NSF 适配（需独立 CPU 核心） | 3–5 天 | 高 | 新依赖评审 |

**建议只做 R0–R2（约 3.5 天）**。完成后的能力是：新增一种格式 = 写一个 loader + 在 `formats/` 注册，不再触碰 Builder。R3 是纯减债，R5 才需要新核心。

## 9. 评分

| 维度 | 评分 | 依据 |
|---|---|---|
| 芯片层可读性 | A- | 每个解码器都有寄存器表与公式出处，注释质量高于平均 |
| 模块性 | B | `chips/` 干净，`backends/libvgm` 职责过载 |
| 新增**芯片** | A- | 解码器 + 1 个 case + 1 行 flush，可在一批 rips 内验证 |
| 新增**格式** | D | 没有 IR，必须复制 462 行分发逻辑 |
| 测试与可验证性 | A- | 单测 + `--pitch-report` + 95 首真实 rips |

一句话：**这是一次成功的 feature 实现，其可扩展性止步于「下一颗芯片」，尚未为「下一种格式」做好准备**。差距不在解码器，而在装配层缺一层 IR——R0–R2 就能补齐。

## 10. 附：与其他格式的距离

| 格式 | 现状 | 距离 |
|---|---|---|
| VGM/VGZ | 已支持 | — |
| NSF/NSFE | libvgm 不支持，需要 6502 核心 | R1 + R5；`NesApuDecoder` 已存在，缺的只是「谁来写 `$4000`」 |
| FMT / PC-98 OPNA | libvgm 支持 YM2608 | 只差 loader 与设备表，属 R1 收益最直接的一类 |
| SPC / HES / KSS | libvgm 不支持 | 各自需要 CPU 核心，原则同上 |
