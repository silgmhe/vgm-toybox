# NSFPlay 的播放原理，以及接入我们这套后端接口的方式

分支：`feature/nsf-format-support-ref-nsfplay`
日期：2026-10-24

## 1. NSF 是什么

一个 NSF 就是**一段 6502 程序加一份头**：

```
$00  NESM                魔数
$05  版本                1 存的是长度，2 以上存的是 load 地址
$06  曲目数（1-based）     $07 起始曲（1-based）
$08  load / init / play   三个地址
$6E  NTSC 速度（微秒）     $78 PAL 速度
$7A  芯片标志             VRC6/VRC7/FDS/MMC5/N163/FME7
$80  程序与数据
```

播放器要做的事只有三件：把程序装进地址空间、调用 init 一次、每帧调用 play。**音乐不在数据里，在程序的寄存器写里**。

## 2. NSFPlay 的分层

```
NSFPlayer  ── 头解析、芯片实例、混音、滤波
   │
   ├── NES_CPU    6502 执行，并**自己负责帧调度**
   ├── NES_MEM    64 KB 地址空间 + $5FF8 的 bank 窗口
   ├── NES_BANK   分 bank 读取
   └── 芯片：NES_APU / NES_DMC / NES_VRC6 / NES_VRC7 / NES_N106 / NES_FME7 / NES_MMC5 / NES_FDS
```

关键点：**芯片之间没有统一的抽象层要我们实现**，每个芯片都是独立对象，播放器分别喂它们。

## 3. 三个真正决定成败的机制

### 3.1 用跳板调用驱动的入口（`nes_cpu.cpp:100 run_from`）

```cpp
context.PC = PLAYER_RESERVED;          // $4100
breakpoint = context.PC + 3;           // $4103
nes_mem->WriteReserved (PLAYER_RESERVED+1, address & 0xff);
nes_mem->WriteReserved (PLAYER_RESERVED+2, address >> 8);
```

`$4100` 处的程序（`nsfplay.cpp:166`）：

```
$4100  20 1E 41    JSR $411E     ← 操作数在 $4101/2，被上面改写成驱动的入口
$4103  4C 03 41    JMP $4103     ← 哨兵：CPU 跑到这里说明"这一帧的 play 返回了"
$4106  ...         保存 A/X/Y，供不返回的 INIT 使用
$4110  20 1E 41    JSR play
$411D  40          RTI
$411E  60          RTS
```

**这就是我们上次失败的根源之一**：跳板必须在 `SetImage()` **之后**写，因为 `SetImage` 会清空整个 64 KB。

### 3.2 帧调度在 CPU 核心里，不在播放器里（`nes_cpu.cpp:114 Exec`）

```cpp
while (clock < clocks) {
    if (!breaked) { exec(); if (PC == breakpoint) breaked = true; }
    else { /* 跳到帧边界 */ }

    if (breaked && play_ready) {          // play_ready 由 fclocks_left_in_frame 决定
        run_from (play_addr);             // 下一帧的 play
    }
}
```

**播放器不需要自己数帧**——只要不断调用 `Exec(n)`，CPU 会在 `fclocks_left_in_frame` 到期时自动重入 play。上次我们手写帧计数，是重复了核心已有的职责。

### 3.3 逐采样推进，并手动驱动帧序列器（`rconv.cpp:98 ClockCPU`）

```cpp
real_cpu_clocks = cpu->Exec(cpu_rest);
if (dmc)  dmc->TickFrameSequence (real_cpu_clocks);
if (mmc5) mmc5->TickFrameSequence (real_cpu_clocks);
```

`NES_DMC::Tick()` 只算三角/噪声/DPCM 三个输出，**包络与长度计数器靠 `TickFrameSequence`**。漏掉它就得到"音高对、没有起伏"的长音。

## 4. 与我们的接口对齐

| 我们的接口 | NSF 侧的对应物 |
|---|---|
| `ISongBackend::canOpen` | `NESM` / `NSFE` 魔数 |
| `ISongBackend::open` | 解析头 → `NsfInstance` |
| `ISongInstance::getInfo` | 头里的 title/artist/copyright、芯片位、`singlePassSeconds` |
| `ISongInstance::getTracks` | 5 条 APU（Pulse1/2/Triangle/Noise/DMC）+ 扩展芯片的通道 |
| `ISongInstance::createPlayer` | 构造一个 `NsfPlayer`（CPU + MEM + 芯片 + 采样率） |
| `ITrackPlayer::start` | `cpu->Start(init, play, play_rate, song, region, nsf2_bits, irq, nullptr)` |
| `ITrackPlayer::render` | 逐采样 `cpu->Exec(dt)` + `TickFrameSequence` + 芯片 `Render` |
| `ITrackPlayer::setTrackAudible` | **芯片的 `SetMask`** |
| `BackendCapabilities` | `canSeek`（重放至目标）、`hasLoopPoint`（探测）、`inPlaceChannelMix` |

### 一个决定性的优势：芯片自带 `SetMask`

```cpp
// nes_apu.h:81 / nes_vrc6.h:49 / nes_n106.h:67 ...
virtual void SetMask (int m) { mask = m; }
```

这意味着**逐轨隔离不需要多份模拟**：

| 方案 | 代价 | 依据 |
|---|---|---|
| `multiInstance`（libgme 只能这样） | 一份模拟 × 轨道数 | 无 mask，只能各自静音重跑 |
| **`inPlaceChannelMix`（NSFPlay 可以）** | 一份模拟 | 芯片内部按 mask 决定哪些通道进混音 |

这是这条分支相对 `feature/nsf-format` 最大的结构收益，也直接解决了上一分支"8 条轨难隔离、sum 对不上"的问题。

## 5. 三个已知的坑（上次逐个撞过）

| 坑 | 症状 | 正确做法 |
|---|---|---|
| 没调 `NES_CPU::Reset()` | 首次取指访问违例 | `Reset()` 安装 `ReadByte/WriteByte`，`Start()` 不会代劳 |
| 跳板写在 `SetImage` 之前 | 执行 0，一条寄存器写都没有 | 先 `SetImage`，后写 `$4100` |
| 不调 `TickFrameSequence` | 有音高、无包络 | 每 tick 与 `Tick()` 一起调 |

## 6. 实施步骤与验收

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | `NsfFile`：头解析 | 11 条断言（魔数、1-based、PAL 位、芯片位、截断拒绝） |
| 2 | `NsfBus`：`NES_MEM` 子类，`$4000-$4017` 转交芯片，`$5FF8-$5FFF` 选 bank | 写寄存器不落内存、bank 切换后读回变化 |
| 3 | `NsfPlayer`：`Reset` → 跳板 → `Start` → 逐采样 | 三个真实文件都产生非零寄存器写，peak > rms |
| 4 | `NsfBackend` 接入注册表 | 引擎报告 "NSF (NSFPlay core)"，`.nsf` 可打开并出声 |
| 5 | `SetMask` 隔离 | 每条轨道单独的 peak 不同，且不等于整曲 |
| 6 | 时长/循环 | 参考 NSFPlay 的 `DetectLoop`，或先用配置值 |

第 3 步是分水岭：**只要写计数非零，就说明 CPU、内存、跳板三件事同时正确**。

## 7. 编译环境的两个事实

- 源码注释是 **Shift-JIS**，已被本分支转成 **UTF-8 with BOM**（无 BOM 时 MSVC 按本地代码页解释，会把注释里的字节当成代码，`nes_mem.cpp` 会报 C2601）；
- `xgm/devices/device.h` 会引用 `../xtypes.h`，`vcm.h` 会引用 `vcm_ui.h`——这两个头文件不参与编译，但必须留在树里。
