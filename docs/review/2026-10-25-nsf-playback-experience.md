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

**状态：代码完成。FDS 进 CI，MMC5 / FME7 手工验证（2026-10-25 决定不做自动化测试）。**

#### 1.1 FDS —— 已进 CI（`e3509aa`）

芯片实例化、总线转发 `$4023` / `$4040-$408A`、一条轨道、495 字节手写资源（一个 6502 INIT
写 64 字节波表和一个频率，PLAY 直接 `RTS`）。实测 `FDS` 轨 RMS 0.11196，其余五轨为 0。

#### 1.2 MMC5 / FME7 —— 代码与资源齐备，不进 CI（`7cd1d19`）

代码、资源、断言都做完了，`[3c]` 三条在手工运行时全绿：

| 芯片 | 资源 | 轨道 | 实测 RMS |
|---|---|---|---|
| MMC5 | `mmc5_song.nsf`（150 字节） | `Square 1` / `Square 2` / `PCM` | **0.06633** |
| FME7 | `fme7_song.nsf`（170 字节） | `Square 1..3` | **0.12996** |

**决定：不注册进 ctest**，两条原因：

1. 套件里“静音所有芯片 → 混音应当无声”的断言对含 2A03 的文件不成立（三角波保持 DAC
   电平，混音恒定 0.147），这是直流偏置问题，见 `docs/memo/2026-10-25-nsf-dc-offset.md`；
2. **含这两颗芯片的 NSF 在网上罕见**，不作为重点保障对象。真遇到问题再回头，
   代码和资源都在，复现只需要一条命令：

```
build/plugin/vgm_mixer_test.exe plugin/Tests/assets/mmc5_song.nsf 1 44100 --tracks
build/plugin/vgm_mixer_test.exe plugin/Tests/assets/fme7_song.nsf 1 44100 --tracks
```

`CMakeLists.txt` 里保留了这两条原因，避免以后有人误以为漏了。

#### 1.3 顺带修掉的两个真 bug

| 现象 | 根因 |
|---|---|
| FDS 在某些路径完全不动 | 一条渲染路径里被 tick **两次**，另两条（跳转预热、逐通道）**一次都没有**；现在三处统一为同一组六颗芯片 |
| MMC5 芯片建了却永远没声音 | 它的寄存器在 CPU 自己的 `$5000-$5015`，而 `NsfBus` 的白名单只放行 `$4000-$4017` 与四个扩展窗口——**与 FDS 完全同型**的缺陷 |

一条值得记住的教训：**“芯片建出来”和“芯片被喂数据”是两件事**。NSF 的总线白名单是按地址段
手工维护的，每接一颗新芯片都要问一句：它的写入会不会被挡在门外？

#### 1.4 已回退：三角波的“静音归零”（`32e8bd0`）

为了让静音读数变成 0，把关闭的三角波输出改成 0，结果每次起音都从 0 跳到相位电平 ——
**音头 click**（用户实测确认），而硬件是保持 DAC 电平、由输出电容去直流。

教训：**不要在芯片核里“清理”硬件本身就有的行为**；去直流属于输出边界的工作，且必须处理
启动瞬态（逐通道阻断会改变独奏平衡，引擎级阻断会带来 150 ms 的噗声）。四次尝试与那个挡住
它们的不变量，都记在 `docs/memo/2026-10-25-nsf-dc-offset.md`。

### 阶段 2：可视化（P2）

**状态：代码完成，五颗芯片全部有 CI 覆盖（42/42）。**

#### 2.1 表驱动

`captureState` 里加了一张表，每行是「名字 / 轨道索引 / 芯片自己的通道号 / 形状 / 占空比档数」。
四颗芯片共用同一套填充逻辑，因为它们都通过 `ITrackInfo` 报自己的状态：

| 芯片 | 行数 | 形状 | 实测（`--chips`） |
|---|---|---|---|
| FDS | 1 | stored（64 样本波表） | `note=64 vol=1.00 wave=64` |
| VRC6 | 3 | pulse ×2 + saw | `Square 3 note=69`、`Saw Wave note=57 kind=6` |
| MMC5 | 3 | pulse ×2 + delta（PCM） | `Square 1 note=53` |
| S5B | 3 | pulse ×3 | `Square 1 note=57` |
| VRC7 | 6 | FM | `FM 1 note=69 kind=7` |

FDS 单独处理：`dynamic_cast<TrackInfoFDS*>` 拿到 `wave[64]`，按单极性（中点 31.5）归一化。

新形状两种：`kShapeSaw = 6`（VRC6 的累加锯齿）、`kShapeFm = 7`（VRC7 的 FM）。
`WaveShape.cpp` 增加对应合成：锯齿画成阶梯上升（不是平滑线），FM 画成调相载波。

**顺带修掉一个地雷**：原有代码用 `waveKind >= kShapeNoise` 判断“无音高”，
新形状 6/7 都大于 4，会让 VRC6 锯齿与 VRC7 一起被判成无音高。改成显式列举
（`== kShapeNoise || == kShapeDelta`）。

#### 2.2 抓到三个真 bug

| 现象 | 根因 |
|---|---|
| VRC6 的 Solo/静音对扩展声道无效，且轨道名与声音错位 | ① 轨道顺序是 `Saw Wave / Square 3 / Square 4`，而 `RenderChannels` 的通道 0 是**第一个脉冲**——名与声整体错位一格；② `RenderChannels` 里 `mask = ~(1 << i)` 直接覆盖了混音器设的掩码（solo / 轨道静音 / 芯片勾选框），三颗芯片（VRC6/MMC5/VRC7）同型 |
| S5B 的 Solo 无效 | 它的 `RenderChannels` 直接读 `psg->cout[i]`，**根本没看 mask** |
| MMC5 的芯片卡比轨道少一组 | `NsfBackend` 的 `info_.chips` 漏了 MMC5 的声明，落到 `default` |

修法分别是：轨道改成芯片的通道顺序（`Square 3 / Square 4 / Saw Wave`）、
`mask = ~(1 << i) | saved`、FME7 保存并检查 `mask`、补上 MMC5 的声明。

#### 2.3 两份新资源，与一次断言修正

语料里没有任何 VRC6 / VRC7 的 NSF（5 个文件全是 N163 或无扩展），所以照阶段 1 的做法自己拼：
`vrc6_song.nsf`（160 字节，脉冲 440 Hz + 锯齿 220 Hz，用来验证**通道顺序**）与
`vrc7_song.nsf`（160 字节，`$9010`/`$9030` 写 OPLL 寄存器 0x30/0x10/0x20，437 Hz）。

VRC7 有两点值得记下：

1. `SetClock` 把时钟**除以 36** 才存起来，频率公式要用 `clock/36`；
2. 音色号 **0 是自定义槽**，不加载 patch 就是静音——用内置音色 1 才有声音。

同时修了它的 `slot_key_status` 位映射（`3 << trk` → `3 << (trk * 2)`）：
原来按键道 0 会连通道 1、2 一起点亮。

**断言修正**：套件里「静音某颗芯片 → 混音应当变化」用的是原始 RMS，
而直流偏置会与音乐**反相抵消**——于是静音一颗芯片反而让读数变大（VRC6：0.0849 → 0.1150/0.1470）。
改成**去均值后的能量**（`acRms`）后，这条断言问的才是「这颗芯片有没有贡献声音」。
五份资源因此全部可以进 CI，此前「MMC5/FME7 只手工验证」的限制随之解除。

#### 2.4 外部验证暴露的两个问题

用户实测两首真实曲目：`fod_snowburnb.nsf`（VRC6）正常；`15_coffeezoneb.nsf`（VRC7）**被识别成 VRC6**，
没有 FM 通道也没有声音。

**根因：chip flags 有两个位置，我们只读了一个。**

| 文件 | 0x7A | 0x7B | 实际芯片 |
|---|---|---|---|
| `fod_snowburnb.nsf` | 0x00 | 0x01 | VRC6 |
| `15_coffeezoneb.nsf` | 0x00 | 0x02 | VRC7 |
| `Entry080.nsf` | 0x00 | 0x01 | **VRC6**（此前被当成 N163） |
| `1983.nsf` | 0x00 | 0x00 | 无 |

0x7A 是规范位置，0x7B 是 NSF 2.0 的扩展位；而测试语料里所有真实文件都把芯片写在 **0x7B**，
0x7A 留 0。此前只读 0x7A 的后果：

1. 这些曲目被判成“无扩展芯片”；
2. `detectUndeclaredChip` 随后按“$9000-$B003 有写入”猜成 **VRC6**（候选表第一项），于是 VRC7 曲目被当成 VRC6；
3. `Entry080` 一直显示为 N163，其实它是 VRC6 —— 修好后三轨都有声音（0.0135 / 0.0192 / 0.0301）。

修法：`chip()` 先看 0x7A，为空再看 0x7B。自制资源写在规范的 0x7A，
**因此测试永远抓不到这个 bug** —— 已在 `NsfFormatTest` 里补两条断言
（0x7B 单独生效、两者都有时 0x7A 优先）。

修后 `15_coffeezoneb.nsf` 是 11 条轨（2A03 五条 + `FM 1..6`），六条 FM 全部有声；
`fod_snowburnb.nsf` 的 VRC6 三轨在 30 秒窗口内也有声（0.0092 / 0.0093 / 0.0135）。

#### 2.5 样本通道的音高

2A03 的噪声通道和 DPCM 通道都没有音高寄存器，界面和 MIDI 都需要一个音符，所以给它们编号。

**噪声**：十六档 period 铺成十六个半音。FamiStudio 的约定是 `(period ^ 0x0F) + 32`，
它的 note 体系里 C4 = 49，而 MIDI 的 C4 = 60，所以这里写成 `(period ^ 0x0F) + 43`，落在 MIDI 43–58。
period 0 最亮，因此取反。**这是编号，不是频率**——真实周期跨度十个八度，没有一个能活到乐器里。

**DPCM**：一个音符对应一个"样本槽"，由 `($4012 地址, $4010 速率)` 决定，从 MIDI 60 起按首次出现顺序分配。
速率是键的一部分，因为同一段字节放慢一半就是低一个八度——FamiStudio 也是这么分的。

**与 FamiStudio 的两处差异**（有意）：
- 它按**样本字节内容**去重（`FindMatchingSample`），这里按地址。真实曲子里不同地址几乎总是不同样本，
  结果一致；只有"两个地址内容完全相同"时会少合并一次。换来的是不必读 ROM、不必处理 bank。
- 它把 `(样本, 速率, loop)` 作为键，这里只看 `(地址, 速率)`。

**预扫**：`start()` 里跑一遍全曲，输出丢掉，只为建这张表。实测 180 秒音频约 **0.7 秒**——
CPU 模拟比实时快十五倍，这是它能同步做的唯一原因。表按**文件内容哈希 + 曲号**缓存在
`NsfPlayer.cpp` 的静态表里，因为 seek 会重建 player，缓存在文件对象上等于没有。
（第一次尝试放在 `NsfFile` 上，`vrc6_song` 一个用例从 1 秒涨到 130 秒。）

测出来一个真 bug：`$4012`（地址）和 `$4010`（速率）可能写在**不同帧**，早期实现只看 `$4012` 就记录，
于是先记下"地址 0、速率 0"这个不存在的槽位，把 60 号音符白白占掉。现在要求两个寄存器都写过才记。

**能力枚举**新增 `discrete`：有音符，但只有固定一组，没有音高滑动。徽章是键盘 + 采样脉冲的组合图形。

#### 2.6 覆盖

`[3d]` 对五份资源断言：快照存在、芯片列表两枚、每条扩展轨都有对应行、
行指向真实轨道、形状可绘制、发声行带音高与电平。五份资源全部注册进 ctest。

```
ctest 42/42（新增 engine.fds_song / vrc6_song / vrc7_song / mmc5_song / fme7_song）
```

- [x] captureState 改成表驱动
- [x] VRC6 三轨 / VRC7 六轨 / FDS 一轨（带 wave[64]）/ MMC5 三轨 / FME7 三轨
- [x] 轨道名与 VGM 侧一致（Square n / Saw Wave / FM n）
- [ ] 键盘视图逐轨独奏的人工确认（需在 UI 里看一遍）

### 阶段 3：MIDI（P1，方案 B）

**预扫已经落地**（见 2.5）：`NsfPlayer::scanSampleChannels` 就是这条路线要的那一遍 CPU，
实测 180 秒约 0.7 秒，缓存在内容哈希上。剩下的是把同一次扫描的音符抽出来。

- [x] 预演耗时实测：180 秒音频 737–826 ms（含音频渲染），纯 CPU 更快
- [ ] 扫描时同时产出 MidiSequence（现在只建 DPCM 槽位表）
- [ ] 测试：三轨各出一个音符，音高与通道频率一致

## 5. 验收标准

- 任意 NSF 在混音器里的轨道数 = 芯片实际声部数（2A03 五条 + 扩展芯片各声部）；
- 每条轨能独奏、能被 MIDI 输出（若 D1 选了 A/B）；
- 键盘视图对每颗芯片都能画出对应的行，波形/占空比与该芯片一致；
- 上述每一条都有端到端断言，而不只是截图。
