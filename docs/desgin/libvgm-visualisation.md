# libvgm 的逐通道可视化：设计

> 前置：`docs/research/2026-11-01-libvgm-visualisation-survey.md`（哪些能拿、哪些拿不到）。
> 分支：`feature/libvgm-visualisation`。
> 范围：**第一版 = 键盘页的 30 行亮起来 + FM 的算子参数**。采样类芯片的真实波形排在其后。

## 0. 目标与非目标

**要做的**

- VGM/VGZ 打开后，Keyboard 页每一颗被支持的芯片每一个通道都有一行，显示名字、音高、音量、形状；
- 键盘页的行为与 NSF 一致：hover 高亮、拖拽导出、Mute/Solo 联动；
- FM 通道画出**它自己的音色**，而不是一条通用正弦。

**不做的（第一版）**

- ~~采样类芯片（C140/C352/QSound/OKIM6295/…）的**真实波形**~~ —— 这条是**第一版**的边界，现已落地：C140/C219、C352、QSound、K053260、K054539、RF5C、2A03 DPCM、OKIM6295 都能画真波形并导出，见 §9.1 的表；
- 示波器的逐通道视图；
- 新的芯片解码器（30 个之外）。

## 1. 三段管道，两段现成

```
  SongIr  { chips, writes, romBlocks }        ← VgmSongLoader，MIDI 已在用
      │  VgmReplayer                            ← 新增①
      ▼
  IChipDecoder × 30                            ← 现成
      │  channelState(i) = chips::ChannelState
      ▼
  翻译层（一张表 + 每芯片几行）                 ← 新增②
      ▼
  playback::ChannelState → playback::ChipStateSnapshot
      ▼
  键盘页 / 示波器 / 拖拽（全部现成）
```

**两个 `ChannelState` 同名不同义**，这是这一支最容易写错的地方：

| | `chips::ChannelState`（有） | `playback::ChannelState`（要） |
|---|---|---|
| 门控 | `gate` / `released` | `active` |
| 音高 | `period` + `block`，或 `hz` | `frequency` / `note` |
| 音量 | `volume` 0..15 | `volume` 0..1 |
| 形状 | `patch`（一个 int） | `waveKind` + `duty` + `shapeParameter` |
| 波形 | **没有** | `wave` |
| 轨 | 没有 | `trackIndex` |

## 2. 新增①：`VgmReplayer`——跟着播放走的寄存器游标

**位置**：`plugin/Source/backends/libvgm/VgmReplayer.h/.cpp`

**它做什么**：持有一份 `SongIr`、一组按设备分好的解码器、一个写入游标；`advanceTo(sampleTime)` 把游标推到目标时刻，`channelState(deviceId, channel)` 返回解码器的答案。

```cpp
class VgmReplayer
{
public:
    /** One song's writes, grouped per device. Built once from the IR. */
    explicit VgmReplayer (const midi::SongIr& ir);

    /** Moves every decoder forward to `sampleTime` (44100 Hz), feeding the writes it passed. */
    void advanceTo (uint32_t sampleTime);

    /** Back to the start: called on loop and on seek, so no decoder carries the old song's state. */
    void rewind (void);

    int deviceCount (void) const;
    const chips::ChannelState& channelState (int deviceId, int channel) const;
    int channelCount (int deviceId) const;
    chips::ChannelKind channelKind (int deviceId, int channel) const;
private:
    ...
};
```

### 2.1 为什么是增量，不是预计算

一首 3 分钟的歌，60 fps × 30 通道 × 每帧一个带 `vector` 的 `ChannelState`，是几十 MB；而且**每次 seek 要重建**。增量只需要一个游标，内存是"每个解码器的寄存器"。

### 2.2 三个必须写对的时刻

| 时刻 | 触发 | 做法 |
|---|---|---|
| **loop** | libvgm 的 `PLREVT_LOOP` | `rewind()`：所有解码器 `reset()`，游标归零。**现在 `handleEvent` 只理 `PLREVT_END`**（`LibvgmTrackPlayer.cpp:399`），要加一支 |
| **seek** | 用户拖进度条 | 消息线程**不碰解码器**（音频线程在跑），只设一个原子 `pendingSeekSamples`；`captureState` 看到它就 `rewind()` 再追到目标位置 |
| **首帧 / 从暂停恢复** | 游标落后很多 | 追。追不上（超过阈值）就 `rewind()` 重放；**绝不显示上一次快照的残留** |

**第三条是这一支最容易出的 bug**：解码器留着上一首歌的寄存器值，面板会画出一个"看起来很合理"的假状态。NSF 那边靠每次 `reset()` 保证，这边必须一样。

### 2.3 线程

`captureState` 在音频线程、`render` 之后、引擎锁内（`PlaybackEngine.cpp:1484-1494`）。**游标和解码器只在这条线程上访问，不需要锁**。唯一的跨线程数据是那个原子的 `pendingSeekSamples`。

**只在需要时才跑**：`stateCaptureWanted` 关着时 `captureState` 根本不被调用（引擎那一层已经判过），游标自然不推进——下一帧重新对齐即可，代价是 `rewind()` 一次。

### 2.4 多 player 实例

`LibvgmTrackPlayer` 一个实例一份 `VgmReplayer`，**惰性创建**（第一次 `captureState` 时）。引擎只问 `trackPlayers[0]`，所以实际只建一份。

**分轨隔离不影响它**：静音是 libvgm 在**播放**时做的，而 `VgmReplayer` 读的是**文件**。这一条是选"读文件"而不是"hook 渲染"的额外收益。

## 3. 新增②：翻译层

**位置**：`plugin/Source/playback/ChannelFromChip.h/.cpp`

**为什么不在 `backends/libvgm/`**（本文件初稿写的位置）：这一层只依赖 `chips::ChannelState` 和 `playback::ChannelState`，和 libvgm 没有任何关系；放在后端目录里会让 `vgm_chip_test` 去链接 libvgm，而那个测试的价值正在于**不链接**。依赖方向也支持放在 `playback/`——`playback/PlaybackEngine.cpp:12` 已经在 include `chips/ChipTypes.h`，而 `chips/` 一次都没有反向引用。

```cpp
/** The shape a channel kind makes, in `waveform::Kind` terms. */
int waveKindForChannelKind (chips::ChannelKind kind);

/** One channel's state, translated for display. */
ChannelState channelFromChip (const chips::ChannelState& chipState,
                              const chips::NoteTable& noteTable,
                              const ChipShapeHints& hints,
                              std::string chipName,
                              std::string channelName);
```

`ChipShapeHints` 是每颗芯片**一次**写好的静态信息：

```cpp
struct ChipShapeHints
{
    int waveKind = 0;          // playback::waveform::Kind，以 int 存放（ChannelState 就是这个类型）
    float duty = 0.5f;         // 除 pulse 外都被忽略
    int volumeMaximum = 15;    // chips::ChannelState::volume 的上限，多数芯片是 4 位
};
```

**每颗芯片一行 `ChipShapeHints`**，加芯片就是加一行。

**`noteTable` 为什么是参数而不是从解码器问**：`chips::ChannelState` 只带 `period`/`block`（period 阵营）或 `hz`（frequency 阵营），而 period 变成 Hz 必须经芯片自己的表——同一个 period 在 CPU 分频和 FM 上完全不是一个音。表由解码器提供（`IChipDecoder::noteTable`），转换层只是用它。

### 3.1 `ChannelKind` → `waveKind`

| `waveKind` | `ChannelKind` |
|---|---|
| `pulse = 2` | `nesPulse`、`gbPulse`、`vrc6Pulse`、`mmc5Pulse`、`aySquare`、`sn76489Square`、`saaSquare`、`pokeySquare` |
| `triangle = 3` | `nesTriangle` |
| `noise = 4` | `nesNoise`、`gbNoise`、`ayNoise`、`sn76489Noise`、`wsNoise` |
| `delta = 5` | `nesDmc` |
| `saw = 6` | `vrc6Saw` |
| `fm = 7` | `vrc7Fm`、`fm`、`fmDrum`、`opnAdpcmB` |
| `stored = 1` | `fdsWave`、`n163Wave`、`c6280Wave`、`sccWave`、`x1Wave`、`gbWave`、`wavetableWave`、`wsWave` 及全部采样类 |

**`stored` 这一类第一版没有 `wave` 数组**，`cycleFor()` 返回空，波形区画"没有周期"。**行仍然是亮的**（名字、音高、音量都对），只是波形区承认"这需要样本字节"。

### 3.2 `patch` 怎么用

`chips::ChannelState::patch` 的定义是"duty / waveform / drum id"，**每种芯片含义不同**，所以它不能用一条通用规则翻译，只能按芯片处理：

| 芯片类 | `patch` 是 | 翻成 |
|---|---|---|
| 脉冲类 | duty 的寄存器值 | `duty = patch / (2^n - 1)` |
| 噪声 | period 0..15 | `shapeParameter` |
| FM | 音色号 | 忽略（形状来自算子参数） |
| 采样 | 样本号 | `shapeParameter` |

**这张表就是"每芯片几行"的地方**，也是 `channelFromChip` 唯一需要分支的地方。

## 4. 新增③：FM 的算子参数

**三个解码器各加四个字段**（`OpnDecoder`、`OpmDecoder`、`OplDecoder`）：

```cpp
// chips::ChannelState 里加，而不是每个解码器自己加，因为翻译层要统一读
int fmMultiple = 0;   // 调制器倍频，0..15
int fmLevel = 0;      // 调制器总电平，0..63
int fmWaveform = 0;   // 载波波形选择，0..3
int fmFeedback = 0;   // 载波自反馈，0..7
```

**为什么放在 `chips::ChannelState` 而不是解码器的私有成员**：翻译层只认这一个结构体，多加一个"问解码器要 FM 参数"的虚函数会让 30 个解码器都要实现一个空实现。

翻译时：

```cpp
state.shapeParameter = playback::waveform::packFm (chipState.fmMultiple, chipState.fmLevel,
                                                   chipState.fmWaveform, chipState.fmFeedback);
```

`packFm()` 和 `cycleFor()` 的 `fm` 分支**都已存在**（NSF 的 VRC7 在用）。

**代价**：三个解码器的寄存器解析里本来就读过这四个值（算音高必需），现在留下来即可；但**要逐个核对寄存器含义**——OPN 和 OPL 的倍频表不同，OPL 的 feedback 在寄存器里的位置也和 OPN 不一样。

## 5. 名称与 `trackIndex`

### 5.1 通道名

`chipName` 来自 `ChipShapeHints` 之外的一张名称表（第 5 步）；同型号两片时带序号：

```cpp
// LibvgmSongInstance.cpp:413 已有同样的写法
groupName << " #" << instanceNumber;   // "YM2612 #2"
```

**这个名字会进拖拽导出的文件名**（`YM2612 #2_FM 3_2026_1101_082356.wav`），所以 `legalFileName()` 会处理 `#`。**已验证**：`legalFileName` 对空格和特殊字符都做替换。

### 5.2 `trackIndex`

`TrackInfo` 已经是一通道一条（`LibvgmSongInstance.cpp:419-431`）：

```cpp
track.deviceId = device.id;
track.channelIndex = channelIndex;    // 联动芯片的通道号接在父芯片之后
```

所以 `trackIndex` 直接由 `(deviceIndex, channelIndex)` 查 `tracks` 得到。**`playback::ChannelState::trackIndex` 的用途是"这一行可以 Mute/Solo"，不是过滤显示**（`ChipState.h:70` 的注释），所以键盘页显示全部通道、和 NSF 一致。

## 6. ADR

### ADR-001：读文件，不 hook libvgm 的渲染

**决策**：用 `SongIr` + `IChipDecoder`，不用 libvgm 的设备写回调。

**为什么**：libvgm 的事件回调只报 `END`/`LOOP`/`SEEK`/速率变化（`LibvgmTrackPlayer.cpp:397`），要拿到逐寄存器写只能改 `libs/` 下的第三方源码。

**代价**：要自己维护"写游标 ↔ 播放位置"的对应（§2.2）。**换来的是**：和已经跑通两个功能的管线同源，且不随 libvgm 升级而失效。

### ADR-002：增量游标，不预计算时间线

**代价**：loop/seek 的重置逻辑要写对（三个时刻，§2.2）。**如果写错，症状是"面板显示上一秒的状态"**，而这种错误看起来像 UI 卡顿，不像状态机错。

### ADR-003：解码器放在 player 里，惰性创建

**备选**：放在 `LibvgmSongInstance`（一份，所有 player 共享）。

**不选的理由**：`captureState` 是 `ITrackPlayer` 的接口，song instance 拿不到"该不该捕获"。放 player 里让"捕获"这件事自包含。

**代价**：隔离模式重建 player 时重建一份（`reset()` 一次，代价可忽略）。

### ADR-004：`stored` 类第一版不填波形

**代价**：11 颗采样芯片与 8 颗波表芯片的波形区是占位。**换来的是**：第一版能在一次改动里让 30 行全亮，而不是被"每个芯片都要去找样本字节"拖住。

### ADR-005：FM 参数现在就做

**代价**：三个解码器各多四个字段、要逐芯片核对寄存器。**换来的是**：MD/街机这批最常听的曲子里，FM 通道画的是它自己的音色。

### ADR-006：波形按需计算

沿用 NSF 的语义：`captureState` 有一个 `includeWaves` 默认参数的版本。**波形每帧算 30 × 256 点不便宜**，只有键盘页可见时才算。

**代价**：`LibvgmTrackPlayer` 的这个方法**不能 override**（`ITrackPlayer` 的签名只有一个参数），所以它和 NSF 一样是"派生类的新重载"，由 `ITrackPlayer::captureState` 的实现转发过来——**或者干脆第一版就算波形，等实测嫌慢再加开关**。这一点留到实现时按实测决定。

### ADR-007：通道空闲时保留上一次采样（2026-11-05）

行画的是**当前**状态，而采样一停 `wave` 就为空——格子变空白，也就无法拖拽。GA20 把它暴露得最彻底：它的采样只有 60 ms 量级，几乎全程都在空档；K007232 动辄 0.6–3 秒，所以看起来"一直在"。

**决定**：在 `LibvgmTrackPlayer::captureState` 里按 `(设备, 通道)` 锁存**上一对完整的 `(wave, rate)`**，当前状态不成对时回放它。一处实现，所有采样芯片统一。

三条附带规则，每条都有理由：

- **必须成对**。只留波形、速率归零，`WaveExport::canExport` 会拒绝导出——格子看着能拖，拖出来是废文件。GA20 恰好会出现"有波形、速率 0"的中间态（voice 已启动、gate 已关闭），所以判定写成"这一对不完整就回退"。
- **`active` 不动**，保持态因此自动走暗色（`active ? 0.9 : 0.35`），不假装在响。
- **跳转与换歌清空**（`pendingRewind` / `setReplaySource`）：跳转后上一个采样属于已经离开的位置。**静音不清空**——静音不改时间位置，清掉反而让你在静音状态下无法拖拽。

**代价**：行的波形不再严格等于"此刻正在响什么"，而是"此刻或最近响过什么"。这个模糊性用亮度区分来抵，并写在这里，免得下一个人以为是 bug。

**验证**：探针 `-4`（`active=0` 的行仍带完整的一对）。**未进 ctest**——应用 harness 的引擎没有显示状态来源（`assert engine wave.<n>` 读到 `0 of 0 channels`），这是已知缺口。

## 7. 可测试性

按规则 6，纯逻辑必须能被调用：

| 单元 | 测试 |
|---|---|
| `VgmReplayer::advanceTo()` | 给定 `SongIr`（手工构造），推到一个时刻，断言 `channelState` 的 period/gate |
| `VgmReplayer::rewind()` | 推进、rewind、再推进到同一时刻，两次状态相同 |
| `channelFromChip()` | 每个 `ChannelKind` 一条，断言 `waveKind`/`duty`/`volume` |
| `ChannelKind` → `waveKind` 表 | **按整数值穷举**（`unknown` 之前每一个都必须有答案），不能用手写列表——列表漏一个值不会有任何提示 |
| FM 参数打包 | `packFm` 往返 |

**最后一条值得单独写**：用 `switch` 而不是查表，编译器会对未覆盖的枚举值报警；表漏一个值不会有任何提示。

## 8. 验证方式

**三块分开写，因为它们的证据不同。**

| 内容 | 谁验 |
|---|---|
| 游标、翻译层、FM 打包 | **单元测试**（上表） |
| `46/46` 不回归（MIDI 输出、导出、拖拽） | `ctest` |
| **键盘页真的画出 30 行、波形像不像** | **你手工验**（harness 到不了，见 `docs/desgin/wavetable-drag-export.md` §8） |

**第三条的理由已经踩过一次**：`engine play` 加 `tick` 不会让音频线程走完整一帧，快照里没有通道，而窗口上画着十二行。**一张空截图会被读成"功能坏了"**，所以这一块不写脚本名，直接写"由你打开 dist 看"。

**看什么**：

1. 打开一个 MD 的 `.vgm`，切 Keyboard：应该有 YM2612 的 6 个 FM + 1 个 DAC 行
2. 播起来：行的音高跟着变，音量条跟着动
3. **同一颗芯片的两个 FM 通道，波形形状应该不同**（这是 ADR-005 唯一能验的地方）
4. 静音 Mixer 里的某一个 track，键盘页那一行应该暗下来
5. 拖拽一个 FM 行出去，Serum 应该能读（第一版 FM 是 `fm` 形状，`stored` 类此时还拖不出内容——见限制 2）

## 9. 已知限制（第一版）

### 9.1 采样与波表的覆盖状态

`wave` 与 `sampleRate` 是"画不画得出真波形、拖不拖得出来"的两个字段。**只有显式填了它们的解码器才有真波形**；其余的画占位，拖拽被拒绝（`writeChannelShape` 返回 `nothingToWrite`）。

**这张表按 `SongIr.h` 的 `ChipKind` 逐项列出，不按记忆或上一版抄。** 上一版漏了四颗采样芯片（RF5C164、YMZ280B、MultiPCM、K007232）和一颗波表（ES5503），还把 `FDS`、`N163` 写进了 VGM 行——那两颗不在 `ChipKind` 里，由 NSF 后端加载，见本节末。

| 类别 | 芯片 | `wave` | `sampleRate` | 整曲导出 |
|---|---|---|---|---|
| 采样 | 2A03 DPCM | ✓ | ✓ | ✓ `collectNesDpcmSamples` |
| 采样 | C140 / C219 | ✓ | ✓ | ✓ `collectC140Samples` |
| 采样 | C352 | ✓ | ✓ | ✓ `collectC352Samples` |
| 采样 | QSound，16 个 PCM voice | ✓ | ✓ | ✓ `collectQSoundSamples` |
| 采样 | QSound，3 个 ADPCM voice | ✓ | ✓ | ✓ `collectQSoundSamples` |
| 采样 | K053260，8 位 PCM | ✓ | ✓ | ✓ `collectK053260Samples` |
| 采样 | K053260，KADPCM | ✓ | — | ✓ `collectK053260Samples` |
| 采样 | K054539，8 位 / 16 位 PCM 与 4 位 DPCM | ✓ | ✓ | ✓ `collectK054539Samples` |
| 采样 | RF5C68 / RF5C164 —— **搁置**，见 §9.5 | ✓ | ✓ | ✓ `collectRf5cSamples` |
| 采样 | GA20 —— 见 §9.8 | ✓ | ✓ | ✓ `collectGa20Samples` |
| 采样 | OKIM6295 —— 见 §9.6 | ✓ | ✓ | ✓ `collectOkim6295Samples` |
| 采样 | **SegaPCM** | — | — | — |
| 采样 | **YMZ280B** | — | — | — |
| 采样 | MultiPCM（YMW258）—— 见 §9.10 | ✓ | ✓ | ✓ `collectMultiPcmSamples` |
| 采样 | K007232 —— 见 §9.7 | ✓ | ✓ | ✓ `collectK007232Samples` |
| 采样 | **OPN-ADPCM**（YM2608 / YM2610） | — | — | — |
| 采样 | **Y8950-ADPCM / ADPCM-drum** | — | — | — |
| 波表 | VSU —— 见 §9.11 | ✓ | ✓ | ✓ `collectVsuSamples` |
| 波表 | ES5503 —— 见 §9.9 | ✓ | ✓ | ✓ `collectEs5503Samples` |
| 波表 | HuC6280、SCC（K051649）、X1-010、WonderSwan、Game Boy 波表、Mikey、MSM5232、NES APU、K005289 | — | — | — |

**不在本表的两类**：PSG 与 FM（AY、SN76489、SAA1099、POKEY、OPL 系、OPN 系、OPM、OPX 等）本来就用 `cycleFor` 画阶梯，不经过 `state.wave`；`FDS` 与 `N163` 由 NSF 后端（`NsfBackend` / `NsfBus`）加载，没有对应的 `ChipKind`，要单独处理。

**填一颗芯片的代价**：`attachMemory` 的通道已经通了（`407c0b1`），每颗只需把"寄存器 → 字节区间"翻译出来挂到 `state.wave`。**C140 那次约 50 行**，其余同量级。

**整曲导出是另一件事**：`ChipState::wave` 只有"当前在播的那个样本"，导出要的是"整曲所有样本"，所以每颗芯片还要一个收集器（`NesDpcmBank` / `C140Samples` 的形状）。

### 9.2 其余限制（仍然成立）

1. **FM 的波形是"这一个音色"的静态曲线**，不反映 LFO 与包络——`cycleFor` 只画一个周期。
2. **VGM 里没有通道概念**。通道是解码器从寄存器推出来的；某个驱动用非常规方式写寄存器时，行会显示得不准，而这种不准**不会报错**。
3. ~~**性能未实测**~~ —— **已测（2026-11-04）**：`vgm_render_probe <song> -3 <秒>`，Release `/O2`，512 帧一块（预算 10.67 ms）：

   | 曲子 | 捕获关 | 捕获开 | 捕获的代价 |
   |---|---|---|---|
   | QSound `01 QSound Logo` | 4.5× | **4.5×** | 1.4% |
   | C352 `09 Ship Captured` | 11.2× | **10.5×** | 5.6% |
   | C219 `02 Knuckle Heads` | 40.7× | **38.3×** | 6.0% |
   | RF5C164 `01 Palmtree Panic` | 137.5× | **132.3×** | 3.8% |
   | K007232 `02 Title Demo` | 409.2× | **368.5×** | 9.9% |

   **结论：显示不是瓶颈。** 最慢的一首（QSound）用掉 22% 的实时预算，仍留 4.5 倍余量。

   **但表中最后一列要按绝对值读，不能按百分比读。** 它比的是"捕获 vs 不捕获"的**墙钟**，而渲染快的曲子分母很小：K007232 的 9.9% 是 2 ms 摊在 750 块上，QSound 的 1.4% 是 25 ms 摊在同样的块数上。换成每块的绝对开销：

   | 曲子 | 捕获的绝对开销 | 占 10.67 ms 预算 |
   |---|---|---|
   | C352 | 56 µs/块 | 0.53% |
   | QSound | 33 µs/块 | 0.31% |
   | C219 | 17 µs/块 | 0.16% |
   | RF5C164 | 2.7 µs/块 | 0.03% |
   | K007232 | 2.7 µs/块 | 0.03% |

   **最大 0.53%，即 1/190 的预算。** 而渲染本身（QSound 核心）才是 22% 的那一项，它在可视化之前就存在。

   **因此 ADR-006 那条"如果掉音，第一刀砍波形计算"没有依据**：波形计算不在这 0.53% 里占主要部分，真要优化该先看核心本身。

   **这台机器、Release、单线程离线**：更慢的机器按比例缩放，QSound 在 4 倍慢的机器上会落到 1.1×——那时候瓶颈是 QSound 核心，不是可视化。
4. **每个采样芯片各写一份收集流程**：`NesDpcmBank` 与 `C140Samples` 已经是近似副本，剩下 9 项采样、11 项波表会继续复制。**这是本支最大的一笔结构债**，见 `docs/memo/2026-11-01-code-review-libvgm-visualisation.md` 问题 4。**`ef4e2b0` 之后这笔债已还**：去重、解码、命名、缓存进了 `SampleCollector`，一颗芯片现在只剩"它的寄存器怎么指样本"。C352 与 QSound 都是这样接的，各约 150 行，其中一半是注释。

5. **一行的画法取决于数据形状，不是有没有数据**：录制（`sampleRate > 0`）画填充带，波表与合成画阶梯。判据为什么是这样、以及它被改坏过三次的过程，见 `docs/memo/2026-11-01-what-a-waveform-row-is-a-picture-of.md`。

### 9.3 已关闭：QSound 的 3 个 ADPCM voice（`7240d49`）

**曾记的障碍**：没有能验证的曲子。`01 QSound Logo` 的 6757 条 `0xC4` 里，ADPCM 参数寄存器写了 5 次、启动 `0xD6`–`0xD8` **0 次**。

**关闭的方式不是找到曲子，而是自己造一个**：`tools/make_qsound_adpcm_probe.ps1` 生成 454 字节的 VGM——256 字节 ROM（`0x88`/`0x77` 交替，4 位字段能给出的最大摆幅）、四个 ADPCM 参数、启动标志、两秒等待。载入后得到 `qsound_adpcm_000010_128`：128 字节、256 个电平、约 8 kHz（ADPCM 的固定率，不是音高寄存器）。

**这证明了整条链路**：header、数据块、寄存器、启动、切字节、解码、命名。**没有证明的**是解出的形状与硬件一致——生成样本没有对照渲染。

**格式要点**：和两个 Konami 的 4 位格式都不同，它的步长是随信号涨落的自适应值（`adpcm_update` 的 `step_size`，表有 16 项按 `8 + step` 索引），所以同一个 nibble 在不同位置代表不同距离。另外**高 nibble 在前**（两个 Konami 格式都是低的在前）。

**探针本身不入库**（`test_file/` 被忽略），脚本入库。

### 9.4 已关闭：K053260 的 KADPCM 导出（`1a77f00`）

**曾记的**：显示已做、导出未做，只差一个 `SampleFormat` 值；**并且这里写着"验证素材现成"——那是错的。**

**关闭时查证**：`01 The Declaration of War` 里 `0x2A` 一次都没写，KADPCM 与 loop 位都从未设置，而它是 `test_file` 里唯一的 K053260 文件。**没有曲子能让它出声，和 §9.3 一样。**

**做法**：加 `SampleFormat::kadpcm4`。两张表的差别只有一项，而这一项决定不能共用：

```
kKadpcmTable  第 8 项是 -128     ← 单步最大，累加会冲出 16 位，必须 clamp
kDpcmTable    第 8 项是   0      ← 累加有界，不需要
```

**验证方式改成构造断言**：八个 nibble 全是 `8` 时必须向负方向移动；用错表则不动。**这是这个分支里"一个断言比一个对照音频更能说明问题"的那一类。**

**仍未验证的**：解出的形状是否与硬件一致。能证明的是寄存器读对、字节切对、解码器跑通、速率来自芯片而不是编的。

### 9.5 搁置：RF5C68 / RF5C164（2026-11-04）

**状态**：代码在，功能可用，但**“导出的字节就是通道正在播的那一段”这一条没有被证明**，因此整行搁置。

**现象**：`01 - Palmtree Panic Zone Past` 的 Ch5，渲染出 0.491 s / 71.5 Hz 的乐音，该槽位导出的是 5186 字节、周期 12 字节（r=0.31）的密集波形——听感上是“慢放的鼓”。71.5 Hz 在 10442.7 B/s 下应有 146 字节周期，**12 与 146 相差 12 倍**。

**搁置的原因不是“没查”，而是“查不动了”**：十一次改动，每次都有测量支撑，每次都被下一个测量推翻；最后一组读数是芯片九个实例的输出能量全为 0，而引擎仍在出声——这四项（唯一设备、无 DAC 流、无其他设备、引擎有声）里必有一项测错了，而现场已经无法靠 `printf` 分辨是哪一项。

**这一行没有完成的含义**：其余六颗采样芯片的“寄存器 → 字节区间”路径是逐颗对照渲染验证过的；RF5C 没有。**它的存在不否定其他行，但它自己不能算 ✓。**

**重新开工前必须知道的**：

1. 先读 `docs/memo/2026-11-04-the-sample-that-was-never-the-one-playing.md`，那里有确凿项、十一次失败的原因和八类测量错误。
2. 不要先改导出、长度或速率——五个改动都走了那条路，没有一个带验证条件。`3c0479f`、`2131691` 因此**待复核**。
3. 唯一还没做的测量：在 `LibvgmTrackPlayer::render` 与 `SCD_PCM_Update` 里**同一个 block** 同时统计输出能量。它有两个穷尽结果，没有第三种。
4. 工具已备好且校准过：`vgm_render_probe <song> -2` 先跑校准，必须读出 `-3776 3776 -3776 3776`，否则探针无权说“芯片静音”。

### 9.6 已关闭：OKIM6295（2026-11-05）

**第一颗真正做完的采样芯片**（相对 §9.5 搁置的 RF5C164）。两次交付都被耳朵否掉，两次的根因都是
**同一段算术在两个地方各写了一遍**：

| 轮次 | 现象 | 根因 |
|---|---|---|
| 一 | 菜单把底鼓拆成两半，拖拽是完整的 | 地址单位：`start/stop` 是**字节**，样本是 `2×(stop-start+1)` 个 nibble；收集器按 nibble 解释，导出少一半 |
| 二 | 音高快 1.6 倍 | `×2`（每个输出样本只有一个 nibble，速率已按 nibble 计）+ 分频比（pin 7 的 132/165，文件从不声明）；`2 × 1.25 = 1.6` |

**收敛的方式**：让两条路径用同一个数，而不是各自算对。

- 长度：与行同一套算术，而不是"我以为表该这么读"
- 速率：`PLR_DEV_INFO::smplRate`（设备自报）→ `ChipConfig::deviceRateHz`，两边共用
- 验收：探针 `-4` 打印**播放中**的行，与菜单导出逐点逐赫兹比对

```
行  (Ch 1)   wave = 3714 points @ 7747.0 Hz
菜单         okim_002_3714      @ 7747.0 Hz
```

**四道坑的完整记录与"下一颗芯片怎么照做"的九步清单**：`docs/memo/2026-11-05-okim6295-four-traps-and-a-sample-that-kept-changing-size.md`。

### 9.7 已关闭：K007232（2026-11-05）

**四颗采样芯片里最省事的一颗**，因为它不需要表、也不需要压缩格式：

- 长度**在数据里**：`k007232.c:246` 在遇到 **bit7 置位**的字节时重播该 voice，所以那个字节是结束符，不是电平。实测 `02 Title Demo` 的 4752 字节里有 2 个、5557 字节里有 1 个——若为 8 位有符号，应约有一半。
- 格式**不压缩**：`(value & 0x7F) - 0x40`，7 位无符号，`0x40` 是静音（`k007232.c:232`）。首个字节正是 `0x40`。
- 地址 = **bank + start**：`start` 十七位，`0x14`/`0x15` 各给一个 voice 的 bank，左移十七位。实测 `02 Title Demo` 写 `0x14=1, 0x15=3`，正好落在 ROM 块 0x20000 与 0x60000——**同一个坑在 RF5C 上以隐藏补丁的形式出现过，这里是寄存器，可观测、可断言**。

**三处必须与驱动行为对齐，否则导出的数量或速率一定错**：

| 现象 | 原因 | 处理 |
|---|---|---|
| 起始地址每字节写一次，导出出现 `...064B3` 与 `...064B6` 这种差 3 字节的重复 | 三个地址字节是分开写的 | 只在**高字节**（`0x04`/`0x0A`）写入时算一次 key-on |
| 大量样本速率是 218.5 Hz（step≈0 的速率） | 驱动**先写地址、后写音高** | 音高写入时**刷新**已登记样本的速率；key-on 时也刷新 |
| 同一段字节出现两个同名文件 | 两颗芯片共用同一张映像，我却在 key 里带了 deviceId | key 只按 `(bank, start)` |

**已知限制（不是缺陷，是芯片行为）**：驱动会在**一个音符中途改音高**——`02 Main BGM` 的 Ch5 在 2 s 读到 10528 Hz、5 s 读到 18643 Hz。一个 WAV 只能声明一个速率，导出取**最后一次**写入的值；行在某一时刻可能与文件不同。同一段字节在别的曲子被别的 step 播放时，同理只有一个文件。

**验收**：`02 Title Demo` 四个声部的点数与行**逐点一致**（3221 / 5556 / 29453 / 11125）；速率 2/4 精确一致，另两个落在上述"中途改音高"里。六首曲子导出 6–19 个样本，46/46 通过。

**交付后的第一个缺陷（2026-11-05，用户报告）**：导出的 WAV 无法打开。文件本身结构合法，问题在内容——`data` 是 524288 字节（131072 点，**正好是 128 KB 窗口上限**），`sampleRate` 是 **218 Hz**。两个数字指向同一件事：**这条样本是半个地址**。当时只在地址**高字节**写入时触发导出，而驱动写三个地址字节的顺序并不保证；写在高字节与另外两个字节之间落下时，我拿到了一个拼起来的地址——窗口里没有结束符，于是读到上限；音高也是上一个音符的。

**两处结构性修正**：

1. **地址序列由「下一个其它写入」收尾**，不再假定某个字节是最后一个。这样与写入顺序无关，半截地址再也不会被当成样本。
2. **找不到结束符就不导出。** 真实样本一定以 bit7 字节结束；读到窗口上限说明地址读错或该窗口是填充。写出去的结果正是那个打不开的 524 KB 文件——**不写，才是诚实的答案**。

修正后同一首歌的 `3_064B3` 速率从 10528 变成 **9943.1**，与行的 9943.2 精确一致；六首曲子的最低速率从 218 Hz 回到 4685 Hz，最长样本从 131072 点回到 43472 点。

**第二个交付后缺陷（同日）：音高全部超出键盘范围。**

`state.hz` 被设成了 `frequencyOf()`——那个函数算的是**每秒消耗多少字节**（9943–29829 B/s），
对应 MIDI **123–135**，而键盘只画 **36–83**。**这不是键盘的问题，是这一行没有描述音高**：采样
芯片知道播放速率，音高必须**从采样里量**（`hz = 字节率 ÷ 周期`），OKI、ES5503、RF5C 都这么做，
只有这颗漏了。

改成 `sampleInferred` 之后：hz 79.9–261.6 → MIDI **39–60**，落在键盘内；鼓无周期 → hz 0、无音符。
`bestPeriod()` 同时提到 `Pitch.h` 共享（此前 ES5503 / GA20 / OKI / RF5C 各有一份拷贝）。

**完整记录**：`docs/memo/2026-11-05-k007232-the-length-in-the-data-and-the-rate-that-was-not-a-pitch.md`。

### 9.8 已关闭：GA20（2026-11-05）

三颗采样芯片里**芯片本身最省事**的一颗：没有采样表、没有 bank、没有压缩格式。要点只有两条，
而且都是"结束符在数据里"的变体：

- **`0x00` 即结束**（`iremga20.c:174`），该字节不发声；`0x00` 在 50 KB ROM 里出现 52 次，
  正是一段一个
- **8 位无符号、`0x80` 为中点**（`iremga20.c:177`），所以它需要自己的 `SampleFormat::ga20`，
  不能落进默认的 `int8` 分支
- 长度寄存器（2、3）**每个驱动都写、核心从不读**——按它切长度会全部错

**真正的两个问题都在显示层**，不在芯片里：

1. **探针把 `null` 与 `0 pts` 打印成同一个 `0`**，我据此误判"解码器没出波形"，还把 **YM2151**
   的行当成了 GA20 的行。工具自身的歧义会制造假象，比没有诊断更贵。
2. **通道空闲时格子空白、无法拖拽**——GA20 的采样只有 60 ms 量级，几乎全程都在空档。
   修法是全芯片统一的：在 `LibvgmTrackPlayer::captureState` 锁存 **`(wave, rate)` 这一对**，
   没有就回放上一次；`active` 不动，所以保持态画成暗色；跳转与换歌时清空。
   决定与代价见 **ADR-007**。

```
t=15.0  [0] GA20 Ch1 wave=2126 pts rate=17209.3 active=0     ← 保持态，可拖拽
```

**完整记录**：`docs/memo/2026-11-05-ga20-a-zero-byte-that-means-stop-and-a-cell-that-went-blank.md`

### 9.9 已关闭：ES5503（2026-11-07）

**唯一一颗“波表”芯片，也是唯一一颗速率会被别的通道改变的芯片。**
`es5503_get_sample_rate` 是 `(clock / 8) / (oscsenabled + 2)`，而 `oscsenabled` 由寄存器 0xE1
控制——**一个通道开关，全芯片的音高一起变**。这决定了三件事：

| 量 | 公式 | 来源 |
|---|---|---|
| 芯片采样率 | `(clock/8) / (enabled + 2)` | `es5503.c:189` |
| 一个振荡器每芯片样本读多少字节 | `freq / 2^(9 + resolution - size)` | `es5503.c:287`、`resshifts` |
| 音高 | 上者 ÷ 表内**实测**周期 | 表可以装多个周期（`testEs5503TableCycle`） |

**数据格式与 GA20 相同**：8 位无符号、`0x80` 中点（`es5503.c:310`），且 **0x00 即停止**
（`es5503.c:301`，任何模式）。因此导出侧新增的是 `SampleFormat::es5503` 这个名字，**解码实现与
`ga20` 共用一个分支**——两份拷贝就是两次分叉的开头。

**RAM 是加载期映像，不需要重放。** `0xE1` 属于 RAM 块（0xC0+），载荷以 **32 位地址**开头
（不是 ROM 的 size/offset），`memory::ram` 已经按地址装配。实测三首曲子的块全部写在第一个
`0xD5` 之前（首个 `0x67` 在 224 字节，首个 `0xD5` 在 5388），所以静态映像与芯片看到的一致。
将来若遇到边播边写 RAM 的曲子，需要 RF5C164 那套按流序重放——解码器的 `attachMemory` 也表达不了，
所以这是**已知限制**，不是这一颗引入的新缺陷。

**验收**（三首，行与菜单逐项比对）：

```
01 Title Screen  4 个表   6682.9 – 361542.7 Hz
02 Danger Nearby 1 个表   37835.3 Hz
03 Game Over     1 个表   481937.0 Hz
```

每一个导出速率都在某个时刻的行里出现过（同样的点数、同样的赫兹）。`481937 Hz` 看着惊人，
但它是 `enabled = 1` 时 `298295 Hz × 1.6 字节/样本` 的真实结果，行也这么报。

**交付后用户实测一次通过**（2026-11-07），没有返工。前三颗的返工，诱因都是“按常识推测了一个数”；
这一颗的每个量都从核心源码抄下来，包括**谁在改它**（0xE1 会改所有声部的速率）。

### 9.10 已关闭：MultiPCM（YMW258，2026-11-07）

**第一颗“长度写在表里”的采样芯片。** ROM 开头是每样本 12 字节的元数据
（`multipcm.c:281-298`）：起始地址（22 位，bit22 表示 12 位打包格式）、循环点，以及**长度的
二进制补码取反**。所以它既不需要找结束符，也不会把后一个样本的头接上来——长度本身是精确的。

**三处必须与核心一致，否则导出的字节或速率一定错**：

| 项 | 事实 | 出处 |
|---|---|---|
| 芯片速率 | 设备自报 **44444 Hz**；`clock / 224` 给的是 35714，**两者不同，用设备值** | `multipcm.c:598` + `smplRate` |
| 声部速率 | `(1024 + pitch) / 1024 × 2^(octave − 1)`，**octave 1 才是 1 倍** | `multipcm.c:671`、`314-324` |
| bank | VGM `0xC3` **不进芯片**，libvgm 转成 0x10/0x11/0x12 的写入，再按 `start` 的 bit19/20 选窗口 | `Cmd_YMW_Bank`、`multipcm.c:733-743` |

`0xC3` 是本轮最像 K007232 的地方，但它走的是**命令**而不是寄存器：读取器新增了 `0xC3` 分支，
加载器把它翻译成寄存器写，解码器与收集器于是看到同一条普通写入流。三首曲子的实测值
`mask=0x03, val=0x20` → 1 MB 模式 → `reg 0x10 = 2` → `bank0 = 0x200000`、`bank1 = 0x280000`；
元数据里最大 `start = 0x1FF24B`（bit20 与 bit19 都置位）→ 落到 `bank1`，导出地址 `0x2FF24B`。
**不做这一步，第二个兆字节的样本会播放第一个兆字节的字节。**

**顺带修正了一个旧缺陷**：解码器以前把 octave 当 `2^octave`，比核心高一个八度。现在与核心
同式，并给出 `hz = 播放速率 ÷ 表内实测周期`（`sampleInferred`），与 K007232 的口径一致。

**12 位格式不导出**：`multipcm.c:849-864` 把两个样本打包进三字节，17 首素材里**没有一个**用到。
按“没有结束符就不导出”的同一条原则，宁可不写，也不写出不是那个声音的字节。

**验收**：

```
17 首 / 255 个样本，单曲 1–19 个；速率 11111 – 132290 Hz；最长 60388 点（< 65536 的表上限）
01 Advertise：全曲每 0.5 秒扫一遍行，17 个导出项的 (点数, 赫兹) 17/17 命中
```

**完整记录**：`docs/memo/2026-11-07-multipcm-the-length-in-a-table-and-a-bank-that-arrives-as-a-command.md`

### 9.11 已关闭：VSU（2026-11-07）

**第一颗没有 ROM 的波表芯片。** 波形不是数据块，而是**寄存器写进 RAM** 的：五张表、每张 32 个
六位条目（`vsu.c:116`），播放时 `WD = WaveData[RAMAddress][WavePos]` 再 `WD -= 0x20`
（`vsu.c:386`、`402`）——六位无符号，**0x20 为静音**。所以既不需要 `memory::rom`，也不需要
数据块装配，`kRequirements` 保持只有时钟。

| 项 | 事实 | 出处 |
|---|---|---|
| 声道 | 6（第 6 个是 LFSR 噪声，无波形） | `vsu.c:379-386` |
| 波表 | 5 × 32 × 6 位 | `vsu.c:116` |
| 表选择 | **寄存器 0x6**，`RAMAddress = V & 0xF`（>4 播放静音） | `vsu.c:358`、`383` |
| 频率 | `2048 - freq` 个时钟走一格，`freq` 11 位 | `vsu.c:266`、`302-322` |
| 时钟 | 头部 **0xC4**，实测 5 000 000 | VGM 头 |

**导出的是“一个周期”，不是“一个样本”**：32 个条目就是整张波，速率 = 每秒走多少条目
（`clock / (2048 - freq)`），所以拖出来的文件在这个音高上无缝循环。

**这颗芯片的坑不在芯片里，在地址上：两个地址空间共用同一个低字节。**
VGM 偏移 < 0x100 是波形 RAM（5×32），0x100..0x17F 是声道寄存器；libvgm 先 `<<2` 再 `& 0x7FF`
（`vsu.c:213-214`）。我们的读取器只保留了低字节，于是**波形上传被当成了控制寄存器写**——
`0x10` 的波形字节会落成声道 1 的 IntlControl，一次上传就可能把某个声部“点亮”。

修法是三段串联，缺一段都不行：

```
读取器    把偏移的 bit 8 存进 data2（原来直接丢掉）
加载器    data2 → RegisterWrite.port（0 = 波形，1 = 寄存器）
路由器    把 port 传给 VsuDecoder（原来这一层把 port 丢了）
```

第三段是这次暴露问题的关键：只改前两段时，**行的波形全部变空、音高全灭**——因为所有写入都
被当成波形字节，没有一次 key-on。行的突然消失比“从来不对”更容易定位，这正是它有用之处。

**验收**：

```
03 Title：全曲每 0.5 秒扫一遍行，4 个导出项的 (32, 速率) 4/4 命中
30 首 / 87 个样本，单曲 1–5 个；速率 2495 – 53191.5 Hz；零样本曲目 0
```

**完整记录**：`docs/memo/2026-11-07-vsu-a-low-byte-that-two-address-spaces-share.md`

## 10. 实施顺序

五步，每步可独立提交、独立验证：

| 步 | 内容 | 能验什么 |
|---|---|---|
| **1** | `ChannelKind` → `waveKind` 表 + `channelFromChip()` + 测试 | 单元测试，不接 UI |
| **2** | `VgmReplayer`（游标、rewind、喂写）+ 测试 | 单元测试，手工构造 IR |
| **3** | 三个 FM 解码器加四个字段 + 打包 | 单元测试 + MIDI 的 46/46 不变 |
| **4** | 接到 `LibvgmTrackPlayer::captureState`（含 loop/seek） | **你手工看键盘页** |
| **5** | 名称、`trackIndex`、Mute/Solo 联动 | **你手工看** |

**第 4 步是分水岭**：之前的每一步都不影响运行时行为，第 4 步开始键盘页会变。
