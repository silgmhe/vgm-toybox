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

### P2-3 两个播放器靠全局缓存交换一个曲子级事实

**位置**：`NsfBackend.cpp` 的 `NsfSongInstance`（扫描用的 `probe`）与 `NsfTrackPlayer`（播放用的 `player`）

**更正**：本节最初写的是"十三个播放器"，那是错的。`ISongInstance::createPlayer()` 只创建一个 `NsfTrackPlayer`，它用 `renderTracks()` 一次渲染所有轨道。真实的实例数是**两个**：扫描一个、播放一个。

**问题**：`NsfTrackPlayer` 构造时调 `player.start()`，`start()` 去查文件级静态缓存 `sampleSlotCache` 换回扫描结果。扫描完成时，结果由扫描线程写进同一份缓存，播放侧的 `render()` 每 93 ms 再看一次。

也就是说：**同一首歌的两个对象之间，唯一的数据通道是一个按文件内容寻址的全局 map**。它现在能工作（`scanSong` 开头查重，且扫描只跑一次），但"这首歌扫出过什么"这件事没有主人——`NsfSongInstance` 自己就持有扫描线程，却把结果交给了一个全局表。

**规则依据**：第 3 条，谁拥有状态谁负责它。

**建议**：见第六节 6.4，本轮已改。

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

---

## 六、评审结论与实施

### 6.1 P1-1 为什么不换成 `juce::ThreadPool`

问得对，但本轮不换，三条理由：

**一、JUCE 没有"统一的线程池"。** `juce::ThreadPool` 每个实例自己管理线程，实例之间不共享。项目里的 `loadPool` 是 `PlaybackEngine` 的成员（一个线程），`NsfSongInstance` 在层次上取不到它——中间隔着 `ISongInstance`。

**二、按实例建池等于没建。** 每首歌有两个 player（扫描一个、播放一个），每个都 `ThreadPool(1)` 就是两个池、两条线程，和现在的 `std::thread` 没有区别，还多了一层 `ThreadPoolJob` 的生命周期要管。

**三、真正的收益在别处。** 池能带来的两件事——多文件排队（不在音频线程旁边同时扫三首）、统一取消——需要一个**全局单例池**加上 `SongBackend` 的接口改造。那正是 P2-3（曲子级事实经全局静态变量跨实例传递）要一起动的东西，单独为它开一刀反而不划算。

**本轮的修法**仍是"移到构造末尾 + `try/catch`"：这是修 bug，不是换机制。换机制留给 P2-3 一起做。

### 6.2 实施

| 编号 | 做法 |
|---|---|
| P1-1 | `scanner = std::thread (...)` 移到构造函数最后一行，注释写明"构造抛异常时析构不会执行" |
| P1-2 | 删 `retiredStreams` 与那段永远为假的分支，注释改为"只发布一次，指针终身有效" |
| P1-3 | 新增 `std::atomic<bool> scanFinished`；`runScan` 无论成功、失败还是被取消都在末尾置位；`isAnalysing()` 改读它 |
| P1-4 | `analysing` 与 `scanning` 改成 `std::atomic<bool>`，读写用 `memory_order_relaxed`——这里不需要顺序，只需要不撕裂 |
| P2-1 | `PlaybackEngine::midiSequence` 改成 `std::atomic<const MidiSequence*>`，写入 `release`、读取 `acquire` |
| P2-2 | 缓存限 4 项，按插入顺序淘汰最旧（`std::deque` 记顺序），不再整体清空 |
| P2-3 | 不改，记录在案 |
| P2-4 | `runScan` 全体包 `try/catch (...)`，异常时仍置 `scanFinished` |

### 6.3 改的时候撞到的一件事

`midiSequence` 原子化之后，`renderMidi` 起初写成"先取快照，再拾取新流"：

```cpp
auto* sequence = midiSequence.load();          // 可能是 null
if (sequence == nullptr) { ... store (arrived) ... }
if (sequence == nullptr) return;               // 刚到的流，这一块用不上
```

后果是序列到达的那个 block 被整个跳过：`RPN 0`（bend range）和第一个音符一起推迟到下一个 block。在 0.05 秒的测试窗口里，那等于全丢。

**四条 NSF 用例同时报 `the bend range is declared as RPN 0 before the first note` 才发现。**改成"先拾取、再读一次"后恢复。

这和第七节 7.3 是同一类错误：**改成原子之后，"读一次就够"这个直觉不再成立**——两次读之间可以发生写入，而这段时间正好是别人交付结果的时刻。

### 6.4 P2-3 的实施：`ScanHandle`

新增 `ScanHandle`（`NsfPlayer.h`）：持有 `shared_ptr<const ScanResult>`，发布与读取都在锁内，读一次拿一份 `shared_ptr` 的拷贝。

```
NsfSongInstance
  ├─ scanHandle ──┬── 扫描线程的 probe.setScanHandle()
  │                └── createPlayer() 传给 NsfTrackPlayer → player.setScanHandle()
  └─ 构造时先问缓存：命中就填 handle、置 scanFinished，不起线程
```

`NsfPlayer::start()` 与 `render()` 都改成从 handle 取，不再查文件级表。`adoptScan(bool waitForLock)` 那个"音频线程拿不到锁就跳过"的参数随之消失——handle 返回的是 `shared_ptr` 拷贝，调用方不需要持锁。

**缓存仍然在**，但角色变了：只负责"再次打开同一个文件时不重扫"，不再承担"同一首歌两个 player 之间传递数据"。这两件事原本被同一张表承担，也是它们互相掩盖的原因。

**一个必须同时处理的细节**：命中缓存时线程不启动，`scanFinished` 就没有人置位——`isAnalysing()` 会永远返回 true，`waitForAnalysis()` 会等满超时。构造里补上 `scanFinished.store (true)` 并把 `midiSequence` 从 handle 取一次。这条被 `engine.vrc6_song` 抓到，它是唯一一个在同一次运行里加载同一文件两次的用例。
