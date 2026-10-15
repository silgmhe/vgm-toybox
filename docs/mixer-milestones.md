# VGMPlay Mixer 混音器 — 需求整理 · 实现方案 · 里程碑

> 状态：M1 / M2 已完成并验证通过（见 §4），M3 待开始
> 分支：`feature/mixer`
> 日期：2026-10-15

---

## 1. 需求整理

### 1.1 用户原始需求

1. 每个轨道（Track）**对应芯片的一个通道**（例：NES 2A03 = Pulse 1 / Pulse 2 / Triangle / Noise / DPCM 五条轨）；无论原生单声道还是立体声，**统一映射为立体声**输出，便于统一管理。
2. 多芯片时要**按芯片对轨道分组**。
3. 具备常规 DAW 混音器功能：每轨**静音（Mute）、独奏（Solo）、音量、声像（Pan）**，以及**峰值表**。
4. 具备可视化能力：每轨**波形（示波器）**与**频谱（FFT）**。

### 1.2 需求细化（实现口径）

| 编号 | 需求 | 细化后的定义 |
|---|---|---|
| R1 | 轨道 = 通道 | 轨道标识 = `(deviceID, channelIndex)`，由 libvgm 的设备/通道元数据枚举生成 |
| R2 | 立体声统一 | 每轨渲染链末端一律为 L/R 双声道 float；原生单声道信号在混音级等功率分配 |
| R3 | 按芯片分组 | 分组 = 设备实例（同类型不同 instance 视为不同组），组头显示芯片名 + 实例号 |
| R4 | Mute / Solo | 混音级增益开关；Solo 语义：存在任一 Solo 时，未 Solo 轨静音（DAW 惯例） |
| R5 | 音量 / 声像 | 每轨独立，实时可调；声像采用等功率（constant-power）法则 |
| R6 | 峰值表 | 每轨 L/R 峰值（含衰减保持），刷新 ≥ 20 Hz |
| R7 | 示波器 | 每轨时域波形（滚动缓冲），刷新 20–30 Hz |
| R8 | 频谱 | 每轨 FFT 幅度谱（对数频率轴），刷新 20–30 Hz |
| R9 | 主输出 | 建议增加 Master 条（总增益 + 总表头），并保留现有 master volume / fade 语义 |
| R10 | 无回归 | Mixer 功能未启用时，播放行为、音质、CPU 与现状完全一致 |

### 1.3 非功能需求（硬约束）

- **RT 安全**：音频线程内**零锁竞争、零内存分配**（沿用 `96a378a` 的教训与纪律：UI 轮询必须无锁）。
- **确定性**：同一文件、同一 seek 位置，多轨渲染求和结果应与单实例渲染一致（线性混音芯片；非线性芯片见 §5 风险）。
- **CPU 预算**：典型文件（≤8 轨）附加开销 < 单核 10%；极端文件有明确降级策略。
- **可测试**：引擎部分必须能在无 UI 的离线测试（`Tests/`）中验证。

---

## 2. 可行性分析（已核查 libvgm 源码）

| 能力 | 结论 | 依据（源码位置） |
|---|---|---|
| 通道级静音 | ✅ 支持 | `PLR_MUTE_OPTS::chnMute[2]`（每设备 64 通道位掩码，`libvgm/player/playerbase.hpp:77`）→ `VGMPlayer::SetDeviceMuting` → `RefreshMuting` → `devDef->SetMuteMask`（`libvgm/player/vgmplayer.cpp:712-725`） |
| 核不支持时的保护 | ✅ 有 | `RefreshMuting` 对 `SetMuteMask == NULL` 做了检查（同上 `:720`） |
| 通道数 / 通道名 | ✅ 可直接获取 | `DEV_DECL::channelCount` / `channelNames`（`libvgm/emu/EmuStructs.h:126-127`），经 `PLR_DEV_INFO::devDecl` + `devCfg` 调用 |
| 通道静音按核映射 | ⚠️ 需按核处理 | 例：NES NSFPlay 核把掩码拆为 APU(2) + DMC(3) + FDS(1)（`libvgm/emu/cores/nesintf.c:650-662`） |
| 设备挂起省 CPU | ✅ 真省（跳过仿真与重采样） | `vgmplayer.cpp:2022-2033`：`muteOpts.disable` 命中时跳过 `Resmpl_Execute` |
| 通道级声像 | 存在但不用 | `PLR_PAN_OPTS` / `RWF_CHN_PAN`；我们统一在混音级实现，行为可预期、RT 更简单 |
| 设备/通道枚举 | ✅ 已有基础 | 现有 `VgmEngine::enumerateChips()`（含 linked device、T6W28 去重逻辑） |

**核心结论**：以「**每轨一个 libvgm 播放实例**」为方案是可行的 —— 每实例仅让目标通道可听（`chnMute` 掩码），其余设备用 `disable` 挂起（真省 CPU），实例输出在混音级做增益/声像/求和。

---

## 3. 实现方案

### 3.1 总体架构

```
┌──────────────────────────── VGMPlay 插件 ────────────────────────────┐
│ MixerEngine（新，接管音频路径；对外 API 兼容现 VgmEngine）              │
│                                                                      │
│  TrackPlayer × N  ──每轨一个 libvgm 实例（共享同一份文件内存）──┐      │
│    ├─ PlayerA/VGMPlayer（目标设备 + 目标通道可听）              │      │
│    ├─ 渲染 → WAVE_32BS → float 立体声                          │      │
│    └─ 位置/配置与主实例严格同步                                 │      │
│                                                               ▼      │
│  TrackMixer：每轨 gain/pan/mute/solo（原子参数）→ 求和 → 主增益 → 输出 │
│                                                               │      │
│  MeterTap × N：峰值 → atomic（UI 无锁读）                      │      │
│  ScopeRing × N：SPSC 环形缓冲（音频线程写，UI 线程读）          │      │
└──────────────────────────────────────────────────────────────┘
        ▲ 20–30 Hz 无锁读取
┌───────┴──────────────────────────────────────────────────────┐
│ ui/MixerPanel：分组头 + 通道条（名称/示波器/频谱/Pan/推子/MS） │
└──────────────────────────────────────────────────────────────┘
```

**迁移策略（零回归）**：把现有 `VgmEngine` 重构为「`TrackPlayer`（单轨渲染+加载/seek/配置）」+「`MixerEngine`（编排 N 个 TrackPlayer + 混音 + 对外 API）」。`N = 1` 时即当前行为（逐样本一致）；启用 Mixer 模式时重建为 `N = 通道总数`。

### 3.2 音频通路与同步

- **同源加载**：文件只读一次，共享同一 `juce::MemoryBlock`；为每个 TrackPlayer 建独立 `DataLoader`（避免 N 份文件副本，大文件友好）。
- **严格同步**：`play/pause/stop/seek/setLoopCount/setFadeSeconds` 在同一消息线程命令下广播到所有实例；音频线程每个 block 驱动所有实例渲染**相同采样数**（VGM 仿真确定性，无随机源）。
- **结束/循环判定**：以实例 0 的 `PLREVT_END` 为准，停止时统一停全部实例。
- **混音**（音频线程，无锁无分配）：
  `out += track[i] × effectiveGain(i) × panMatrix(pan[i])`，其中
  `effectiveGain = (mute || (anySolo && !solo)) ? 0 : gain`；`anySolo` 由原子计数维护，每 block 计算一次 O(N)。

### 3.3 数据模型（草案）

```cpp
struct MixerTrack
{
    UINT32 deviceID;            // libvgm 设备 ID
    int    channelIndex;        // 通道序号（掩码位由核映射决定）
    juce::String name;          // 通道名（libvgm channelNames，缺失时 "Ch n"）
    juce::String groupName;     // 芯片分组名（设备名 + 实例号）

    std::atomic<float> gain { 1.0f };
    std::atomic<float> pan  { 0.0f };     // -1..+1
    std::atomic<bool>  mute { false };
    std::atomic<bool>  solo { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };   // 音频线程写，UI 读
};
```

### 3.4 线程与 RT 规则（继承现有纪律）

- 音频线程：try-lock 失败即静音整块（同现状），锁内仅做 N 次 `Render` + 混音；**UI 轮询一律无锁**（位置原子量、缓存时长/循环点、峰值原子量、示波器环形缓冲）。
- 消息线程：命令（play/seek/mute/gain 等）用原子参数；只有加载/重建实例/seek 才进锁。
- 可视化：UI 线程做 FFT（`juce::dsp::FFT`，512–1024 点），不对音频线程提任何同步要求。

### 3.5 UI 设计草案

- **入口（已定）**：不做视图切换 —— Mixer 是编辑器内的**一张新卡片，放在 Sound Chip 卡片下方**；卡片内按芯片分组，**每组一行轨道（Track）**，横向滚动，内容超出时提供滚动条（ScrollBar）。
- **通道条**（宽约 76 px，自上而下）：通道名 → mini 示波器 → mini 频谱 → Pan 旋钮 → 推子 + 峰值表 → `M`/`S` 按钮。
- **分组**：每个芯片一个分组头（芯片名/实例号/通道数），组内横排通道条。
- **Master 条**：最右固定列（总推子 + 总峰值表）。
- 复用现有 `Theme` / `CardComponent` / `SoftShadow` / `LightLookAndFeel` 视觉体系。

### 3.6 计划新增文件

| 文件 | 职责 |
|---|---|
| `plugin/Source/vgm/TrackLayout.{h,cpp}` | 通道枚举（设备+通道数+名称+掩码位映射）、分组构建 |
| `plugin/Source/vgm/MixerEngine.{h,cpp}` | 多实例编排、混音、无锁对外接口 |
| `plugin/Source/vgm/TrackPlayer.{h,cpp}` | 由现 `VgmEngine` 拆出的单轨引擎 |
| `plugin/Source/ui/MixerPanel.{h,cpp}` | 混音器卡片（分组 + 通道条容器） |
| `plugin/Source/ui/ChannelStrip.{h,cpp}` | 单通道条控件 |
| `plugin/Source/ui/ScopeView.{h,cpp}` / `SpectrumView.{h,cpp}` | 可视化组件 |
| `Tests/MixerEngineTest.cpp` | 离线验收测试（见 §4 M1） |

### 3.7 关键决策（ADR 摘要）

**ADR-1：每轨独立渲染实例（而非单实例设备级静音 / 离线预渲染）**
- 备选 A：单实例 + libvgm 设备静音 —— 无法提供**每通道**独立音量与每轨音频流，否决。
- 备选 B：离线预渲染整曲分轨到内存 —— 每轨数十 MB、seek/循环复杂、加载时间线性增长，否决（保留为极端优化的后备）。
- 结论：多实例；用 `disable` 挂起非目标设备控制成本；先以典型文件的 CPU 实测为准（M1 验收项）。

**ADR-2：Mute / Solo / 增益 / 声像全部在混音级实现**（不改动 libvgm 播放状态）
- 好处：任意时刻可改、原子参数 RT 安全、换文件自动复位、不影响 seek/循环正确性。

**ADR-3：通道元数据优先取自 libvgm（`channelCount` / `channelNames`），缺失时兜底 "Ch n"；掩码位映射按核差异单测覆盖。**

**ADR-4：Mixer 模式为显式开关**；关闭时 `N = 1`（现状路径）。启用时重新加载实例并 seek 到当前缓存位置（切换瞬间短暂静音，与加载行为一致）。

**ADR-5：可视化数据走「音频线程无锁环形缓冲 + UI 线程 FFT」**，音频线程不承担任何 FFT/绘制开销。

### 3.8 性能与降级策略

- 成本模型：`CPU ≈ Σ(每芯片仿真成本 × 该芯片轨道数)`（其余设备 `disable` 后不耗 CPU）。
- **轨道数不做合并（已定）**：轨道数 = 歌曲实际通道数；成本靠 `disable` 挂起与实测数据控制，必要时再评估优化（例如重型芯片的按需降级）。
- 实测（M1）：SEGA PSG 4 轨文件 —— 单实例 0.7 ms / 20 块 vs 混音 2.5 ms（≈ ×3.8，符合 4 个隔离实例的预期；多芯片文件因 `disable` 会更省）。

---

## 4. 里程碑

### M1 — 引擎：多轨渲染骨架（无 UI，可测）✅ 已完成（2026-10-15）
- **交付**：`Source/vgm/TrackLayout.{h,cpp}`（通道枚举/分组/命名）、`Source/vgm/TrackPlayer.{h,cpp}`（单轨 libvgm 实例，由 `VgmEngine` 拆分）、`Source/vgm/MixerEngine.{h,cpp}`（N 实例 + 混音 + 无锁接口，取代 `VgmEngine`）；离线验收测试 `Tests/MixerEngineTest.cpp`（目标 `vgm_mixer_test`）。
- **验收结果**（`chips_test.vgz` / `test.vgm` / `test_loop.vgm`，全部通过）：
  1. ✅ 轨道数 = 设备通道数（SEGA PSG = 4 轨：`1` / `2` / `3` / `Noise`，名称取自 libvgm `channelNames`；`.vgz` 与 `.vgm` 均验证）；
  2. ✅ 单位增益求和与单实例渲染**逐样本一致（差 −1000 dB = 完全相同）**；
  3. ✅ seek / loop / 结束全部同步（轨道间位置偏差 0.0000 s；播放越过结尾后输出静音，`isFinished` 为真）；
  4. ✅ `disable` 生效：4 轨文件混音渲染耗时 ≈ ×3.8（4 个隔离实例的预期值）。
- **附带修复**：`enumerateChips()` 现在保留用户勾选的芯片静音状态（采样率变化、模式切换重建播放器时不再丢失）。
- **已知遗留**：T6W28 的第二半在混音器里**保留为独立轨道**（与 Sound Chips 卡片不同 —— 卡片隐藏它是出于显示考虑，混音器隐藏它会丢音频）；核没有 `SetMuteMask` 时该设备的通道无法单独隔离（会退化为设备级 1 轨），遇到实际文件再处理。

### M2 — 混音核心 + 通道条 UI ✅ 已完成（2026-10-15）
- **交付**：`ui/ChannelStrip.{h,cpp}`（**标准 DAW 通道条布局**：名称 → Pan 旋钮 → 垂直电平表 + 垂直增益推子 → 方形 M/S 按钮；静音=红色、独奏=琥珀色，Master 条复用同类）、`ui/MixerPanel.{h,cpp}`（Sound Chip 卡片下方的新卡片：每芯片一行、分组头、`Viewport` 双向滚动条、右侧常驻 Master 条、卡片头部的混音开关与光晕）、`LightLookAndFeel` 新增垂直推子与旋钮绘制、`Icons`/`IconButton` 增加混音器图标；引擎侧新增总输出峰值表与「核心不支持通道静音时退化为设备级单轨」的兜底；**混音模式默认开启**（与单实例逐样本一致，开关作为省 CPU 的退出选项）。
- **验收结果**（`vgm_mixer_test`，全部通过）：
  1. ✅ Solo 与「静音其他轨」电平一致（差异 0.0054 %）；
  2. ✅ 硬左声像时右声道完全静音（balance 法则）、左声道正常；
  3. ✅ 增益 0.5 → 电平比例 0.500；
  4. ✅ 全部静音 → 输出静音；关闭混音模式 → 恢复与单实例一致的混合。
- **重要发现（测试方法）**：libvgm 在 seek 时**不复位振荡器相位**，因此任何"由 seek 开始"的两次渲染都无法逐样本一致（≈ −30 dB，**单实例模式完全相同**）——比较混音结果时要用电平/频谱，或让两侧走同一种起点。已记录进 `CODEBUDDY.md` 的测试章节。
- **UI 细化（第二轮，按用户反馈）**：M/S 按钮加大到 24 px 高并**均分整个通道条宽度**（不再出现省略号）；通道条加高到 170 px 且**随卡片高度伸展**（最大化时长推子）；电平表/推子保持垂直；Mixer 卡片**自适应 Sound Chips 与走带卡片之间的全部区域**；窗口**只保留最小尺寸 660×520**（解除上限）并为独立运行窗口**补上最大化按钮**。

### M3 — 可视化：示波器 + 频谱
- **交付**：`ScopeView`（滚动波形，20–30 fps）、`SpectrumView`（FFT 对数轴）、每轨独立数据通路。
- **验收**：静音轨波形/频谱归零；满轨 UI 刷新时 CPU 附加 < 5%；无撕裂/闪烁。

### M4 — 打磨与收尾
- **交付**：窗口尺寸/滚动体验、轨道设置持久化（插件状态）、性能上限策略落地、`CODEBUDDY.md` 与文档更新、全量回归。
- **验收**：长会话稳定（无卡顿、无锁竞争回归）；宿主中保存/恢复会话正确。

---

## 5. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 非线性混音的芯片（如 NES APU、部分 DAC 共享通道）求和与原生混合有电平/相位偏差 | 中 | M1 离线量化偏差；可接受则记录，必要时提供 A/B 校准系数 |
| 部分核无通道静音（`SetMuteMask == NULL`） | 中 | 降级为设备级轨道；UI 标注 |
| 重型芯片（OPN2 等）多实例 CPU 偏高 | 高 | `disable` 非目标设备；轨道数阈值与降级；M1 实测数据驱动 |
| 多实例 seek/循环不同步 | 高 | 统一命令广播 + 单测覆盖（M1 验收 3） |
| RT 安全回归（新代码引入锁/分配） | 高 | 沿用既有纪律与代码审查清单（音频线程零锁竞争零分配） |
| 大文件共享内存被误复制 | 中 | 共享 `MemoryBlock` + 每实例独立 `DataLoader`，M1 验证内存占用 |

---

## 6. 开放问题（需确认后进入实现）

1. ~~视图入口与窗口形态~~（**已定**）：Mixer 是编辑器内的新卡片，位于 **Sound Chip 卡片下方**；不切换视图、不做独立窗口。卡片按芯片分组、每组一行轨道，并提供滚动条。
2. ~~轨道数上限~~（**已定**）：**不做合并**，轨道数按歌曲实际通道数。
3. **Master 条**：是否需要（推荐需要）？是否需要总表 + 总推子之外的内容？
4. **换文件行为**：轨道上的音量/声像/静音设置按「芯片类型+通道序号」尽量保留，还是每次重置为默认？
5. **可视化规格**：示波器窗口长度（建议 ~100 ms）、频谱分辨率（512/1024 点）与刷新率（20–30 Hz）是否符合预期？
6. **延伸功能**：是否需要「分轨导出（bounce）」等后续能力（本次不做，仅确认方向）？
