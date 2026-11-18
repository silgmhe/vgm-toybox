# 一条轨一个模拟器：多轨可视化的内存价格

> 打开任何一首 VGM 都要 **290 MB**，与曲子多大几乎无关。
> 94 KB 的曲子 312 MB，1.8 MB 的曲子 289 MB —— 差值是噪声。
>
> 所以这不是「某首 PCM 特别多的歌占得多」，是**每条 mixer 轨各养一个完整模拟器**的结构成本。

## 一、症状

`test_file/neo_geo/04 Stage 1.vgz`（1.8 MB 压缩）播放时，工作集接近 500 MB。

## 二、实测

用 `vgm_app_test` 跑脚本，进程外轮询 `WorkingSet64` 取峰值。

| 场景 | 峰值 |
|---|---|
| 空窗口（不加载） | 19.6 MB |
| 加载 **94 KB** 的 vgz | **312.4 MB** |
| 加载 **1.8 MB** 的 vgz | **289.2 MB** |
| 大文件 + 打开键盘页 | 341.4 MB |

**三个结论**：

1. **与文件大小无关**。94 KB 与 1.8 MB 占用相同，大文件甚至略低（采样时机噪声）。
2. **与可视化无关**。键盘页只加 52 MB，其中 `noteWaveform` 约 5 MB。
3. **`vgz` 解压后只有 2.32 MB** —— 289 MB 是它的 124 倍，不可能是数据本身。

**所以「加载时分析一堆采样」不是主因。** 采样收集确实存在（下文 `prepare` 里的 +16 MB），但它只占 5%。

## 三、钱的去向

在 `LibvgmSongInstance::prepare` 和 `createPlayer` 里临时打点（用 `K32GetProcessMemoryInfo`）：

```
prepare:start          23.8 MB
after probe            26.6
after enumerate        26.6
after midi sequence    28.1
after song ir          29.4
prepare:end            45.4     ← 采样收集 +16 MB

createPlayer × 16：
  prepared → loaded   每站 +2.3 MB
  最终                 146.6 MB
```

**两件事叠在一起**：

**1. `buildSong` 被调了两次**

```
[call] line 1323     ← rebuildPlayers
[build] song 2
[open] instance 2
```

第一次来自 `loadImage`，第二次来自 `rebuildPlayers`。触发条件是：

```78:78:plugin/Source/playback/PlaybackEngine.cpp
needsRebuild = loaded.load() && settingsChanged;
```

**先加载、后 prepare**（采样率从默认变成实际值）就必然完整重建一次，**两份 instance 在峰值时并存**。

**2. 每条 mixer 轨一个 `LibvgmTrackPlayer`**

16 轨 = 16 个 player，每个 6.3 MB。这 6.3 MB 是**每颗芯片自己的实例状态**（`OPN` 结构、`OPLL` 结构……14 个设备），不是共享数据 —— 它们是独立的模拟器。

## 四、两个开源项目怎么做的

**FamiStudio**：

```
Source/ChannelStates/ChannelState*.cs   ← 13 个子类，每通道一个状态对象
Source/Player/NesApu.cs                 ← 一个 NSFE 实例（P/Invoke）
```

`ChannelState` 对象**不拥有模拟器**，从**同一个** `NesApu` 读 `$4000–$4017`。NES APU 每个通道的 period / volume 寄存器都可读，一个实例就能回答所有通道。

**furnace**：

```810:810:reference/furnace/src/engine/engine.h
DivChannelState chan[DIV_MAX_CHANS];
```

一个 `DivEngine`，一个定长数组。`DivChannelState` 里是 note / volume / pitch / vibrato —— **tracker 自己的乐谱状态**，不是芯片输出。

**两者都不搞多实例。** 因为它们是**编辑器**：知道自己演奏了什么。

而我们是**播放器**：只有 VGM 命令流，波形必须让芯片算。

## 五、为什么 NSF 能，libvgm 不能

**NSFPlay 的每个声音对象都有 `RenderChannels`**：

```1602:1626:plugin/Source/backends/nsf/NsfPlayer.cpp
apu->RenderChannels (squares, 2, 2);
dmc->RenderChannels (tnd, 2, 3);
...
if (vrc7) { vrc7->RenderChannels (expansion, 2, 6); }
```

**一次渲染，每个通道一节输出。** 所以 `NsfBackend` 可以声明 `inPlaceChannelMix`，让整个混音台用一个 player。

验证方法已经在树里（`vgm_nsf_probe --check`：同一段渲染两次，一次混合一次拆分，求和比对）：

```
channels sum vs mixed: worst difference 0.000000 over 4096 frames   ← 2A03
channels sum vs mixed: worst difference 0.000000 over 4096 frames   ← VRC7
```

**libvgm 没有这一层。** 它的渲染在芯片内部就混合了：

```c
// fmopn.c
...Update (void* info, UINT32 samples, DEV_SMPL** dest)
```

`dest[2]` 是**已经混好六个通道**的立体声。要拆开，得改 `Update` 的签名，然后 `VGMPlayer` 的 `devBufs` 和 14 个芯片核心全都要跟着改。

**那不是重构，是重写 libvgm 的输出层。**

至于「逐通道 mute 再渲染 N 次」—— **不行**，因为每次 `Render` 都推进播放位置，N 次等于把歌快进 N 倍。

## 六、结论：不做

三件事决定了它不做：

1. **NSF 那条已经拿到了**（一个 6502 而不是十一个），改动留在 `refactor/shared-playback-instance` 分支，**未合并** —— NSF 本来就不占多少内存，收益不值得引入一轮回归风险。
2. **libvgm 的 145 MB 无法靠改源码消除**，只能靠改架构。
3. **剩下的唯一办法是改产品**：混音台从「通道级」退到「设备级」（一颗芯片一轨），能省 140 MB，但代价是 YM2612 的六个 FM 通道只能整体调音量。

第 3 条是产品决定，不是技术决定。

**一个值得记住的事实**：`perTrack` 只在 `config.mixerEnabled` 为真时成立。

```1201:1213:plugin/Source/playback/PlaybackEngine.cpp
const bool perTrack = config.mixerEnabled && ... ;
const size_t playersWanted = perTrack ? pending.layout.size() : 1;
```

**混音台关闭时只有一个 player。** 所以 290 MB 只在混音台打开时存在，键盘页本身是便宜的。

## 七、如果将来要做

**A. 混音台改设备级**（省 140 MB）

改 `LibvgmSongInstance::enumerate()`，让 track 按 device 分组而不是按 channel。键盘页不受影响 —— 它读的是 `trackPlayers[0]` 的 `captureState`，那个聚合 player 能看到全部通道。

**B. 消除重复的 `buildSong`**（省约 145 MB 的峰值）

`rebuildPlayers` 复用已有 instance，只重建 player。这一条独立于 A，且不需要改产品。

**C. 改 libvgm 输出层**（彻底，但不划算）

让 `Update` 输出 per-channel。14 个芯片核心，风险远大于收益。

**A 和 B 都不做，理由已记录在第六节。** 这份文档存在，是为了让下一次有人问「为什么打开一首歌要 290 MB」时，不必重新测一遍。
