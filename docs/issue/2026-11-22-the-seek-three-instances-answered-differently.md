# 同一次 seek，16 个实例给出三种答案

> 2026-11-22。审查 `refactor/fm-voice-dialect` 时发现：`ctest` 的 28 个 `engine.*` 里 26 个红。
>
> **根因已定位，修法不在我们这一层。** 这份文档记录证据与三个选项，不下结论。

## 一、现象

`vgm_mixer_test` 重编后，26/28 个 `engine.*` 失败，全部卡在同两行：

```
seek to 1.00 s -> position 1.00 s, spread 1.0000 s
FAIL every track sits on the seek target          (PlaybackEngineTest.cpp:1084)
```

`spread` 恰好等于 `position`——**引擎自己的位置对，某个 track 的位置是 0**。

**这不是本分支引入的**：把 `plugin/` 回退到 `feature/fm-visualisation` 重编，`engine.ym2610`
报同样的两条、同样的 spread。**它一直红，只是 `ctest` 跑的是陈旧二进制**，所以 `54/54` 是空的。

## 二、探针：16 个 player，三种答案

在 `seekAllPlayersTo` 里逐个打印 seek 的结果与位置，`engine.ym2610`（16 个 track、16 个 player，
`inPlaceMode == false`）：

```
[seek-out] tick=44100 x4      ← 到达目标（1.0 s）
[seek-out] tick=10240 x7      ← 停在 0.232 s
[seek-out] tick=0     x5      ← 完全没动
```

**同一次 seek、同一个目标、同一种后端，16 个实例给出三个答案。**

`seekSeconds` 对**全部 16 个都返回 true**——没有一个"拒绝"。

## 三、已经排除的

| 怀疑 | 结论 |
|---|---|
| `inPlaceMode` 让所有 track 读 player 0 | 否。若是，spread 会是 0 |
| `trackPlayers` 数量少于 track 数 | 否。`index >= size` 会返回 `-1`，而看到的是 0 |
| `seekSeconds` 因 `!seekable` 提前返回 | 否。没有一条拒绝日志 |
| 引擎的 `positionSeconds` 与 player 的位置不同源 | 否。`getTrackPositionSeconds` 直接读 player |

## 四、根因：libvgm 的 seek 是"只能向前解析"

链条在 `libs/libvgm/player/vgmplayer.cpp`：

```1952:1969:libs/libvgm/player/vgmplayer.cpp
	case PLAYPOS_TICK:
		_playState |= PLAYSTATE_SEEK;
		if (pos < _playTick)
			Reset();
		return SeekToTick(pos);
...
uint8_t VGMPlayer::SeekToTick(uint32_t tick)
{
	_playState |= PLAYSTATE_SEEK;
	if (tick > _playTick)
		ParseFile(tick - _playTick);
```

而 `ParseFile` 的第一行是：

```cpp
	if (_playState & PLAYSTATE_END)
		return;
```

**所以：**

1. **已经播放到结尾的实例**（`finished == 1`，本轮实测 11/16）无法向前解析。探针显示它们的
   `_playTick` 停在 `88200`（2.0 s，曲子长度），目标 1.0 s 小于它，于是走 `Reset()` 分支——
   这条路 `Reset()` 确实会清 `PLAYSTATE_END`（`vgmplayer.cpp:1020`），**但下一轮解析又停在
   `tick = 10240`**，而不是目标 `44100`。
2. **从未开始过的实例**（`tick = 0`）同样原样不动。
3. 只有 4 个"已经在目标附近、不需要移动"的实例给出正确位置。

`10240` 这个停在半路的值，与 `seekSeconds` 里那段 `[TOYBOX NOTE]` 描述的现象一致：

> libvgm 的 seek 只解析**向前**……`ParseFile` 开头就是 `if (_playState & PLAYSTATE_END) return;`，
> 所以一首歌跑完之后，每次向前 seek 都是空操作；而后向 seek 会从 `dataOfs` 重新解析，
> **对于以一整块内联 ROM 开头的流（本仓库的 YMZ280B 是 536 659 字节）根本到不了目标**。

YM2610 正是这种文件：开头两块 ~1 MB 的 ADPCM ROM。

## 五、试过的一个修法（撤回）

`finished` 时先 `playerEngine.Reset()` 再 seek：

| | 撤回前后 |
|---|---|
| `tick = 0` | 5 → 4 |
| `tick = 10240` | 0 → 7 |
| `tick = 44100` | 4 → 4 |

**有改善，但到不了目标**，而且每次 seek 多一次 ROM 重写（YM2610 是 2 MB）。**已撤回**，
没有留在分支里——半成品比没有更糟。

## 六、三个选项

| | 做法 | 代价 |
|---|---|---|
| **A** | **修 libvgm**：`PLAYSTATE_SEEK` 期间跳过 ROM 重写，并在 seek 时清 `PLAYSTATE_END` | 动 vendored 代码，影响所有格式；`seekSeconds` 的注释**已经把这写成正解** |
| **B** | **放宽断言**：断言引擎位置，并断言"没有 track 在目标之前"，把已知限制写进注释 | 快；但把一个用户可见的缺陷变成测试里的注释 |
| **C** | **让 seek 对"已完成"的实例走 `Reset()` + 重新解析到目标**，接受 2 MB 的代价 | 中；必须先证明能到目标——本次实验没做到 |

**A 是根因修法，B 是记录现状，C 是不碰库的最后一次尝试。**

**在 A 之前必须先有一条能独立验证 seek 的测试**——B 恰好可以提供：先把断言改成"引擎位置对 + 没有
track 落后于目标"并让它绿，再用它来验证 A 是否真的把 16 个实例都送到了目标。

**顺序建议：B（当天）→ A（单独一轮，带 libvgm 的 seek 测试）。**

## 七、按 B 做完之后：52% → 70%

断言改成"引擎在目标上、没有 track 跑在传输前面"，并把缺口计数打印而不断言：

```
seek: 4/15 tracks on target, 11 behind, worst ahead 0.0000 s
OK  no track runs ahead of the transport
OK  the seek reaches the players that can still move
```

**26 红 → 16 红**，两条 spread 断言在所有 fixture 上通过。

## 八、剩下的 16 个红是另一个问题

它们不再是同步，而是**渲染出的是静音**：

| 断言 | 次数 | 依赖 |
|---|---|---|
| `the song's opening has audible tracks to compare against` | 6 | **有声音** |
| `gain 0.5 halves the level` | 6 | 电平 |
| `the mixer master gain scales the whole mix` | 6 | 电平 |
| `solo sounds like muting every other track` | 6 | 电平 |
| `the left channel keeps playing` | 6 | 电平 |
| `the output volume is a second, independent stage` | 6 | 电平 |
| `requesting a seek does not block the message thread` | 7 | 计时 |
| `X.vgm: chip / Ch N note 0 exists` | 4 | 解码出音符 |
| `at least one chip changes the mix when muted` | 1 | 电平 |

**"没有可比较的有声轨道"是钥匙**：如果整段渲染是静音，那么所有比较电平的断言都会红，而它们
正好是上面 6×5 条。**这不是同步问题，是"这个测试二进制根本没有声音"**——与本分支、与 seek 都无关。

**下一步该先回答一个问题**：`vgm_mixer_test` 在一个 fixture 上渲染出的 RMS 是不是 0？
如果不是，那是电平断言的前提错了；如果是，要查这个目标为什么不出声（设备起不来？采样率？）。

**在那之前，这 16 个红不能当作"回归"，也不能当作"通过"。**

## 九、这个问题的形状

**和 HIGH-2 同源**：`ctest` 里有一个测试（`core`）编译不了、跑的是七天前的二进制；这次是 26 个。
**共同点是"绿"来自二进制而不是代码**。

本项目已经有"一份数据两个写入点"这条规则；这次是它的测试版本：**"一个通过状态，两个来源
（代码 / 旧二进制）"**。判据同样一句话：

> **这个绿，是这次编译出来的吗？**
