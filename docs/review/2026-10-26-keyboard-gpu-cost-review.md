# Keyboard GPU 优化分支评审

**评审对象**：`fix/keyboard-gpu-cost`，17 个提交，18 文件 +1025 / −67
**对照基线**：`master`（`c3043a3` 之前）
**依据**：`.codebuddy/rules/code-quality.md`
**方法**：读 diff 与关键函数，逐条对照规则；不评价已经用户验收的视觉与性能结果

---

## 结论

**性能目标达成，架构方向正确，但留下了一批"与代码矛盾的注释"和两处不完整的缓存键。**

优化本身是干净的：`ShadowImage` 把卷积变成 blit，`RowSignature` 把重绘从整行缩到列，`WavePathCache` 把波形变成参数的函数——这些都是"把不变的东西移出每帧路径"，和规则第 3 条（谁拥有状态谁负责它）一致。

问题集中在两类：

| 类型 | 条数 | 性质 |
|---|---|---|
| **注释与实现矛盾** | 4 | 直接在误导读者，且都是这次改动自己造成的 |
| **缓存键不完整** | 2 | 今天不出错，靠的是"当前调用点恰好固定"，这正是这个分支已经栽过两次的形状 |
| **逻辑不可测** | 1 | 最该被验证的部分埋在 private 嵌套类里 |
| **封装与死代码** | 4 | 小，但都属于规则第 5 条点名的"先这样" |

**没有发现正确性缺陷。** 脏区、缓存、`setOpaque` 三条路径的逻辑都对；滚动后「多画一次」而非「少画」是我确认过的保守方向。

---

## 一、必须修：注释与代码矛盾

这四条**不是风格问题**——它们说的是代码没有做的事。下一次有人读到这里，会按注释理解系统，而注释是错的。

### P1-1 `PluginEditor.cpp:25-41` 探针的注释停留在中间状态

```cpp
// --- paint accounting ------------------------------------------------------------------
// Temporary probe, and deliberately not behind an environment variable: the first attempt put
// it behind one, and a GUI binary launched from Explorer inherits whatever the shell had at
// logon, so nothing appeared. It now always counts and always reports into the status line,
// where it cannot be missed.
// ...
// To remove: this block, the fetch_add in paint(), the call in timerCallback(), paintReport in
// the header and the two lines in updateStatusLine().
```

**三处与现状不符**：

| 注释说 | 实际 |
|---|---|
| "Temporary probe"、"To remove: …" | 已经做成正式设置项 `diagnostics.paintStats`，`efebd2a` 的提交说明写的是"探针留下" |
| "deliberately not behind an environment variable" | 现在由设置项控制，既不是环境变量，也不是"永远" |
| "It now **always** counts and **always** reports" | `paint()` 里是 `if (paintStats)`，`takePaintReport(bool)` 第一句就是 `if (! enabled) return {}` |

`L884` 的 `// Temporary probe: read this off ...` 同样过时。

**修法**：这段应该描述**现在是什么**，以及**为什么它值得留**。第二点原本是对的（"这个问题读代码回答不了"），保留；第一点的整段历史（环境变量、日志文件、找不到）属于提交说明，不属于源码。

### P1-2 `SettingsStore.h:213` 与 `:229` 同文件自相矛盾

```cpp
// L213
The one setting here that trades picture against cost directly: every refresh repaints what
changed, and at 4K that is real work. 30 is the default this view always used; 10 is enough
for a slow passage and 60 matches a display. Clamped on read and on write, so a hand-edited
file cannot ask for 500.

// L229
static constexpr int defaultRefreshHz = 15;
```

**改默认值那次只改了常量，没改这段 doc。** 而 L231 的注释又正确地说"15 rather than the 30 this view used to hard-code"——同一份声明里，一处说 30、一处说 15。

**修法**：L213 改成 15，或者删掉那句（数字已经在 L229 定义了，doc 里重复一遍就是第二个来源）。

### P1-3 `KeyboardRowPaint.h:60` 写死了已不成立的频率

```cpp
Docs/issue/2026-10-26-keyboard-view-gpu-cost.md: the view repaints at 30 Hz, and almost none of
```

刷新率现在是用户可调的 10–60，默认 15。**"30 Hz" 从这次改动之后不再是一个常量。**

**修法**：`"repaints at its own rate"` 或 `"repaints many times a second"`。频率进了设置，注释就不该再引用具体数字。

### P1-4 `SettingsOverlay.cpp:27` 与 `:425` 描述已经搬走的东西

```cpp
// L27
// Grown from 640 by 500 when the General page gained the refresh-rate row: five switch rows, a
// slider with its caption, and the path block beneath them no longer fit without pushing the
// footer buttons into each other.
```

**滑块已经不在 General 页了**（`3b9c4b9` 移到 Keyboard 页）。现在需要这个高度的是 Keyboard 页——它有五组设置。这段注释让人去找一个不存在的控件。

```cpp
// L425
// The refresh rate used to be here; it belongs to the Keyboard page and moved there.
```

**墓碑注释。** 它描述的是一个不存在的东西的位置。规则第 5 条："删掉不再被引用的代码、文案键、常量"——注释里的"曾经"同理。**将来读代码的人不关心它曾经在哪，他只关心这里现在有什么（什么都没有）。**

**修法**：L27 改成"因为 Keyboard 页有五组设置"；L425 整行删掉。

---

## 二、建议修：缓存键不完整

这个分支已经因为**键不完整**栽过两次：

- `WavePathCache` 最初用绝对坐标存路径、键只有宽高 → 滚动后线画在旧位置（`9233928` 修的）
- 行级脏区用 `row.area` 的并集 → 8 行都在动，并集就是整窗口

下面两条是同一个形状，**今天不出错，因为调用点恰好固定**。

### P2-1 `SoftShadow.h:99-103` 的键里没有 `cornerRadius`

```cpp
inline void drawCachedShadow (juce::Graphics& g, ShadowImage& cache,
							  juce::Rectangle<float> shape, float cornerRadius,   // ← 参数收了
							  juce::Colour colour, int blurRadius, juce::Point<int> offset)
{
	...
	const bool stale = ! cache.valid
					|| cache.area != bounds
					|| cache.colour != colour.getARGB ()
					|| cache.blur != blurRadius
					|| cache.offset != offset;      // ← 没有 cornerRadius
```

**函数接受一个影响输出的参数，但缓存键里没有它。** 现在安全，是因为每个 `ShadowImage` 只服务一个调用点，圆角彼此固定（`theme::buttonRadius` / `5.0f`）。

**但这个安全性没有写在任何地方**，而 `drawCachedShadow` 是 `SoftShadow.h` 里的公共 inline 函数——下次有人在别处复用一个 cache 变量，圆角变了图不变。

**修法**：`ShadowImage` 加 `float radius = -1.0f;`，进键。三行。

### P2-2 `PluginEditor.h` `backdrop` 的键只有 `windowTop`

```cpp
if (! backdrop.isValid () || backdropWidth != getWidth ()
	|| backdropHeight != getHeight () || backdropTop != top.getARGB ())
```

图里画的是 `windowTop → windowBottom` 的渐变，**键里只有 `windowTop`**。

今天正确，因为主题切换会同时改变两者（整份 palette 换掉）。**但这条推理不在代码里**，而且 `lookAndFeelChanged()` 走的是 `repaint()`——如果哪天有一个只改 `windowBottom` 的调色板调整，渐变会静默停用旧值。

**修法**：`backdropBottom` 一起进键，或存一个 `juce::Colour` 对的哈希。两行。

---

## 三、建议修：脏区逻辑不可测

### P3-1 `RowSignature` 的判定埋在 private 嵌套类里

规则第 6 条：

> 算术、映射、合成、布局一律写成**可以传参调用的函数**，不依赖窗口、不依赖单例。
> 反例（本项目真实发生过）：音域跨度、行布局、波形合成曾埋在 private 嵌套类里，只能"画出来看"，一晚因此返工 7 次。

`ContentSlot.cpp` 的现状正是这个形状：

```cpp
struct RowSignature { ... };            // L~1258，KeyboardPanel 的 private 成员
std::vector<RowSignature> painted;
void repaintChangedRows (void) { ... }  // L246，三个 repaint() 的映射在这里
```

**它决定的是"哪一列重绘"，是纯逻辑**：输入两个签名，输出一组矩形。它不碰窗口、不碰单例、不需要 `Graphics`——**但它被写成了一个只能通过真实播放来观察的成员函数。**

后果和规则里举的例子一样：**判错的症状是"那一块停在旧画面上"**，而这种症状从代码里读不出来。

**修法**：把判定抽成 `namespace keyboard` 的自由函数：

```cpp
/** Which of a row's columns must be repainted, given what it looked like and what it looks like now. */
struct RowChange { bool wholeRow, note, wave, meter; };
RowChange compareRow (const RowSignature& before, const RowSignature& after);
```

`RowSignature` 一起搬到 `KeyboardRowLayout.h`（那一层已经有 `Row`、`layoutRows`，都是可测的），`repaintChangedRows()` 退化成"调它 + 按结果标脏"。

**顺带解决 P2-3**（见下）的索引问题：`compareRow` 可以被单测覆盖静音阈值、`note == -2` 的首次绘制、`wave` 非空的永不跳过——**这三条现在全靠注释保证**。

### P3-2 `painted` 与 `rows` 的生命周期没有绑定

```cpp
relayout ();              // 重建 rows，scrollOffset 变了这里会换一整批通道
syncScrollBar ();
repaintChangedRows ();    // 却拿 painted[i] 和新的 rows[i] 比
```

`painted` 按下标与 `rows` 配对，而 `relayout()` 重建 `rows` **不清空 `painted`**。滚动之后第 `i` 行是另一个通道，比较的是不同通道的签名。

**当前不出错**，因为 `scrollBarMoved()` 和 `mouseWheelMove()` 在 `relayout()` 之后都调了全量 `repaint()`，画面已经是最新的；之后 `setSnapshot()` 拿错位的旧签名去比，只会**保守地多判定"变了"→ 多画一次**。

**但这是巧合，不是设计**。`repaintChangedRows()` 的注释说 "Parallel to `rows`"，没有说这个前提在 `relayout()` 之后不成立。

**修法**：二选一。

1. `relayout()` 末尾 `painted.clear();` —— 触发全量重绘，而滚动本来就要全量，零成本；
2. 让 `painted` 按 `row.channel` 索引（和 `rowCaches` 一致），下标不再是行位置。

**第 2 种更好**：`rowCaches` 已经用 `row.channel` 作索引了，同一份代码里两个缓存用两套索引，正是规则第 7 条说的"同一个语义只能有一个来源"的变体。

---

## 四、观察

### P4-1 `ShadowImage::clear()` 是死代码

`SoftShadow.h:78-82` 定义了 `clear()`，**全仓无调用点**。主题变化由 `colour != colour.getARGB()` 兜住，所以它确实是多余的。

规则第 5 条：删掉不再被引用的东西。

### P4-2 两层缓存的等价判断不一致

| 缓存 | duty 的比较 |
|---|---|
| `RowSignature::dutyStep`（决定**要不要重绘**） | `lround (duty * 64.0f)`，量化到 1/64 |
| `WavePathCache::duty`（决定**要不要重建路径**） | 原始 `float` 精确比较 |

外层已经按 1/64 判定"没变"时，`paintWave()` **根本不会被调用**，所以 `WavePathCache` 那段精确比较在阈值之下是**走不到的代码**。

**不是 bug**（阈值以下的变化本来就不该显示），但它让人以为"改 duty 一定更新波形"。两处应该用同一个量化——同一个概念的两个来源是规则第 7 条的同一个毛病。

### P4-3 探针的计数器是文件级 `std::atomic`

```cpp
std::atomic<int> editorPaints { 0 };
std::atomic<long long> paintedPixels { 0 };
std::atomic<double> lastPaintReportMs { 0.0 };
```

插件可以在多个轨道上实例化，每个有自己的 editor。**计数会串**——只影响诊断数字的准确性，但规则第 3 条说"谁拥有状态，谁负责它"，它们应该是 `ToyboxAudioProcessorEditor` 的成员。

### P4-4 `refreshSlider.valueFromTextFunction` 对非法输入静默归零

```cpp
refreshSlider.valueFromTextFunction = [] (const juce::String& text)
{
	return text.retainCharacters ("0123456789").getDoubleValue ();
};
```

输入 `"abc"` → `retainCharacters` 得空串 → `getDoubleValue()` 得 `0` → 被 `setRange` clamp 到 **10 Hz**，用户看不到任何提示。

**不影响正确性**（最终值仍被 clamp），但一次手滑把 60 改成 10，没有任何反馈。可以在为空时返回当前值，两行。

---

## 五、规则对照

| 规则 | 判定 | 说明 |
|---|---|---|
| 1. 名字说明意图 | 通过 | `RowSignature`、`WavePathCache`、`drawCachedShadow`、`kRowReach` / `kColumnReach` 都是"是什么"，不是"怎么算" |
| 2. 注释写为什么，只写一次 | **不通过** | 4 条注释与实现矛盾（P1-1…P1-4）。全篇"为什么"的质量很高，**问题全在"改了代码没改注释"** |
| 3. 高内聚低耦合 | 部分 | UI 层没有芯片名，跨模块传值；但 `afterSnapshot()` / `repaintChangedRows()` 落在 `public:`（L128–L683），外部可绕过 `setSnapshot()` |
| 4. 可扩展：加东西只改一处 | 通过 | 新设置项走 `SettingsStore` + 控件 + i18n 三处，与既有设置一致 |
| 5. 不留技术债 | **不通过** | P1-4 墓碑注释、P4-1 死代码、P1-1 的"To remove"清单 |
| 6. 可测试：纯逻辑必须能被调用 | **不通过** | P3-1：脏区判定埋在 private 嵌套类里，规则点名的正是这个形状 |
| 7. 数据与呈现分离，颜色走主题 | 通过 | 新代码无字面量颜色 |
| 9. 重构不夹带行为变更 | 需注意 | `setOpaque` + `fillAll` 是一次**真实的像素变化**（边缘 1px），已在该提交说明里单列，合规；`defaultRefreshHz` 从 30 改 15 也是行为变更，但写在 `feat(settings)` 里，合规 |
| 10. 掩码类改动跨型号验证 | 不适用 | 无位域改动 |

**提交质量**：17 个提交，每个一件事，说明里写清了放弃过什么（`9a43aac` 放弃 `setOpaque`、`c27bf09` 收回那个判断）。**这是这个分支最好的部分。**

---

## 六、一句话

**这次改动的代价不是"代码变复杂了"，而是把三种知识塞进了同一个类：**

| 知识 | 属于哪 |
|---|---|
| 哪一列会随音频变化 | 领域 |
| 变了要重绘哪块矩形 | JUCE 机制 |
| 重绘一次多贵、值不值得缓存 | 性能 |

它们混在 `KeyboardPanel` 里的后果是：**任何一处判错都表现为"画面停在旧值"，而这是最难从代码里读出来的一类错误**——它不崩溃、不报错、单元测试看不见、45/45 全绿。

`RowSignature` 抽成可调用函数（P3-1）能把前两种知识拿出来，让它们第一次具备"被断言"的可能。这是这个分支唯一值得再动一次结构的地方。

---

## 附：修法与工时

| 编号 | 修法 | 量级 |
|---|---|---|
| P1-1…P1-4 | 改 4 段注释、删 1 行 | 10 分钟 |
| P2-1 / P2-2 | 缓存键各加一个字段 | 10 分钟 |
| P3-2 | `painted` 改按 `channel` 索引 | 20 分钟 |
| P3-1 | `RowSignature` + `compareRow` 搬到 `KeyboardRowLayout`，补 3 条单测 | 1–2 小时 |
| P4-1…P4-4 | 删死代码、统一量化、成员化、空输入 | 20 分钟 |

**P1 与 P2 合计半小时，建议合并前完成**——它们不影响性能，但会让下一个人按错误的描述读这套代码。

**P3-1 是唯一的结构性问题**，可以单独一个提交。
