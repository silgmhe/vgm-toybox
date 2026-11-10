# 调研：让 YM2612 的 DAC 行像其他采样行一样显示

> 背景：DAC 行现在是“最近 4096 字节的滚动窗口 + 滑动平均速率”，视觉上是在**流**；其他采样芯片的行是“当前触发的那一个样本 + 它自己的速率 + 播完保持”。用户希望 DAC 行与后者一致。
> 目的：先确认**有没有办法**、代价在哪、要动哪一层。**本文只做分析与方案，不改代码。**
> 结论先行：能做，而且不必重复写第三套边界逻辑——**用导出器已有的 Sampler 在载入时建一张“击打表”，由播放层按时间查表覆盖 DAC 行的 `wave`/`sampleRate`** 最省、也最一致。难点不在显示，而在 `0xE0` 目前根本到不了解码器。

---

## 1. 现在这一行是怎么画的

`OpnDecoder.cpp:1042-1078`：

- `dacWave` = **环形窗口 4096 字节**，每来一个字节就置脏、下一次快照重建一遍；
- `sampleRate = dacBytesPerSecond()` —— 最近 256 个间隔的**滑动均值**；
- `gate = now < dac.end`，而 `dac.end = time + kDacGate`（50 ms，`OpnDecoder.cpp:148`）——一个触发脉冲，不是样本时长。

面板据 `sampleRate > 0 && values.size() > kDisplayPoints(128)` 把这一行画成**填充带**（`KeyboardRowPaint.cpp:366`），所以它看起来已经像“录制”，但内容是**滚动**的：

| 曲子 | 一次击打 | 窗口 4096 覆盖 |
|---|---|---|
| Green Hill | 1 584–5 398 B | 一击或一击半，且每来一个字节就平移 |
| Marble | 1 693 B（82 次） | 约 2.4 击 |
| Spring Yard | 3 808 B | 约 1.1 击 |

同一行的拖拽导出也吃这份窗口（`ContentSlot` → `writeChannelShape` → `writeSampleWav`），所以**现在拖出来的是“最后 4096 字节 @ 滑动均值”**，与批量导出的 `ym2612dac_*` 不是同一份东西——这是个已经存在的、只是还没被注意到的分歧。

## 2. 其他采样行长什么样

以 C140 为例（`C140Decoder.cpp:430-465`）：

- `waveCache[(offset << 32) | length]` 懒解码，一次一击；
- `state.wave` = **当前那一个样本**的全部字节；
- `state.sampleRate` = 该样本自己的字节率；
- `state.gate` = 正在发声（不是固定脉冲）。

ADPCM-A/B 与 Y8950 同理：`end` 时间 + 缓存波形。**它们的共同点是“有一个 (起点, 长度) 可以指”**，而 DAC 现在没有。

## 3. 差的那一步：击打边界不在解码器里

导出器（`Ym2612DacSamples.cpp`）已经定义过一次击打：`0xE0`（正在播/未播）或 **≥512 采样（11.6 ms）** 的停顿。`02 Green Hill Zone` 的实测是 **229 次击打、228 次 `0xE0`、几乎不靠停顿**。

而 `0xE0` 目前只被解析成 `SongIr::dacSeeks`（导出器与 MIDI 用），`VgmReplayer` 只把**寄存器写**喂给解码器：

```
VGM ──► VgmCommandReader ──► SongIr { writes, dacSeeks }
                                        │        └── 导出器、MIDI
                                        └──► VgmReplayer ──► OpnDecoder（只看得到 0x2A 字节）
```

所以“显示当前击打”这件事，**必须先让行知道边界**。停顿可以自己算（字节时间戳在手上），但 seek 不行。

## 4. 两条路线

### A. 解码器加 `seek()`，在 `OpnDecoder` 里做 Sampler

- `IChipDecoder` 增加 `virtual void seek (uint32_t offset, uint32_t sampleTime) {}`；
- `VgmReplayer` 在喂写之前，按时间把 `song->dacSeeks` 合并进 YM2612 实例的流；
- `OpnDecoder` 复制导出器的 Sampler：seek/停顿切一次击打、`duration` 累加、按 `(start,length)` 缓存波形。

优点：与播放同源，行永远是最新；不需要另外遍历 IR。
缺点：**第三次复制“击打 + 速率”逻辑**（第一次导出器、第二次 vgm2fur 对照、第三次这里），前两次的教训就是复制会漂移；解码发生在**音频线程**（每击最多 5 398 点，可接受但仍是实时工作）。

### B. 在播放层用离线击打表覆盖（推荐）

- 载入时（`LibvgmTrackPlayer.cpp:481` 设置 `replayIr` 的地方）用**导出器同一个 Sampler** 产出：

  ```
  Hit { startSampleTime, endSampleTime, start, length, rate }
  ```

  与 `ym2612dac_*` 一一对应；
- `captureState()`（`LibvgmTrackPlayer.cpp:667`，已有 `seconds`）二分查当前时间落在哪一击，把 `snapshot.channels[dac]` 的 `wave`/`sampleRate` 换成该击的（波形懒解码 + `(start,length)` 缓存）；
- 播完保持最后一击，与其他采样行一致；`rewind`/切歌时清缓存（现已有清 `ledger.lastPlayed` 的位置）。

优点：**零重复逻辑**（边界、速率、去重全来自导出器），音频线程只做二分 + 一次 `shared_ptr` 拷贝；行与导出文件必然一致；解码器保持“描述锁存器收到了什么”的职责不变（与 §9.1 的分层一致）。
缺点：需要在 track player 里认出 DAC 行（它已经按 `sourceDeviceId`/`deviceChannel` 做这件事，用于 ADPCM）；表在载入时构建一次（GHZ 44.7 万条写，导出器本来就这么做，毫秒级）。

**推荐 B**：它把“驱动层面的表”留在播放层，正是 `vgm2fur` 的模型——note 触发、样本播放；解码器不该知道“驱动把指针挪到哪了”。

## 5. 共同的细节与风险

1. **双 YM2612（32X）**：`SongIr::dacSeeks` 没有 device id，两台会共用一张 seek 表。A、B 都受影响；要修就得给 `dacSeeks` 加 `deviceId`（VGM 的 `0xE0` 本身也只指第一颗，实际影响小，但要在文档里标注）。
2. **长度上限**：导出有 30 s 硬顶 + 设置里的 5 s；**可视化**应另有一个更小的上限（例如 4 096 点，与现在窗口一致），否则一行要画几万点。
3. **行的拖拽会变**：从“最后 4096 字节 @ 滑动均值”变成“当前击打 @ 实测速率”。这会让拖拽与批量导出对齐——大概率是想要的，但它是一个可见的行为变化，需要确认。
4. **`sampleRate` 换掉的影响**：它只影响“画成录制”和拖拽文件头，不影响音符（DAC 行是 drum，固定 C4），所以换速率不会改变行上的音名。
5. **性能**：B 的表一次构建；波形懒解码，缓存键 `(start,length)`，与 C140 的 `waveCache` 同形。
6. **没有 seek 的文件**（连续流驱动）：表退化成“按停顿切”，行为与现在接近，不会更差。
7. **暂停/停止**：其他采样行保持最后一击；DAC 行应一致，而不是清空。

## 6. 需要你决定

1. **单位**：显示**一次击打**（如 3 810 字节，与导出/`vgm2fur` 一致），还是**整个表项**（到下一个 `0xE0`，可能含从未播放的字节）？我建议前者。
2. **播完之后**：保持最后一击，还是清空？建议保持。
3. **拖拽导出**是否也跟着改成“当前击打”？建议是，但这会改变现有拖拽结果。
4. **走 B 还是 A**？我推荐 B。
5. **双 YM2612**：先按“共用一张表”并在文档标注，还是顺手给 `dacSeeks` 加 `deviceId`？
6. **可视化长度上限**：固定 4 096 点，还是跟导出设置（5 s）走？
7. **暂停/停止时**：显示最后一击，还是空白？

确认后我按 B 做：抽一条共享的 `Sampler` 给导出器与播放层（避免两份边界逻辑），track player 建表并在 `captureState` 覆盖 DAC 行，测试钉住“某一时刻显示的是哪一击”。

---

## 7. 已实现（2026-11-10）

按方案 B 落地，选择全按讨论结果（一次击打、保持、拖拽同源、上限 8 192 点）：

- 新建 `Ym2612DacSampler.{h,cpp}`：把导出器里的 Sampler 抽出来共享，`runsFor(ir, device, tickRate)`
  产出 `Run{beginTime, endTime, start, length, duration}`，`bankFor()` 产出 PCM 银行；导出器
  `Ym2612DacSamples.cpp` 改成调用它，行为不变（8 个样本、名字与速率全部一致）；
- `LibvgmSongInstance` 在 `prepare()` 里建**一次** `dac::Table`，`setReplaySource()` 把它交给每个
  track player（见 `docs/research/2026-11-10-dac-load-cost.md`）；`captureState()` 对 YM2612 的
  第 6 通道调用 `applyDacHit()`：`dac::hitAt()` 取“此刻在播或最后一击”，懒解码波形（`(start,length)`
  缓存，超过 8 192 点均匀抽样），`sampleRate` 用该击的 `tickRate × length ÷ duration`；
- 拖拽路径不变代码，因为它读的是同一份 `waveValues()`，于是自动与 `ym2612dac_*` 对齐；
- 双 YM2612：按讨论保留“一台一张表”的写法，但 `0xE0` 在格式里不指芯片，loader 也只创建一颗
  （`0x53` 是 port 1），所以现实里只有一张表，行为没有分支。

验证：`NsfFormatTest` 新增 `hitAt` 的 5 条断言（未开始 / 播放中 / 播完保持 / 下一击接管 / 末尾保持）；
harness 实测：Green Hill 播放到约 0.32 s 时 `engine.wave.10 == 5398`（第一击），改前该行永远是 4 096
点的滚动窗口。