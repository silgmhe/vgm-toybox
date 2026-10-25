# NSF 播放体验：两个问题与一份清单

日期：2026-10-25
分支：`feat/nsf-playback`
状态：**决策已确认，阶段 0 已完成**（2026-10-25；顺序见第 3 节）

## 1. 结论摘要

| 编号 | 用户描述 | 实际根因 | 性质 |
|---|---|---|---|
| P1 | NSF 的通道不支持 MIDI 输出；界面能选到部分通道，三角波选不到 | **NSF 后端没有任何 MIDI 序列**（`getMidiSequence()` 未重写，默认 `nullptr`），界面按 `midiCapability` 显示选择框，与实际能否发送无关。三角波被误标为不可用是第二个独立缺陷 | 功能缺失 + 界面不诚实 |
| P2 | 可视化只支持 2A03 和 N163 | `NsfPlayer::captureState` 只填了这两类通道；底层 xgm 的 8 个声音设备**全部实现 `GetTrackInfo`**，FDS 与 N163 还自带波形数组 | 前端未接，非能力缺失 |
| P3（附带） | — | 混音器的逐通道渲染只覆盖 2A03 + VRC6/VRC7/N163；FME7 无 `RenderChannels`，FDS/MMC5 连轨道都没建 | 音频缺失，需先确认是否纳入范围 |

## 2. 证据

### 2.1 P1：MIDI

`plugin/Source/backends/nsf/NsfBackend.cpp:138-152`

```cpp
static const char* const apuNames[] = { "Pulse 1", "Pulse 2" };
static const char* const dmcNames[] = { "Triangle", "Noise", "DMC" };
// 前者 melodic=true，后者 melodic=false → MidiCapability::unavailable
```

三角波是 4-bit 波表、音高由 11-bit 周期决定，被与噪声/DPCM 归为一类。

`plugin/Source/playback/SongBackend.h:204`

```cpp
virtual const MidiSequence* getMidiSequence (void) const { return nullptr; }
```

只有 libvgm（VGM）后端重写；NSF 后端没有（`NsfBackend.cpp` 内搜不到 `MidiSequence`）。`PlaybackEngine.cpp:1008` 取该指针，为空时 `emitMidiForBlock` 直接返回（`PlaybackEngine.cpp:1270`）。

界面侧只看能力标记：`ChannelStrip.cpp:368`、`:394`、`:404`。

由此得到的现状：

| 轨道 | 界面有 MIDI 框 | 实际发送 |
|---|---|---|
| Pulse 1 / Pulse 2 | 有 | 无 |
| VRC6 / VRC7 / N163 / S5B | 有 | 无 |
| Triangle / Noise / DMC | 无 | 无 |

### 2.2 P2：可视化

`plugin/Source/backends/nsf/NsfPlayer.cpp:409-560`

- `snapshot.chips`：按 header 填入 `VRC6 / VRC7 / FDS / MMC5 / N163 / FME7`；
- `snapshot.channels`：只填 2A03 五条 + N163 八条；
- 源码注释（第 430 行）自认：“Only the N163 reports per-channel state in this core”。

底层能力（`libs/nsfplay/xgm/devices/Sound/*.h`）：8 个设备全部实现 `GetTrackInfo`。

| 芯片 | 可成轨数 | 波形数据 | 说明 |
|---|---|---|---|
| VRC6 | 3 | 无 | 方波 ×2 + 锯齿（可用 ramp 形状合成） |
| VRC7 | 6 | 无 | OPLL/FM，给 stored 形状 |
| FDS | 1 | `TrackInfoFDS::wave[64]` | 可直接画波形 |
| MMC5 | 2 (+ PCM) | 无 | 方波，duty 可控 |
| FME7 (S5B) | 5 | 无 | 方波 ×3 + 噪声 ×2 |
| N163 | 8 | `wave[256]` | 已实现 |

`ITrackInfo` 提供 `GetFreqHz / GetKeyStatus / GetVolume / GetMaxVolume / GetOutput`，与现在画 2A03 用的是同一套接口。

### 2.3 P3：混音缺失

`NsfPlayer::renderChannels`（`NsfPlayer.cpp:655-725`）决定混音器里能听到什么：

| 芯片 | 有轨道 | 逐通道渲染 | 混音模式结果 |
|---|---|---|---|
| 2A03 | 5 | 有 | 正常 |
| VRC6 / VRC7 / N163 | 有 | 有 | 正常（实测 Entry080.nsf：三条 VRC6 轨 RMS 0.0147 / 0.0147 / 0.0309） |
| FME7 | 5 | **无** | 疑似全静音（无测试文件，未验证） |
| FDS / MMC5 | **无** | 无 | 扩展芯片声音在混音模式丢失 |

代码注释（第 705-707 行）称“FME7 的歌仍走一轨一播放器路径”，但 `getCapabilities()`（`NsfBackend.cpp:201`）对全部文件返回 `multiInstance`，注释已与实现不符。

## 3. 决策（2026-10-25 已确认）

**D1. NSF 的 MIDI 走哪条路？→ 选 B（离线预演 + 懒加载）。** 引擎的 MIDI 是“按位置查已算好的序列”，运行期生成会把音符边界检测搬进音频线程；预演只跑 CPU、不渲染音频，按 5 ms 采样即可，且只在用户第一次给某条 NSF 轨指定 MIDI 通道时触发。在此之前先做阶段 0 的诚实性修正。

| 方案 | 做法 | 精度 | 成本 | 风险 |
|---|---|---|---|---|
| A 运行期生成 | 用 `captureState` 按音频块采样 gate/pitch，实时发 MIDI | 一个块（约 11 ms @512） | 中 | 事件生成落在音频线程 |
| B 离线预演 | 加载时静音快跑全曲，记录通道状态 → 生成 `MidiSequence`，复用现有发送路径 | 同上 | 中高（需实测 180 s 曲的预演耗时） | 与 VGM 架构一致，seek 后仍准确 |
| C 暂不支持 | 隐藏 NSF 所有 MIDI 框，只保留能力标记 | — | 很小 | 诚实但少一功能 |

**D2. 可视化范围 → 五颗一次做完，表驱动。** 顺序按性价比 FDS → VRC6 → MMC5 → FME7 → VRC7；加芯片应该只加一行表项，而不是第五次打开同一个 switch。

**D3. 修 P3，且提前到可视化之前。** 可视化的价值是“看得见的等于听得见的”；先画再发现没声音是最坏顺序。缺的三份测试样本由我们写最小 6502 INIT/PLAY 生成，不依赖外部文件。

## 4. 任务清单（按确认后的顺序）

### 阶段 0：诚实性 —— ✅ 已完成（提交 eat/nsf-playback 上）
- [x] Triangle 改为 melodic；Noise / DMC 保持 unavailable
- [x] BackendCapabilities::canEmitMidi + 引擎 canEmitMidi()；混音器据此决定是否提供 MIDI 通道
- [x] 测试：NSF 的 Triangle 能力为 melodic；canEmitMidi 与格式一致（.vgm/.vgz 为真，.nsf 为假）
- [x] 诊断输出区分“格式无 MIDI 序列”与“该通道漏了音符”
- [x] harness 验证：NSF 条带显示 Not available，VGM 条带仍为 Off（可选）

### 阶段 1：混音缺失（P3）

**已完成：FDS**（e3509aa）——芯片实例、总线转发 $4023/$4040-、轨道、495 字节测试资源，RMS 0.11196。

**已回退：三角波的“静音归零”**（32e8bd0）。为了让静音读数为 0，把关闭的三角波输出改成 0，结果每次起音都从 0 跳到相位电平——**音头 click**，而硬件是保持 DAC 电平、由输出电容去直流。教训：不要在芯片核里“清理”硬件本身就有的行为；去直流要放在输出边界，且要处理启动瞬态（逐通道阻断会改变独奏平衡，引擎级阻断会带来 150 ms 的噗声）。

**待做：MMC5、FME7**
- [ ] captureState 之外先补渲染：FDS / MMC5 建轨道，FME7 实现 RenderChannels
- [ ] 三份最小 NSF 测试资源（手写 6502 INIT/PLAY）：FDS、MMC5、FME7
- [ ] 每份资源断言“该芯片的轨道必须有声”（RMS > 阈值），而不是只断言“有轨道”

### 阶段 2：可视化（P2）
- [ ] captureState 改成表驱动：一颗芯片一条表项（名称、起止轨、shape）
- [ ] VRC6 三轨 / VRC7 六轨 / FDS 一轨（带 wave[64]）/ MMC5 两轨 / FME7 五轨
- [ ] 轨道名与 VGM 侧一致（Square n / Saw Wave / FM n）
- [ ] 每个芯片至少一张键盘视图截图，逐轨独奏确认行与声音对应

### 阶段 3：MIDI（P1，方案 B）
- [ ] 懒加载预演：第一次指定 MIDI 通道时，在工作线程跑一遍 CPU（5 ms 采样），生成 MidiSequence
- [ ] 预演耗时实测（至少一首 180 s 曲），结果写进本节
- [ ] 测试：三轨各出一个音符，音高与通道频率一致

## 5. 验收标准

- 任意 NSF 在混音器里的轨道数 = 芯片实际声部数（2A03 五条 + 扩展芯片各声部）；
- 每条轨能独奏、能被 MIDI 输出（若 D1 选了 A/B）；
- 键盘视图对每颗芯片都能画出对应的行，波形/占空比与该芯片一致；
- 上述每一条都有端到端断言，而不只是截图。
