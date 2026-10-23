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

`reference/nsfplay/nsfplay-master/xgm/devices/Sound/` 下：

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
