# NSF：Super Mario USA 只播出一个事件（2026-11-08 调查记录）

> 结论先行：**不是我们的后端，也不是 VGM/VGZ 工作引入的回归**。`libs/nsfplay`（xgm 核心）
> 对这卷驱动的模拟与 NotSoFatso / MDPlayer 不同，导致驱动的音乐引擎只被触发一次。
> 本文记录证据、已排除项、以及"改库"时要动的位置。

## 一、现象

`test_file/nsf/Super Mario Bros. 2 JP [Super Mario USA] (1988-10)(Nintendo EAD)(Nintendo).nsf`

- 能加载、能"播放"，但**三角波恒为直流偏置**，脉冲/噪声无声；
- 开头有一记 DPCM，之后完全静止；
- 同一文件在其他播放器正常。

## 二、范围（谁坏、谁不坏）

| 播放器 | 核心 | 结果 |
|---|---|---|
| FamiStudio | NotSoFatso | 正常 |
| MDPlayer | 自带 NSF 播放器 | 正常 |
| 官方 nsfplay 二进制 | xgm | **同样失败** |
| 本项目 | `libs/nsfplay`（xgm） | 同样失败 |
| tag `0.8.0` 的构建 | xgm | 同样失败 |

`git log` 事实：`NsfPlayer.cpp` 由 `c55475f` 引入后再未被替换；`cpu->Start` 与 `framesPerSecond`
的调用约定只在 `c55475f`/`da7c91c` 出现过；近期的 NSF 提交（`1644e21`、`98a9e29`、`2499626`、
`24fada2`）都只涉及 DPCM 采样扫描/导出与快照波形，**不在播放回路**。

因此：**不是回归**，是核心能力差异。

## 三、文件与驱动的静态事实

```
NESM v1 · 34 首 · start=16 · load=$8000 · init=$8800 · play=$8000 · 24 KB · banks 全 0
```

`init $8800`：

```
TAX                     ; X = A = 曲目号
LDA #$80; STA $4017
LDA $8829,X             ; 该曲的一个打包字节
PHA; LDX #0; TXA; STA $4015; STA $0600,X; INX; BNE      ; 清 $0600..$06FF
PLA; TAX; ROR×3; AND #$1F; TAY
TXA; AND #$07; TAX
LDA $884B,X; STA $0600,Y ; $884B = 01 02 04 08 10 20 40 80（位掩码表）
RTS
```

`play $8000`：

```
LDA #$FF; STA $4017
LDA $0100; CMP #$41; BNE → $8014      ; 读【栈页】当标志
$8014: JSR $8078 / $8138 / $82A2 / $82FD / $8361
```

五个子程序的门槛（全部读 `$0601..$060D`）：

```
$8078: LDA $060D …      $8138: LDA $0602 …      $82A2: LDY $0605 …
$82FD: LDA $0601 …      $8361: LDA $0603 …
```

## 四、动态证据（探针实测）

`NSF_TRACE=1`（探针新增，见 `plugin/Tests/NsfProbe.cpp`）：

```
[apu] #001 $4017 = 80      ← init
[apu] #002 $4015 = 00
[apu] #003 $4017 = 80      ← 预扫描/正式各一次
[apu] #004 $4015 = 00
[ram] $0600..$060F: 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[apu] #005 $4017 = FF      ← play 第一句
[apu] #006 $4010 = 0E      ┐
[apu] #007 $4012 = 4F      │ 一记 DPCM
[apu] #008 $4013 = 43      │
[apu] #009 $4015 = 0F      │
[apu] #010 $4015 = 1F      ┘
[apu] #011 $4017 = FF      ← 之后每帧只有这一条
```

逐帧状态（`NSF_SONG=16`，文件默认曲目）：

```
t=0.44s  … $0608=01  $060A=85
t=0.91s  … $0608=01  $060A=69
t=1.37s  … $0608=01  $060A=4D
t=1.83s  … $0608=01  $060A=31
t=2.30s  … $0608=01  $060A=15
t=2.76s  全零（此后不动）
```

`$060A` 每 0.46 s 减少 0x1C ≈ 每秒 61 次 —— 与 NSF 帧率（60.09 Hz）吻合，说明 **play 被调用的
节奏是对的**；驱动按帧递减计时器，计时器归零后**没有取下一个事件**。

用 `NSF_SONG` 遍历 34 个曲目：每个曲目只产生**一个**事件（不同的 `$0608`/`$0609` 组合，
其中 24、28 完全无事件）。这在真正的音乐 rip 里不合理 —— 说明引擎进入了"单次音效"路径后停摆。

## 五、调用协议已排除

`libs/nsfplay/xgm/devices/CPU/nes_cpu.cpp:285` 的 `NES_CPU::Start`：

```cpp
context.A = song; context.X = region; context.Y = extra_init ? 0x80 : 0;
context.P = 0x26;
run_from (init_addr);
```

我们的 `NsfPlayer::resetToStart` 用同一签名调用（`song = startSong - 1`、`region = PAL?1:0`），
`NSF_SONG` 实验也证明曲目号确实到达了驱动的 A 寄存器。

## 六、尚未定位的核心侧差异（"改库"时要查的位置）

按可能性排序：

1. **`$4017` 写入的帧计数器行为** —— 驱动每帧写 `$FF`（bit7=1 → 5-step，bit6=1 → 抑制 IRQ）。
   xgm 的 `nes_apu.cpp` 对 `$4017` 的处理若与硬件在"5-step/4-step 切换时钳制定时器"这一点不同，
   依赖帧定时的驱动会停摆。文件：`libs/nsfplay/xgm/devices/Sound/nes_apu.cpp`（`Write` 中 `$4017` 分支）。
2. **`$4015` 读回值** —— 驱动可能在别处轮询通道状态；`NsfBus` 对 `$4015` 的读必须落到 APU
   （`NsfBus.cpp:208`），若被总线短路成 0，状态机会卡住。
3. **DMC / 帧 IRQ 的启用时序** —— drive 写 `$4010 = 0E`（IRQ 关闭），但 `$4015` 的 DPCM 启停与
   `$4011` 电平可能影响后续分支。
4. **`NSF2` 位域**：`nes_cpu.cpp` 里 `nsf2_bits & 0x20`（extra init / NMI play）等分支，若被误判，
   会改变播放方式。本文件是 NSF v1，理论上全为 0，值得核对解析。

## 七、下一步（一次编译的测量）

在探针里加三项打印，直接判定 1 还是 2：

```
[frame] 每帧 CPU 时钟数（应为 29780 @ NTSC）
[4017 ] 每次写入后的帧计数器状态与下一帧间隔
[4015R] $4015 的读回值与时刻（首次 200 次）
```

- 若每帧时钟数与 29780 有偏差 → 帧时序问题（第 1 项）；
- 若 `$4015` 读回恒为 0 → 总线短路（第 2 项）。

## 八、若确认要改库

范围：`libs/nsfplay/xgm/devices/{Sound/nes_apu.cpp, CPU/nes_cpu.cpp}`，以及 `NsfBus.cpp` 的读路径。
风险：xgm 同时承载 DPCM 采样导出与 note walk，改动须以 `test_file/nsf` 下 9 个已通过的 NSF
为回归集（`46/46` 不覆盖它们，需手工跑探针写入数对比）。

替代方案（若核心差异过大）：改用 `reference/FamiStudio/ThirdParty/NotSoFatso`（C++ 源码齐全，
FamiStudio 在用，已知能播本文件）。代价是替换整块 NSF 后端（播放 + 采样导出 + note walk）。

## 九、复现命令

```powershell
# 寄存器写入序列 + 引擎状态页
$env:NSF_TRACE='1'; build\plugin\vgm_nsf_probe.exe "<nsf>" 6

# 指定曲目
$env:NSF_SONG='16'; build\plugin\vgm_nsf_probe.exe "<nsf>" 3
```

相关提交：`cca5916`（探针追踪）、`2ab94fa`（`NSF_SONG` + 逐帧状态）。分支 `fix/nsf-driver-init`。
