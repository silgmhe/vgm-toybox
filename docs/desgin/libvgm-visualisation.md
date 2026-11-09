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
- **正在发声却没有波形时，不套用保持**（2026-11-07 补，见 §9.13）。采样芯片"没有波形"通常意味着那一击结束了；而**噪声发生器**是"正在响，但没有周期可画"。两者在 `wave == nullptr` 上无法区分，只能再看 `gate`：只要通道仍在发声，就照实显示"无波形"，否则会把上一段旋律的形状画在噪声段上。判定写成 `playing || sounding`，保持只在**空闲**时生效。

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
| 采样 | SegaPCM —— 见 §9.16，**部分完成**（§9.16.2 已知问题） | ✓ | ✓ | ✓ `collectSegaPcmSamples` |
| 采样 | YMZ280B —— 见 §9.15（seek 不支持，见 §9.15.3） | ✓ | ✓ | ✓ `collectYmz280bSamples` |
| 采样 | MultiPCM（YMW258）—— 见 §9.10 | ✓ | ✓ | ✓ `collectMultiPcmSamples` |
| 采样 | K007232 —— 见 §9.7 | ✓ | ✓ | ✓ `collectK007232Samples` |
| 采样 | **OPN-ADPCM**（ADPCM-A 与 ADPCM-B/Delta-T）—— 见 §9.17 | ✓ | — | ✓ `collectOpnAdpcmSamples` + `collectOpnDeltaTSamples` |
| 采样 | **YM2612 DAC**（第 6 通道的 `0x2A` 流）—— 见 §9.20 | ✓ | ✓ | ✓ `collectYm2612DacSamples` |
| 采样 | **Y8950-ADPCM / ADPCM-drum** | — | — | — |
| 波表 | VSU —— 见 §9.11 | ✓ | ✓ | ✓ `collectVsuSamples` |
| 波表 | ES5503 —— 见 §9.9 | ✓ | ✓ | ✓ `collectEs5503Samples` |
| 波表 | K005289 —— 见 §9.12 | ✓ | ✓ | ✓ `collectK005289Samples` |
| 波表 | Mikey —— 见 §9.13 | ✓ | ✓ | ✓ `collectMikeySamples` |
| 波表 | MSM5232 —— 见 §9.14 | ✓ | ✓ | ✓ `collectMsm5232Samples` |
| 波表 | HuC6280、SCC（K051649）、X1-010、WonderSwan、Game Boy 波表、NES APU | — | — | — |

**不在本表的两类**：PSG 与 FM（AY、SN76489、SAA1099、POKEY、OPL 系、OPN 系、OPM、OPX 等）本来就用 `cycleFor` 画阶梯，不经过 `state.wave`；`FDS` 与 `N163` 由 NSF 后端（`NsfBackend` / `NsfBus`）加载，没有对应的 `ChipKind`，要单独处理。

**填一颗芯片的代价**：`attachMemory` 的通道已经通了（`407c0b1`），每颗只需把"寄存器 → 字节区间"翻译出来挂到 `state.wave`。**C140 那次约 50 行**，其余同量级。

**波表芯片还要多做一件事**：置 `state.tableWave`。波表也会报 `sampleRate`（每秒走多少条目），而导出用 `sampleRate > 0` 判断"这是不是一段录音"——不置这个标志，波表会被当成采样写出，位深与单周期标记全部失效（§9.11.1 就是这次事故）。

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

#### 9.11.1 交付后修正：波表被当成了采样（2026-11-07）

用户反馈：选“芯片自身的位深”导出时**没有阶梯感**。原因是**我把 VSU 从波表路径挤到了采样路径**：

```
isSampledChannel = sampleRate > 0 && wave.size() >= 2
```

VSU 报了 `sampleRate`（每秒走多少条目，这是对的），于是被判为采样 → 走 `writeSampleWav`：
**不量化、无 `clm` 标记、`single/loop` 与位深全部失效**。文件变成 32 个浮点的“一段录音”，
导入波表合成器后被当音频插值——看起来就是“平滑”的。

**修法**：给 `chips::ChannelState` 加 `tableWave`（“这个 `wave` 按定义就是一个周期”），由 VSU
置位，经 `ChannelFromChip` 传到 `playback::ChannelState`，`isSampledChannel` 增加
`&& ! tableWave`。这是**声明语义**而不是猜：速率说明“读得多快”，不说明“是不是一段录音”。

**验证**（探针新增第五个参数：`-4 <秒> -1 <目录>` 会写 File 菜单会写的文件）：

```
导出 VBoy-VSU_Ch-1.wav：fmt=float32、clm=“<!>2048”、2048 点、电平落在 2k/63−1 的六位网格上
OKIM 对照：无 clm，仍是原始浮点采样 —— 采样路径未受影响
```

### 9.12 已关闭：K005289（2026-11-07）

**波表，但波形来自 PROM，而不是寄存器。** 两个声部各自从 512 字节的 PROM 里读：`prom[ch * 0x100 +
waveform * 0x20 + step]`，每个条目**四位**，播放前 `- 8`（`k005289.c:195-198`）。控制寄存器的
bits 5-7 只负责选八个波形中的一个。

| 项 | 事实 | 出处 |
|---|---|---|
| 声道 | 2 | `k005289.c:184` |
| 波形 | 2 × 8 × 32 × 4 位，静音在 0x08 | `k005289.c:59`、`198` |
| 表选择 | 控制寄存器 bits 5-7 | `k005289.c:218` |
| 音高 | `clock / (32 * (freq + 1))`，寄存器**取反**存入（`0xFFF - value`） | `k005289.c:224`、`190` |
| 时钟 | 头部 **0xEC**，实测 3 579 545 | `_CHIPCLK_OFS` |
| 数据块 | **0xC3**，**RAM** 类型、**16 位地址**、512 字节 | `_VGM_RAM_CHIPS`、`Cmd_DataBlock` |

**0xC3 和 ES5503 的 0xE1 是同一类、不同宽度。** 两者都在 libvgm 的 RAM 表里，但 0xC3 属于
`C0..DF`（16 位地址），0xE1 属于 `E0..FF`（32 位）。读取器本来就按 `addressSize` 分两支解析，
所以这次没有动它——而 `ChipRequirements` 的第五个字段仍是 `true`（RAM），否则 512 字节会被当成
一个带 size/offset 头的 ROM 块，落位就全错了。

**音高是算出来的，因此没有 PROM 也照样响。** 这正是与采样芯片的区别：没有 `0xC3` 块时 `wave` 为
空（行退回触发态），但 `hz` 仍在。所以 `state.wave` 与 `state.sampleRate` 只在拿到 PROM 后给出，
`tableWave` 也只在有 shape 时置位。

**验收**：

```
15 首 / 45 个样本，单曲 1–5 个；速率 8363 – 28185 Hz；零样本曲目 0
04 BGM 1（34.69 s）：全曲每 0.5 秒扫行 → 21 个 (32 点, 速率) 组合，5 个导出项 5/5 命中
```

**交付后一次通过**（2026-11-07），用户听感确认。这一颗没有返工：三个量（表布局、音高取反、块类型）
全部从核心源码取出，`0xC3` 的地址宽度也是先读了 `_VGM_RAM_CHIPS` 才动手。

**完整记录**：`docs/memo/2026-11-07-k005289-a-prom-four-bits-deep-in-a-ram-block.md`

### 9.13 已关闭：Mikey（2026-11-07）

**它不是波表芯片，这一颗必须换一种建模方式。** 每个声道由一个定时器驱动一个 **12 位移位寄存器**，
输出是寄存器的新位：非积分模式取 `±volume`，积分模式累加（`mikey.c:490-503`）。所以"波形"不是
歌曲上传的表，而是 **(seed, taps, integrate)** 三者决定的一条递推序列——短周期是音调，长周期是噪声。

| 项 | 事实 | 出处 |
|---|---|---|
| 声道 | 4（本批曲子里 ch1 全程没有写入） | `mikey.c:184` |
| 输出 | `reg = ((reg<<1) & 0x0FFE) \| (parity^1)`，`out = reg&1 ? +vol : -vol` | `mikey.c:490-503` |
| 抽头 | FEEDBACK 低 6 位 + 高 2 位左移 4，另加 CTRLA bit7 | `mikey.c:420`、`442` |
| 积分 | **CTRLA bit5**（`AC_ENABLE_INTEGRATE = 0x20`），不是音量寄存器的位 | `mikey.c:372` |
| 步进率 | `clock / ((backup + 1) * 2^(shift + 4))` | `mikey.c:355` |
| 音高 | 步进率 ÷ **周期长度**（2 步=方波，4 步=占空比，几千步=噪声） | 本节的模型 |

实测周期：`taps 0x07 → 4`、`0x2C → 62`、`0xEC → 3556`。前两个可画、可导出；第三个判为噪声。

**交付后修了两个问题**（都由用户实测发现）：

#### 9.13.1 加载从秒开变成 2 秒

收集器在**每条**寄存器写入后都跑一遍周期探测。这些曲子有 1.2–2.3 万条写入，每条最多 4096 步、
带 map 插入，于是加载要 1.9 秒。

修法两条，缺一不可：

| 改动 | 作用 |
|---|---|
| 按 `(seed, taps, integrate)` **记忆化** | 大量重复写入不再重算；音量不进键（只缩放，形状归一化后相同） |
| 探测上界 4096 → **576** 步 | 超过 512 点的周期本来就要丢弃，多走三千步只为证明它长，是纯成本 |

```
1872 ms → 180 ms（同目录 K005289 对照 162 ms）
```

**教训**：**收集器跑在加载路径上**。任何"每个事件做一次小计算"的写法，在事件数是万级时就是秒级开销；
先想清楚它是 O(事件) 还是 O(事件 × 每事件成本)。

#### 9.13.2 噪声段仍显示旋律的方波

两个独立缺陷叠在一起：

1. **保持逻辑套错了对象**（ADR-007 的第四条规则就是这次补的）。噪声段 `wave` 为空，共用的锁存逻辑
   把上一段旋律的 4 点波形留了下来——**看到的 PWM 和听到的噪声来自同一通道的不同时刻**。
   修法：只有 `gate == false`（空闲）时才回退到保持态；正在发声就照实显示空。
2. **音符表编造了音高**。`noteFor()` 把步进分频当音高分频、并按"周期 2"除以二，于是空闲帧稳定
   报出 `3937 Hz / note 107`。修法：这张表直接返回"无音符"，音高只由真实周期给出。

验证（`02 Trout Shout`）：噪声段 `wave=null hz=0 active=1`；旋律段 `4 pts rate=261.5 hz=65.4`（rate/4 = hz）。

**教训**：`wave == nullptr` 有两种含义——"这一击结束了"和"它在响但没有周期"。共享层的判据必须
把两者分开，否则修一个芯片会悄悄改变另一个芯片的语义。

### 9.14 已关闭：MSM5232（2026-11-07）

**第三种形态：不是 RAM 表，也不是 LFSR，而是“键索引 + 抽头求和”。** 寄存器 `0x00..0x07` 每声道一个，
bit7 是门控、低七位是**八十八项 ROM 的键索引**（`msm5232.c:35`）；值 ≥ `0xD8` 改为噪声
（`msm5232.c:290`）。八十八项正好是 A0..C8，所以索引就是键：

```
note = 21 + index          // A4 = index 48 = 440 Hz，不需要测量
```

**音高是查表，波形是求和。** 每个声部向四个抽头送方波——16'、8'、4'、2'——都取自同一个计数器的
相邻位，因此同相位、逐级高二倍频（`msm5232.c:305-320`）。组控制寄存器 `0x0C/0x0D` 的低四位决定
该组实际驱动哪几个（`msm5232.c:620-650`）；**一个都没开，这一组就是静音**，无论声部是否 key-on。

| 项 | 事实 | 出处 |
|---|---|---|
| 声部 | 8（2 组 × 4） | `msm5232.c:64` |
| 音高 | `21 + index`，88 项 | ROM 表 |
| 噪声 | `value >= 0xD8` | `msm5232.c:290` |
| 波形 | 使能抽头的方波之和，周期 `2^(divider+1)` 拍 | `msm5232.c:305-320` |
| divider | ROM 条目的 bit 9-11 | `msm5232.c:310` |
| 时钟 | 头部**没有**这一项，用芯片标称 2 119 040 | 实测 |

**因此是阶梯波而不是方波**：开一个抽头才是 50% 方波，开四个得到 256 点的四级阶梯。这一点让
`channelKind` 必须返回 `wavetableWave` 而不是 `sn76489Square`——后者会让显示与导出按 `duty`
合成一个方波，把阶梯丢掉（与 Mikey 同一条理由）。

**88 项 ROM 是程序化导出的**，不是手抄：从 `msm5232.c` 的 `ROM(counter, bindiv)` 文本解析后
按 `counter | (bindiv << 9)` 生成。手写那一版我写错了，生成后逐项一致。

**验收**：

```
7 首 / 75 个样本；零样本曲目 0；点数覆盖 4..256
rate / 点数 = hz 逐行成立（9956.1/64 = 155.6、13289.8/8 = 1661.2）
46/46 + 芯片断言 PASS
```

**交付后用户听感确认**：导出的波表正确。

**已知简化**：`volume` 恒为满——这一颗的音量在电容积分上，没有可读的寄存器字段；`0x08..0x0B`
的起音/衰减与 `0x10..0x1A` 的每路输出音量都未参与。

**完整记录**：`docs/memo/2026-11-07-msm5232-a-keyboard-rom-and-a-staircase.md`

### 9.15 已关闭（除 seek）：YMZ280B（2026-11-08）

**第四种形态：也是采样类，但一首歌只有一条流。** 十首素材里，声部 1–7 各被写一次（fnum/ctrl/level/pan 全 0），
之后再不触碰；只有声部 0 被给了一段很长的 start..stop，而且驱动**边播边把区间往前改**。整首约 50 条
寄存器写入，没有任何逐音符流量——所以这里的一行代表“某声部正在读 ROM 的哪一段”，不是“某个乐器”。

**启动来自全局寄存器。** `0xFF` bit7 是 key-on 使能，上升沿会启动所有「ctrl keyon && looping」的声部
（`ymz280b.c:985-992`），曲子的写法正是 `0xFF=0xD0` 然后 `0x01=0xB0`。只看每声部 key-on 会**错过第一次启动**。

| 项 | 事实 | 出处 |
|---|---|---|
| 声部 | 8 | `ymz280b.c:175` |
| ROM | 数据块 `0x86`，`memSize` 8 MB，载荷 536 659 字节 | 实测 |
| 地址 | **字节**（不是 nibble），三段 H/M/L | 实测，见下 |
| 速率 | `clock/384 × (fnum+1)/256`，`0x172` 的 0.172–44.1 kHz | `ymz280b.c:18`、`259` |
| 解码 | 模式 1 = 4 位 ADPCM，2 = PCM8，3 = PCM16 | `ymz280b.c:655-657` |

#### 9.15.1 四个把画面变成“一条曲线/直流”的原因

排查依次排除了三层，每层都有测量：

| # | 错误 | 证据 |
|---|---|---|
| 1 | 镜像只给 512 KB，块在 `dataOfs=0x3AF638`、长 `0x83053` | 块尾 `0x3AF638+0x83053=0x43268B` == 驱动写的 `stop=0x432683` |
| 2 | 把地址当 nibble 除以 2 | 除 2 后落到块前 1.9 MB 的全零区 |
| 3 | 窗口按峰值归一化后，全零解出的**爬坡**被放大成平滑曲线 | 归一化前峰值 0.047，符号变化 0 |
| 4 | 收集器并集从**初始化写的静音区间**开始 | `start=0x394B61` 读 0x00；播放区间 `0x3BF6DA` 读 0xB2 |

判定“是不是音频”用**符号变化次数**：真实音频 256 点里 >20 次，爬坡/直流 ≤1 次。

#### 9.15.2 导出：整块 ROM，不按区间

按驱动写的地址切区间不成立——并集可能起于从未播放的区域（`06 Sonic Diver` 因此导出成直流），
`02 Spirit of Fang` 甚至没有可用的并集。改成：取 ROM 里**第一个到最后一个非零字节**，整段解码一次，
速率用最响那个声部的参数，文件名 `ymz280b_stream`。11 首全部有导出，0.9–72 秒。

#### 9.15.3 seek：这类文件在原理上无法跳转

`VGMPlayer::Seek` 只做两件事：挪 `_playTick/_playSmpl`，然后重放这段时间的寄存器写入。而这些驱动
**头一秒之后不再写任何寄存器**，于是重放得到的芯片状态与 t=0 完全相同：

```
seek=0   引擎 0.20 s   RMS 0.04256
seek=5   引擎 5.20 s   RMS 0.04256      ← 位置在动，音频没变
seek=30  引擎 30.20 s  RMS 0.04256
```

**这是信息缺失，不是 bug**：VGM 里没有记录“此刻该读到 ROM 的第几个字节”。与其让位置栏走动而声音
还在开头，不如拒绝——现在遇到含 YMZ280B 的歌，`seekSeconds` 直接返回 false，位置停在原处
（VSU 等不受影响：拖到 5 秒得 5.20 s、RMS 0.37470）。

**真跳尝试过两次，都失败，因此判定为不支持**（不是“待实现”）：

| 尝试 | 做法 | 结果 |
|---|---|---|
| 1 | 按时间比例换算字节偏移，重写 `start` 后重新 key-on | **噪音**：4 位 ADPCM 带预测器，芯片在 key-on 时清零，从样本中间进入会一直抖动到重新收敛 |
| 2 | 吸附到驱动自己写过的段起点，再重新 key-on | 仍**噪音**，且能隐约听到**原流仍在下面从头播放**——重定位没有接管那条流，只是额外 key 出了一个噪声音 |

第 2 条的听感（`06 Sonic Diver`）是决定性的：问题不是“进错位置”，而是**无法接管一条正在内部推进的流**。
VGM 不记录预测器状态，libvgm 也没有“从任意偏移恢复 ADPCM”的接口；要做到真跳，得改核心解码器并新增
状态保存/恢复，超出这一颗芯片的范围。

对照组：`02 Spirit of Fang`（驱动从未重写 start，只有一个段）从一开始就走“拒绝”分支，听感干净——
**拒绝是唯一没有副作用的行为**。

**完整记录**：`docs/memo/2026-11-08-ymz280b-a-stream-wearing-eight-voices.md`

### 9.16 已关闭：SegaPCM（2026-11-08）

**第五种采样形态：寄存器本身就是播放状态。** 十六个声部各八个寄存器，没有采样表也没有指令流：

| 偏移 | 含义 |
|---|---|
| `8n+2` / `8n+3` | 左/右音量，七位 |
| `8n+4` / `8n+5` | 循环地址，**256 字节为单位** |
| `8n+6` | 结束地址，同样以 256 字节为单位；核心比较 `addr >> 16`，所以样本止于 `(reg+1) * 256` |
| `8n+7` | 地址步进，每输出样本前进这么多（1/256 字节） |
| `0x80+8n+4/5` | 当前地址 |
| `0x80+8n+6` | **bit0 为 0 时播放**（写入 0 是 key-on）、bit1 禁止循环、**其余是 bank** |

速率：核心以 `clock / 128` 渲染，位置步进是 1/256 字节，所以 `字节率 = pitch × clock / 32768`；
`pitch = 128` 是半速。样本是 **0x80 中心的 8 位无符号**（`segapcm.c:182`），长度由结束地址给定——
它与 GA20/ES5503 的“遇 0x00 结束”不同，因此新增了 `SampleFormat::pcm8Unsigned`。

#### 9.16.1 bank 在控制字节里，而且每个文件不同

这是这一颗唯一真正难的地方。取字节是：

```c
offset = (regs[0x86] & bankmask) << bankshift;    // 控制字节高位 = ROM 银行
v = rom[offset | (addr >> 8)] - 0x80;
```

`bankmask` 与 `bankshift` 来自 **VGM 头 0x3E / 0x3C**，两首素材并不一致：

| 文件 | 头 0x3C / 0x3E | 关键写入 | 正确起点 |
|---|---|---|---|
| `03 Scene Select` | 13 / 0xF8 | `ctrl = 0x0A`、`0x12` | `0x10000`、`0x20000` |
| `01 Magical Sound Shower` | 12 / 0x00（0 意为默认 0x70） | `ctrl = 0xD2` | `0x5437C`（而非 `0x1A437C`） |

第一版把 13 / 0xF8 写成常量，于是第二首的每个声部都算到 `0x1A437C`——超过 512 KB 的镜像，
读到的是空区：**行是一条直线，导出 0 个**。第一首碰巧对，所以“有的对有的不对”。

修法是把这一对参数一路带下去：读取器解析头部 → 加载器放进 `ChipSlot::flags`（低位 shift、
次字节 mask）→ `VgmReplayer` 在 `reset` 时塞进 `ChipConfig::flags` → 解码器与收集器各自读取。
缺任何一段，取值就退回默认，而默认只对其中一首成立。

**验收**：

```
01 Magical Sound Shower  3 个样本（0x50951 / 0x5437C / 0x55830，均 16113.3 Hz，1–3.5 KB）
03 Scene Select          6 个样本（含 15361.7 Hz / 8448 点的 Slap Bass）
46/46 + 芯片断言 PASS
```

**完整记录**：`docs/memo/2026-11-08-segapcm-registers-are-the-playhead.md`

### 9.16.2 已知问题（未解决，2026-11-08）

bank 折算（§9.16.1）是对的——两首素材的导出现在都能对出真实音色，`03 Scene Select` 的
Slap Bass 也在其中。**但下面两条没有解决，本芯片按“部分完成”记档：**

**① 导出不全。** 收集器只在控制字节 bit0 的 **key-on 边沿**归档一次。实测这首曲子里有
三类击打因此丢失：key-on 之后再写音量的、靠重写 `0x84/0x85` 在区间内跳转重触发的、
从头到尾只靠 `loop` 回绕从不重新 key-on 的。

**② 波形窗口与实际发声不一致。** 行里画的是 key-on 时锁存的 `start..end`，而驱动的
`start` 会在区间内部跳转，`end` 又常常指向整个 64 KB bank 的末尾。结果是：显示一段、
听到另一段；一行里出现好几段相邻采样；或者只画出一个采样的一部分。

**为什么没修完**：`start` / `loop` / `end` 三者的**真实语义没有可验证的地面真值**。
`end` 是页号（`(addr>>16) == endReg+1`），但它既可能指向样本末尾，也可能指向 bank 末尾；
`loop` 才是样本起点这一点是从两首素材的块落点反推的（MSShower 的 `0x40090/0x40951/0x41C03…`
与 VGM 头里的 `dataOfs` 逐个吻合），**尚未被听感逐条确认**。

已尝试并**全部回退**的四种改法：

| 改法 | 结果 | 回退原因 |
|---|---|---|
| 按 `loop..end` 收集 | 3/6 → 24/14 个文件 | 加载从 170–340 ms 涨到秒级；效果反而更差 |
| 窗口跟随估算位置（`start + 时间 × 字节率`） | 行会滚动 | 位置靠估算，漂移后显示与实际发声错开 |
| 地址重写 + end 写入都归档 | 3/6 → 161/19 个 | 同一区间被切成几字节一档，合并后又变成整个 bank |
| 窗口上限 16384 → 8192 | — | `end ≤ start`（页回绕）的声部直接变空 |

**下次动手前必须先做的**：一个**只读**的取证工具——按时间打印每一条 SegaPCM 写入
（`时间 / 通道 / 寄存器 / 值 / 折算后的 start·loop·end`），配合一次试听逐条对齐，
确认“一次击打对应哪几次写入”“`end` 指向哪里”。没有这张表，任何模型都只是猜，
而这四次尝试已经证明猜的代价是用户多等一轮。

**当前可用的部分**：`03 Scene Select` 6 个、`01 Magical Sound Shower` 3 个导出，
区间与速率正确（分别对得上 ROM 块落点）；行有波形但语义只保证“是真实采样的字节”。

### 9.17 已关闭：OPN 的 ADPCM-A（2026-11-08）

**这颗芯片最难的地方不在解码，而在“表里根本没有那些行”。**

#### 9.17.1 六个采样声部，一个 ROM

ADPCM-A 在**第二个寄存器端口**（YM2610 的 `0x59`、YM2608 的 `0x57`）的 `0x00..0x2D`：

| 偏移 | 含义 |
|---|---|
| `0x00` | key-on/off，bit7 = 全停，bit0-5 = 各声道 |
| `0x01` | 总音量 TL（`(v & 0x3F) ^ 0x3F`） |
| `0x08+c` | 各声道电平/pan |
| `0x10+c` / `0x18+c` | 起始页 低/高（**字节地址 = 页 << 8**） |
| `0x20+c` / `0x28+c` | 结束页 低/高，`+0xFF` |

解码（`fmopn.c:2747`、`:2769`）：4 位、高半字节在前；`steps[49]` 步长表 × `step_inc[8] =
{−16×4, +32, +80, +112, +144}`；累加器 **12 位回绕**（MSM5205 血统）；**速率 = `clock / 432`**
（8 MHz → 18 518.5 Hz，核心写作 `freqbase / 3`）。

#### 9.17.2 两个 bug，都由听感发现

| 现象 | 原因 |
|---|---|
| ADPCM 行**整行缺失**（13 声部的芯片只列 7 行） | `ChipBuffer::makeDecoder` 用默认构造，四种 OPN 共用 `OpnDecoder` → 全部按 **YM2612** 建（6 FM + DAC）。型号改由 `ChipConfig::flags` 传入 `reset` |
| 拖拽导出的波形**音高明显偏高** | 速率写成 `clock / 144`，**快 3 倍**（≈19 个半音）。改为 `clock / 432` |

用户的原话“这两首 ADPCM 通道应该有的有音符的”直接推翻了我“素材没用 ADPCM”的结论——行数与写入直方图都不可靠，只有**已实现的行**才说得清。

#### 9.17.3 导出

按 key-on 锁存的 `start..end` 切文件，取 `0x82` 的 8 MB 镜像，`SampleFormat::adpcmA4`。
区间先按“相邻 ≤ 0x100 字节且合并后 ≤ 16 KB”并合，再按内容 FNV 哈希去重：

```
04 Stage 1   90 个   最长 3.01 s
07 Stage 2   83 个   最长 3.40 s
全部 18 518.5 Hz，起点与 ROM 块落位吻合（0x100000 / 0x17100 / 0x1FE00 …）
```

**合并的边界是量出来的**：纯按 0x100 间隙合并会串出**一个 44 秒的文件**（连续页连成整 bank，
与 Sega PCM 那轮同一种失败），所以加了 16 KB 上限；但计数只从 94 降到 90——剩下的区间在 ROM
里彼此隔得很远、字节并不同，按“同一段录音”的标准不该合并。

#### 9.17.4 已修：镜像按**地址位宽**装，不是按观测值

`02 The Military System` 的六行 `ADPCM-A` 全画同一道斜坡、导出 0 个采样，而 `04/07` 正常。原因是
镜像上限写成了 **8 MB**（解码器要求与导出收集器各写一份），而这首的 `0x82` 块声明 **16 MB** 且把载荷
放在 `0xA79600` 以上：`memory::rom` 裁到 8 MB 后逐块越界丢弃 → 解码器读全零 → 零 nibble 单调累加 =
**确定性斜坡**；导出则因 `begin >= rom.size()` 一个区间都没收下。

声道的起始地址是 `((hi << 8) | lo) << 8`，24 位页号 → 芯片能寻址 **16 MB**，这与块头的声明一致。
现在 `OpnDecoder::kAdpcmAImageBytes` 是唯一定义，解码器与导出共用；实测 0 → 4 个采样，六行
8192 / 3070 点 @18 518.5 Hz，`04/07` 仍是 90 / 83。

**教训**：上限从取值范围推，不从"见过的最大文件"推；同一语义不要写两份。

#### 9.17.5 已知简化

- **~~ADPCM-B（Delta-T）没有接 ROM~~（已修）**：`ChipRequirements` 现在可以声明第二、第三个镜像
  （`secondaryMemoryType` / `alternateMemoryType`），`ChipMemory` 带上来源 block 类型，`IChipDecoder`
  多一个默认为空的 `attachSecondaryMemory()`。OPN 声明了两种编号——`0x83`（OPNB）与 `0x81`（OPNA），
  replayer 把歌曲实际带的那个交过去；`attachMemory` 按类型分派，OPNA 上则退化为"与 ADPCM-A 共用同一
  颗 ROM"（`setAdpcmBRom` 用同一份字节）。`channelState` 的 Delta-T 分支按 start/end 寄存器解码出
  16 位钳位的波形，并把 `sampleRate` 设为 `DELTA-N` 的播放速率，因此行上显示的是真实形状而不是
  空行。实测：`02 The Military System` 的 `ADPCM-B` 从 `wave = null` 变成 8192 点 @41 331 Hz；
  `07 Stage 2` 也从空变成 8192 点 @38 9xx Hz；未启动的曲子（`YM2610B/01 A.D.1993`）仍是空行。

  **仍有的边界**：画出来的只是样本的**前 4096 字节**（与 ADPCM-A 同一个 `kWindow`）。一个字节是两
  个 nibble，也就是 8192 个采样，所以时间长度取决于播放速率：ADPCM-A 18 518.5 Hz → 约 **0.44 s**；
  `07 Stage 2` 的 DeltaT 34 833 Hz → 约 **0.24 s**；`02 The Military System` 41 331 Hz → 约 0.20 s。
  更长的样本只画开头——一行要的是形状，不是整段录音。**它不影响音符与音高**：那来自 DELTA-N 与周期
  估计（后者同样只看 4096 字节）。

  **导出（2026-11-09 补齐）**：`collectOpnDeltaTSamples` 把每段被 key-on 过的 DeltaT range 导出成一个
  WAV，与 ADPCM-A 并列但有三处不同——地址页按变体移位（OPNB 8 / OPNA 5，`addressShiftFor()` 是唯
  一规则）、块按变体取（`sampleBlocksFor()`）、**速率固定为参照 DELTA-N `0x4000`**（一份录音一份文
  件；曲中的 DELTA-N 只是移调）。长度包含 `end` 指向的**最后一个字节**（画面与文件同时修正，一页 256
  字节不再丢最后一个 nibble）。超过 30 秒的 range 视为 bank 直接跳过；Settings → 导出里的“单个采样的
  最长时长”（默认 5 s，0 = 不限）在写文件时截断并在报告里给出数量——它是写入器的一条规则，因此对
  所有芯片生效，改变后也无需重新加载。实测 `04 Stage 1` 的 115 个样本里 25 个来自 DeltaT，首个
  `opndeltat_67B800_05200` @13 888.9 Hz。

  **MIDI 路径（同日修）**：`MidiSequenceBuilder` 过去把 `0x82` 写死当 DeltaT 镜像，而且每族只建一份，
  于是 OPNB 的周期是从 ADPCM-A 的字节量出来的 —— `rootCents` 因此是错的，MIDI 音符只在
  `pitch.useSampleRoot` 开启时才看得出来。现在 `OpnDecoder::sampleBlocksFor(variant)` 是唯一答案
  （OPNB `0x83`；OPNA `0x81`，缺失时回退到两颗族共享的 `0x82`；YM2612/YM2203 无），`kRequirements`、
  `MidiSequenceBuilder::flushOpn` 都引用同一组常量，镜像按**设备**取而不是按族。

  **两个缺陷叠加**才让开关"看起来没作用"：①块类型错（根音恒为 0）；②镜像按**内容末尾**裁剪
  （`memory::rom(..., clipToContent = true)`），`07 Stage 2` 的 DeltaT 被裁到 6.96 MB，而寄存器地址到
  `0x6B9000`，上方字节读成 0 → 周期测不出。现在与播放路径同一条规则（声明大小，不裁剪）。实测 07：
  1287 个 ADPCM-B 音符里 **1097 个带根音**（−2630…−1444 音分），其余 190 个是鼓（无周期 → 0，按设计
  保持 L1）。**默认开启（2026-11-09 起）**：此时 ADPCM-B 带上每个 key-on 各自测得的根音，与可视化同一个数；关闭后 MIDI 音符回到 L1（DELTA-N 速率相对 8 kHz 参考），与 ROM 无关，整首会差一到两个八度——那个状态只用于对照与规避；
  开启后（`reloadForPitchChange()` 重载）ADPCM-B 带上每个 key-on 各自测得的根音。
  `testOpnDeltaTBlockChoice` 断言了 OPNB 不接受 `0x82`（波形与根音都为空）、OPNA 回退共享 ROM、
  YM2612 无 DeltaT 块。

  **根音的三个后续修正（同一链条）**：①周期只从样本起点测，而打击类样本的起点是起音噪声（`04 Stage 1`
  540/803 个 key-on 测不到）——起点失败后再试 +1 KB、+2 KB；②音符取整后的余数只加进“恰好存在的弯音”，
  没有弯音的样本因此丢掉最多 50 音分（`04` 的根音 −1444/−2630/−2039 对应的 −44/−30/−39 音分），现在
  无弯音时单独发一条起点弯音；③`reloadForPitchChange()` 改为用内存镜像重建（`selectSongAsync`），不再以
  “文件仍在磁盘上”为前提——临时目录的文件被移动后，关闭开关会因提前返回而保留旧序列。全过程与判据见
  `docs/memo/2026-11-09-the-root-that-was-measured-and-thrown-away.md`。

  **性能（同日修）**：形状按 `(start, end)` 缓存。`channelState()` 每写一次寄存器就会被调用一次，未缓存时
  同一段字节在 `07 Stage 2` 上被解码上万次，加载从 ~150 ms 变成 **3.7 s**；缓存后 07 回到 149 ms、
  04 102 ms、`02 The Military System` 15 ms。缓存于 `setAdpcmBRom()` 清空（字节变了），超过 64 条整体
  清空（一首歌只有几个样本）。另：曾试图把测周期窗口从 4096 字节缩到 1024（估计器只看 2048 个采样），
  结果同一首的音符从 65 变成 85 —— **窗口会影响结果，不能当作免费优化**，所以窗口保持原样，只缓存结果。定位过程、原理与断言写法见
  `docs/memo/2026-11-09-the-shape-that-was-decoded-once-too-often.md`。
- **~~ADPCM-B 行在个别曲目仍可能显示波形~~（已修）**：族的边界过去由算式决定——`channelState()`、
  `noteTable()`、`isDrum()` 三处各写一次 `fmCount + 6`。今天四种变体都带 6 个 FM，所以恰好正确；
  一旦出现"带采样但 FM 不是 6"的型号，Delta-T 行会被指到别的声道，`adpcmARegs[6]` 也会越界。
  现在 `Impl::Layout { fm, adpcmA, adpcmB, dac }` 是唯一定义，`channelCount` 由它推出，ADPCM-A 槽位
  按表夹紧；`testOpnChannelFamilies` 对 YM2612（7 行）、YM2203（3 行）、OPNA/OPNB（13 行）断言行数与
  每个族各一条哨兵。实测行数不变：`07 Stage 2` 16、`02 The Military System` 16、`Green Hill Zone` 11、
  `YM2610B/01 A.D.1993` 16。
- 未做“内容指纹聚类”，因此同一记鼓的长短两版仍可能是两个文件。

**完整记录**：`docs/memo/2026-11-08-opn-adpcm-six-players-and-the-wrong-variant.md`、
`docs/memo/2026-11-09-the-image-that-was-too-small.md`

### 9.18 键盘页的行与 Mixer 同源（2026-11-08）

**一条行只该有一个身份来源。** 这个页面此前有两次枚举：Mixer 读 `describeSong` 建的 `tracks`，键盘页读 `replayer->instances()` 并用 `trackFor(deviceId, channel)` 翻译。凡是两边规则不同，行就错位，而错位会被误读成“这个芯片的可视化坏了”。

四次补丁都失败在同一个前提上——**以为能用某个设备字段把主芯片和它的联动词道组区分开**。

| 判据 | 为什么不成立 |
|---|---|
| `deviceId` | libvgm 给链接的 SSG **与其父相同的 id** |
| `instance.device` 指针 | 两者是**同一个** `PLR_DEV_INFO` |
| `channelCount` 改小 | 主机随之换名字表，出现重复 FM 行与 Solo 连体 |
| 偏移量加减 | 一旦枚举顺序与建轨道顺序不同，就挪走别的设备的声道（Sega PSG 因此无可视化） |

**现在的规则（三条，全部无芯片特例）**：

1. **行的身份由 `describeSong` 写下**：`TrackInfo` 带 `sourceDeviceId`（这一行真正属于哪个设备；链接设备用 `child.id`，不是父的）、`deviceChannel`（设备内声道号，`-1` 表示整设备）与 `sourceDeviceType`（libvgm 设备类型）。快照按 `(类型, 声道号)` 精确配对，**不使用任何顺序或偏移**；唯一的一处芯片特例写在 `captureState` 里并注明了理由——YM2610 名下的 3 声道子设备就是它的 SSG，因为 libvgm 把该 SSG 的 id、指针与类型都报成父芯片的，三者都无法区分；
2. **行的数量、顺序与名字来自 `tracks`**：`capture` 结束后按轨道铺满，没有状态的轨道也给一行空行；名字与分组名直接取轨道；
3. **一个轨道一行**：设备被 libvgm 递交两次时，两份状态落在同一轨道，取“内容最完整”的那份（有波形 > 有音符 > 空）。

**两个必须记住的坑**：

- **实例是惰性创建的**：libvgm 只在第一次有命令写到某设备时才造实例。SSG 在首次发声前没有实例，于是它的三行曾经“迟到”，并在出现时把下面所有行推下三位。行既然由 `tracks` 铺满，这个问题就不再存在；
- **`trackFor` 的整设备轨道不能当通配符**：`channelIndex < 0 || channelIndex == channel` 会让有整设备轨道的设备抢走同 id 的所有声道。精确匹配优先，整设备轨道只作兜底。

**验收**：`07 Stage 2` 16 行（`FM 1..6 / ADPCM-A 1..6 / ADPCM-B / SSG 1..3`）与 Mixer 同名同序；SSG 行从第一帧存在；任意一行 Solo 只影响自己；Sega PCM、YMZ280B、C140、NES 行数不变；加载 250–350 ms；`46/46`。

**过程记录**：`docs/memo/2026-11-08-keyboard-rows-rearchitecture-plan.md`

### 9.19 NSF 播放：三个判据与两处默认值（2026-11-08）

NSF 走的是自研后端（`backends/nsf`，核心来自 `libs/nsfplay`），与 libvgm 无关。当天修的三处故障都属于"判据来源错误"：

**1. bank 判定不能看镜像大小。** `NsfFile::isBanked()` 曾写成 `rom.size() > 0x8000`。`Ai Senshi Nicol` 镜像 20 KB，头部 bank 表却是 `01 02 00 00 00 00 03 04`，`init=$D02B` 只有把 `$D000` 窗口映射到 bank 0 才访问得到；按大小判为线性后该地址落在镜像外，CPU 读到 0、落进零页（`pc=0002`），表现是"三角波直流、零写入"。现在与 NSFPlay / NotSoFatso 一致：**bank 表非零即 banked**。断言见 `NsfFormatTest`。

**2. 默认曲目是"文件说的"，不是"用户想要的"。** `startSong` 只是默认值，有些 rip 把它指向一记音效（Super Mario USA 的 16，音乐在 1–14）。界面需要能选曲：`SongMemory::songIndex`（-1 = 用文件头的）→ `PlaybackEngine::selectSong()` → 信息卡下拉框（`song.selector`，仅 `songCount > 1` 时出现）。

**3. 打开文件默认第 1 首。** 目标曲目是 `PlaybackEngine::loadImage()` 的**参数**（`loadFile` 与
`loadFileAsync` 各传 0），不再有可变的成员——因此不存在"某条加载路径忘记重置"这种可能。切曲目走
`selectSongAsync()`，在加载 worker 上重建，失败则停播并在状态行报出原因（与打开文件同一语义）。

**测量优先。** 这三处都不是靠听感定位的，而是靠探针：写入序列、每声道峰值（`--check`）、输出电平（`[level]`）、帧周期（两次 `$4017` 之间的 CPU 时钟）。判据表与回归基线写在
`docs/memo/2026-11-08-nsf-super-mario-usa-single-event.md` 第 12–14 节。
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


### 9.20 YM2612 的 DAC：不是样本，是一条流（2026-11-09）

**这颗芯片和其他采样芯片不是一类。** 别的芯片用寄存器指定一段 ROM；YM2612 的第六通道是被逐个字节
喂的（`0x2A`），字节从哪里来由驱动决定。VGM 里因此有两种形态：

1. **带 PCM 块**（`0x67` type `0x00`，ROM 标记形式是 `0x80`）：字节在文件里，`0x80-0x8F` 负责把它们
   流进 DAC，`0xE0` 负责把读指针挪到某个偏移。`02 Green Hill Zone` 就是这样：13 132 字节，228 次
   `0xE0` 指向 5 个偏移，52.9 s 里写了 447 541 个字节。
2. **没有块**：字节来自 Z80 自己的内存，文件里只剩 `0x2A` 流，唯一的结构是停顿。

**实测（这首）**：写入间隔 1–6 tick 抖动，均值 3.2 → **13 799 Hz**（不是 447 541 / 52.9 s = 8.5 kHz，
那会把 21 s 的停顿算成流的一部分）；按静音切银行得 **1 段**（≥32 字节静音只分出 3 段），所以
**单次击打的边界不在文件里**。

**因此导出单位是银行**：一份 13 132 字节的循环素材，`ym2612dac_000000_0334C`。按击打切会得到一段
1.55 s 的连续音频，那不是“样本”，是猜测。没有块的文件退回按 30 ms 停顿切段（每段内均值定速率、
内容 hash 去重），这条路径还没有真实文件验证过，只在代码里成立。

**可视化**是同一个结论的另一面：没有 ROM 可以指，行画的是**最近的 4 096 字节**（约 0.5 s），速率来自
写入间隔的均值（`kDacRateGap = 64` tick 以上算停顿，不计入）。`0x80` 字节进入窗口——它是驱动在击打
之间流的静音，挖掉会让“会停顿的流”看起来不停顿。