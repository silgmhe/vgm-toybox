# Keyboard 视图为什么吃 GPU

> 现象：播放时打开 Keyboard 视图，GPU 占用从 **5% 左右升到 15%**。
> 结论：不是某一处特别慢，是**每帧 30 次做了六件本可以不重复做的事**，其中最大的一项是
> 每秒近千次的 gaussian 模糊。

## 一、先说这个数字是怎么来的

`ContentSlot` 构造时：

```cpp
startTimerHz (kRefreshHz);            // ContentSlot.cpp:1169
constexpr int kRefreshHz = 30;        // ContentSlot.cpp:26
```

定时器回调（`ContentSlot::timerCallback`，L1265）：

```cpp
engine.setStateCaptureWanted (true);
const bool captured = engine.captureChipState (snapshot);   // 取一份通道快照
keyboardPanel->setSnapshot (snapshot);                      // 整份拷贝
keyboardPanel->repaint ();                                  // 整个面板重绘，无脏区
```

**所以「打开 Keyboard」= 让一个 30 Hz 的循环开始，每一拍都重画整个面板。**

Scope 视图不走这条路径（它的 `timerCallback` 在 `current != ContentView::keyboard` 时就 return），所以只有 Keyboard 这一个页面有这个代价。

## 二、一次重绘里画了什么

`KeyboardPanel::paint()`（L208）遍历可见行，每行调 `keyboard::paintChannelRow()`。
以 13 行（典型的多芯片曲目）为例，**每一帧**发生：

| 次数 | 操作 | 位置 |
|---|---|---|
| **13** | `drawSoftShadow`（blur 6）— 行的卡片阴影 | `KeyboardRowPaint.cpp:71` |
| **13** | `drawSoftShadow`（blur 5）— 键盘底座的阴影 | `ContentSlot.cpp:748` |
| **0–13** | `juce::DropShadow`（blur 2）— 按下琴键上的音名圆盘 | `ContentSlot.cpp:823` |
| **0–26** | `drawButtonGlow` → `drawSoftShadow` — 点亮的 M/S 按钮 | `KeyboardRowPaint.cpp:119` |

**合计 26–65 次 gaussian 模糊 / 帧，30 fps 下是 780–1950 次 / 秒。**

## 三、根因，按开销从大到小

### 1. 每一行都做一次真的 gaussian 模糊（最大的一项）

```cpp
// SoftShadow.h:25
inline void drawSoftShadow (juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius,
                            juce::Colour colour, int blurRadius, juce::Point<int> offset)
{
    ...
    juce::Path path;
    path.addRoundedRectangle (area, cornerRadius);
    juce::DropShadow (colour, juce::jmax (1, blurRadius), offset).drawForPath (g, path);
}
```

`DropShadow::drawForPath` 在 JUCE 里是这样的：**新建一张 ARGB Image → 把路径画进 alpha → 用
`ImageConvolutionKernel::createGaussianBlur` 逐像素卷积 → 合成回目标**。

**每一次都是一次内存分配加一次二维卷积。** 一行一张卡、一张底座，13 行就是 26 次。

而 `SoftShadow.h` 自己的注释写着一句关键的话：

> The blur is skipped when the current repaint is entirely inside the shape - no shadow pixel can be
> visible there, so ordinary control repaints […] stay cheap.

**这个跳过只在「重绘区域完全落在形状内部」时生效。** 而这里每帧重绘的是**整个面板**，clip
是整块边界，永远不满足「落在形状内部」——所以**每一次都真的模糊**。

这段优化是为「重绘一小块」设计的，被「重绘全部」绕过了。

### 2. `paint()` 里每帧重新布局

```cpp
void ContentSlot::KeyboardPanel::paint (juce::Graphics& g)
{
    ...
    relayout ();        // ContentSlot.cpp:229   ← 在 paint 里
```

`relayout()`（L521）：

```cpp
const auto layout = keyboard::layoutRows (chipNamesFor (snapshot), getWidth (), scrollOffset, kHeader, metrics);
rows = std::move (layout.rows);
```

- `chipNamesFor()` 每帧构造一个 `std::vector<std::string>`，**把每个 `chipName` 拷一遍**；
- `layoutRows()` 每帧重算全部行的矩形，**重新分配 `rows`**。

**布局是「内容或宽度变了」才需要做的事，却挂在「每帧都要做」的路径上。** 绘制期间改布局本身
也是反模式：这一帧画的是「刚算出来的」rows，和上一帧 clip 对应的不是同一份数据。

### 3. 每帧整份拷贝快照

```cpp
void setSnapshot (const ChipStateSnapshot& snapshotToUse)   // L149
{
    snapshot = snapshotToUse;        // 深拷贝
```

`ChipStateSnapshot` 里是 `std::vector<ChannelState>`，每个 `ChannelState` 带
`std::string name`、`std::string chipName`、`std::vector<float> wave`。

**30 次/秒，把所有这些 string 和 vector 深拷贝一遍。**

### 4. 每帧重新构造波形路径与波形数据

```cpp
// KeyboardRowPaint.cpp:161
std::vector<float> cycle = channel.wave;              // 整份拷贝，无论后面用不用
if (cycle.empty ())
    cycle = waveform::cycleFor (channel.waveKind, channel.duty, channel.shapeParameter);
...
juce::Path path;                                      // 每帧新建
for (int i = 0; i < count; ++i) { path.lineTo (...); }
g.strokePath (path, juce::PathStrokeType (1.0f));
```

波形**只在 duty / waveKind / shapeParameter 改变时才会变**，但这里每帧都拷一遍数据、重建一遍
路径、再描一次边。`cycle = channel.wave` 这一行尤其浪费：多数情况下它是空的，拷的是空 vector，
但仍然每帧分配一次。

### 5. 每个分组标题每帧数一遍通道

```cpp
// ContentSlot.cpp:253
const int count = (int) std::count_if (snapshot.channels.begin (), snapshot.channels.end (),
                                       [&row] (const auto& channel)
                                       { return juce::String (channel.chipName) == row.title; });
```

**每个 header 每帧遍历全部通道，每次比较都构造一个 `juce::String`。** 通道数和分组都不会在一帧
之内变化。

### 6. 面板不是不透明的

`KeyboardPanel` 从未调用 `setOpaque (true)`。它的背景其实是纯色（`theme::card()`）。

**JUCE 对非不透明组件的处理是：先把它下面的父组件也重绘一遍。** 所以每帧的重绘面积实际上是
「ContentSlot 的背景 + 面板」，而不是「面板」。

## 四、为什么这个代价平时看不出来

- **只有 Keyboard 页面挂 30 Hz 定时器**，Mixer 和 Scope 不挂；
- 5% → 15% 是**绝对值 10 个百分点**，在独立显卡上不显眼，在核显或笔记本上就是风扇声；
- 没有任何一帧「画错了」，所以截图测试和 45 个功能测试**都不可能发现它**——这与
  `2026-10-26-a-font-name-nobody-could-find.md` 记的是同一类盲区：**测试验证的是「画对了没有」，
  不是「花多大代价画的」。**

## 五、建议的修法，按收益排序

### ① 把阴影挪出「每帧」这条路径（收益最大）

三个方向，可叠加：

- **静态部分缓存成 Image**：行的卡片底、键盘底座**只在尺寸或主题变化时**重画一次，用
  `juce::Image` 存起来，每帧只 `drawImageAt`。一行一张，13 行就是 13 次 blit 替代 26 次卷积。
- **或者干脆去掉行的卡片阴影**。`SoftShadow.h` 的注释说得很清楚：这张卡的材质和 mixer strip
  一样，但 mixer 是**静态**界面（只在交互时重绘），键盘是**每帧**界面。**同一套视觉词汇，放在
  每帧路径上就变成了不同的成本。**
- **让重绘变成脏区重绘**（见 ③），这样 `drawSoftShadow` 原有的「落在形状内部就跳过」优化
  才会真正生效——它是为这个场景写的，现在被整块重绘废掉了。

### ② 布局搬出 `paint()`

`relayout()` 只在三种情况下需要跑：**通道数变了、面板宽度变了、滚动位置变了**。
`rows` 已经是成员，加一个 `layoutDirty` 标志，在 `resized()`、`setSnapshot()` 发现通道数变化、
滚动这四处置位，`paint()` 里只读不建。

**顺带修掉「绘制期间改状态」这个反模式。**

### ③ 只重绘变化的部分

`timerCallback` 现在无条件 `keyboardPanel->repaint ()`。可以按行比较：

- 行的**静态部分**（卡片底、名称、M/S 板）只在 muted/soloed 变化时重绘；
- **动态部分**（音名、波形、琴键、电平条）才需要每帧刷新。

最省事的近似：**给每行一个「上一帧的 `ChannelState`」，只对变了的那几行重绘**
（`repaint (row.area)`）。曲子里十几个通道同时每帧都变的概率很低。

### ④ 快照用移动而非拷贝

`setSnapshot` 可以改成接受 `ChipStateSnapshot&&`，`timerCallback` 里的局部 `snapshot` 直接
`std::move` 进去；`captureChipState` 若是每个通道都填满，也可以返回引用/复用缓冲。
这一条是纯机械改动，风险最低。

### ⑤ 波形路径按参数缓存

`paintWave` 里以 `(waveKind, duty, shapeParameter)` 为键缓存 `juce::Path`，一行一份。
参数不变就不重建。`std::vector<float> cycle = channel.wave` 的整份拷贝可以直接改成引用。

### ⑥ 分组标题的计数缓存

`std::count_if` 的结果跟着 `snapshot.channels` 一起算一次，存在 `rows` 的 header 项里。

### ⑦ 把面板设成不透明

```cpp
setOpaque (true);          // 并在 paint() 开头 fillAll (theme::card ())
```

一行改动，省掉父组件的背景重绘。

## 六、怎么验证改完了

**这一次必须有可测的数字，不能靠「看起来流畅」。**

1. **帧时间**：在 `KeyboardPanel::paint()` 里用 `juce::Time::getHighResolutionTicks()` 记一次
   耗时，连续 300 帧取中位数与 p95。改前改后各测一次。
2. **计数**：临时在 `drawSoftShadow` 里加静态计数器，跑 10 秒打印总次数。**改前应该接近
   「行数 × 2 × 30 × 10」**，改后应该接近 0（除非主题或尺寸变化）。
3. **GPU**：用户环境的任务管理器，5% 对 15% 这个差异足够大，肉眼可判。
4. **回归**：`app.mixer-glow-shot` 和 `app.window-basics` 的截图比对——**优化不能改变像素**。

## 七、一句话

**这个视图是 30 Hz 刷新的，但它的绝大部分像素每帧都一样。**

现在的代码把「一张会动的琴键表」当成「整页重绘」来画，于是所有为静态卡片设计的昂贵操作
——gaussian 阴影、重新布局、深拷贝快照——都被拉进了每秒 30 次的循环。

**要做的不是把模糊调小，是让静态的东西只画一次。**
