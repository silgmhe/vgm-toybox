# 调研：YM2612 曲目加载变慢的来源与优化建议

> 背景：DAC 行改成“显示当前击打”之后，含 YM2612 的 VGM/VGZ 加载变慢（可接受，但能感觉到）。
> 目的：定位耗时在哪、给出优化顺序与预期收益。**本文只做测量与方案，不改代码。**

---

## 1. 结论

**慢的是“同一个表被建了 12 次”，不是表本身。**

`02 Green Hill Zone`（`ir.writes` = **459 546** 条，其中 44.7 万条是 `0x8n` 的 DAC 字节）：

| 项 | 实测 |
|---|---|
| `dac::runsFor()` 单次 | **15–19 ms**（均值 ≈ 16 ms） |
| 每次加载调用次数 | **12 次**（每个 track player 一次） |
| 表的构建合计 | **≈ 197 ms** |
| 导出器再扫一遍（同一份数据） | **≈ 15 ms** |
| **DAC 相关合计** | **≈ 212 ms** |
| 进程整体（含 JUCE/音频设备启动） | 633–773 ms |
| 无 YM2612 的曲目（Y8950）作对照 | 204 ms |

也就是说：**这 210 ms 里 180 ms 是纯重复**，另外 15 ms 是导出器的第二次扫描。

---

## 2. 为什么是 12 次

`PlaybackEngine` 按轨道建播放器（`trackPlayers`，GHZ 是 11 个通道 + 1），每个播放器在
`setReplaySource()` 里都调用 `LibvgmTrackPlayer::buildDacTable()`：

```
for (auto& player : trackPlayers)      // 12 个
    player->setReplaySource (ir, devices)   // 每个都 buildDacTable()
```

而**只有 `trackPlayers[0]` 会被 `captureChipState` 问到**（`PlaybackEngine.cpp:1630`）。其余 11 台
各建一张 460k 事件、排序后只取 229 个 `Run`，然后永远没人读。

---

## 3. 为什么单次要 16 ms

`dac::runsFor()` 现在的做法是：

1. 遍历 `ir.writes`，把 44.7 万条 `0x8n` 字节拷进一个 `std::vector<Event>`（16 B/条 ≈ **7.3 MB**）；
2. 再插入 228 条 seek；
3. `std::stable_sort` 46 万条事件；
4. 顺序喂给 Sampler。

第 1、3 步都是可以省掉的：**`ir.writes` 与 `ir.dacSeeks` 本身都已经按时间有序**，只需要双指针合并、
直接喂 Sampler，不需要中间数组，也不需要排序。

> 顺带说明：导出器在这次改动前用的是“索引走一遍”的 O(n) 写法，没有排序；现在的共享实现引入了排序，
> 所以导出器那段也从 O(n) 变成 O(n log n)——这正是它多出的 ~15 ms。

---

## 4. 优化建议（按收益排序）

### A. 一张表，建一次，共享（最大收益，−180 ms）

谁建：**引擎**。它在 `trackPlayers` 的循环之前已经持有 `shared_ptr<const SongIr>`，在那里建一次：

```
auto table = dac::buildTable (*songIr);       // runs + bank + tickRate
for (auto& player : trackPlayers)
    player->setDacTable (table);              // shared_ptr<const DacTable>
```

`ITrackPlayer` 增加一个可选的 `setDacTable()`（默认空实现）；`LibvgmTrackPlayer` 只持有
`shared_ptr<const Table>`，不再自己建。表随歌曲生命周期结束（`SongIr` 换掉时释放）。

`LibvgmSongInstance` 的导出收集器也读同一张表——这样 `collectYm2612DacSamples` 不再做第二次扫描。

### B. 去掉排序，双指针合并（单次 16 ms → 约 3 ms）

`runsFor()` 改成：

```
w = 0; s = 0;
while (w < writes.size () || s < seeks.size ())
{
    if (没写完 && (没 seek 了 || 写的时间 < seek 的时间 || (同刻 && 写在前)))
        喂 byte (writes[w]); ++w;
    else
        喂 seek (seeks[s]); ++s;
}
```

两个输入都是时间序，合并 O(n)，不分配 7 MB，也不需要 `stable_sort`。同刻的次序保持现在的规则
（seek 先于字节，其次 `streamOrder`），否则导出的 `(start,length)` 顺序会变。

### C. 导出器复用表（−15 ms）

`collectYm2612DacSamples` 只把 `Run` 映射成 `DecodedSample`（去重 + 命名 + 有符号化），不再建 bank、
不再扫写。它与 A 是同一个前提：表由引擎持有。

### D. （后续，非本次）把 44.7 万条字节从 `ir.writes` 里挪成紧凑流

`ir.writes` 现在把每个 `0x8n` 字节存成一条完整的 `RegisterWrite`（≈20 B），44.7 万条 ≈ **9 MB**，也是
IR 构建时间的一部分。可以改成一条“DAC 流”记录（偏移 + 时间差），只有需要时才展开。影响面大（解码器、
导出、测试都读 `writes`），**建议先做 A+B+C，再决定要不要动这里**。

### E. 不做：把建表推迟到键盘页首次打开

A+B 之后整个表只要 3–5 ms，为了省它引入“视图打开时才建表”的生命周期，不划算。

---

## 5. 预期结果

| 项 | 现在 | A+B+C 之后 |
|---|---|---|
| 表构建 | 12 × 16 ms ≈ 197 ms | 1 × ~3 ms |
| 导出扫描 | 15 ms | ~0（复用） |
| **DAC 合计** | **≈ 212 ms** | **≈ 3–5 ms** |
| GHZ 加载（进程总时长） | 633–773 ms | 约回到 420–560 ms |

也就是回到这次可视化改动之前的水平，甚至略好（原来导出器那次 O(n) 扫描也被省掉）。

---

## 6. 风险与验证

1. **合并顺序必须与排序一致**，否则导出的 8 个 `(start,length)` 名字/顺序会变；
   验证：`pcm-ym2612-dac.txt`（8 个样本 + 首名）与 `hitAt` 的 5 条断言。
2. **共享的生命周期**：表必须与 `SongIr` 同生共死；切歌时旧表释放，`dacWaves` 缓存一并清。
3. **12 个播放器只共享不各建**：`trackPlayers[0]` 之外的播放器拿到的表可以只是同一个 `shared_ptr`，
   不需要拷贝（它们从不被 `captureState` 问到）。
4. **多设备**：表按设备建；今天只有一台 YM2612，接口按 `device` 索引留好即可。

---

## 7. 需要你确认

1. 按 **A+B+C** 一起做？（预计把 DAC 开销从 ~210 ms 压到 ~5 ms）
2. 是否顺便把 **D**（紧凑 DAC 流）列入后续？我建议先不做，A+B+C 后如果还想再快再评估。
3. 验收口径：用 GHZ 的加载耗时（进程总时长）对比 + 现有 `pcm-ym2612-dac` 与 `hitAt` 断言，可以吗？

---

## 8. 已实现（2026-11-10）：结果

按 **A+B+C** 落地：

- `dac::Table`（tickRate + device + bank + runs）与 `dac::buildTable()`；`LibvgmSongInstance::prepare()`
  建一次，`setReplaySource()` 把 `shared_ptr` 交给每个 track player，导出器也读同一张表；
- `runsFor()` 改为**双指针合并**两个已排序输入，不再建 46 万条事件、不再排序；
- `collectYm2612DacSamples()` 接受可选表：有表就只做 `Run → DecodedSample` 的映射。

实测（同一台机器，同一首 `02 Green Hill Zone`）：

| 项 | 改前 | 改后 |
|---|---|---|
| `buildTable`（单次） | 无（12 次 `runsFor` × 16 ms） | **1.81 ms** |
| 表构建合计 | ≈ 197 ms | **1.81 ms** |
| 导出扫描 | 15 ms | ~0（复用表） |
| 进程总时长（4 次） | 633 / 773 ms | **353 / 374 / 379 / 383 ms** |
| 对照：Y8950 曲目（无 YM2612） | 198–204 ms | 198–204 ms |

GHZ 与 Y8950 之间剩下的 ~175 ms 是 **IR 本身**的成本（44.7 万条 `0x8n` 写 + 508 KB 解压），与这次
可视化改动无关，也正是 §4-D 那条“以后再说”的优化方向。

回归：`pcm-ym2612-dac` 仍是同样的 8 个名字/长度/速率（合并顺序与旧的排序一致）；`hitAt` 5 条断言通过；
harness 实测第 10 行在 0.32 s 仍是 5398 点。
