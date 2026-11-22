# FM 导出与驱动重构：审查

> 分支 `refactor/fm-voice-dialect`，相对 `feature/fm-visualisation`：12 个提交，1362 行插入 / 347 删除。
>
> 其中 595 行是文档（4 篇），代码约 770 行。
>
> 结论：**逻辑正确、验证扎实（三次回归都被手工复现并修好），但有三处并发问题需要处理。** 其中两处是这次新引入的，一处是"注释描述了一个没写的保护"。

## 一、审查范围

| 文件 | 变更 | 性质 |
|---|---|---|
| `FmVoiceDriver.cpp` | +365 / −347（含改名） | `OpnVoiceDriver` 改名 + `Dialect` |
| `ContentSlot.cpp` / `.h` | +175 / −55 | 导出后台化 + 拖拽重渲染 |
| `LibvgmTrackPlayer.cpp` / `.h` | +62 / −44 | `renderNoteAt` 返回速率 + 传 patch |
| `VoicePatch.h` | +50 | `kVisualNoteRate` / `RenderedNote` / 两张表的注释 |
| `SongBackend.h` | +21 | `RenderedNote` 接口 |
| 测试 | +136 | 三条断言 |
| 文档 | +595 | 4 篇 |

## 二、值得保留的做法

1. **每次回归都有可复现的证据**。拖拽那次是两份 wav 的逐样本对比，`每 6 个重复 = 48000/8000`——**从"感觉撕裂"到"确认是零阶保持"只花了一次对比**。
2. **为每一项修复配一条会失败的断言**，并且在提交说明里写出"怎么让它红"。三条都验过：改 `op*4`、关 pan、改回 `noteWidth`。
3. **改名的理由是行为，不是洁癖**：`kNoteRate` → `kVisualNoteRate` 是因为**名字让两处用了同一个值**，而那正是 bug 的来源。

## 三、发现

### HIGH-1 `exportAlive` 是一个没写的保护，注释却说它存在

```121:127:plugin/Source/ui/ContentSlot.h
/** Shared with the pending job: the destructor clears it, so a result that arrives after this
    slot is gone is dropped instead of writing into freed state. Same shape as
    `PlaybackEngine::alive`. */
std::shared_ptr<std::atomic<bool>> exportAlive = std::make_shared<std::atomic<bool>> (true);
```

**`~ContentSlot` 没有清它。** 全文只有两处引用：构造时的 `true`，和 `beginExport` 里的 `const auto lifetime = exportAlive;`。

**所以 `lifetime->load ()` 永远为真，那个"插槽已消失"的早退分支是死代码。**

**实际保护来自 `SafePointer`**（`callAsync` 里的 `safe == nullptr`），**所以不会崩**——但这条注释让人以为多了一层，而它不存在。

**修法**（照注释写的做）：

```cpp
ContentSlot::~ContentSlot ()
{
    exportAlive->store (false);
    ...
}
```

`exportPool` 析构会等 job 结束，job 里看到 `false` 就早退，不投递 `callAsync`。

### HIGH-2 `renderNoteAt` 在工作线程读设置

```1282:1288:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
const auto& store = settings::SettingsStore::getInstance ();
const double sustain = (double) juce::jmax (1, store.getFmSustainMs ()) / 1000.0;
const double release = (double) juce::jmax (1, store.getFmReleaseMs ()) / 1000.0;
...
store.getFmNormalise ()));
```

**`renderNoteAt` 被两个线程调用**：拖拽（消息线程）和快照导出（`exportPool` 的工作线程）。

**我在 `ContentSlot` 的消息线程快照里特意读了 `atPlayedPitch` / `fixedNoteHz`，理由是"job 不许碰设置"——然后 `renderNoteAt` 里又读了三个。** 同一个原则，一半遵守一半没守。

**风险**：`PropertiesFile::getValue` 有内部锁，读本身安全；但用户在导出期间改设置（`setValue` 写文件）就是读写并发。**这是低概率、但一旦发生就难复现的问题。**

**修法**：把三个值也做进一个结构，由调用者在消息线程填。

```cpp
struct NoteRenderShape { double sustain; double release; bool normalise; };
```

### HIGH-3 拖拽和快照可以同时使用 `exportDriver`

**`exporting` 只保护了 `beginExport`**：

```
ContentSlot.cpp:1618  if (exporting.exchange (true)) { return; }
ContentSlot.cpp:1720  safe->exporting.store (false);
```

**`startWaveDrag` 没有这个检查。** 所以：批量导出正在进行（工作线程在 `renderNoteAt` 里跑 `exportDriver`），用户拖一行 → 消息线程也进 `renderNoteAt` → **同一个 `Core` 的寄存器被两个线程同时改写**。

**症状**：波形错乱、偶尔崩、偶尔"听起来像两首歌叠在一起"。**而且不可复现**。

**修法**：`renderNoteAt` 加锁（`mutable juce::CriticalSection`），拖拽最多等一次导出渲染——**比数据竞争好**。

### MEDIUM-1 兜底查找读 `noteWaves` 时有竞争

```cpp
if (chosen == nullptr)
{
    for (const auto& entry : noteWaves) { ... }
}
```

`noteWaves` 由 `captureState` 在消息线程写。**新路径（传 patch）不触发这段，但拖拽兜底时仍在工作线程读它。**

**修法**：既然两条导出路径都会传 patch，**这段兜底可以删掉**（`chosen == nullptr` 直接返回空），并注释说明"调用者必须给 patch"。**留着它就是留一个没人用、但有人会踩的并发入口。**

### LOW-1 `Dialect` 的两个实例在匿名命名空间

`constexpr Dialect kOpnDialect { ... }`——`constexpr` 在文件作用域隐含 internal linkage，**没有 ODR 问题**。只是提醒：**它们不是 `static`，别以为需要加。**

## 四、规范逐条对照

| 规则 | 结论 |
|---|---|
| §1 名字说明意图 | ✅ `kVisualNoteRate` / `RenderedNote` / `Dialect` 都比原来准 |
| §2 注释写为什么 | ⚠️ **HIGH-1 的注释描述了一个没实现的行为**——这比没注释更糟 |
| §3 高内聚低耦合 | ✅ `Core` 提到命名空间级，`Dialect` 归位 |
| §4 加东西只改一处 | ✅ `isOpm` 从 5 处降到 1 处；加 OPL 家族只加一个 `Dialect` |
| §5 不留技术债 | ⚠️ `exportAlive` 是死代码 |
| §6 可测试 | ⚠️ **两条导出路径仍无断言**（需要先拆"选目录"与"导出"） |
| §7 颜色 | ✅ 未涉及 |
| §7.5 核心断言 + 界面截图 | ✅ 三条新断言都验过会红 |
| §8 提交即最小单元 | ✅ 12 个提交各自一件事；`perf` / `fix` / `feat` / `docs` 分得清 |
| §9 重构不夹带行为变更 | ✅ `72e1220` 是纯结构，54/54 + 三条断言都在 |
| §10 通道映射跨型号验证 | ✅ `op*8` vs `op*4` 有断言；`channelMask` 有注释 |
| §13 一份数据一个来源 | ⚠️ **HIGH-2 是"设置"的第二处读取**；其余都收好了 |

**统计**：✅ 9 条、⚠️ 4 条。

## 五、这批改动最值钱的一点

**三次回归都不是算错，而是"一个值两个含义"**：

| 事件 | 一个值，两个含义 |
|---|---|
| 键盘空白 | `channels.size () == 0`：没加载 / 采集没到 |
| `peak = 0` | 没音符 / 有音符没出声 |
| 拖拽的阶梯 | `renderNoteAt` 返回空：没音色 / 没找到 |

**而三次的修法都是同一个：给那个值加一维信息**（`gate`、`isSampledChannel`、显式 `patch` 参数）。

**这条已经写进 `docs/issue/2026-11-22-the-drag-that-kept-the-panels-shape.md` 第七节，建议以后每次加"失败即回退"分支时先读一遍。**

## 六、建议的修复顺序

| 步 | 内容 | 为什么先做它 |
|---|---|---|
| **1** | HIGH-1 清 `exportAlive` | 一行，且让注释变成真的 |
| **2** | HIGH-3 `renderNoteAt` 加锁 | 数据竞争，越晚修越难复现 |
| **3** | HIGH-2 设置预读成结构 | 与前一步同一个函数，一起改 |
| **4** | MEDIUM-1 删兜底查找 | 删代码，降低表面 |
| **5** | 导出断言 | 需要先拆 `exportToFolder`，是独立一件 |

**前四步都在 `renderNoteAt` 与 `ContentSlot` 上，应该一个提交做完**——它们解决的是同一件事："导出路径被两个线程共享，而共享的东西没有全部收好"。
