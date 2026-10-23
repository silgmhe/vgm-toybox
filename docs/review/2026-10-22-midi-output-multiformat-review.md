# MIDI 输出：可维护性与多格式扩展评审

| 项目 | 内容 |
|---|---|
| 评审日期 | 2026-10-22；重构记录更新至 2026-10-23 |
| 评审对象 | `feature/midi-output`，评审时 `5d538c1`，现在 `03a3817` |
| 范围 | `plugin/Source/chips/`（31 个解码器）、`backends/libvgm/`（装配层）、`formats/`、`midi/`、`playback/`；不含 UI 细节与 libvgm 本身 |
| 方法 | 静态阅读接口与实现、统计规模与重复度、以 95 首真实 rips 建立行为基线并逐行比对 |
| 结论 | 评审时「芯片层已解耦、装配层仍绑死 VGM」。**R0–R2 之后装配层也已解耦**：新增一种格式只剩一个 loader 文件 |
| 状态 | 已完成：R0/R1/R2、H-1~H-3、M-1~M-3、L-1；**仅剩待办：H-4**（§4） |

## 1. 实现了什么

| 维度 | 评审时 | 现在 |
|---|---|---|
| `MidiSequenceBuilder.cpp` | 1,098 行，`parse` **462 行**内含 57 个 VGM `case` | 1,018 行，`parse` **215 行**只做装配 |
| VGM 命令知识 | 散在装配层的 57 个 `case` | 集中在 `formats/vgm/VgmSongLoader.cpp`（约 330 行） |
| 设备路由 | 31 个 `ChipBuffer<T>` 成员 | 1 个 `map<type_index, …>` 注册表 |
| 新增芯片的成本 | 解码器 + case + `flush` + `clear`（四处） | 解码器 + `ChipKind` 值 + `dispatch` 一个 case |
| 新增格式的成本 | 再写一个 460 行函数 | **写一个 loader**，装配层不动 |
| 芯片启动接口 | `reset(clock, sampleRate)`，注释提 VGM | `reset(const ChipConfig&)`，无格式概念 |
| 宽寄存器 | `value` + `value2`（C352 专用字段） | `uint16_t value`，打包在读取器完成 |
| 行为验证 | `ctest` 34/34 | `ctest` 34/34 **加** 95 首 rips 基线逐行一致 |

代码总量略增（新增 loader 与 IR 头文件），换来的是**职责边界**：命令、装配、解码三层各自只做一件事。

## 2. 问题与状态

| ID | 级别 | 问题 | 状态 |
|---|---|---|---|
| C-1 | 阻断 | `Builder::parse` 把命令解析与 MIDI 装配放在同一函数（462 行 / 57 个 case） | **已解决（R1）** |
| C-2 | 阻断 | 设备路由靠 31 个成员 + `deviceFor(DEVID_*)`，无法跨格式复用 | **已解决（R2）** |
| C-3 | 阻断 | `IChipDecoder::reset` 的契约把 VGM 写进芯片层 | **已解决（R0a）** |
| H-1 | 高 | `RegisterWrite.value2` 为单颗芯片而加 | **已解决（R0b）** |
| H-2 | 高 | `kVgmRate = 44100.0` 是全局常量，时间栅格无处传递 | **已解决** `bf30b28` |
| H-3 | 高 | 30 个 `NoteTable` 各写一遍 `noteFor` | **已解决**：采样族收为一个守则；FM/噪声/one-shot 保留（各有公式） |
| H-4 | 高 | `LibvgmSongInstance` 同时做解包、libvgm 生命周期、轨枚举、能力表、序列构建 | 待办 |
| M-1 | 中 | `ChannelState` 同时带查表派与速率派两组字段，新芯片作者需自行判断 | **已解决（诊断版）** `b8335bb` |
| M-2 | 中 | `Family` 混装芯片族与声道种类 | **已解决** `9297260`（改名 `ChannelKind`；未新增 `ChipFamily`，因为无消费者） |
| M-3 | 中 | 跨格式的时间基契约未文档化 | **已解决** `cb7113d`（`SongIr.h` 三步契约） |
| L-1 | 低 | `deviceId == 0` 兼作「未指定」哨兵 | **已解决** `cb7113d` |

## 3. 三处已解决项的要点

**R0（C-3、H-1）** —— 只换接口，不换行为。31 个解码器的函数体一行未改，只在 `reset` 开头把旧名字从 `ChipConfig` 映射过来；宽寄存器的字节序责任移到读取器，`RegisterWrite` 回到「一个寄存器值」。

**R1（C-1）** —— 新增 `midi/SongIr.h` 与 `formats/vgm/VgmSongLoader`。关键决定有两个：

1. `ChipKind` 是**设备级**的（`ym2413` 与 `ymf262` 不同值），不是解码器族。族名会让装配层不得不记住“这条写入来自 0x51 还是 0x5E”；
2. `ChipSlot::clockHz` 由运行时回填，loader 不读头部时钟——设备是运行时启动的，时钟归它。

**R2（C-2）** —— 注册表的键是**解码器类型**（`std::type_index(typeid(D))`），不是 `ChipKind`。六个 OPL 设备共用一个 `OplDecoder`、四个 OPN 设备共用一个 `OpnDecoder`，按 kind 建表会把一颗芯片拆成两个解码器：音符变少、不报错。两条原则因此明确：**IR 描述设备，注册表描述代码复用**。

## 4. 待办事项与做法

### H-2 时间栅格从常量变成参数

- **目标**：`kVgmRate` 从装配层的全局常量变成 `SongIr::tickRate`（字段已存在，目前未被使用）。
- **改动面**：`Builder` 用 `ir.tickRate` 换算展示时间；`--pitch-report`、`loop at A of B` 的换算集中到一个函数。
- **验收**：95 首基线不变；`--pitch-report` 输出逐字不变。
- **风险**：低。这是纯传递，没有语义变化。

### M-1 `ChannelState` 的两套音高字段

- **目标**：让新解码器作者**不可能**填错。两条路线：`pitchSource` 上写明哪组字段有效，或拆成 `PeriodPitch { period, block, finePitch }` 与 `RatePitch { hz, transposeCents, rootCents }`。
- **建议**：先做文档与断言（`jassert`），观察一轮再决定是否拆类型——拆分会影响 31 个解码器与 extractor。
- **验收**：`ctest` 全绿。
- **风险**：低（只加诊断）。

### H-3 合并可复用的 `NoteTable`（已完成：采样族收敛，未引入 RateNoteTable）

- **分类结果（2026-10-23 实测）**：30 个 `noteFor` 里只有三种形态，但**没有 30 份重复**——每个公式都是硬件的：

  | 族 | 数量 | 代表 | 可合并程度 |
  |---|---|---|---|
  | 纯速率派 `quantisePlaybackRate(rate, ref)` | 4 | C140、C352、OPN ADPCM-B、GA20 直读档 | 可抽 `RateNoteTable`，约 40 行 |
  | 采样测量派 `quantiseFrequency(rate / cycleLength)` | 7 | QSound、K053260、K054539、OKIM6295、ES5503、RF5C68、SegaPCM | 可抽一个自由函数，约 60 行 |
  | 频率公式 / 查表 / 噪声 / one-shot | 19 | AY、SN、NES、SCC、OPM、OPN FM、OPL、MSM5232 | **保留**，公式各不相同 |

- **修正**：评审时估的「约 1,000 行重复」不成立。真实可消除量约 **100 行**，收益在**一致性**（同一段 `melodic`/`cycleLength` 判定只写一遍）而不是行数。
- **做了什么**：`Pitch.h` 新增 `noteFromSampleCycle` 与 `noteFromPlaybackRate`，7 颗采样芯片改为调用它们；`Pitch.h` 顶部写下「采样解码器只有三种形态」的规则，避免后来者发明第四种。
- **没做什么，以及为什么**：`RateNoteTable` 只服务于 C140/C352/OPN-ADPCM-B 三颗，而它们的 `noteFor` 本来就是一行 `quantisePlaybackRate (…, 参考率, …)`。再包一层类要引入 `std::function` 或模板，**读起来更绕、没有语义收益**；三个公式各自的参考率与乘数才是重点，保留在原处更清楚。
- **验证**：黄金基线 117 首逐行一致；`ctest` 34/34，其中 7 条断言专门钉住「鼓 / 静音 / 未测」三种结局。
- **做法**：先出一张「可合并 / 保留」清单，再逐颗替换，**每颗单独提交**并跑 `ctest`。
- **验收**：每颗的既有单测断言不变；95 首基线不变。
- **风险**：中（30 个文件），但每次只动一颗，可回滚。

### H-4 `LibvgmSongInstance` 拆分

- **目标**：容器解包 / libvgm 生命周期 / 轨枚举 / 能力表 / 序列构建分开。
- **何时做**：new format 接入时收益最大；现在做属于提前投资。
- **验收**：轨枚举与能力表快照不变。
- **风险**：中（517 行、涉及播放路径）。

### M-2 / M-3 / L-1

- **M-2**：`Family` 拆成 `ChipFamily` + `ChannelKind`；会触及编码器的 `patch` 语义，宜与 H-3 同批。
- **M-3**：把「loader 负责换算到统一栅格」写进 `SongIr` 的契约文档（代码已如此，缺文字）。
- **L-1**：`kNoDevice = UINT32_MAX` 或 `std::optional`，改动小，可与任一任务顺路做。

## 5. 建议的下一步顺序

| 步骤 | 内容 | 完成后得到什么 | 预计 |
|---|---|---|---|
| 1 | ~~**M-1** 加断言与文档~~ | ~~新解码器作者不会再填错音高字段~~ **已完成** `b8335bb` | — |
| 2 | ~~**H-2** `tickRate` 贯通~~ | ~~时间基不再是隐藏常量~~ **已完成** `bf30b28` | — |
| 3 | ~~**H-3** 先分类再合并~~ | ~~重复消失~~ **已完成第 1 步**：`noteFromSampleCycle` / `noteFromPlaybackRate`，7 颗芯片收敛 | — |
| 4 | **H-4** 拆 `LibvgmSongInstance` | 新格式的接入点清晰 | 2 天 |
| 5 | M-2 / M-3 / L-1 收尾 | ~~1 天~~ **全部完成**：`cb7113d`、`9297260` | — |

理由：前两步是**便宜且降低后续风险**的（M-1 防止新代码进错模型，H-2 为多格式铺路）；H-3 收益最大但涉及面最广，放在模型澄清之后；H-4 在没有第二种格式前收益有限，可最后做。

## 6. 目标结构

```
Source/
  chips/          31 个解码器；只认 ChipConfig 与寄存器写
  midi/           SongIr、MidiSequenceBuilder、NoteExtractor、Pitch
  formats/
    vgm/          VgmCommandReader -> VgmSongLoader -> SongIr
    nsf/          (CPU 核心)      -> NsfSongLoader -> SongIr
  playback/       PlaybackEngine / ITrackPlayer
```

两条判据：`midi/` 不认识任何格式；`formats/*` 不认识 MIDI。

## 7. 迁移路径与进度

| 阶段 | 内容 | 状态 | 提交 | 验收 |
|---|---|---|---|---|
| R0a | `ChipConfig` / `IChipDecoder::reset` | **已完成** | `ac66325` | `ctest` 34/34；抽查与改前一致 |
| R0b | `RegisterWrite::value` → `uint16_t`，删 `value2` | **已完成** | `b6ea757` | QSound 49、K005289 16、c352 77/77 |
| R1 | `SongIr` + `ISongLoader`（C-1） | **已完成** | `53c0691`、`e7b356c`、`b11927b` | **95 首逐行一致**；新增 `testVgmLoaderIr` |
| R2 | `DecoderRegistry`（C-2） | **已完成** | `467067a` | **95 首逐行一致**；31 成员 → 1 注册表 |
| R3 | 合并可复用 `NoteTable`（H-3） | 待做（先分类） | — | 每颗单测断言不变 |
| R4 | 拆 `LibvgmSongInstance`（H-4） | 待做 | — | 轨枚举快照不变 |
| R5 | NSF 适配 | 单独立项 | — | 需新 CPU 核心 |

**收尾回归（2026-10-23，`9188ad8`）**：与 `plugin/Tests/golden/rip_notes.txt` 的 **117 首逐行一致** —— 12758 个音符、115 首 `all checks passed`。仅有的 2 首失败是 K007232 04/07，即已知缺陷 **D1**（循环回跳），与本次重构无关。`ctest` 34/34。

## 8. 评分

| 维度 | 评审时 | 现在 | 依据 |
|---|---|---|---|
| 芯片层可读性 | A- | A- | 未变，逐颗有寄存器表与公式出处 |
| 模块性 | B | **A-** | 三层职责分离，装配层不再认识格式 |
| 新增**芯片** | A- | **A** | 三处改动收敛为一处 `dispatch` |
| 新增**格式** | D | **B** | 只需写 loader；真正的门槛变成“有没有 CPU 核心” |
| 测试与可验证性 | A- | **A** | 单测 + 95 首行为基线（可逐行比对）+ `--pitch-report` |

## 9. 附：与其他格式的距离

| 格式 | 现状 | 距离 |
|---|---|---|
| VGM/VGZ | 已支持 | — |
| NSF/NSFE | libvgm 不支持，需要 6502 核心 | 写 `NsfSongLoader` + R5；`NesApuDecoder` 已存在，缺的只是“谁来写 `$4000`” |
| FMT / PC-98 OPNA | libvgm 支持 YM2608 | 只差 loader；是 R1/R2 收益最直接的一类 |
| SPC / HES / KSS | libvgm 不支持 | 各自需要 CPU 核心，接入方式与 NSF 相同 |

## 10. 经验记录

- **批量重构必须带数量断言**。`Okim6295Decoder::reset` 的签名与 `{` 之间多一个空行，31 处批量替换只命中 30，是 `expected_count` 当场报的警。
- **行为基线不是测试的替代品，而是补充**。`ctest` 覆盖公式，覆盖不到 57 个命令的装配；95 首 rips 能覆盖，但必须可重复——因此基线入库（`tools/capture_rip_baseline.ps1`）。
- **类型宽度按硬件最大值定义**。`RegisterWrite.value` 取 16 位而不是 32 位：当前最宽寄存器是 16 位，多出的位只会让“这个值有多宽”变模糊。
- **注册表按实现类型索引，不按领域概念**。按 `ChipKind` 建表会把共享解码器的六个 OPL 设备拆成六个实例，音符变少且不报错。
- **IR 描述硬件，loader 声明时间基**。时钟来自运行时，时间基来自格式；两者混在一起就会把格式知识带回装配层。
