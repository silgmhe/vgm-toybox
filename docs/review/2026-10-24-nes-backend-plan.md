# NES 后端：用 NSFPlay 的芯片核心播放 FTM

日期：2026-10-24　分支：`feature/multiformat-core`

## 1. 为什么需要新后端

`libs/libvgm` 的芯片核心只覆盖 NES 的 APU 与 FDS：

```
emu/cores/  178 个文件，NES 系只有 np_nes_apu.c / np_nes_dmc.c / np_nes_fds.c
```

语料实测：

| 模块 | 扩展芯片 | 现在能听到 |
|---|---|---|
| `fod_pancake` | 无 | 完整 |
| `Sunrise…` / `snowburnb` / `Vietnamese…` | VRC6 | 仅 2A03 |
| `fod_hopeless` / `TMNT…` | N163 | 仅 2A03 |
| `15_coffeezoneb` | VRC7 | 仅 2A03 |

另外还有一条硬约束：**VGM 格式没有 VRC6/MMC5 等命令**（`vgmplayer.hpp` 的命令表里只有 `B4` NES APU、`B5` FDS、`B3/B5..BB` 的通用写），所以“扩展 FTM 编译成 VGM”这条路在格式层面就走不通。

## 2. 两条可选路线

| | **A. FTM → NSF 流 → NSFPlay** | **B. FTM → 寄存器流 → NSFPlay 核心** |
|---|---|---|
| 需要的中间物 | 6502 驱动的数据打包（FT `CCompiler` 的最大一块） | 无（`SongIr` 已经有了） |
| 需要 6502 执行 | 是 | 否 |
| 每颗芯片要做 | 无（NSFPlay 全支持） | 寄存器映射，约 150 行/颗 |
| 额外收益 | 可导出 `.nsf` | — |
| 工作量 | 大 | 中 |

**选 B**：它把“让扩展芯片出声”从“做完编译器”降级为“写每颗芯片的寄存器映射”，并且立刻可听。A 留作将来“导出 NSF”的功能，那时编译器前半（`ISongSource`、`CChunk`、乐器 `Compile`）正好复用。

## 3. NSFPlay 提供什么

`plugin/Source/backends/nes/third_party/nsfplay/xgm/devices/Sound/` 下（从本地参考树复制而来，见 §8）：

```
nes_apu.cpp/.h     2A03 方波/三角/噪声
nes_dmc.cpp/.h     DPCM
nes_vrc6.cpp/.h    VRC6（2 脉冲 + 锯齿）
nes_n106.cpp/.h    N163（波表）
nes_vrc7.cpp/.h    VRC7（FM）
nes_fds.cpp/.h     FDS
nes_mmc5.cpp/.h    MMC5
nes_fme7.cpp/.h    Sunsoft 5B
```

全部实现 `ISoundChip`（Setup / Reset / Write(adr,val) / Render），即**寄存器级接口**——正是 `SongIr` 的形状。

许可：readme 表述含糊（“presumed … distributable”），`xgm` 设备源自 ValleyBell 一系，通常按 GPL 分发；本项目 AGPL，兼容。合入时把原始 license 文本一并引入并注明来源与改动。

## 4. 目标结构

```
plugin/Source/backends/nes/
  NesBackend.{h,cpp}        getFileExtensions {".ftm"}；canOpen 按魔数；open → NesSongInstance
  NesSongInstance.{h,cpp}   解析 SongIr；枚举芯片与轨道；createPlayer → NesTrackPlayer
  NesTrackPlayer.{h,cpp}    实现 ITrackPlayer：按 sampleTime 应用寄存器写、渲染、单轨隔离
  NsfChipSet.{h,cpp}        把 NSFPlay 的 ISoundChip 包成统一接口（创建/写/渲染/静音掩码）
  third_party/nsfplay/      引入所需源文件（apu、dmc、vrc6、n106、vrc7 + 依赖）
```

契约与 libvgm 后端一致：`prepare(hostSampleRate, maxBlockSize)`、`render(buses, n, blockSize)`、`createPlayer()`；混音、DAW 同步、seek 全部复用，不改上层。

## 5. 分期与验收

| 步 | 内容 | 验收判据 |
|---|---|---|
| 1 | 骨架 + 只接 `nes_apu`/`nes_dmc`，用现有 FTM→`SongIr` 渲染 | `fod_pancake` 在新后端出声，且与 libvgm 版本的包络接近 |
| 2 | 接 `nes_vrc6` + FTM 侧通道 5/6/7 寄存器映射 | `Sunrise_to_Sunset` 出现三条扩展声部 |
| 3 | 接 `nes_n106`、`nes_vrc7` | 语料 7 首全部有声部 |
| 4 | 与 FT 导出的 WAV 比对包络 | 客观一致（RMS 包络相关度） |

第 1 步是整个方案成立与否的判据：它证明“我们的寄存器流 + NSFPlay 核心”能出声，且不碰 libvgm 一行。

### 5.1 进度（截至本轮）

| 步 | 状态 | 证据 |
|---|---|---|
| 1 骨架 + APU | ✅ | `fod_pancake` 经 NSFPlay 渲染 peak 0.0630 / rms 0.0202 |
| 2 `nes_vrc6` + 通道 5-7 | ✅ | `Sunrise` 独奏峰值 0.0547 = 混音峰值 |
| 3 `nes_n106` + 通道 11-18 | ✅ | `TMNT` 混音 0.0646 → 0.1232，N163 独奏 0.0586 |
| 3 `nes_vrc7` + 通道 20-25 | ✅ | `15_coffeezoneb` 前 5 秒 VRC7 独奏 0.0466 |
| 4 与 FT 导 WAV 比对 | ❌ | 尚未做；目前判据是"有声且独奏非零" |

**尚未接入应用**：以上全部跑在 `--ftm-nsf` 探针里。应用仍走 VGM → libvgm，因此 `.ftm` 在
播放器里只有 2A03 声部。把 `NesBackend`/`NesSongInstance`/`NesTrackPlayer` 建起来是下一步。

### 5.2 N163 的实现要点（供后来者）

- 波形来自乐器：装载器需读 `CInstrumentN163` 的记录（尺寸、RAM 位置、波数、4 bit 采样）；
- 位置是**半字节地址**，RAM 写入在 `wavePos >> 1`，两个采样打包成一个字节（低半字节在前）；
- 频率是**相位增量**：`f * 15 * waveSize * 65536 / clock`；
- 声道数由 `reg[0x7F]` 高 3 位决定，必须写入，否则 `get_channels()` 为 1 且混音极轻；
- 声道编号**自高向低**：跟踪器通道 i 对应硬件 `7 - i`。

## 6. 已知风险

| 风险 | 说明 |
|---|---|
| NSFPlay 的构建依赖 | `ISoundChip`、`Types.h`、`Path` 等来自 `xgm`/`vcm`，需要挑选最小源文件集与 include 路径 |
| 寄存器语义 | VRC6/N163 的寄存器映射以 FT 的 `ChannelsVRC6.cpp` / `ChannelsN163.cpp` 为准（参考实现在本仓库） |
| 时间基 | `SongIr` 的时间戳是 44.1 kHz 采样；`sampleTime` 与块边界的对齐决定是否会咔哒 |
| 双后端并存 | `.vgm` 仍走 libvgm；`.ftm` 走 nes 后端。注册表按扩展名/魔数分派即可，但两套后端都要过同一批播放测试 |

## 7. 与既有工作的关系

- `FtmSongIr.cpp` 已经把 FTM 编译成寄存器写（音符、音量、占空比、宏、行效果、DPCM）；
- 本后端消费的正是这份输出，不重新解析模块；
- 已有的 `--ftm-audio` 探针与 `ftm_vgm` 生成路径继续用于 libvgm 侧的回归。

## 8. 第三方代码与依赖边界

`reference/` 是**本地参考树，且被 `.gitignore` 排除**——它只在开发者的机器上存在，构建、`ctest` 与
任何自动化流程都不得依赖它。因此凡是被编译的第三方代码都已复制进仓库：

| 位置 | 来源 | 用途 | 是否参与构建 |
|---|---|---|---|
| `plugin/Source/backends/nes/third_party/nsfplay/` | NSFPlay `xgm/` | NES 芯片核心（APU/DMC 已编译，VRC6/N163 等备用） | 是（3 个 .cpp） |
| `plugin/Source/formats/ftm/compiler/DriverData2A03.h` | Dn-FamiTracker `drivers/drv_2a03.h` | 2A03 驱动二进制（8 + 7,076 字节） | 是（数据） |
| `plugin/Source/formats/ftm/compiler/{SongSource,ModuleSongSource,Chunk,Instrument}` | 本项目自行编写 | 编译器移植的接口与数据缓冲 | 是 |

**仅文档与可选工具仍提到 `reference/`**：

| 项 | 说明 |
|---|---|
| `tools/build_dnft.ps1` | 编译本地参考实现，用于生成权威导出；缺参考树时脚本自行失败，不影响构建 |
| `tools/ftm_regression.ps1` | 默认 `-TrackerExe` 指向参考树里的 exe；缺省时逐首 SKIP |
| `docs/**` | 设计文档引用了参考源码的行号，属于出处标注 |

验证方式（本文件写作时的实测）：CMake 与 `plugin/Source` 中的 `reference/` 出现次数为 **0**，
且 `ctest` 的 21 个测试无一调用 `tools/` 下的脚本。
