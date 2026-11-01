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

- 采样类芯片（C140/C352/QSound/OKIM6295/…）的**真实波形**——`wave` 留空，波形区画占位；
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

1. **采样与波表芯片的波形是占位**。行是亮的，波形区写着"这一个需要样本字节"。补的时候按芯片分批，每批一个提交。
2. **`stored` 类的行拖拽出去只能得到 `paintNoCycle` 同样的空**——`writeChannelShape` 会返回 `nothingToWrite`，拖拽在 hover 阶段就被拒绝。**和限制 1 是同一件事的两面。**
3. **FM 的波形是"这一个音色"的静态曲线**，不反映 LFO 与包络——`cycleFor` 只画一个周期。
4. **VGM 里没有通道概念**。通道是解码器从寄存器推出来的；某个驱动用非常规方式写寄存器时，行会显示得不准，而这种不准**不会报错**。
5. **性能未实测**：音频线程里跑 30 个解码器，每个 block 喂几条写。NSF 那边只喂一个核，这里多 30 倍。**第一次跑起来要量**，如果掉音，第一刀砍波形计算（ADR-006）。

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
