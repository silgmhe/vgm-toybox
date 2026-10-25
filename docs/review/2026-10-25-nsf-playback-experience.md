# NSF 播放体验：两个问题与一份清单

日期：2026-10-25
分支：`feat/nsf-playback`
状态：**分析完成，方案待确认**（第三节的决策点未定，先不动代码）

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

## 3. 决策点（待确认）

**D1. NSF 的 MIDI 走哪条路？**

| 方案 | 做法 | 精度 | 成本 | 风险 |
|---|---|---|---|---|
| A 运行期生成 | 用 `captureState` 按音频块采样 gate/pitch，实时发 MIDI | 一个块（约 11 ms @512） | 中 | 事件生成落在音频线程 |
| B 离线预演 | 加载时静音快跑全曲，记录通道状态 → 生成 `MidiSequence`，复用现有发送路径 | 同上 | 中高（需实测 180 s 曲的预演耗时） | 与 VGM 架构一致，seek 后仍准确 |
| C 暂不支持 | 隐藏 NSF 所有 MIDI 框，只保留能力标记 | — | 很小 | 诚实但少一功能 |

**D2. 可视化范围**：四颗扩展芯片一次做完，还是先做 FDS（带波形）+ VRC6？

**D3. 是否顺带修 P3**（FDS/MMC5 无轨道、FME7 无逐通道渲染）？不修的话，新画出来的轨可能没有声音。

## 4. 任务清单

### 阶段 0：诚实性（不依赖任何决策）
- [ ] `Triangle` 改为 `melodic`；`Noise` / `DMC` 保持 `unavailable`
- [ ] 界面：后端没有 MIDI 序列时，不显示可用的 MIDI 选择框（或显示为禁用并带说明）
- [ ] 测试：断言 NSF 的 Triangle 能力为 melodic；断言无序列时 UI 不给 MIDI 框

### 阶段 1：可视化（P2）
- [ ] `captureState` 补 VRC6 三轨（方波/方波/锯齿）
- [ ] 补 VRC7 六轨（stored 形状）
- [ ] 补 FDS 一轨（带 `wave[64]`，与 N163 同路径）
- [ ] 补 MMC5 两轨（方波，duty）
- [ ] 补 FME7 五轨（方波 ×3 + 噪声 ×2）
- [ ] 每颗芯片的轨道名与 VGM 侧命名保持一致（`Square n` / `Saw Wave` / `FM n`）
- [ ] 键盘视图截图：每颗芯片至少一张，逐轨独奏确认行与声音对应

### 阶段 2：MIDI（P1，取决于 D1）
- [ ] 若选 A：引擎增加“从状态快照生成 MIDI”的通道，NSF 后端声明支持
- [ ] 若选 B：加载期预演 + `MidiSequence` 生成，复用 `emitMidiForBlock`
- [ ] 若选 C：只做阶段 0 的第二条
- [ ] 测试：三轨各出一个音符，音高与通道频率一致

### 阶段 3：混音缺失（P3，取决于 D3）
- [ ] FDS/MMC5 建轨道
- [ ] FME7 实现 `RenderChannels`
- [ ] 三条新路径各加一个端到端断言（有音轨必须出声）

### 测试资源缺口
- 现有 `test_file/nsf`：`1983.nsf`（纯 2A03）、`Entry025.nsf`（纯 2A03）、`Entry080.nsf`（VRC6）、`fod_snowburnb.nsf`（VRC6）、`fod_hopeless.nsf`（N163）
- **缺**：FDS、MMC5、FME7、VRC7 的样本 → 需要外部提供，或写一个最小 6502 驱动生成 NSF（工作量另计）

## 5. 验收标准

- 任意 NSF 在混音器里的轨道数 = 芯片实际声部数（2A03 五条 + 扩展芯片各声部）；
- 每条轨能独奏、能被 MIDI 输出（若 D1 选了 A/B）；
- 键盘视图对每颗芯片都能画出对应的行，波形/占空比与该芯片一致；
- 上述每一条都有端到端断言，而不只是截图。
