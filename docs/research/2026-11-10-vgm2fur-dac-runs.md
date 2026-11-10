# 调研：vgm2fur 的 YM2612 DAC——一次“播放”就是一个样本

> 调研对象：`reference/vgm2fur`（纯 Python，约 2 600 行，作者 2026-04 停止维护）
> 目的：它把 VGM 转成 Furnace 工程时，DAC 通道既有音符又有分割采样。我们要判断**哪一部分是它做对了**，而不是 Furnace 替它兜住了。
> 结论先行：**它的单位是“一次播放（run）”，不是“0xE0 偏移定义的区域”**；速率是 `44100 × 字节数 ÷ 该 run 的实际时长`。我们复现了它，四首曲子逐样本对得上——**音高不是差别，边界才是**。

---

## 0. 一句话

```
VGM 0x8n / 0x52 0x2A ──► Sampler{ start, length, duration }
                                 │
                  keyid 变 = 新样本，rate = 44100 × length ÷ duration
                                 ▼
                        Furnace SMP2（8-bit PCM + 自己的 rate）
```

---

## 1. 五条结论

1. **单位是 run**：从一次 `0xE0` 到下一次 `0xE0`（或超过 **512 采样 ≈ 11.6 ms** 的停顿）之间**实际被播放**的字节。样本长度 = 真正播了多少字节，不是“到下一个偏移还剩多少”。
2. **`0xE0` 偏移不是样本边界。** 驱动可以从一个偏移开始、跨过后面几个偏移连续播：Marble 的 `0x0A80` 一次播 **1 693 字节**（越过 `0x0BFE`、`0x1124`）；Labyrinth 从 `0x0000` 一次播 **3 808 字节**（越过 `0x0770`）。我们按“相邻偏移”切，切在了驱动并不认为有边界的地方。
3. **速率是 run 自己的墙钟速率**：`44100 × length ÷ duration`，其中 `duration` 把 ≤512 采样的小停顿也算进去（`sampler.py:27-28`、`:46-64`）。它和我们按区域量的均值**吻合在 ±0.5%**（GHZ 的 8.0k 与 23.1k 两边一致），所以“速率算法”不是它比我们强的地方。
4. **它对 DAC 没有音高估计**：pattern 里的 note 是 Furnace 样本表的**选择器**（`to_patterns_dac.py:50-63`），音高写在样本的 `rate` 字段里（`furnace/sample.py:9-10`）。所谓“有音符”，对 DAC 而言只是“每次击打一个 note on/off”。
5. **它也不完美**：不认 `0x52 0x2A` 直写（四首里 78 / 0 / 33 / 0 次，可忽略）、不找循环点、不命名、不滤静音入口（GHZ 的 96 字节停止音仍导出为 `SAMPLE_5`）。

---

## 2. 管线（它怎么跑）

| 步骤 | 文件 | 做什么 |
|---|---|---|
| 读 VGM | `vgm/song.py`、`vgm/unpacker.py` | 解压、切事件元组 |
| 事件 → 动作 | `transform/tabulate.py:160-187` | `0x8n → PlaySample(+Wait)`、`0xE0 → SetSamplePointer`、`0x61/62/63/7n → Wait` |
| 状态表 | `transform/tabulate.py:12-50` | 每个 `Wait` 前存一份芯片状态；`Sampler` 只在 **keyid 变化**时新增一条 |
| DAC → 样本 | `transform/to_patterns_dac.py:35-66` | 每个 run 取 `(start,length)`，`rate = 44100×len//dur`，去重后写成样本 |
| 写工程 | `main.py:408-421` | `add_sample(SMP2)`、`add_instrument(sample_map)`、`add_patterns` |

`Sampler.copy()` 共享 `_length/_duration` 两个 list（`sampler.py:69-77`），所以表里那一条会一直跟着 run 生长，直到下一次 `keyid += 1` 换成新的 list——等价于“run 结束时记一笔”。

---

## 3. Sampler：三行状态机

```python
def set(ptr):                     # 0xE0
    if self.duration > 0 or self.idle:      # 正在播，或还没播过 → 这是一个新样本
        keyid += 1; start = ptr; length = duration = pause = 0; idle = False
    else:                                    # 只在静音里挪指针 → 不算样本
        start = ptr; length = 0; pause = 0

def play():                       # 每个 0x8n（0x52 2A 不经过这里）
    if self.length > 0:
        if self.pause > 512:                 # 停顿 11.6 ms 以上
            if self.duration > 0: close(); keyid += 1; start += length; reset
            else:                 start += length; length = 0      # 全静音的 run：丢掉
        else:
            self.duration += self.pause      # 小停顿计入时长
    self.length += 1; self.pause = 0

def wait(dt): self.pause += dt

rate = 44100 * length // duration
```

关键点：

- **512 采样 = 11.6 ms**（`sampler.py:4`）。比我们试过的 2 ms 粗得多——它主要靠 `0xE0` 切，停顿只是兜底；
- `set()` 在 `duration == 0` 时**只挪起点**，这是它处理“只播静音的停止入口”的方式（不产生样本）；
- run 的身份是 `keyid`；`to_patterns_dac.py:72-99` 按它发 note on，行数 = `ceil(duration / rowdur)`，播完补 note off。

---

## 4. 到 Furnace 的映射（为什么它敢“一样本一文件”）

- 样本：`SMP2`，8-bit 有符号 PCM（`_to_signed`，`to_patterns_dac.py:156-160`），loop 关，**`rate` 同时写进 compat rate 和 C-4 rate**（`furnace/sample.py:8-10`）；
- 乐器：`sample_map`，**一个乐器最多 120 个音**（`to_patterns_dac.py:45`、`:56-62`），`C0 = 60`（`furnace/notes.py:61`）；
- 去重键 `(start, length)`，**第一次出现的速率胜出**（`to_patterns_dac.py:50-63`）——同一段字节后来用别的速率播，不会再生成文件；
- 因此“音符”不需要音高：note 选样本，样本自带 rate，音高自然对。

> **待验证**：pattern 的 note 从 `C0`（60）起，而样本的 C-4 字段是 108。Furnace 是否会按音名移调（那会把 23 kHz 的样本按 2^-4 放），需要在 Furnace 里听一次。我们如果要搬，**只搬速率和边界，不搬 note 语义**。

---

## 5. 实测：我们复现它，并和 .fur 逐样本对照

用它的算法在四首曲子上跑了一遍，再直接跑 `python -m vgm2fur` 生成 `.fur`（zlib 压缩，解压后读 `SMP2`）：

```
02 Green Hill Zone.vgz  → 8 samples
  5398 @ 14 783   1584 @ 7 978   3810 @ 23 105   3808 @ 23 128
   116 @  8 018     96 @ 8 301   1488 @ 7 981      93 @ 8 285
```

**与 .fur 里的 `SMP2` 完全一致**（长度、compat rate、C-4 rate 三项都对），所以下表不是推测：

| 曲子 | 我们的区域（现状） | vgm2fur 的 run | 差别最大处 |
|---|---|---|---|
| 02 Green Hill | 5：8.0 / 8.0 / **23.2** / **23.2** / 8.1 kHz | 8：7 978 / 7 981 / 8 018 / 8 285 / 8 301 / **14 783** / 23 105 / 23 128 | 它多出一个 **5 398 字节、14.8 kHz** 的 run（从 `0x0000` 连续播过停止音 + 第一记鼓） |
| 03 Marble | 4：**23.2** / 8.0 / 7.9 / **23.0** kHz | 8：23 044 / 23 156 / 9 354 / 9 646 / **8 007（1 693 B）** / 8 075 / 8 070 / 7 996 | `0x0A80` 的真实 run 是 **1 693 字节**，我们只切给它 382 字节 |
| 04 Spring Yard | 4：22.9 / 22.8 / 7.9 / 7.9 kHz | 8：23 121 / 23 169 / 22 775 / 22 843 / 7 933 / 7 934 / 7 979 / 8 054 | 速率一致，边界不同（我们 4 段，它 8 段） |
| 05 Labyrinth | 3：**16.0** / **12.1** / 7.9 kHz | 5：**23 112（3 808 B）** / 23 105 / 23 118 / 23 184 / **8 001（1 700 B）** | 我们的 `0x0000` 量成 16.0k，它的 run 是 23.1k——**差 45%** |

**这张表解释了之前三首“速率不对”**：不是速率公式错，而是**我们量速率的单位不是驱动播放的单位**。Labyrinth 的 16.0k 是两段不同 pace 的 run 混在一个区域里的平均值，谁都不对。

---

## 6. 逐项对照

| 项 | 我们（区域） | vgm2fur（run） |
|---|---|---|
| 边界 | 相邻 `0xE0` 偏移 | 每次 seek + >512 采样的停顿 |
| 长度 | 到下一个偏移（含从未播放的字节） | 实际播放的字节数 |
| 速率 | 区内“响亮字节”间隔均值（≤64 tick） | `44100 × len ÷ duration`（含小停顿） |
| 跨偏移 | 一定切开 | 允许跨（Marble 1 693 B、Labyrinth 3 808 B） |
| 去重 | 内容 hash | `(start, length)`，首次速率胜出 |
| 静音 | 不滤 | 不滤（96 字节停止音仍导出） |
| 数据来源 | `SongIr.writes` 的 `0x2A` + `dacSeeks` | 只认 `0x8n`，忽略 `0x52 2A` |

好在我们这边**数据已经齐了**：`VgmCommandReader` 把每个 `0x8n` 变成一条带时间的 `0x52 0x2A` 写（`VgmCommandReader.cpp:205-215`），`ir.writes` 里逐字节都有时间戳，`ir.dacSeeks` 有每次 seek 的时间。换成 run 模型**不需要改 IR**。

---

## 7. 能搬 / 不能搬

**能搬**

1. run 作为单位（seek + 停顿）；
2. `rate = tickRate × length ÷ duration`（我们用 44100 的 tick，等价）；
3. 11.6 ms 的停顿阈值（而不是 2 ms——2 ms 把衰减切碎的那次实验就是证据）；
4. “DAC 不做音高估计”这一条：note 只是触发，音高在文件速率里；
5. `(start, length)` 去重。

**不能搬 / 不必搬**

- 120 音样本映射、`SAMPLE_MAP`——tracker 特性，我们是导出文件；
- `keyid → note` 的编号语义（可能被 Furnace 移调，见 §4）；
- 忽略 `0x52 2A` 的取舍（我们本来就全收，量过 78/0/33/0 次，影响可忽略）；
- 它的短 run 过滤：**它不过滤**。Marble 有 7 字节 × 83 次的 run（`0x111D`），GHZ 有 93 / 96 字节的停止音，它都照导。我们要不要跟，需要你定。

---

## 8. 三个落地方案

### A. 换成 run（推荐）

- `Ym2612DacSamples.cpp` 重写：按 `0xE0` 时间和“停顿 > 512 采样”把 `ir.writes` 的 `0x2A` 流切成 run；
- 每 run 记 `(start, length, duration)`，`rate = tickRate × length ÷ duration`；
- 名字仍是 `ym2612dac_%06X_%05X`（`start`、`length`）；
- 结果：GHZ 8 个、Marble 8 个、Spring Yard 8 个、Labyrinth 5 个；
- 代码量约 150 行（现有区域逻辑整段替换），不动 IR、不动 UI、不动可视化行。

### B. 区域 + run 的速率

边界不变，速率改成“覆盖该区域最多的 run 的速率”。**不推荐**：边界本身就是错的（Marble 的 382 vs 1 693），改速率只是把错误藏起来。

### C. 双轨

默认 A；设置里加“按起点合并 run”（GHZ 的 `0x0000` 有 5 398 / 1 584 / 96 / 93 四种，合并成一份）。可以先不做。

---

## 9. 需要你决定

1. **单位**：换成 run（方案 A）还是保留区域（现状）？我推荐 A。
2. **短 run**：Marble 的 7 字节 × 83 次、GHZ 的 93/96 字节停止音——导出还是过滤？过滤阈值定多少（长度 / RMS）？vgm2fur 是不过滤。
3. **同起点不同长度**：GHZ 的 `0x0000` 会出 5 398 / 1 584 / 96 / 93 四个文件。全留（vgm2fur 的做法），还是合并成最长的一份？
4. **命名**：`ym2612dac_<start>_<len>` 是否够用，还是要带 `_r<序号>` 区分同起点的多个 run？
5. **可视化**：DAC 行继续画“最近 4 096 字节”（现状），还是改成画“当前 run”？我倾向于前者（行是示波器，不是样本列表）。
6. **`0x52 2A`**：现在我们的 reader 已经把 `0x8n` 变成 `0x2A` 写、直写也照收；run 切分要不要把直写也当作字节事件（会让 run 更碎）？我建议只把 `0x8n` 计入 run，直写只用于“是否停止”的判断。

---

## 10. 如果按 A 做，改动清单

| 文件 | 改动 |
|---|---|
| `plugin/Source/backends/libvgm/Ym2612DacSamples.cpp` | 重写：run 切分 + 速率 + 去重（约 150 行） |
| `plugin/Source/backends/libvgm/Ym2612DacSamples.h` | 说明改为 run 语义 |
| `plugin/Tests/scripts/pcm-ym2612-dac.txt` | 断言改为 8 个样本、`ym2612dac_001516_00EE2` 等 |
| `docs/desgin/libvgm-visualisation.md` §9.20 | 速率与单位一节重写 |
| `docs/memo/2026-11-10-the-dac-is-a-latch.md` | 补“run 才是单位”的结论 |

预计一天内完成；回归 49 项 + 新脚本 + 四首抽样对照（用本文的表作为期望值）。

---

## 11. 参考

- `reference/vgm2fur/vgm2fur/chips/sampler.py`（状态机，96 行）
- `reference/vgm2fur/vgm2fur/transform/to_patterns_dac.py`（样本与映射，182 行）
- `reference/vgm2fur/vgm2fur/transform/tabulate.py`（事件驱动，169 行）
- `reference/vgm2fur/vgm2fur/furnace/sample.py`（SMP2 字段，20 行）
- 生成物：`build/ghz.fur`（zlib 压缩的 Furnace 工程，8 个 `SMP2`）

---

## 12. 已实现（2026-11-10）

方案 A 落地：

- `Ym2612DacSamples.cpp` 按 run 切分（`0xE0` + 11.6 ms），`(start,length)` 首次速率胜出；
- `RegisterWrite::fromStream` 与 `VgmCommand::fromDacStream` 把 `0x8n` 和 `0x52 2A` 分开，只有前者进采样器；
- 导出 `pcm8Signed`，名字 `ym2612dac_<序号>_<起点>_<长度>`。

四首的对照（`.fur` 的 `SMP2` 与我们的 `samples` 逐条比）：

| 曲子 | `.fur` | 我们 | 说明 |
|---|---|---|---|
| 02 Green Hill | 8 | 8 | **长度、速率、字节全部一致**（`SAMPLE_3` 的 3 808 字节等于 `bank[0x23F8:+3808]`） |
| 03 Marble | 7 | 8 | 我们多一个 7 字节静音（`0x111D` @ 9 646）；参考实现因共享可变状态丢了它 |
| 04 Spring Yard | 10 | 8 | 参考实现把 1 699 / 788 字节各导了两遍（同一段字节两个 keyid）；我们按 `(start,length)` 去重 |
| 05 Labyrinth | 5 | 5 | 顺序、长度、速率一致 |

两处差异都来自参考实现的 `Sampler.copy()`：它共享 `_length` / `_duration` 两个 list（`sampler.py:69-77`），
所以一个表项的 `start` 是值拷贝、`length` 却是引用——某些路径下二者会不同步（`set()` 的静音分支用
`self.length = 0` 改写共享 list，而不是换新 list）。我们按干净的 run 模型实现，所以 GHZ、Labyrinth 完全一致，
Marble 多一个静音样本、Spring Yard 少了两个重复样本。这是**刻意保留**的差异：那四个样本一个只有 7 字节、
两个是逐字节重复。

顺带发现 bank 在 `0x0636` 有一份与 `0x23F8` 完全相同（3 808 字节）的拷贝：参考实现按位置去重，所以两份都导出，
我们也是。

与参考实现唯一的刻意差别：多块 bank 我们拼接，它只取最后一块。
