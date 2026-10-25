# NSF 播放分支评审

**评审对象**：`master` 上的 `17c84cc`（`feat/nsf-playback` 的 33 个提交，50 文件 +2953/−138）
**评审分支**：`review/nsf-playback`
**依据**：`.codebuddy/rules/code-quality.md`
**方法**：读 diff 与关键函数，对照规则逐条查。不评价已经过用户验收的视觉改动。

## 结论

核心逻辑（扫描、映射、序列生成）可读、有断言、注释讲清了取舍。**问题集中在新增的并发面**——后台扫描把一整套单线程代码变成了多线程，但共享状态里有一半还不是原子的。

阻塞级 4 条，全部与"线程能看到什么、什么时候能看到"有关；建议级 4 条；观察 5 条。没有发现逻辑错误，也没有发现规则里"颜色/表驱动/可测试"那几条的硬违反。

---

## 一、阻塞级

### P1-1 线程在构造函数中间启动

**位置**：`NsfBackend.cpp`，`NsfSongInstance` 构造函数

```cpp
detectUndeclaredChip ();
scanner = std::thread ([this] { runScan (); });   // ← 这里
info_.formatName = "NSF (NSFPlay core)";
tracks_.push_back (...);                          // 可能抛 bad_alloc
```

**问题**：线程在对象还没构造完时就开始。如果构造函数的**后半段**抛异常（`push_back` 分配失败、`juce::String` 构造失败），析构函数不会执行——对象没构造成功——而线程已经拿着 `this` 在跑 `runScan()`，访问 `file`、`streamMutex`、`midiSequence`。

**规则依据**：第 5 条，不留技术债。这是"能工作但错了"的典型：正常路径永远碰不到，崩溃时表现为随机的堆破坏。

**建议**：把这一行移到构造函数最后一行。`runScan()` 只读 `file`、只碰 `cancelScan`/`streamMutex`/`midiSequence`，这几个成员都在 `scanner` 之前声明，构造末尾启动没有任何依赖缺失。

**代价**：一行位置的移动，零行为变化。

---

### P1-2 `retiredStreams` 是死代码

**位置**：`NsfBackend.cpp`，`runScan()` 与成员声明

```cpp
if (midiSequence != nullptr)
{
    retiredStreams.push_back (midiSequence);
}
midiSequence = std::move (finished);
```

**问题**：`runScan()` 每个实例只跑一次，`midiSequence` 的初值是 `nullptr`，所以 `if (midiSequence != nullptr)` **永远为假**，`retiredStreams` 永远是空容器。

注释说"Every stream this instance has ever handed out"和"The pointer may be replaced under the caller"——**描述的场景不存在**。

**规则依据**：第 5 条，删掉不再被引用的代码；第 2 条，注释必须与实现一致。

**建议**：删掉 `retiredStreams` 及其分支，把注释改成事实：

> 本实例只发布一次，从 null 变成非 null，所以交给引擎的裸指针终身有效。

**代价**：删 8 行。如果将来真要热替换（例如循环点重扫），那时再加保活，且用 `shared_ptr` 而不是裸指针约定。

---

### P1-3 扫描失败会让"分析中"永久亮着

**位置**：`NsfBackend.cpp` `runScan()`、`isAnalysing()`

```cpp
auto finished = probe.shareMidiSequence ();
if (finished == nullptr) { return; }        // ← 没有任何人知道它结束了
```

```cpp
bool isAnalysing (void) const override { return midiSequence == nullptr; }
```

**问题**：`isAnalysing()` 用"序列是否存在"推断"是否还在扫"。扫描**没产出结果**（被取消、没有音符、`scanSong` 提前返回）时两者都成立，于是状态永远停在"分析中"：

- 测试里 `waitForAnalysis(30000)` 会实等 30 秒再失败，错误信息是"did not finish"，掩盖真正原因；
- UI 上 DPCM 行永远显示"分析中"。

**规则依据**：第 5 条；第 6 条，状态要能被断言，而不是从别的字段反推。

**建议**：加一个显式的结束标志，成功失败都设：

```cpp
std::atomic<bool> scanFinished { false };
// runScan 末尾（return 之前、正常路径之后都算）
scanFinished.store (true, std::memory_order_release);
```

`isAnalysing()` 改为 `return ! scanFinished.load ();`。

**代价**：约 10 行。顺带让 `waitForAnalysis` 的失败信息能区分"超时"和"扫不出东西"。

---

### P1-4 `analysing` 与 `scanning` 是非原子的跨线程状态

**位置**：`NsfPlayer.h:224`（`bool analysing`）、`NsfPlayer.cpp:145/176/182/515/837`

| 字段 | 谁写 | 谁读 |
|---|---|---|
| `analysing` | `start()`（消息线程）、`adoptScan()`（消息线程 / **音频线程**） | `render()`（**音频线程**）、`captureState()`（**音频线程**） |
| `scanning` | `scanSampleChannels()`（**扫描线程**） | `render()`（**音频线程**） |

**问题**：两个 `bool` 都在至少两条线程上被读写，没有原子性也没有同步。C++ 上这是数据竞争，属于未定义行为。

它现在"看起来能工作"，是因为 `bool` 的读写通常是单条指令、编译器没有把它优化进死循环。但这正是最难查的一类缺陷：换编译器版本、开更高优化、加一个日志就可能变。

**规则依据**：第 6 条的前提——能被可靠地断言的前提是状态本身没有竞争。

**建议**：两个都改成 `std::atomic<bool>`，`captureState` 里用 `analysing.load (std::memory_order_relaxed)`。开销可忽略：音频线程每 block 读一次，`relaxed` 在 x86 上就是普通读。

**代价**：改动 6 处读写，2 个字段类型。

---

## 二、建议级

### P2-1 音频线程在写引擎成员

**位置**：`PlaybackEngine.cpp:1533-1539`

```cpp
if (midiSequence == nullptr && song != nullptr)
{
    if (const auto* arrived = song->getMidiSequence (); arrived != nullptr)
    {
        midiSequence = arrived;
        midiCursor = 0;
        midiResetGeneration.fetch_add (1);
    }
}
```

**问题**：`render()` 不持 `lock`（只有 `followDawTransport` 可能短暂持锁）。`midiSequence` 的其它写入点——`activateLoad`（L1113）与卸载（L1234）——都在消息线程。两者之间没有同步。

正常路径下 `playing` 会被先置 false，音频线程提前返回，窗口很小；但"很小"和"没有"是两回事，而这类竞争一旦触发就是随机的指针错乱。

**规则依据**：第 3 条，谁拥有状态谁负责它。

**建议**：这段拾取放进 `lock` 保护，用 `ScopedTryLock`——拿不到就跳过这个 block，93 ms 后还会再来。而不是 `ScopedLock`（音频线程不能等）。

**代价**：3 行。

---

### P2-2 缓存按项数限制，内存无界

**位置**：`NsfPlayer.cpp`，`kSampleSlotCacheLimit = 16`

**问题**：一条 `MidiSequence` 视曲长有 1–5 万个事件，单个事件约 40 字节，即 **0.4–2 MB**。16 项 = 6–32 MB，且是**全清**而不是淘汰最旧。

多曲 NSF（同一 ROM、不同 `startSong`，key 各不相同）逐首听一遍，会反复填满 → 全清 → 重新扫。

**规则依据**：第 5 条，已知缺陷不要留着。

**建议**：只保留最近 4 首（按插入顺序覆盖最旧），或按总事件数封顶（如 50 万事件）。前者更简单，也不会让"切回上一首"变慢。

**代价**：约 20 行（`std::list` + `unordered_map`，或记录每项的插入序号）。

---

### P2-3 十三个播放器各自一套仿真，靠全局缓存交换结果

**位置**：`NsfBackend.cpp` 的 `NsfTrackPlayer`（每轨一个 `NsfPlayer`）与 `NsfPlayer.cpp` 的静态 `sampleSlotCache`

**问题**：一个曲子级的事实（扫描结果）在 8–13 个互相不知道对方存在的实例之间，通过**文件级静态变量**传递。

这是上一轮为"打开即可播放"付出的代价，方向对；但它是设计债：三个 `NsfPlayer` 会同时启动扫描（已被 `scanSong` 开头的查重挡住），13 个 `start()` 会各自等一次全局锁。

**规则依据**：第 3 条，领域逻辑不靠全局状态通信。

**建议**：本轮不改。写进文档作为已知结构，等下一次动 NSF 后端时考虑把扫描结果提升到 `NsfSongInstance`，由构造参数注入给 track player。

**代价**：改动大，且需要重新验证"缓存命中即跳过扫描"这条断言。

---

### P2-4 `runScan` 没有异常出口

**位置**：`NsfBackend.cpp`，`runScan()`

**问题**：线程函数里任何异常都会走到 `std::terminate`，整个进程消失。`NsfPlayer` 的构造与 `scanSong` 内部有分配，理论上可抛。

**规则依据**：第 5 条。

**建议**：函数体包一层 `try/catch (...)`，捕获后设 P1-3 引入的 `scanFinished`，让界面停在"分析中"而不是让进程退出。

**代价**：5 行。

---

## 三、观察（不建议本轮改）

| # | 位置 | 说明 |
|---|---|---|
| O-1 | `NsfPlayer.cpp:515` | `(rendered % 4096) < frames` 依赖 `frames` 小于 4096 且不跨界。能工作，但读的人要停下来算一遍。可改为 `rendered / 4096 != (rendered + frames) / 4096`。 |
| O-2 | `NsfBackend.cpp` | `dmcNames[]` + `addSampleTrack()` 只表达了一半："哪些叫这个名字"在表里，"哪两个是采样通道"在两次调用里。改成 `{name, capability}` 数组更贴合第 4 条。 |
| O-3 | `NsfBackend.cpp` | 那段 "Every chip above has a row here or it does not exist…" 的注释放在 `default: break;` **之前**，读起来像在解释 `default` 该做什么。它解释的是"不许漏 case"，应贴在上一个 `case` 之后或挪到 switch 上方。 |
| O-4 | `NsfPlayer.cpp:145` | `start()` 先置 `analysing = true`，`adoptScan()` 在缓存未命中时又置一次。重复但无害；删掉前者会丢掉"缓存命中时不会先闪一下"的意图，保留更好。 |
| O-5 | `NsfBackend.cpp` | `probe.skipPrescan ()` 让 DPCM 探测不再触发一次全曲扫描，是好改动。注释 "only the write counters are read below" 准确。 |

---

## 四、规则符合性

| 规则 | 结论 |
|---|---|
| 1. 名字说明意图 | 通过。`adoptScan` / `retireStreams` / `scanFinished` 这类命名都能直接读出意图 |
| 2. 注释写为什么、只写一次 | **一处不符**：`retiredStreams` 的两段注释描述了一个不存在的场景（P1-2） |
| 3. 高内聚、低耦合 | **一处存疑**：曲子级事实经全局静态缓存跨实例传递（P2-3，本轮不改） |
| 4. 表驱动优于分支 | 基本通过。芯片清单是 `switch`，属既有结构；`dmcNames` 半个表（O-2） |
| 5. 不留技术债 | **四处不符**：P1-1、P1-2、P1-3、P2-2 |
| 6. 可测试 | 通过。扫描、映射、序列都有断言，VRC6 的 A4/A3 是逐音比对而非计数 |
| 7. 颜色走主题 | 通过。本轮 UI 改动的颜色全部来自 `theme::`，`Icons.cpp` 的 `contrasting()` 是从传入色派生 |
| 9. 重构不夹带行为 | **有一处**：提交 `26c3f97`（N163 芯片卡）同时改了 `[3d]` 的断言范围（"只有发声的行才需要形状"）。理由写在提交里，但标题是 fix，属于可接受 |
| 10. 跨型号验证 | 通过。VRC6 / VRC7 / FDS / MMC5 / S5B / N163 各有资源与用例 |

---

## 五、待你决策

| 编号 | 问题 | 建议 | 工作量 |
|---|---|---|---|
| P1-1 | 线程在构造函数中间启动 | 移到末尾 | 1 行移动 |
| P1-2 | `retiredStreams` 死代码 | 删除，改正注释 | 删 8 行 |
| P1-3 | 扫描失败永久"分析中" | 加 `scanFinished` 原子标志 | ~10 行 |
| P1-4 | `analysing`/`scanning` 非原子 | 改 `std::atomic<bool>` | 6 处 |
| P2-1 | 音频线程写 `midiSequence` | 加 `ScopedTryLock` | 3 行 |
| P2-2 | 缓存内存无界 | 保留最近 4 首 | ~20 行 |
| P2-3 | 全局缓存跨实例通信 | 本轮不改，记入文档 | — |
| P2-4 | `runScan` 无异常出口 | `try/catch` + 置标志 | 5 行 |

**如果只做四条**：P1-1、P1-3、P1-4、P2-1。前两条是"错了会崩/会永久错"，后两条是并发正确性的底线，合计约 30 行。

**P1-2 和 P2-2** 属于清理与容量，可以合成一个"缓存整理"提交。
