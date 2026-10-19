# 调研：MIDI 输出与可视化

- **状态**：调研完成，未实现
- **日期**：2026-10-19
- **分支**：`feature/midi-output`
- **参考项目**：`reference/imgui-vgmplayer-v0.44`（MIT，已加入 `.gitignore`）
- **一句话**：VGM/VGZ 的寄存器流足以还原“音符事件”，但 MIDI 只能表达音高、力度和控制器，不能表达 FM 音色；YM2610 的 FM、SSG、ADPCM-A、ADPCM-B 必须分开建模，尤其 ADPCM-A 不是“固定音高”，而是固定采样率的一次性采样。
- **实施决定（2026-10-19）**：不做 GM 鼓映射。旋律通道输出原始音高；one-shot 通道没有可靠音高，统一用 C4（60）作为 trigger，用户可在 DAW 中自行加 MIDI 效果器映射。

---

## 1. 结论

**可行，但要把“MIDI 输出”拆成三层，不能把它当成“把 VGM 还原成 GM 音乐”。**

1. **事件提取层**：解析 VGM/VGZ 的寄存器写，维护每种芯片的影子寄存器，产出格式无关的 `MidiEvent` 序列（note on/off、pitch bend、CC、program）。
2. **消费层**：
   - **可视化**：钢琴键盘/钢琴卷帘、FM 算法图、包络、通道活动；
   - **MIDI 文件导出**：离线生成 SMF，最容易做到跨格式一致；
   - **实时 MIDI 输出**：standalone 先做，插件端受宿主和 JUCE 的 MIDI 输出能力限制，单独评估。
3. **音色层**：MIDI 只承载演奏信息。外部 GM 合成器不会发出 YM2610 的 FM 音色；若要“听起来一样”，仍然要渲染音频，MIDI 是控制/可视化数据，不是音频替代品。

推荐的第一版范围：

- 只解析 **VGM/VGZ**；
- 先覆盖 **YM2610/YM2610B、YM2612、YM2151、YM2203/YM2608、AY8910、SN76489**；
- 先做 **事件提取 + 可视化 + MIDI 文件**；
- 实时 MIDI 输出放到 standalone 的下一阶段；
- 不碰 S98/DRO/GYM，不为未知芯片做音频猜音高。

---

## 2. VGM/VGZ 与 libvgm 的事件模型

### 2.1 VGM 命令流

VGM 的数据区是一串命令，核心只有几类：

| 命令 | 含义 |
|---|---|
| `0x50` | SN76489 寄存器写 |
| `0x51` | YM2413 寄存器写 |
| `0x52/0x53` | YM2612 端口 0/1 寄存器写 |
| `0x54` | YM2151 寄存器写 |
| `0x55` | YM2203 寄存器写 |
| `0x56/0x57` | YM2608 端口 0/1 寄存器写 |
| `0x58/0x59` | YM2610 端口 0/1 寄存器写 |
| `0x5A` | YM3812 寄存器写 |
| `0x5B/0x5C` | YM3526/Y8950 寄存器写 |
| `0x5D` | YMZ280B 寄存器写 |
| `0x61 nn nn` | 等待 n 个 44.1 kHz 采样 |
| `0x62` / `0x63` | 等待 735 / 882 个采样 |
| `0x70-0x7F` | 等待 n+1 个采样 |
| `0x67` | 数据块（PCM/ROM），长度在命令里 |
| `0x90-0x95` | DAC Stream Control |
| `0x66` | 数据结束 |

这些命令在 libvgm 里对应 `vgmplayer_cmdhandler.cpp` 的命令表；`0x58/0x59` 是 YM2610 的两个端口，见 `libs/libvgm/player/vgmplayer_cmdhandler.cpp:103-113`。VGM 的时间单位是 44.1 kHz 采样，因此事件时间可以直接换算成秒；不需要依赖音频线程。

### 2.2 libvgm 已经提供了什么

- `LibvgmSongInstance` 持有原始文件镜像 `SongMemory`，`.vgz` 在 `unpackImage()` 里解压成 `playerMemory`，见 `plugin/Source/backends/libvgm/LibvgmSongInstance.h:29`、`.cpp:44-60`；
- `ISongInstance` 已经能报告设备、通道、轨道和时长，见 `plugin/Source/playback/SongBackend.h:153-176`；
- 但 `PlayerBase` 的事件回调只有 START/STOP/LOOP/END，没有“寄存器写”事件，见 `libs/libvgm/player/playerbase.hpp:23-30`；
- `VGMPlayer` 内部有 `CHIP_DEVICE::write8` 指针，寄存器写在命令处理器里完成，但不对外暴露，见 `libs/libvgm/player/vgmplayer.hpp:61-79`。

因此有三条路：

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 解析 VGM 命令流** | 在 `LibvgmSongInstance` 里读 `playerMemory`，自己维护影子寄存器 | 不改 libvgm；事件顺序和文件一致；可离线生成 | 只覆盖 VGM/VGZ；要自己处理数据块、循环、DAC Stream |
| **B. 给 libvgm 加写回调** | 在 `Cmd_*Reg*` 或 `CHIP_DEVICE::write8` 外套一个回调 | 对 S98/DRO/GYM 也有效；不重复解析 | 要改 vendored libvgm；每个核心的写路径不同；线程/ABI 风险 |
| **C. 音频分析** | 对每轨做基频检测 | 芯片无关 | FM 是多 operator 复音，ADPCM 是采样，检测不可靠；不建议 |

**建议 v1 选 A**：改动集中在我们的后端，不碰 libvgm；先把 VGM/VGZ 做对，再考虑 B 覆盖其他格式。

### 2.3 必须处理的 VGM 细节

- `0x67` 数据块的 `type` 和 `size`：YM2610 的 ADPCM-A/ADPCM-B ROM 就在这里；不知道 ROM 时只能触发，无法知道采样根音；
- `0x90-0x95` DAC Stream：可能改变采样播放速率/位置，不能只看普通寄存器；
- 文件头 `loopOfs`：循环时事件要重新走一遍；实时输出要在循环点重置活动音符；
- `0x66` 结束、`0x67` 之后的 `dataEnd`、GD3 标签区：解析器要按 libvgm 的边界处理；
- 多个同型号芯片：VGM 用第二组命令（`0xA1-0xAF`、`0xB?` 等）或实例号区分，事件模型要能标出 `(deviceType, instance)`。

---

## 3. YM2610 / YM2610B

### 3.1 组成

| 部分 | 数量 | 说明 |
|---|---:|---|
| FM | 4（YM2610）/ 6（YM2610B） | 4 operator/通道，8 种 algorithm，带 LFO |
| SSG | 3 | AY8910/YM2149 兼容，3 个方波 + 噪声 + 包络 |
| ADPCM-A | 6 | 固定采样率 18.5 kHz，每通道独立 start/end/音量 |
| ADPCM-B | 1 | Delta-T，可变播放速率 2–55.5 kHz，16-bit 输出 |
| DAC | 1 | YM3016 立体声 DAC，只是输出级，不是独立通道 |

libvgm 暴露的 YM2610 通道数是 **13**：6 FM + 6 ADPCM-A + 1 ADPCM-B；SSG 是 linked AY8910，单独 3 通道。见 `libs/libvgm/emu/cores/opnintf.c:235-251`。

### 3.2 FM 部分

- 4 operator，8 种 algorithm；和 YM2612 的 FM 结构同源，因此参考项目的 `DrawFmAlgorithm()` 的 8 种布局可以直接映射到 YM2610 的 FM 通道。
- **通道号不是 0..3**：libvgm 的 YM2610 非 B 版本实际输出 `CH[1], CH[2], CH[4], CH[5]`，见 `libs/libvgm/emu/cores/fmopn.c:3621-3624`；YM2610B 才输出 `CH[0..5]`，见 `:3798-3803`。如果按“FM1..FM4”顺序硬编码寄存器通道，会错位。
- YM2610B 的判定：libvgm 用 VGM 头部时钟值的 bit 31 作为 `devCfg.flags`，`DeviceName_YM2610()` 用它区分 B 版本，见 `libs/libvgm/player/vgmplayer.cpp:1269-1270`、`libs/libvgm/emu/cores/opnintf.c:228-233`。
- 关键寄存器（端口 0/1，地址 `0x30-0x9F`）：
  - `0x28`：key on/off（bit 4-7 对应 4 个 operator，低 2 位选通道，bit 2 选高/低三通道组）；
  - `0xA0-0xA6`：FNUM 低 8 位；
  - `0xA4-0xA6`：BLOCK + FNUM 高 3 位；
  - `0xB0-0xB6`：algorithm/feedback；
  - `0xB4-0xB6`：pan/AMS/PMS；
  - `0x30-0x8F`：operator 的 DT/MUL/TL/KS/AR/DR/SR/RR/SL 等。
- FM 的 MIDI 映射：
  - key on/off -> note on/off；
  - FNUM/BLOCK -> 频率 -> 最近 MIDI note + pitch bend（保留分数音高，可视化也用它）；
  - carrier TL / 通道音量 -> velocity（近似）；
  - `0xB4` 的 L/R -> CC10 pan；
  - algorithm/feedback/operator 参数不进入 GM，但可以进入可视化状态。

**注意**：参考项目用 `clock / 72.0 * fnum / 2^(20-block)` 计算 YM2612 频率（`main.cpp:1695-1698`）。这个公式不能直接照搬到 YM2610：OPN 系列的 prescaler 不同，libvgm 的 YM2610 用 `OPNPrescaler_w(..., 2)`，见 `fmopn.c:4014`、`:4038`。实现时应以 libvgm 的 `fn_table`/实际核心公式为准，并用已知音高的 VGM 做校准；不要在文档里把 `/72` 当成通用公式。

### 3.3 SSG 部分

- 3 个方波通道，AY8910 兼容；寄存器 `0x00-0x0D`：
  - `0x00/0x01`、`0x02/0x03`、`0x04/0x05`：三个通道的 tone period 低/高；
  - `0x06`：noise period；
  - `0x07`：mixer（tone/noise 开关）；
  - `0x08-0x0A`：三个通道的 amplitude；
  - `0x0B/0x0C`：envelope period；
  - `0x0D`：envelope shape。
- MIDI 映射：
  - tone 通道：period -> 频率 -> note + pitch bend；amplitude -> velocity/CC7；
  - noise：统一发 C4 trigger，不做 GM 鼓映射；
  - envelope：可用 CC11/CC7 近似，不能完整表达 AY 的硬件包络。
- SSG 在 libvgm 里是 linked device，混音器默认不把它当独立 track；MIDI 提取可以直接解析它的寄存器，不需要依赖 `PLR_DEV_INFO` 的 track 列表。

### 3.4 ADPCM-A：固定的是采样率，不是音高

这是最容易误解的一点。

- ADPCM-A 的 6 个通道共享固定的 step/采样率：libvgm 在 reset 时把 `adpcm[i].step` 设为 `(1<<16) * freqbase / 3`，见 `fmopn.c:4077`。这对应 18.5 kHz 的名义采样率。
- 但每个通道播放的是 **不同的 ROM 采样**，采样本身可能是鼓、效果、语音，也可能是乐音。固定 step 只表示“以固定速率播放”，**不表示六个通道都发出同一个固定音高**。
- key on 在端口 1 的 `0x00`：bit 0-5 对应 6 个通道；bit 7 置位表示 key off，见 `fmopn.c:2843-2888`。start/end 地址在 `0x10-0x2F`，音量/pan 在 `0x08-0x0D` 附近。
- 因此：
  - **不要**用“信号有无”直接生成一个有音高的 MIDI note；那只能得到 trigger；
  - 正确做法是 **one-shot note on**：key on 时统一发 C4（60）作为 trigger，key off 或采样结束时 note off；不做 GM 鼓映射；
  - 如果某个采样确实是乐音，必须额外提供 **root key/采样率元数据**，或离线做基频检测；不能从“有信号”推出音高。
- 对可视化的意义：ADPCM-A 适合显示“触发/包络/波形”，不适合钢琴卷帘上的音高线；可以画成 drum lane。

### 3.5 ADPCM-B：可变播放速率，但也不是合成器

- ADPCM-B 是 Delta-T：控制寄存器 `0x10/0x11`，start `0x12/0x13`，end `0x14/0x15`，delta-N `0x19/0x1A`，volume `0x1B`，见 `fmopn.c:4138-4153`。
- 它的音高由 **采样根音 × 播放速率** 决定；delta-N 改变的是播放速率，不是一个“音符频率寄存器”。
- 如果采样是乐音且知道根音/根采样率，可以映射成 note + pitch bend；否则只能做 one-shot（可变长度的采样触发），并在可视化中显示播放速率/包络，而不是假装成钢琴音符。
- key on/off、循环/结束状态可以从 Delta-T 控制寄存器得到；note off 要处理“采样自然结束”和“stop”两个来源。

### 3.6 YM3016

YM3016 只是立体声 DAC，负责把 YM2610 的混合结果转成模拟输出；它没有独立通道，也不产生 note 事件。MIDI 提取层应忽略它。

---

## 4. libvgm 其他芯片的适配分级

libvgm 的设备 ID 在 `libs/libvgm/emu/SoundDevs.h:4-53`，共 40 多个；不需要一次全做。按“能否从寄存器稳定得到 note”分三档：

### A. 可直接得到音高（优先）

| 芯片 | 通道 | 说明 |
|---|---:|---|
| YM2612 / YM3438 | 6 FM + DAC | FM 与 YM2610 同源；DAC 不是音符 |
| YM2610 / YM2610B | 4/6 FM + 3 SSG + 6 ADPCM-A + 1 ADPCM-B | 本调研重点 |
| YM2608 | 6 FM + 3 SSG + 6 ADPCM-A + 1 ADPCM-B | 与 YM2610 类似，ADPCM-A 的 4/5 通道速率不同 |
| YM2203 | 3 FM + 3 SSG | SSG 为 linked AY8910 |
| YM2151 | 8 FM | OPM，4 operator；KC/KF 直接给音高 |
| YM2413 / VRC7 | 9 FM + 5 rhythm（VRC7 6 FM） | OPLL，参数较少；rhythm 是预设鼓 |
| YM3526 / YM3812 / Y8950 | 9 FM + 5 rhythm | OPL2；Y8950 多 ADPCM |
| YMF262 | 18 FM + 5 rhythm | OPL3；4-op 模式要单独处理 |
| AY8910 / YM2149 | 3 tone + noise | 方波，note 稳定 |
| SN76489 | 3 tone + noise | 方波，note 稳定 |
| NES APU | 2 pulse + triangle + noise + DMC | DMC 是采样，其余可 note |
| GB DMG | 2 pulse + wave + noise | wave 通道可读波形频率 |
| C6280 | 6 | 可编程波形，note 稳定 |
| K051649 | 5 | SCC，方波/波形 |
| WSwan | 4 | 方波/波形/噪声 |
| VSU | 6 | Virtual Boy，波形 |
| SAA1099 | 6 | 方波/噪声 |
| POKEY | 4 | 方波/噪声 |
| MSM5232 | 11 输出 | 多组方波/包络，note 可算 |

### B. 只能可靠触发，音高需要额外信息（第二批）

| 芯片 | 通道 | 说明 |
|---|---:|---|
| YM2610 ADPCM-A | 6 | 固定采样率；采样内容决定音高/是否乐音 |
| YM2608 ADPCM-A | 6 | 同上；4/5 通道速率不同 |
| YM2612 DAC | 1 | PCM 流，不是 note |
| SegaPCM | 16 | 采样播放；有 pitch 寄存器，但根音未知 |
| RF5C68 / RF5C164 | 8 | 采样；有 pitch |
| OKIM6258 | 1 | ADPCM，固定/可选速率 |
| OKIM6295 | 4 | ADPCM，采样触发 |
| MSM5205 | 1 | ADPCM，语音/效果 |
| YMZ280B | 8 | PCM，有 pitch |
| YM2608/2610 ADPCM-B | 1 | 可变速率，根音未知 |
| YMF278B PCM | 24 | OPL4 wave part，采样 |
| YMF271 | 12 | FM + PCM 混合，需逐通道分析 |
| BSMT2000 | 12 PCM + 1 ADPCM | 采样 |

### C. 复杂采样/多 voice，暂不做（第三批）

| 芯片 | 通道 | 原因 |
|---|---:|---|
| C140 | 24 | 多采样、bank、循环；根音/映射复杂 |
| C219 | 16 | 同上 |
| K053260 | 4 | 采样，pitch 有但根音未知 |
| K054539 | 8 | 采样 + 包络/循环 |
| QSound | 16 PCM + 3 ADPCM | 采样/ADPCM 混合 |
| SCSP | 32 | 多采样、LFO、滤波；Saturn |
| ES5503 / ES5506 | 32 / 动态 | 多采样；ES5506 的 libvgm 通道数返回 0（不暴露细分） |
| X1-010 | 16 | 采样 |
| C352 | 32 | 采样 |
| ICS2115 | 32 | 采样 |
| 32X PWM | 1（libvgm 暴露） | PWM，不是 note |
| MultiPCM / uPD7759 / GA20 / Mikey / K007232 / K005289 | 各 1-28 | 语音/采样/特殊；后续单独评估 |

**结论**：第一版只做 A 档，B 档用“one-shot/鼓轨”处理，C 档不承诺。这样 MIDI 事件的可信度可控，也不会把采样误标成音符。

---

## 5. MIDI 映射设计

### 5.1 格式无关的事件

建议在 `plugin/Source/playback/` 增加一个不依赖 libvgm 的类型（具体名字可在实现时定）：

```cpp
struct MidiEvent
{
    double timeSeconds;
    std::uint8_t status;   // 0x90 note on, 0x80 note off, 0xE0 pitch bend, 0xB0 CC
    std::uint8_t data1;
    std::uint8_t data2;
    std::uint32_t sourceTrack; // 用于可视化和调试
};

struct MidiSequence
{
    std::vector<MidiEvent> events;
    // 每轨的名字、芯片、通道、颜色等
};
```

`ISongInstance` 可以增加一个可选能力：

```cpp
virtual const MidiSequence* getMidiSequence() const { return nullptr; }
```

这样：
- 不破坏现有后端（默认 nullptr）；
- 将来 SPC 后端也可以提供自己的事件；
- UI 和 MIDI 导出只依赖这个类型，不依赖 libvgm；
- 事件在加载时构建，音频线程不分配。

### 5.2 音符与控制器

| VGM 来源 | MIDI |
|---|---|
| FM key on/off | note on/off；velocity 由载波 TL/音量近似 |
| FM FNUM/BLOCK | 最近 note + pitch bend；保留分数音高供可视化 |
| FM pan | CC10 |
| PSG tone period | note + pitch bend |
| PSG amplitude | velocity / CC7 |
| PSG noise | 固定 C4 trigger，不做鼓映射 |
| ADPCM-A key on | 固定 C4 one-shot trigger |
| ADPCM-B key on | note on；有根音时加 pitch bend，否则 one-shot |
| ADPCM volume/pan | velocity / CC10 |
| 芯片静音/轨道静音 | 不发事件，或在切换时 all-notes-off |

### 5.3 通道预算

- MIDI 1.0 每个端口 16 通道；
- YM2610 非 B：4 FM + 3 SSG + 6 ADPCM-A + 1 ADPCM-B = **14**，刚好一个端口；
- YM2610B：6 + 3 + 6 + 1 = **16**，正好用满，没有独立鼓通道；
- 一首歌有多个芯片时，必须多端口或动态分配/降级；
- 可视化不需要遵守 16 通道限制，可以按芯片通道画更多轨。

### 5.4 时间与循环

- VGM 事件时间是 44.1 kHz 采样，转换成秒后可直接用于可视化；
- MIDI 文件建议 PPQ 960、固定 tempo（例如 120 BPM），按秒换算 tick；
- 实时输出按播放位置调度；seek/stop/load 时先发 **all notes off + reset controllers**，避免挂音；
- 循环点要重置每个通道的 `lastNote`、`lastPitch`，否则循环后会把旧 note 当成仍按着。

---

## 6. 在本项目里的落点

### 6.1 事件提取放在后端

`LibvgmSongInstance` 已经持有 `playerMemory`（`.vgz` 已解压），最适合在 `prepare()` 后构建 `MidiSequence`：

```
LibvgmSongInstance::prepare()
  -> probe->load(playerMemory)
  -> MidiExtractor::build(playerMemory)   // 新
  -> enumerate() / refreshTimelineFacts()
```

这样：
- 不碰 `PlaybackEngine` 的实时路径；
- 不增加音频线程负担；
- 可视化和导出都能直接读序列；
- 以后再接 S98/DRO/GYM，可以替换/扩展 extractor。

### 6.2 可视化

`VisualiserView` 目前是占位页，注释里已经列出 waveform/spectrum/MIDI/chip 四个面板，见 `plugin/Source/ui/VisualiserView.h:10-20`。MIDI 事件可以直接支撑：

- **钢琴键盘**：当前时间附近的活动 note；黑键/白键几何和分数 pitch 高亮；
- **钢琴卷帘**：按时间轴画 note 矩形，颜色按芯片/通道；
- **FM 算法图**：从影子寄存器取 algorithm、feedback、operator 参数；algorithm 变化时更新；
- **包络/示波器**：包络可以用 AR/DR/SR/SL/RR 近似；示波器可复用现有 per-track isolation，或后续单独做。

### 6.3 实时 MIDI 输出

- **Standalone**：`juce::MidiOutput::openDevice()`，用独立线程/定时器按 `MidiSequence` 和播放位置发送；不要在音频线程里做设备查找/分配。
- **插件**：JUCE 的跨格式插件接口没有稳定的“MIDI 输出总线”；VST3 本身有 output events，但 AU/VST3/JUCE wrapper 的支持不一致。不要把它当作第一版能力。若确实需要，可以：
  - 由插件的进程打开一个 OS 虚拟 MIDI 端口（独立于宿主），但要处理多实例、时钟同步和路由；
  - 或只做 MIDI 文件导出/可视化，让用户自己把 MIDI 拖到 DAW。
- **MIDI 文件导出**：不依赖宿主，跨平台最稳，建议作为第一个“可交付”的 MIDI 功能。

---

## 7. 参考项目分析：imgui-vgmplayer

参考项目是 MIT，代码可以作为思路参考；不要直接复制它的三份 libvgm 架构。

### 7.1 钢琴键盘

`DrawPiano()` 在 `reference/imgui-vgmplayer-v0.44/examples/example_vgm_player/main.cpp:258-397`：

- 只用 `ImDrawList` 画白键/黑键，不依赖图片；
- 先算白键数量与宽度，再画黑键（高度约 0.67）；
- `active_note` + `pitch_offset` 支持分数音高：在相邻键之间插值高亮；
- `envelope` 控制高亮透明度；
- 音域由 `vgm_min_note` / `vgm_max_note` 动态决定，见 `AnalyzeVgmNoteRange()`（`main.cpp:217-256`）。

可借鉴的点：键盘几何、黑键比例、分数音高高亮、按音域裁剪。JUCE 里用 `Graphics::fillRect`/`Path` 重写即可。

### 7.2 FM 算法图

`DrawFmAlgorithm()` 在 `main.cpp:483-637`：

- 4 个 operator，8 种 algorithm（0-7）；
- 每种算法硬编码 operator 位置、连接线、carrier；
- 画彩色方框、编号、连线、carrier 输出箭头；
- 自动计算 bounding box 并居中。

YM2610 的 FM 也是 4 operator/8 algorithm，所以这张图可以复用；但要注意 YM2610 的通道映射（非 B 只显示 1/2/4/5）和 OPL3 的 4-op 模式不同。

### 7.3 ADSR 与通道状态

`DrawAdsrGraph()` 在 `main.cpp:414-481`：把 AR/DR/SR/SL/RR 映射成折线，是近似图，不是精确包络。适合做“参数可视化”，不要用它反推音频。

YM2612 的通道 UI（`main.cpp:1660-1787`）演示了：

- 从寄存器算 FNUM/BLOCK/频率/note；
- 用 `DrawFmAlgorithm` + `DrawAdsrGraph` + 每通道示波器；
- 但它的 `key_on` 判断用的是 TL < 127（`main.cpp:1684-1693`），不是真正的 key-on 位；这是参考实现的一个弱点，我们不能照搬。

YM2151 的通道 UI（`main.cpp:1889-2148`）演示了：

- 用 `0x08` 的 key-on 位判断；
- 用 KC/KF 算分数 note；
- 用 `DrawPiano` 显示当前 note 和 pitch offset。

YM2610 在参考项目里只有原始寄存器表（`main.cpp:2208-2240`），没有钢琴/FM 可视化；这正好是我们需要补的部分。

### 7.4 参考项目的架构教训

参考项目 README 明确说它用了三份 libvgm：播放、影子寄存器、可视化修改版。这个结构适合快速原型，但不适合我们：

- 三份 libvgm 会带来版本漂移、重复编译、状态不同步；
- 我们的后端已经有一份 libvgm 和 `SongMemory`，应在同一份数据上做事件提取；
- 参考项目的影子寄存器思路可以借鉴，但实现要放在我们自己的后端，不修改 vendored libvgm。

---

## 8. 风险与未决问题

1. **音色不可还原**：MIDI 没有 FM operator/algorithm 的标准表达；只能做演奏信息。若用户期望“MIDI 听起来就是 YM2610”，需要明确不支持。
2. **ADPCM-A 音高**：固定采样率不等于固定音高；没有 root key 时只能做 one-shot/鼓轨。这是本调研最重要的纠正。
3. **ADPCM-B 根音**：delta-N 只给播放速率，根音要从采样元数据/分析得到。
4. **通道/端口**：YM2610B 正好 16 通道；多芯片歌曲会超；需要多端口或降级策略。
5. **插件 MIDI 输出**：JUCE 插件没有统一 MIDI out 总线；第一版只做 standalone/文件导出。
6. **seek/loop/stop 挂音**：必须 all-notes-off；循环点重置影子状态。
7. **VGM 解析边界**：数据块、DAC Stream、第二芯片命令、GD3 区、损坏文件；要复用 libvgm 的边界规则。
8. **性能**：长曲目事件多；按时间排序 + 二分查找；不要每帧重建。
9. **许可**：参考项目 MIT，libvgm 按文件 BSD/GPL/LGPL/MIT；我们项目 AGPL。借鉴思路可以，直接复制代码要保留版权/许可并确认兼容。
10. **MIDI 输出抖动**：实时输出需要高优先级调度；MIDI 文件没有这个问题。

---

## 9. 建议的实施顺序

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 本调研 | 明确范围、风险、不做的事 |
| P1 | VGM/VGZ 事件提取；YM2610/YM2612/YM2151/AY/SN | 合成寄存器序列的单元测试；note on/off/pitch 正确 |
| P2 | `MidiSequence` 接入 `ISongInstance`；钢琴键盘 + FM 算法图 | 可视化不依赖音频线程；seek/loop 不挂音 |
| P3 | MIDI 文件导出（SMF type 1） | 用 DAW 打开，轨/音符/时间正确 |
| P4 | standalone 实时 MIDI 输出 | 可选设备、停止/seek all-notes-off |
| P5 | 其他芯片、S98/DRO/GYM | 按 A/B/C 档逐步扩展 |

第一版不要做：C 档采样芯片、插件实时 MIDI、FM 音色 SysEx、自动基频检测。

---

## 10. 决策记录

| 日期 | 结论 |
|---|---|
| 2026-10-19 | 采用“VGM 事件解析 + 影子寄存器”作为第一版；不改 vendored libvgm |
| 2026-10-19 | 第一版只覆盖 VGM/VGZ 和 A 档芯片；ADPCM-A 做 one-shot C4 trigger，不做鼓映射，不伪造音高 |
| 2026-10-19 | 先做可视化 + MIDI 文件；standalone 实时输出下一阶段；插件 MIDI out 暂不承诺 |

## 11. 参考位置

- libvgm 设备 ID：`libs/libvgm/emu/SoundDevs.h:4-53`
- YM2610 设备/通道：`libs/libvgm/emu/cores/opnintf.c:228-265`
- YM2610 写寄存器：`libs/libvgm/emu/cores/fmopn.c:4104-4205`
- YM2610 FM key on：`libs/libvgm/emu/cores/fmopn.c:1758-1767`
- YM2610 ADPCM-A：`libs/libvgm/emu/cores/fmopn.c:2834-2951`、`:4077`
- YM2610 非 B 的 FM 通道：`libs/libvgm/emu/cores/fmopn.c:3621-3624`；B：`:3798-3803`
- VGM 命令表：`libs/libvgm/player/vgmplayer_cmdhandler.cpp:103-113`
- 原始镜像/解压：`plugin/Source/backends/libvgm/LibvgmSongInstance.h:29`、`.cpp:44-60`
- 后端接口：`plugin/Source/playback/SongBackend.h:153-176`
- 可视化占位：`plugin/Source/ui/VisualiserView.h:10-20`
- 参考钢琴：`reference/imgui-vgmplayer-v0.44/examples/example_vgm_player/main.cpp:258-397`
- 参考 FM 算法：同文件 `:483-637`
- 参考 ADSR：同文件 `:414-481`
- 参考 YM2612 频率/通道：同文件 `:1695-1787`
- 参考 YM2151 音符：同文件 `:1909-1914`
- 参考 YM2610 原始寄存器：同文件 `:2208-2240`
- 参考 VGM 解析：`reference/.../vgm_parser.cpp:326-412`
