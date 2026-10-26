# 静止的窗口为什么也在花 GPU

> 三个观察：**最大化时 GPU 飙升、缩小时下降**；**什么都不做也占 5%**；**比同类的即时模式
> GUI 高得多，甚至高过静态的浏览器**。
>
> 第二条和第三条是同一件事的两面，第一条指出了它的形状：**开销跟着面积走，不跟着内容走。**

## 一、面积效应说明了什么

一个界面的开销如果与**窗口面积**成正比，而不是与**变化的元素个数**成正比，那它就不是在
"画变化的东西"，而是在"每次提交整块画面"。

这有两种成因，都必须排除：

1. **每次重绘都覆盖整个窗口**（脏区信息丢失）；
2. **每次提交都整窗上传给合成器**（分层窗口 / layered path）。

本项目两者都有嫌疑，见下。

## 二、已知的三条驱动路径

### 1. 窗口自身：10 Hz 与 15 Hz

```cpp
// PluginEditor.cpp:20
constexpr int kPlayingRefreshHz = 15;
constexpr int kIdleRefreshHz = 10;
```

`timerCallback()`（L646）→ `refreshUi()`（L695）→ `ViewStack::refresh()` + `updateStatusLine()`。

`ViewStack::refresh()`（`ViewStack.h:107`）只转发给当前页的 `view->refresh()`，**不 repaint**。
`TransportPanel::refresh()`（`TransportPanel.cpp:657`）的每一次 repaint 都是**有条件的**——
值变了才画（L709、L714、L722、L731、L742、L755）。**所以静止时这条路径不该产生重绘。**

### 2. 内容槽：30 Hz，但只在 Keyboard 页

```cpp
// ContentSlot.cpp:1335
startTimerHz (kRefreshHz);        // 30
```

`timerCallback()` 的第一句就是 `if (current != ContentView::keyboard || ! keyboardPanel->isVisible ()) return;`。
**其他页面不付这个钱。** 这条已经在前一轮处理过（见 `2026-10-26-keyboard-view-gpu-cost.md`）。

### 3. 窗口的 `paint()`：整窗渐变 + 所有卡片阴影

```cpp
// PluginEditor.cpp:275
void ToyboxAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (theme::windowTop(), 0.0f, 0.0f,
                                             theme::windowBottom(), 0.0f, (float) getHeight(), false));
    g.fillRect (getLocalBounds ());          // ← 整窗渐变

    if (auto* page = viewStack.getCurrentView ())
    {
        ...
        page->paintCardShadows (g);          // ← 每张卡的 soft shadow（真正的 gaussian）
    }
```

**这条路径本身不是每帧调用的。** 但它很贵，而且——见下一节——它可能被**任何**子控件的
一次重绘拉进来。

## 三、`setOpaque` 的尝试，以及它为什么失败

`ToyboxAudioProcessorEditor` 从未调用 `setOpaque(true)`。默认的 `false` 意味着 JUCE 认为这个
组件下面是透的，于是：

- 在 Windows 上，顶层 peer 倾向走 **layered 路径**（每次 present 交整窗 ARGB 位图），
  这正好产生"开销与面积成正比"；
- 子控件的重绘会被当作"需要连同下面的内容一起重画"。

看起来一行就能解决。**我试了，然后撤回了。**

```
加入 setOpaque (true) 之后，同一张截图的最大单通道差 = 153（窗口范围几乎全覆盖）
```

**这说明 `paint()` 并没有画满每一个像素**，尽管它第一行是 `g.fillRect (getLocalBounds ())`。
`setOpaque(true)` 告诉 JUCE"不必预填背景"，于是那些没被覆盖的像素露了出来——在快照里是
透明，在真窗口里就是**未定义内容**（残留、黑块或闪烁）。

**所以这条路不是不能走，而是要先弄清哪些像素没被画。** 候选：

- `paintCardShadows` 用 `addTransform` 平移到页面坐标后绘制，可能落在编辑器 `paint` 的
  渐变之外；
- 页面切换的瞬间，`ViewStack` 与新页面的矩形未必重合。

**在弄清之前，`setOpaque(true)` 会换来看不见的故障。**

## 四、和 JUCE 的关系：它没错，但它的模型有代价

用户的直觉有一半是对的。JUCE 不是即时模式 GUI，它的 repaint 是**脏区 + 事件驱动**，本身
很省。代价在别处：

| | 即时模式（ImGui 类） | JUCE |
|---|---|---|
| 一帧的定义 | 主动画一整帧 | 有脏区才画，**没有帧的概念** |
| 提交 | 每帧一次 swap chain | **每次脏区重绘一次 present** |
| 与合成器 | 硬件合成，vsync 对齐 | 多次 present，**不与 vsync 对齐** |

**"每次脏区重绘一次 present"是关键。** 一个 15 Hz 的 seek bar 更新，如果每次都让窗口提交
一次整窗位图（layered 路径），那就是每秒 15 次全窗口合成——**和画面有没有变无关，只和
提交次数有关。** 这也解释了为什么它比静态浏览器高：浏览器静止时**一次都不提交**。

## 五、下一步该怎么查（按代价排序）

**1. 先量，不要再猜。**

在 `ToyboxAudioProcessorEditor::paint()` 里加一个静态计数与累计耗时，进程退出时打到
`juce::Logger`；在 `KeyboardPanel::paint()` 同样加一份。静止一分钟，看**被调用了多少次**。

- 如果静止时是 **0 次** → 问题在提交层（layered / peer），不在绘制层；
- 如果是 **600 次**（10 Hz）→ 有东西在每帧 repaint，顺着计数往上一层查。

**这一步必须先做**，因为上面三条路径都"看起来不该每帧跑"，而 GPU 说有人在跑。

**2. 确认真窗口的 peer 路径。**

`juce::ComponentPeer::isOpaque()` 只对**顶层窗口**有意义。`AudioProcessorEditor` 是
`contentComponent`，不是顶层——所以对**编辑器**调 `setOpaque(true)` 未必能改变 peer 的
分层决策。**真正要问的是 `StandaloneFilterWindow` 是否 opaque**，以及 JUCE 版本里
`HWNDComponentPeer` 在 `!isOpaque` 时是否真的走 `UpdateLayeredWindow`。

**3. 之后再考虑把 `paintCardShadows` 缓存成 Image**（做法与键盘那轮相同）。
它现在是"每次编辑器重绘 → N 张卡 × 2 层 gaussian"。一旦 `paint()` 被高频调用，
这一项就是主犯；如果 `paint()` 一次都没被调，它一分钱都不花。

## 六、结论

**能确证的**：

- 静止时的 10 Hz 定时器**不该**产生重绘，代码里每处 repaint 都是条件性的；
- 30 Hz 的定时器只在 Keyboard 页生效；
- `paint()` 的整窗渐变与卡片阴影很贵，但**没有证据表明它每帧被调用**；
- `setOpaque(true)` 现在还不能加——加了会露出未绘制区域。

**还不能确证的**：那 5% 究竟来自"高频 paint"还是"每次 present 的整窗上传"。

**这两者的修法完全不同**，所以下一步是**加计数**，不是继续改绘制代码。

**在没有这个数字之前，任何"优化"都只是把不知道的东西改得更复杂。**
