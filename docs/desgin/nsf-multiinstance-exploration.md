# NSF 再探：改用多实例隔离

- **状态**：已实现并验证（`b159cb6`）
- **日期**：2026-10-24
- **分支**：`feature/nsf-format-multiinstance`，起点 `e3d5baa`（原 `feature/nsf-format-support-ref-nsfplay`）
- **相关**：`docs/memo/2026-10-24-nsf-shelved.md`（上一轮放弃的记录）、`playback-backend-design.md`

## 1. 为什么再试一次

上一轮放弃 NSF，直接原因是**分轨做不到**。三次尝试都倒在同一个地方：

| 尝试 | 做法 | 结果 |
|---|---|---|
| A | libgme `osc_output(i, NULL)` 静音其他声道 | 静音无效：`Classic_Emu::play_` 只读**一个共享 buffer**，实测求和比原混音高 **+16.9 dB ≈ 7 倍** |
| B | 自建 per-voice `Blip_Buffer` + 覆盖 `play_` | 独立探针跑通（`peak=-514`），接进后端 `0xC0000005` |
| C | NSFPlay 核心 + `inPlaceChannelMix` | 引擎首次走通单实例多轨，但扩展声道仍重复、unity-gain 对不上 |

而 VGM/VGZ 侧**从来就是多实例**，一直是对的：

```cpp
// LibvgmSongInstance.cpp:567
caps.isolation = playback::IsolationMode::multiInstance;
// PlaybackEngine.cpp:904-907
const bool perTrack = config.mixerEnabled && caps.isolation == multiInstance && !layout.empty();
const size_t playersWanted = perTrack ? layout.size() : 1;
```

所以这次的思路很简单：**NSF 也走这条已验证的路**。不再要求"一份模拟出多轨"，而是"一轨一份模拟，各自把别的声道静音"。

## 2. 这次为什么能成

区别在于**静音手段是否真的作用到渲染**。

| 库 | 静音接口 | 是否影响渲染 |
|---|---|---|
| libgme | `osc_output(i, NULL)` | ❌ 播放循环读共享 buffer，绕过了它 |
| **NSFPlay 核心** | `NES_APU::SetMask` / `NES_DMC::SetMask` / 各扩展芯片 `SetMask` | ✅ 芯片**内部**按掩码混音，静音真的少一路 |

现成的实现已经在 `NsfPlayer` 里：

```cpp
void NsfPlayer::setTrackAudible (int trackIndex)   // soloTrack = index; applyMasks()
void NsfPlayer::applyMasks ()                      // 每个芯片按位设掩码
```

也就是说，多实例所需的"每实例只留一路"**已经具备**，缺的只是让后端按多实例的方式使用它。

## 3. 现状（可直接复用的部分）

`e3d5baa` 上已可用，本次**不改动**：

| 能力 | 位置 |
|---|---|
| 头解析（1-based 曲目、PAL 位、v1 load 为长度） | `formats/nsf/NsfFile.*` |
| 地址空间、8×4KB bank（`$5FF8-$5FFF`）、声音寄存器转发 | `backends/nsf/NsfBus.*` |
| 驱动执行、`$4100` 跳板、逐帧 tick | `backends/nsf/NsfPlayer.cpp` |
| 轨道表 5 / 8 / 13（2A03 + 扩展） | `NsfBackend.cpp` |
| seek（`Exec(4096)` 快进）、时长（设置项默认 180 s）、`isFinished` | 同上 |
| 未声明扩展芯片探测 | 同上，`detectUndeclaredChip()` |

## 4. 设计

### 4.1 能力声明

```cpp
// NsfBackend.cpp:169 —— 现在
caps.isolation = expansionPresent ? multiInstance : inPlaceChannelMix;
// 改为
caps.isolation = playback::IsolationMode::multiInstance;   // 总是
```

`inPlaceChannelMix` 分支保留在引擎里但不再被 NSF 使用；一行即可切回。

### 4.2 每个实例只管一条轨

引擎已按轨创建播放器并逐个告知自己的轨道 id：

```cpp
// PlaybackEngine.cpp:812
for (size_t t = 0; t < players.size(); ++t)
    players[t]->setTrackAudible (layout[t].id, true);
```

后端实现改为：

```cpp
void setTrackAudible (playback::TrackId track, bool audible) override
{
    if (audible && track >= 1)  player.setTrackAudible ((int) track - 1);  // solo
    else                        player.setSilent (true);                   // 全静音
}
```

`track` 是 1-based 的 `TrackId`，`NsfPlayer` 的声道索引是 0-based（`0-1` 方波、`2-4` 三角/噪声/DPCM、`5+` 扩展）。

### 4.3 需要新增的一处

`soloTrack < 0` 目前表示"全部可听"，所以**没有"全静音"状态**。新增：

```cpp
void NsfPlayer::setSilent (bool silent);   // silent 时四个 SetMask 全置满
```

用在 `setTrackAudible(..., false)`（用户关掉某轨），以及 chip 静音为空的情况。

### 4.4 渲染路径

多实例下引擎只调 `ITrackPlayer::render()`（单声道求和），不调 `renderTracks()`：

```cpp
int render (float* left, float* right, int numSamples) override
{ return player.render (left, right, numSamples); }   // 已有
```

`renderTracks()` / `NsfPlayer::renderChannels()` 保留在代码里（引擎的 `inPlace` 通路也在），本次不作为执行路径。

## 5. 代价（必须先说清）

| 项 | 数值 |
|---|---|
| 模拟份数 | 每轨一份：2A03 only = **5**，VRC6 = **8**，N163 = **13** |
| 打开耗时 | 每实例各自 `start()`（含 init）+ 未声明芯片探测 |
| 内存 | N 份 64KB 镜像 + N 份芯片状态 |
| 对比：VGM 现状 | 一首 16 声道的 VGZ 就是 16 份，用户已接受这一代价 |

**探测要收紧**：`detectUndeclaredChip()` 现在在实例构造里跑，多实例下会跑 N 次。方案：探测结果存在 `NsfSongInstance`，`createPlayer()` 时把 `chipFlags` 传给 `NsfPlayer`，让每个实例直接按已知标志构造，不重复试跑。

## 6. 风险

| 风险 | 判断依据 |
|---|---|
| `SetMask` 静音是否改变模拟精度 | NSFPlay 的掩码只影响输出求和，不改变内部计时；但 DMC 的 DPCM 中断与噪声 LFSR 仍需实测 |
| 各轨求和能否回到 unity-gain | 多实例下每实例只有一路，求和应约等于原混音；若偏离 >0.5 dB，说明静音影响了芯片行为 |
| 打开变慢 | 13 份 init；需要实测，必要时把探测改成一次 |
| 扩展声道重复（旧症状） | 旧分支的 `setTrackAudible` 曾残留 `track <= oscCount` 上界，本次实现不得再出现 |

## 7. 验收标准（可测，不靠听）

1. **轨道数**：`Entry080` 5、`fod_snowburnb` 8、`fod_hopeless` 13；
2. **互不相同**：逐轨单独渲染 1 秒，导出各轨 RMS/峰值，**任意两轨不得完全相同**（旧症状就是"三条一模一样"）；
3. **求和一致**：各轨相加 vs 原混音，差 ≤ 0.5 dB（若因静音略偏，记录实测值而不是放宽了事）；
4. **seek**：`seek to 90.00 s -> position 90.00 s`，落点一致；
5. `ctest` 全绿（当前 35/35）；`build.ps1` 通过并核对 dist 时间戳晚于提交。

## 8. 实施步骤

1. `caps.isolation = multiInstance`，`setTrackAudible` 按 4.2 改写；
2. 加 `NsfPlayer::setSilent()`；
3. 探测结果提到实例层，`createPlayer` 传入标志（消除 N 次试跑）；
4. 用 `vgm_mixer_test` 跑三首文件，记录轨道数与逐轨 RMS；
5. 若求和偏离，先查 DMC 的 `SetMask` 是否连 DPCM 一起关掉（`out[2]`）；
6. 通过后出 dist，并在 `docs/memo/2026-10-24-nsf-shelved.md` 末尾追加"已重启"的指针。

## 9. 本次不做

- 不改 libgme（per-voice buffer 那条路已证明是死路）；
- 不实现 FME7 的 `RenderChannels`（`mask` 成员与其余芯片不同名）；
- 不解决 `1983.nsf`（驱动从不上报寄存器写）与 `fod_hopeless.nsf` 的既有失败；
- 不做循环检测（已证伪，`kDetectLoop = false`）。

## 10. 失败判据（提前写下来，避免又一次"再改一版就好"）

若第 3 条（求和一致）与第 2 条（互不相同）**不能同时成立**，说明 `SetMask` 静音会改变模拟行为，那么多实例同样不成立；此时应停在分支上、写结论，而不是继续调参。

## 11. 实测结果（2026-10-24，`b159cb6`）

失败判据没有触发：两条结论同时成立。

| 文件 | 轨道 / 芯片 | seek spread | 混音验收 |
|---|---|---|---|
| `Entry080.nsf` | 8 / 2 | 0.0000 s | 全部通过 |
| `fod_snowburnb.nsf` | 8 / 2 | 0.0000 s | 全部通过 |
| `fod_hopeless.nsf` | 13 / 2 | 0.0000 s | 全部通过 |

逐轨 RMS（`fod_snowburnb` 前 0.5 s，八条互不相同）：

```
Pulse 1 0.0023   Pulse 2 0.0000   Triangle 0.0045   Noise 0.0065
DMC     0.0000   Saw     0.0000   Square3  0.0000   Square4 0.0000
```

比例断言连跑两次一致：`master 0.5 -> 0.500`、`output volume 0.5 -> 0.250`。

代价：`plain 2.2 ms vs mixer 25.6 ms`（13 轨，11.5×），与 VGM 同构。

### 实现过程中修掉的两个真 bug（都会在别处重现）

1. **seek 的单位**：`rendered` 记采样，循环里加的是 CPU 时钟，导致每次 seek 只走到目标的三十分之一（报告 90 s，音频在 2.2 s），十三个实例各自停在不同位置，正是 `spread 0.0163 s` 的来源。改为按时钟计数 + 末步裁剪。
2. **NSFPlay 在构造时随机化噪声与三角初相**（`OPT_RANDOMIZE_NOISE` / `OPT_RANDOMIZE_TRI` 默认为 1）：同一位置两次渲染结果不同，任何比例断言都无法稳定复现——同一文件曾跑出 0.862 与 0.436 两个比值。两个选项必须关掉，否则"可重复"这个前提不成立。
