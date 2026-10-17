# 结构盘点与三个设计问题的评审（2026-10-17）

分支 `review/2026-10`，基线 `8e760cc`。上一轮 `2026-10-16` 的债务已全部结清（`L-3` 为有意保留），所以这一轮从头看两件事：**代码现在长什么样**，以及三个设计问题——无锁队列、命令/事件模型、协程——**在这个代码库里到底值不值得引入**。

本文只给依据和判断，不改代码。文末列了五个待拍板的决定。

---

## 1. 一页结论

| 问题 | 结论 | 理由 |
|---|---|---|
| **Q1 无锁队列** | **现在不引入**；可视化页落地时用 `juce::AbstractFifo` 加一条 SPSC 流，**不引外部库** | 标量用原子已正确；换会话已做到"锁外构建 + 锁内一次安装"；真正缺队列的只有还没写的波形流 |
| **Q2 命令 / 事件总线** | **不引事件总线，不上 `ApplicationCommandManager`**；做三件小事：快捷键并入菜单表、离散事件用 `ChangeBroadcaster`、状态持久化补齐 | 菜单**已经是**命令表（`MenuEntry`），设置**已经在**广播（`ChangeBroadcaster`），15 Hz 拉模型是刻意的；缺的是持久化和一份真值，不是框架 |
| **Q3 协程** | **不做协程，先做异步加载**（`ThreadPool` + `callAsync`） | 问题在"消息线程做 I/O 和建核心"，换调用栈写法解决不了；协程要 C++20（当前 `-std:c++17`），且 JUCE 没有现成 awaitable |

自查发现（详见 §6）：

| 编号 | 问题 | 严重度 |
|---|---|---|
| **H-1** | 混音状态（推子/声像/静音/solo/路由/总线开关）和**歌曲数据本身**都没有写进 VST 状态，宿主工程重开即丢、换机器打开没歌（仅插件；Standalone 不走这条路） | H |
| **M-1** | 加载在消息线程同步执行（构造、拖放、菜单、宿主恢复），无进度、无取消 | M |
| **M-2** | `setStateInformation` 的线程假设未声明，加载失败被静默吞掉 | M |
| **M-3** | 菜单里的快捷键是"描述"，`keyPressed()` 里另有一份真值 | M |
| **M-4** | `PluginEditor.cpp`（448 行）兼五职，异步加载会再塞进一层 | M |
| **L-1** | 源码与测试里的文档路径**回归**失效 6 处（上轮修过同类） | L |
| **L-2** | 设计目录名 `desgin`（design 拼错），被 29 处写死 | L |
| **L-3** | 全仓 `// DEBT:` 标记为 0，债务实际只记在本目录 | L |

---

## 2. 事实基线

### 2.1 规模

| 目录 | 行数 | 文件 | 职责 |
|---|---:|---:|---|
| `plugin/Source/ui/` | 4156 | 37 | 视图、面板、绘制、主题、菜单 |
| `plugin/Source/playback/` | 1462 | 7 | 引擎、后端注册表、路由、后端接口 |
| `plugin/Source/backends/` | 1087 | 12 | libvgm 适配、fake 后端、vgz 解包 |
| `plugin/Source/settings/` | 236 | 2 | 全局设置（单例） |
| `plugin/Source/`（根） | 737 | 4 | `PluginProcessor` / `PluginEditor` |
| 合计 | **7678** | 62 | |
| `plugin/Tests/` | 1724 | 4 | 核心 / 引擎 / 按钮 / 装配 |
| `vgmplayer/` | 6059 | — | 遗留前端（已冻结，见 policy §1） |

最大文件：`PlaybackEngine.cpp` 840、`PluginEditor.cpp` 448、`TransportPanel.cpp` 411、`SettingsOverlay.cpp` 391、`MixerPanel.cpp` 327 —— 这一栏就是"下次该拆谁"的名单。

### 2.2 依赖方向

`ui → playback → backends → libs/libvgm`，**没有反向 include**（`backends/`、`playback/` 里搜 `#include "ui/` 命中 0）。UI 是叶子，引擎不知道 UI 存在——这条线是后面所有讨论的前提：**任何"引入事件总线"的方案，都不能把它变成双向依赖。**

### 2.3 线程与数据通路

全仓只有两个线程参与：**音频线程**（`render()`）和**消息线程**（其余一切）。没有后台线程——搜 `juce::Thread|ThreadPool|callAsync|AsyncUpdater` 命中 0，这是理解三个问题的起点。（M-1 落地后多了加载 worker 一个后台线程，它只碰自己的拷贝、结论回消息线程，见 §6 M-1。）

| 数据 | 谁写 | 谁读 | 机制 |
|---|---|---|---|
| 位置、电平峰值、`isLoaded/playing/finished` | 音频线程 | UI（15 / 10 Hz 轮询） | `std::atomic` |
| 通道条增益/声像/静音/solo/输出目标 | 消息线程 | 音频线程 | `std::atomic`（`PlaybackEngine.h:155-161`） |
| 总线连接、master 输出/静音 | 音频线程 | UI | `std::atomic` |
| 歌曲对象（播放器图） | 消息线程（加载） | 音频线程 | 锁 + `ScopedTryLock`，拿不到则静音并 `starvedBlocks++` |
| 时长、循环点、标签、芯片表、轨道表 | 消息线程 | UI | 普通成员，靠"只有消息线程碰"这条约定 |

两条已经做对的：

1. **音频线程永不阻塞**：暂停/空闲时根本不碰锁（`PlaybackEngine.cpp:741-747`）；播放中只 `ScopedTryLock`，失败就静音并计数（`:752-760`），测试断言加载不会让计数非零。
2. **加载已拆成"锁外"和"锁内"**：读文件（`:65-74`）、解压与按芯片建播放器（`:76-84`）全在锁外，锁内只剩安装与启动（`:86-112`，有界工作）。源码里的注释把这三段写成 Phase 1/2/3，说明这是有意为之，而不是碰巧。

所以"音频线程会不会被锁住"**目前不是问题**；问题在另一头——消息线程被加载占住了（§6 M-1）。

### 2.4 上一轮债务现状

`H-1/H-2/H-3/M-1..M-7/L-1..L-5` 已结清（`L-3` 有意保留）。本轮复查：`M-1` 的 `render()` 已从 220 行拆成 8 步 ✓，`M-4` 的格式文案已收进 `ui/Format.h` ✓；但 **`L-1` 回归**——源码与测试里又有 6 处指向旧路径（§6 L-1）。

---

## 3. Q1：无锁循环队列——是什么，要不要上

### 3.1 它是什么

一个**定长环形缓冲**：生产者只写 `write` 指针、消费者只写 `read` 指针，两个指针是原子量，靠 acquire/release 保证"数据先写入、指针后发布"。没有互斥量、没有系统调用、不分配内存，所以能在音频回调里用。

- **SPSC**（单生产者单消费者）最简单：一个 `head`、一个 `tail`，两者永不争同一变量——几乎等于无锁。
- **MPMC** 需要每槽序号（Vyukov 队列）或内存回收，复杂度和出错率高一档。
- 约束：容量固定（满了怎么办要自己定策略）；元素平凡可拷贝或预分配（音频线程不能 `new`）；指针与缓冲区分处不同 cache line（否则伪共享吃掉收益）。

### 3.2 业界怎么做

| 方案 | 形态 | 适合 | 代价 |
|---|---|---|---|
| `juce::AbstractFifo` | **只有索引协议**，缓冲区自己给（SPSC） | 音频 ↔ UI 数据流；JUCE 自带 | 容量、溢出策略、内存序自己定 |
| `juce::AudioVisualiserComponent` | 成品波形组件 | 直接要一个波形图 | 样式不可控；按"音频线程 push、UI 线程 paint"设计 |
| `rigtorp/SPSCQueue` | 定长 SPSC，头文件，cache line 对齐 | 想要久经考验的 SPSC | 多一个依赖；仍是 SPSC |
| `moodycamel::ReaderWriterQueue` | SPSC；`BlockingConcurrentQueue` 是 MPMC 可阻塞 | 多生产者/消费者 | 体积大；阻塞语义在音频线程是禁忌 |
| `farbot`（`RealtimeObject`） | **对象交接**：非实时线程构造、实时线程无锁接管 | 替换"锁换对象图" | 生命周期协议写错就是悬垂指针 |

`AbstractFifo` 的文档把边界说得很直白：它只提供 `prepareToWrite/finishedWrite`、`prepareToRead/finishedRead` 这套索引协议，**保证"单读者单写者"下无锁**，不替你分配、不替你决定满了丢谁。这正是我们需要的——一条协议，不是一个库。

### 3.3 这个代码库缺哪一条流

把跨线程数据分三类，答案就清楚：

| 类别 | 例子 | 现在的做法 | 该不该换队列 |
|---|---|---|---|
| **状态标量**（每帧读、值很小） | 位置、峰值、推子、静音 | 原子量 | **不换**。队列在这里更慢也更难读 |
| **一次性命令**（频率低、要改对象图） | 加载、seek、切混音模式 | 消息线程改 + 短暂持锁 | **不换**。队列解决不了"对象图要换" |
| **采样流**（每帧几百到几千样本） | 波形/频谱 | **还没有**（`VisualiserView` 是 26 行占位页） | **要**。这正是队列的用武之地 |

分档结论：

1. **现在**：保持现状。标量用原子是对的；命令稀少且有锁外构建兜底，加队列只多一层间接。
2. **可视化页落地时**：`ScopeTap` 一个类，音频线程 `push` 抽头样本，UI 定时器 `pop` 一批画上去，满了**丢最新的**。用 `AbstractFifo`，缓冲区在 `prepareToPlay` 预分配，`processBlock` 零分配。
3. **永远不做**（除非出现新证据）：为"消灭那把锁"引入 `farbot` 式对象交接。当前锁只在用户操作时持有毫秒级，音频线程拿不到就静音并计数，测试断言计数为 0。真要做，是 ~150 行生命周期协议，收益是"消掉一个从未观测到失败的 try-lock"。

### 3.4 如果做可视化，草图

```cpp
// ui/ScopeTap.h —— 音频线程 push，UI 线程 drain；不分配、不阻塞
class ScopeTap
{
public:
    void prepare (int capacity)          // prepareToPlay 里调用，之后不再分配
    {
        fifo.setTotalSize (capacity);
        ring.assign ((size_t) capacity, 0.0f);
    }

    void push (const float* samples, int n) noexcept      // 音频线程
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (n, start1, size1, start2, size2);
        std::copy (samples,         samples + size1,         ring.begin() + start1);
        std::copy (samples + size1, samples + size1 + size2, ring.begin() + start2);
        fifo.finishedWrite (size1 + size2);   // 写不满就丢最新的几个样本
    }

    int drain (float* out, int max) noexcept              // 消息线程
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead (max, start1, size1, start2, size2);
        std::copy (ring.begin() + start1, ring.begin() + start1 + size1, out);
        std::copy (ring.begin() + start2, ring.begin() + start2 + size2, out + size1);
        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> ring;
};
```

约 40 行，外加 UI 侧一个定时器。`AbstractFifo` 内部只有两个原子索引，音频侧是 `memcpy` 级成本。

### 3.5 结论

**不引入队列库**：外部库解决的是"多生产者"或"带阻塞"，两样都不需要；`AbstractFifo` 已在依赖里、行为可读、测试好写。等可视化页真要一条流，加 40 行比引一个要审许可证、审内存序、审升级路径的依赖划算。

**触发引入外部库的条件**（写下来是为了以后不用重新争论）：出现第二个生产者线程（宿主自动化 + UI 同时下发），或需要跨进程/跨 DLL 传递，或队列深度需要动态扩容。

参考：[`juce::AbstractFifo`](https://docs.juce.com/master/classjuce_1_1AbstractFifo.html)、[`juce::AudioVisualiserComponent`](https://docs.juce.com/master/classjuce_1_1AudioVisualiserComponent.html)、[rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)、[moodycamel/readerwriterqueue](https://github.com/cameron314/readerwriterqueue)、[farbot](https://github.com/hogliux/farbot)。

---

## 4. Q2：Command / Action / 事件总线——要不要

### 4.1 现状：已经有半个命令模式

| 机制 | 在哪 | 性质 |
|---|---|---|
| 菜单即数据 | `MainMenu.h:14-30`（`MenuEntry{id,label,shortcut,checked,enabled,separatorBefore,action}`） | **命令表**，`describe()` 还能给控制台测试读 |
| 设置变更广播 | `SettingsStore`（`ChangeBroadcaster`） | **观察者**，多个窗口一起换主题 |
| 状态读取 | 15 / 10 Hz 轮询（`PluginEditor.cpp:19-20`、`:454`） | **拉模型**，天然合并、不会有事件风暴 |
| 命令执行 | UI 直接调 `PlaybackEngine` 方法 | 直接调用，类型安全 |

这不是"没有架构"，而是刻意的选择：**低频离散的东西用回调/广播；连续变化的东西用轮询**。桌面音频软件普遍如此——`AudioProcessorValueTreeState` 也是参数走原子量 + 定时器/`AsyncUpdater` 合并。

### 4.2 业界几种模型，和 JUCE 提供什么

| 模型 | JUCE 对应物 | 解决什么 | 不适合什么 |
|---|---|---|---|
| 命令对象 + 目标解析 | `ApplicationCommandManager` + `ApplicationCommandTarget` | 命令集中注册、**键位可改**、菜单/按钮共用、启用状态集中 | **不提供撤销**（文档明确）；单窗口少量命令时是重炮打蚊子 |
| 观察者（带合并） | `ChangeBroadcaster` / `ChangeListener` | 设置、模式这类"变了但不必每帧"的广播 | 高频变化（会淹掉消息队列） |
| 带模型的状态树 | `ValueTree`（+ `UndoManager`） | 结构化状态、序列化、属性变更通知、撤销 | 音频线程（要加锁或走 APVTS 的原子量） |
| 宿主参数 | `AudioProcessorValueTreeState` | 宿主自动化、预设、undo | 本项目**没有**暴露任何 `AudioProcessorParameter`，用不上 |
| 全局事件总线（字符串键） | 无 | 模块极多、跨插件通信 | 本项目：编译期耦合换成运行期查找，生命周期失控，调试变难 |

`ApplicationCommandManager` 的边界值得写清楚，因为它常被当成万灵药：它管**注册、目标链、键位映射、命令元数据**，不管菜单绘制（`PopupMenu::addCommandItem()` 只是消费方），也**不管撤销**——要撤销得自己接 `UndoManager`。

### 4.3 对这个代码库的判断

**不做**的三件：

1. **不引入全局事件总线**。62 个源文件、单向依赖、两个线程，事件总线的收益（解耦）换不来什么，代价（隐式依赖、断点失效、生命周期）却是实打实的。
2. **不引入 `ApplicationCommandManager`**。现在 5 个菜单组、十余个条目、一个窗口、没有可配置键位需求，它的强项一条都用不上。
3. **不引入 APVTS**。混音推子不是宿主参数，也不该是——暴露成参数会污染宿主自动化列表。

**要做**的三件（都很小）：

1. **快捷键并入菜单表**（M-3）。`MenuEntry` 加一个 `juce::KeyPress key` 字段，`shortcut` 文本由它生成，`keyPressed()` 遍历菜单表匹配。消掉两份真值，新增命令变成一处编辑。
2. **离散事件给一个 `ChangeBroadcaster`**（可选，等有需求）。例如 `songLoaded` / `loadFailed`。现在错误由调用方直接弹提示，够用；只有"引擎自己发现的问题"（采样率变化、后端失联）需要通知 UI 时才加。
3. **状态持久化补齐**（H-1，独立于 Q2）。这才是"状态管理"的真问题：不是缺框架，是**缺字段**——`getStateInformation()` 只写了 4 个属性（`PluginProcessor.cpp:120-131`），而引擎有每条轨道的增益/声像/静音/solo/输出、混音模式、master 增益/静音、默认路由。**工程一重开，混音全丢。** 注意这是两套存储：会话状态进 VST 状态给宿主保存，主题这类用户偏好继续留在 `settings.xml`（插件与 Standalone 共用），见 §6 H-1 的边界表。

状态变多以后，`ValueTree` 是比"手写 XML 属性"更好的家：能直接序列化、有属性变更通知、能接 `UndoManager`。建议路径：**先把 XML schema 写全（v2，带版本号，旧格式不兼容），字段超过 ~15 个或需要撤销时再迁到 `ValueTree`**——迁移时 schema 已经有了，几乎只是换个容器。

### 4.4 结论

- 现在：**保持拉模型 + 直接调用**，补快捷键真值、补状态持久化。
- 触发 `ValueTree` 的条件：状态字段 > 15，或需要撤销/撤销组，或需要多窗口共享同一份可变状态。
- 触发 `ApplicationCommandManager` 的条件：命令 > 20，或需要用户自定义快捷键，或菜单之外还要工具条/右键菜单共用同一套启用状态。

---

## 5. Q3：协程——要不要用

### 5.1 现状：加载全在消息线程

| 入口 | 位置 | 行为 |
|---|---|---|
| 编辑器构造 | `PluginEditor.cpp:145` → `openStartupSong()` | 建窗口时就同步读文件、建核心 |
| 菜单/快捷键打开 | `PluginEditor.cpp:256-306` → `loadFile()` `:368-390` | 文件选择器返回后在消息线程同步加载 |
| 拖放 | 同 `loadFile()` | 同上 |
| 宿主恢复工程 | `PluginProcessor.cpp:157-161` | `setStateInformation` 里直接 `eng.loadFile()` |

实测方法在 §9；没有实测之前不下"卡多久"的断言，但**结构上是确定的**：文件读取、`.vgz` 解压、按芯片构造播放器全部发生在消息线程，其间窗口不重绘、按钮不响应。

### 5.2 业界标准做法

- **C++20 之前就已经有答案**：`juce::ThreadPool`（或 `std::thread`）做后台工作，完成后 `MessageManager::callAsync`（或 `AsyncUpdater`）回消息线程改 UI。JUCE 自带这三种积木，插件里到处这么写。
- **协程只是写法**：`co_await` 能把"回调套回调"摊平成顺序代码，但它不提供线程、不提供调度器。C++20 标准库里**没有** executor——那是 C++26 的 `std::execution`（P2300）。这意味着协程的"怎么跑"仍需自己搭。
- **生态现状**：`cppcoro` 早已停更；`libunifex` 被 `stdexec`/P2300 取代；Boost.Asio 的 awaitable 很强但为一个插件引 Asio 不划算。JUCE 本身没有 awaitable 支持。
- **本项目门槛**：编译选项是 `-std:c++17`（CMake 没有显式设置，JUCE 8 的 `juce_add_plugin` 给的是 17，`build.ninja` 里可见）。上协程＝整个插件目标升到 C++20，是一次全项目的构建变更。

### 5.3 结论：先异步加载，不碰协程

要解决的既有问题是"消息线程做 I/O"，同步回调就能解决，不需要新语法。建议的实现轮廓：

```
用户点击/拖放
  ├─ generation++          （取消上一笔）
  ├─ UI: 禁用按钮，状态栏 "Loading…"
  ├─ ThreadPool::addJob:   读字节 → 解压 → backend->buildSong()   （全在锁外）
  └─ 完成时 MessageManager::callAsync:
       ├─ generation 不匹配 → 丢弃
       ├─ 成功 → engine.install(结果)（锁内只做安装与启动）+ 状态栏复位 + refresh()
       └─ 失败 → 走现有的 openPrompt()，带上错误文本
```

改动集中在 `PluginEditor::loadFile()` 与 `PluginProcessor::setStateInformation()` 两处，引擎侧只需要把现有的 `loadFile` 拆成"构建"与"安装"两个入口（`PlaybackEngine.cpp:65-113` 的三段本来就是分开的，拆开是把私有步骤开成公开接口，不改逻辑）。工作量约 150 行（含取消与 `SafePointer` 保护），风险低。

`setStateInformation` 还要多加一手：**不要在未知线程上直接加载**。VST3 的 `setState` 没有文档保证一定在消息线程；而引擎的 `chips`、`tags`、`currentFile` 都按"只有消息线程碰"维护（`PlaybackEngine.cpp:76-79` 关于 `chips` 的注释、`:396-398` 关于 `tags` 的注释，都在说明这条约定）。安全的做法是：不在消息线程就 `callAsync` 一次，再按上面的流程加载，并保证宿主拿到状态时**不依赖它已加载完成**（状态里有歌曲字节，但恢复可以晚一拍完成，宿主不需要等它）。

### 5.4 协程的真实成本

| 成本 | 说明 |
|---|---|
| 标准升级 | 插件目标 C++17 → C++20，JUCE 与全部 TU 一起过；要重跑三平台构建 |
| 生命周期 | `co_await` 恢复时编辑器可能已被宿主销毁；每个挂起点都要 `SafePointer` 或弱引用 |
| 异常 | 协程里抛异常即 `terminate`，除非每个 `Task` 都定义 `unhandled_exception` |
| 可调试性 | 栈回溯丢失协程帧；断点跨 `co_await` 行为依编译器而异 |
| 生态 | 没有标准 executor、没有标准 `Task`；自己写等于维护一个小库 |

### 5.5 什么时候再考虑

三条**同时**满足才值得：

1. 异步流程 ≥ 3 条（加载、导出 WAV、目录扫描、更新检查……）；
2. 项目已因别的原因升到 C++20；
3. 有一套带测试的 `Task<T>` + 消息线程 awaiter（约 80 行），且团队接受它的调试代价。

在那之前，**回调 + `callAsync` 是更便宜的正确解**。

参考：[`juce::ThreadPool`](https://docs.juce.com/master/classjuce_1_1ThreadPool.html)、[`MessageManager::callAsync`](https://docs.juce.com/master/classjuce_1_1MessageManager.html)、[cppcoro（已归档）](https://github.com/lewissbaker/cppcoro)。

---

## 6. 自查发现

### H-1 混音与歌曲都没有进 VST 状态（只影响插件）

**先把边界定清楚**，这类问题以后按这张表判断"该写哪"：

| 状态 | 归属 | 存在哪 |
|---|---|---|
| **当前曲目数据本身**（字节，不只是路径）、文件名、loop 次数、淡出时长、输出音量 | 会话 | **VST 状态**（宿主工程） |
| 混音模式、每条轨道的 gain/pan/mute/solo/输出、master 增益/静音、默认路由 | 会话 | **VST 状态** |
| 主题模式，以及以后可能有的界面偏好 | 用户 | **`settings.xml`**（AppData，插件与 Standalone 共用，已是现状） |
| 启动曲 | 仅 Standalone | 命令行参数，不持久化 |

三条附则：**播放位置不进状态**——用户打开一首歌就从头播（`loadFile` 把位置归零后立即播放，这是产品行为，保持），而**恢复工程是停在开头的**，打开工程不会自己出声；`installLoaded()` 把"从哪开始"和交换放在一起，所以没有"先装好再设一次"的中间态可被音频线程看见（见 §10 第 1 条）；**主题绝不能进 VST 状态**——否则打开一个别人的旧工程会把用户当前主题改掉；**路由也不要挪进 `settings.xml`**——64 路总线怎么摆是宿主工程的事，换一台机器/换一个工程就该不同。

**为什么歌曲必须是数据而不是路径**：工程要能发给别人、在另一台机器上打开，对方没有那份文件，路径只是本机的一张便条；一个工程里还可能同时有多个 VGMToybox 实例，各放各的曲子，路径方案会让"换机器"变成"逐个找文件拷过去"。VST 没有跨实例共享 chunk 的机制，所以每个实例各存一份是唯一能保证可移植的做法——同一首曲子被两个实例加载就存两份，这个代价要接受（见下面第 8、9 条）。

- **证据**：`getStateInformation()` 只写 4 个属性（`PluginProcessor.cpp:120-131`：`file`、`loopCount`、`fadeSeconds`、`volume`），**没有任何歌曲数据**；而引擎公开的混音面有 `setTrackGain/Pan/Mute/Solo`、`setTrackOutput`、`setMixerEnabled`、`setMixerMasterGain`、`setMasterMuted`、`setMasterOutput`、`setDefaultRouting`（`PlaybackEngine.h:69-143`）。另外两侧不对称：`setStateInformation` 已经有 Standalone 早退（`PluginProcessor.cpp:137-140`），`getStateInformation` 没有。
- **影响**：宿主保存工程再打开，推子、声像、静音、路由、混音模式**全部回默认**；把工程发给别人或换一台机器，**曲子直接消失**（状态里只有一个本机路径），而多层输出正是这个插件的主打。
- **修法**：
  1. 两侧都在 Standalone 早退。现在 `getStateInformation` 仍会生成一份只有宿主才读的 blob，Standalone 把它塞进自己的 properties 里也没人读（`setStateInformation` 直接 return）。
  2. **状态格式：XML + base64，再整体 gzip**。不引入自定义二进制容器——`copyXmlToBinary` 写的是"magicXmlNumber + 长度 + 单行 XML + NUL"（`juce_AudioProcessor.cpp:938-951`），把这段原样压进 gzip 即可；读的时候先看头两个字节：`1f 8b` 是 gzip → 解压后交给 `getXmlFromBinary`，不是 gzip 头就是旧格式或损坏 → **不加载**，状态行提示一句，不尝试解析、不猜。
     - **base64 的 33% 会被外层 gzip 基本抹平**，这是它可行的关键：实测 1 MiB 不可压缩数据 `gzip(raw)` = 1 048 914、`gzip(base64)` = 1 059 094，**+0.97%**；极度可压缩的 1 MiB 文本 3 115 → 4 146，相对 +33% 但绝对值只差 1 KB。和"逐曲压"相比没有实质体积劣势。
     - 只能用 juce_core 现成的 `Base64::toBase64` / `convertFromBase64`（`juce_Base64.h:56-59`）与 `GZIPCompressorOutputStream`，不需要手写二进制读写、字节序和长度校验。
  3. 元数据 schema（`version` 只用于**以后**的迁移，不再兼容 v1）：
     ```xml
     <VGMToyboxState version="2" loopCount="1" fadeSeconds="3" volume="1">
       <song name="stage1.vgz" size="142337">H4sIAAAAAAAA...</song>
       <mixer enabled="1" masterGain="1" masterMuted="0" masterOutput="0" defaultRouting="master">
         <track index="0" gain="1" pan="0" mute="0" solo="0" output="0"/>
       </mixer>
     </VGMToyboxState>
     ```
     - `<song>` 里就是**原始文件字节**的 base64，不再区分 `.vgz` 还是 `.vgm`——外层 gzip 统一负责压缩，不需要 `encoding` 属性，也不会出现"二次压缩"的歧义。
     - `size` 是原始字节数，解码后比对，防截断。
     - `name` **至少要留住扩展名**：`.s98/.dro/.gym` 靠扩展名选引擎（`LibvgmBackend.cpp:16-23`）；`.vgz` 即使丢了名字也能靠 gzip magic 认出来。
     - 不存本机绝对路径（分享出去会泄露目录结构，而且它已经不再承担加载职责）。
     - base64 只管 `A-Z a-z 0-9 + / =`，不含 XML 要转义的字符，放进元素文本或属性都不会被 `&amp;` 之类放大。
     - **以后加字段的约定**（XML 可扩展性就靠这条）：读的时候忽略不认识的属性与子元素、缺字段一律取默认；写的时候总是写全并带上 `version`。这样新工程在旧版本里不会崩，只是丢掉新字段——不需要再设计迁移代码。
  4. **payload 已经有现成的**：引擎本来就留着加载用的 image——`fileData`（`PlaybackEngine.h:258`，`:103` 装入、`:612` 重建时复用、`:674` 清空），所以恢复歌曲**不需要新增存储**，只要加一个带锁拷贝的 `getSongImage()`（约 1 MB memcpy，几微秒），编码与压缩都在锁外做。**缓存最终 blob**：状态没变就不重算，宿主自动保存会反复调 `getStateInformation`，每 1 MB 重压一次是几十毫秒。压缩放 M-1 的 worker 里做。
  5. **恢复顺序是易错点**：`mixerEnabled`、`defaultRouting`、`loopCount`、`fadeSeconds` 必须在**触发加载之前**设好——`buildSong` 用它们决定建几个播放器、新曲子按哪套路由铺开（`PlaybackEngine.cpp:83`）。每条轨道的 gain/pan/mute/solo/output 必须在**加载完成之后**设——`installSong` 会整个换掉 `tracks` 容器（`PlaybackEngine.cpp:106`），而且轨道数来自新曲子。`masterVolume`/`mixerMasterGain`/`masterMuted` 是原子量，什么时候设都行。按 `index` 匹配，越界条目忽略、越界 `output` 引擎自己 clamp。
  6. **从内存构建，不落临时文件**：`SongMemory {data, size, File(name)}` 就是后端要的东西（`SongBackend.h:50-57`），`.vgz` 由 `LibvgmVgz` 按 magic 解包。恢复时不要把歌曲写到 `%TEMP%` 再加载——那会在用户磁盘上留东西，还要处理清理。
  7. **旧格式一律不读**：没有 gzip 头（旧工程）、`getXmlFromBinary` 解不出 XML、或 `version` 不是 2 → 保持引擎现有内容不动（刚打开的实例就是空的）+ 状态行提示"工程里的状态版本不兼容，请重新载入歌曲"，**不按 `file` 属性回退**。既然已经决定打破格式，回退路径就是纯债务。
  8. **体积上限 16 MB**（已定）：base64 让内存里多出 1.33 倍、单行 XML 又一份、gzip 输出再一份，峰值约 4 倍——16 MB 的极端文件一次性吃约 64 MB，不落盘、随作用域释放，可以接受；代价是这支曲子**首次**保存要花约半秒（压缩是瓶颈），之后走缓存。要让这 64 MB 不常驻：base64 字符串和单行 XML 只在这一次保存里存在，常驻的只有 `fileData` 与最终的压缩 blob。超过上限只存 `name`/`size` 并置 `embedded="0"`，状态行提示"歌曲过大，未随工程保存"。
  9. **多实例**：每个实例各存自己那份，宿主不会替我们共享；**不要**用 AppData 缓存去重——那恰好破坏"发给别人"这个目标。常规 VGM 几十 KB 到几百 KB，三五个实例仍然可控。
  10. 磁盘上的文件后来改过，不影响已保存的工程——工程里是加载那一刻的快照。这条要写进 README，免得用户以为工程跟着文件走。
  11. 序列化放到引擎侧（`writeState` / `readState` + payload），不要留在 `ToyboxAudioProcessor` 里——核心测试不链 libvgm 也能覆盖。
  12. **这是一次 breaking change，按版本号说清楚**：`project(vgmtoybox VERSION 0.0.5)` → **0.1.0**（0.x 阶段破坏性变更抬 MINOR；`release.yml` 的 `sed` 读的就是这个值，并要求 tag 等于它，所以 tag 是 `v0.1.0`）。同一笔里还要：在 `release.yml` 的 notes.md 加一段"**升级说明：状态格式变了**，旧工程里保存的歌曲与混音会被忽略，重新载入歌曲即可"；删掉 `PluginProcessor.cpp:145-147` 的 `VGMPlayState` 分支与 `CODEBUDDY.md` 里"旧的 `VGMPlayState` 仍被接受"那句——旧 blob 反正读不了，留着只是死代码。
- **代价**：约 130 行（base64 与 gzip 封装 30、引擎取值与 blob 缓存 20、恢复与顺序 45、上限与提示 35）+ 3 个测试；不碰音频线程。
- **测试**：
  - 混音 round-trip：设 5 条参数 → 序列化 → 新引擎反序列化 → 逐条比对 `getTrackGain/Pan/Mute/Solo/Output` + `isMixerEnabled` + `getMixerMasterGain`。
  - 歌曲 round-trip：把 blob 写进 MemoryBlock → 新引擎从内存构建 → 轨道数、时长、标签一致，且**临时目录没有新增文件**；解压后能 `parseXML` 看到 `<song>`，base64 解回来与 `fileData` 逐字节相等。
  - 体积与版本：1 MB 的 `.vgm` 保存后 blob 明显小于原文件；`.vgz` 的 blob 与原文件相差 1% 以内；旧格式（无 gzip 头）被拒绝——不崩、不加载、给出提示，且不覆盖引擎已有内容。

### M-1 加载占着消息线程

- **证据**：构造里 `openStartupSong()`（`PluginEditor.cpp:145`）、`loadFile()`（`:368-390`）、拖放与菜单都走同一条同步路径；`loadFile` 内部读文件、解压、`buildSong` 一气做完。
- **影响**：大文件或慢盘上窗口白屏、按钮无响应；构造期加载还会推迟首帧。宿主恢复工程时同理（见 M-2）。
- **修法**：§5.3 的 worker + `callAsync`，带 generation 取消与 `SafePointer`。
- **为什么是 M 不是 H**：VGM 通常不到 1 MB，主要成本是每个芯片一个播放器的初始化；在实测到秒级卡顿之前不升级。测量方法见 §9。
- **落地（已完成）**：`PlaybackEngine::loadFileAsync()`（`PlaybackEngine.{h,cpp}`）用一个 `juce::ThreadPool`（1 线程）读字节并构建，`MessageManager::callAsync` 回到消息线程安装。引擎侧拆出两半：`BuildConfig makeBuildConfig()` 在消息线程快照配置（音质、块大小、loop/淡出、混音模式、默认路由、芯片列表），`static buildSong(config, …)` 只读这份拷贝，`installLoaded()` 在锁内做交换；同步的 `loadFile()`/`rebuild()` 走同一条 `buildSong + installLoaded` 路径，所以只有一条构建路径。取消用 `loadGeneration`：`loadFileAsync`、同步加载、`unloadFile` 都会 `++`，完成时对不上就丢弃且**不回调**（更新的请求才是要报告的那个）。析构先置 `alive=false` 再 `removeAllJobs(true, 3000)`，迟到的结果不会写进已释放的对象。编辑器侧 `loadFile()` 改成发起请求 + `SafePointer` 回调，状态行在构建期间显示 `Loading <name>...`；启动曲不再定时重试（`startupAttempts` 删掉），因为结果本来就是回调。核心测试不链窗口模块，但 `PlaybackEngine.cpp` 现在要编 `MessageManager`，所以 `vgm_core_test`/`vgm_mixer_test` 链接了 `juce::juce_events`（测试不走异步路径，那条路径需要一个跑着的消息循环）。

### M-2 `setStateInformation` 的线程假设 + 错误静默

- **证据**：`PluginProcessor.cpp:133-161`；`eng.loadFile (file, errorMessage)`（`:160`）的返回值没有检查，`errorMessage` 出了作用域就丢了。
- **影响**：① 宿主若在非消息线程调用，加载会写 `chips`、`tags`、`currentFile` 这些**按"只有消息线程碰"约定**维护的字段（`PlaybackEngine.cpp:76-79`、`:396-398` 的注释就是这条约定），与 UI 轮询构成数据竞争；② 加载失败时工程"看起来打开了但没有歌"，没有任何提示。
- **修法**：不在消息线程就 `callAsync` 一次；错误通过状态栏或 `ChangeBroadcaster` 暴露。与 M-1 同一笔提交。
- **落地（已完成）**：`ToyboxAudioProcessor::setStateInformation()` 先判断线程，不在消息线程就 `callAsync` 并 **`WaitableEvent` 等到它做完**（30 s 上限）。与 §5.3 原来写的"晚一拍完成、宿主不必等"不同：`setState` 返回后宿主马上就会用这个实例，异步完成等于让状态在未知时刻出现，而"恢复晚一拍"本身没有收益；等待发生在宿主的调用线程上，不是音频线程（`render()` 只碰锁），代价只是打开工程时多等一次构建。错误文本改由 `restoreState()` 返回，`setStateInformation` 用它填状态行——原来的 `readState` 返回值没人检查、`errorMessage` 出了作用域就丢，这条一起修了。

### M-3 快捷键两份真值

- **证据**：`MainMenu.h:18` 的 `shortcut` 是给人看的字符串（`"Ctrl+O"`），`keyPressed()`（`PluginEditor.cpp:256-306`）又独立写了一遍 `KeyPress` 匹配。
- **影响**：新增命令要改两处；改一处就会出现"菜单写着 Ctrl+O、按下去没反应"，而这类错误编译器和测试都抓不到。
- **修法**：`MenuEntry` 存 `juce::KeyPress`，文本由 `getTextDescriptionWithIcons()` 生成；`keyPressed()` 遍历菜单表匹配。约 40 行。
- **落地（已完成）**：新增 `Source/ui/EditorCommands.h`，一张 `id → KeyPress + std::function` 的表：`getKey()` 给菜单画，`handleKey()` 给窗口匹配，`perform()` 给菜单项执行（菜单回调也改成走 `perform()`，避免第二条动作路径），`describe()` 统一文本。`MenuEntry::shortcut` 换成 `juce::KeyPress key`，`MainMenu` 通过 `const EditorCommands*` 取键；`Ctrl+1..9` 由 `EditorCommands::pageKey(i)` / `pageForKey(key)` 提供，菜单与 `keyPressed()` 都调它，所以页号也不再是两处硬编码。

### M-4 `PluginEditor` 兼五职

- **证据**：448 行里塞了窗口与视图切换（`:48-100` 构造函数里接线）、菜单回调、快捷键（`:256`）、加载（`:368`）、启动曲（`:423`）、定时刷新节流（`:477-486`）、拖放。
- **影响**：异步加载（M-1）、命令表（M-3）都要改这个文件，冲突面大、评审难读。
- **修法**：先做一次**纯搬移**——抽出 `EditorCommands`（菜单表 + `keyPressed` + 动作触发），约 100 行；再做 M-1。
- **落地（已完成）**：命令表与快捷键搬进 `Source/ui/EditorCommands.h`，动作仍留在 `PluginEditor`（它们碰 `viewStack`、`processor` 和对话框，搬走只会把编辑器再拆一半）；加载相关的新成员（`loadInFlight`/`loadingName`/`reportedStarvedBlocks`）集中放在头文件的一组注释下面，`keyPressed()` 现在只剩 Esc、命令表、页号三段，每段都指向它依赖的那张表。没有为拆而拆出新的中间层。

### L-1 文档路径回归

源码与测试里 6 处指向已移动的设计文档（上一轮修过同类，属于回归）：

| 文件 | 指向 |
|---|---|
| `PlaybackEngine.h:31`、`BackendRegistry.h:16`、`BuiltInBackends.cpp:12`、`PlaybackCoreTest.cpp:8` | `docs/playback-backend-design.md` |
| `PluginProcessor.h:56`、`ChannelStrip.cpp:182` | `docs/multitrack-output-design.md` |

实际路径都在 `docs/desgin/` 下。修法是改字符串；更值得做的是把"源码里的 `docs/` 路径必须存在"写成 CI 里的一行检查（§9 的那段脚本可以直接搬）。

**落地（已完成）**：六处都改成了 `docs/desgin/…`；CI 检查这一条**没有**加——§9 那段脚本在 Windows 上依赖 `Get-ChildItem`，跨平台要重写，而这类回归靠人工评审发现一次就够了，不值得为它加一条只在一个平台跑得起来的检查。留在登记册里，等哪天真成了反复回归再说。

### L-2 目录名 `desgin`

`docs/desgin/` 是 `design` 的拼写错误，被 **29 处**引用写死。改名一次可做（`git mv` + 全量替换），但会让旧文档里的路径引用与原仓库历史对不上，收益低于成本。**建议保持原名**，在 `CODEBUDDY.md` 里注明"目录名是历史拼写，勿改"，免得每轮评审都重新发现一次。

**落地（已完成）**：保持原名；`CODEBUDDY.md` 的文档段落里写明了这条约定（"`docs/desgin/` keeps its spelling"），并说明新链接照写 `desgin`、不要"修正"。

### L-3 零 `// DEBT:` 标记

全仓 0 个。约定（policy §6）存在但没被执行，债务实际只活在 `docs/review/`。这不算问题，但建议落地时在提交信息里引用编号（`Debt: H-1`），否则代码与记录会脱钩。

### L-4 `starvedBlocks` 没有消费者

`getStarvedBlocks()` 只有测试在读（`PlaybackCoreTest.cpp:486` 起），运行时既不记录也不显示。这个计数器是"音频线程被锁饿死"的唯一证据来源，建议在编辑器析构或诊断页打一行日志，5 行成本。

**落地（已完成）**：`PluginEditor::refreshUi()` 里比较上次报告的值，只有**增加**时才写一行 `VGMToybox: N block(s) starved so far …` 到 `juce::Logger`（不是每帧刷屏）。随定时器走而不是只在析构打，是因为窗口关掉后引擎可能还活着，析构那行会晚于真正发生卡顿的时刻。

---

## 7. 债务登记册

| 编号 | 问题 | 严重度 | 建议时机 | 预估 | 状态 |
|---|---|---|---|---|---|
| H-1 | 混音状态与歌曲数据都未进 VST 状态 | H | 本轮（M-1 之后） | 130 行 + 测试 | 已清（`934f59f`） |
| M-1 | 加载同步占消息线程 | M | 本轮（M-4 之后） | 150 行 | 已清 |
| M-2 | `setStateInformation` 线程与错误 | M | 与 M-1 同笔 | 30 行 | 已清 |
| M-3 | 快捷键两份真值 | M | 本轮 | 40 行 | 已清 |
| M-4 | `PluginEditor` 拆命令表 | M | 先于 M-1 | ~100 行纯搬移 | 已清（只搬命令表，见 §6 M-4） |
| L-1 | 6 处文档路径失效 | L | 本轮 | 6 行 | 已清（CI 检查未加，见 §6 L-1） |
| L-2 | `desgin` 目录名 | L | 只做记录 | 0 | 已定（保持原名，已写进 CODEBUDDY.md） |
| L-3 | 无 `// DEBT:` 标记 | L | 持续 | 0 | 持续 |
| L-4 | `starvedBlocks` 无消费者 | L | 顺手 | 5 行 | 已清 |

---

## 8. 本轮提交怎么切

按 policy §7"重构与功能不混进同一个提交"，建议五笔：

1. `docs: point the design references at docs/desgin, and keep the spelling`（L-1 / L-2）
2. `refactor: take the command table out of the editor`（M-4 + M-3）
3. `feat!: carry the song data and the mixer in the project`（H-1，**breaking**：状态格式不再兼容旧工程；`VERSION` 0.0.5 → 0.1.0，release notes 写明）——**已完成**（`934f59f`；本文 §10 记了与方案的一处出入）
4. `refactor: load songs on a worker thread`（M-1 + M-2 + L-4）
5. `docs: record the 2026-10 review`（本文）

第 2 笔是纯搬移，应当**零行为变化**（测试全绿、界面不变），这样才能在事后放心地把它和后面的功能改动分开。

实际切分与上面略有出入：第 2、4 笔各自都要碰 `PluginEditor.{h,cpp}`（命令表与加载状态行在同一批成员里），按"文件级"分不出两笔互不重叠的提交，所以第 2、4 笔的边界按**行为**划——命令表那笔不改加载，加载那笔不改快捷键。L-1 的两处（`PlaybackEngine.h`、`PluginProcessor.h`）同这些文件一起走，第 1 笔只覆盖另外四个文件。

---

## 9. 附录：复现命令

```powershell
# 规模
Get-ChildItem plugin\Source -Recurse -File -Include *.cpp,*.h | ForEach-Object {
    (Get-Content $_.FullName | Measure-Object -Line).Lines } | Measure-Object -Sum

# 后台线程：本轮之前 0 命中；现在只剩加载 worker 与它的 callAsync（M-1 落地后应有
# PlaybackEngine.h 的 ThreadPool、PlaybackEngine.cpp 的 loadFileAsync、PluginProcessor.cpp 的
# callAsync；再出现别的就是新线程，值得停下来看）
Select-String -Path plugin\Source\**\*.cpp,plugin\Source\**\*.h -Pattern `
    'juce::Thread|ThreadPool|callAsync|AsyncUpdater'

# 跨线程数据：原子量与锁
Select-String -Path plugin\Source\playback\PlaybackEngine.h -Pattern 'std::atomic|ScopedLock'

# 文档路径失效（应无输出）
Get-ChildItem plugin -Recurse -File -Include *.h,*.cpp | ForEach-Object {
    $t = Get-Content $_.FullName -Raw
    [regex]::Matches($t, 'docs/[A-Za-z0-9_/.-]+\.md') | ForEach-Object {
        if (-not (Test-Path $_.Value)) { "{0}: {1}" -f $_.Name, $_.Value } } }

# 编译标准（JUCE 8 默认 cxx_std_17）
Select-String -Path build\build.ninja -Pattern 'std:c\+\+'

# 加载耗时（需本机构建；把 "Some Song.vgm" 换成一个大的 .vgz）
Measure-Command { .\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgz" }
```

最后一条只能给出"进程启动到窗口出现"的量级；更准确的做法是在 `PluginEditor::loadFile()` 里临时插一对 `juce::Time::getMillisecondCounterHiRes()`。

---

## 10. 待拍板的事（H-1、M-1 已落地）

1. **H-1（已实现）**：① 状态 v2 = XML + `<song>` base64 + 整体 gzip；**不兼容旧 blob 与旧 tag**，读不出来就不加载、状态行提示，不按 `file` 回退；② 歌曲字节随工程走，只存文件名不存绝对路径，不写临时文件、不落 AppData；③ 主题等用户偏好继续留在 `settings.xml`；④ 单曲上限 **16 MB**，超出只存文件名、大小与 `embedded="0"`；⑤ 版本 `0.0.5` → **`0.1.0`**，release notes 写明 breaking change。
   - 代码：`plugin/Source/playback/SessionState.{h,cpp}`（schema + 编解码）、`PlaybackEngine::writeState/readState`、`PluginProcessor` 两侧。
   - 走带状态**不进状态**：保存时是否正在播放取决于宿主何时自动保存，纯属偶然；恢复一律**停止、回到开头**，打开工程不会自己出声。
   - 与方案的一处出入：**保存**时超出上限不再弹提示（`getStateInformation` 可能被宿主自动保存反复调用，每次弹会盖掉真正的错误），改由**读取**侧提示"歌曲过大，未随工程保存"；上限本身写进 README。
   - 第二处出入（已拍板）：方案第 4 条建议**缓存最终 blob**，实现**不缓存**——`getStateInformation` 每调一次就重跑 base64 + gzip，16 MB 的极端文件约半秒，常规曲子可以忽略。缓存要额外的 `stateRevision` 与失效判断，收益只在"宿主反复自动保存大文件"这一个场景；先按不缓存走，若真有人抱怨再补。
2. **M-1（已实现，本轮做）**：见 §6 M-1 的"落地"。一处与 §5.3 的出入：`setStateInformation` 选择"跳到消息线程并**等待**"而不是"晚一拍完成"，理由在 §6 M-2。
3. **`desgin`（已定）**：保持原名，约定写进 `CODEBUDDY.md`。
4. **可视化页**：是否排进下一个功能？如果是，§3.4 的 `ScopeTap` 可以随该功能一起进，不必单独一轮。
5. **混音推子**：将来是否要暴露成宿主参数（可自动化、可被宿主撤销）？这个答案会决定 Q2 里 `ValueTree` 与 APVTS 的取舍。


