# 按钮设计规范（Button Design Spec）

> 状态：**A–D 已拍板，实施中**（2026-10-16 定稿；M-UI.1/2/4 已实现，M-UI.3/5 待做）
> 范围：`plugin/Source/ui/` 下所有按钮类控件（图标按钮、文本按钮、切换按钮、下拉框外观）
> 参考：macOS Human Interface Guidelines（Buttons / Materials / Color / Layout）—— 借鉴**概念**，数值为本项目自定
> 关联：`docs/mixer-milestones.md`（M1/M2 视觉基线）、`Theme.h`（现有 token）

---

## 1. 目的

当前界面里存在**两套深度语言**：

- 滑杆拇指、旋钮、推子帽、勾选框 → **有**柔和投影（`theme::controlShadow`，blur 5），看起来"浮"在卡片上；
- 所有按钮（走带 Play/Pause/Stop、卡片头部图标按钮、混音 M/S、Open）→ **没有**任何阴影，只有一块白色圆角 + 1px 描边，看起来"贴"在卡片上。

于是出现了用户看到的现象：**按钮没有背景轻微高斯模糊的浮动感**。本规范给出一套统一的"材质 + 层级 + 状态"定义，让：

1. 现有按钮一次对齐；
2. **以后新增按钮不需要再做设计决策** —— 选一个级别、调一个 painter 即可。

---

## 2. 现状盘点（事实，来自当前代码）

| 控件 | 绘制位置 | 填充 | 边框 | 阴影 |
|---|---|---|---|---|
| 走带 Play/Pause/Stop/cursor/loop | `IconButton::paintButton` | `theme::card()`（白）／on 时 accent | 非 on 时 hairline | **无**（loop 的 on 态由 `TransportPanel::paintCardContent` 补画彩色 glow） |
| 混音开关（卡片右上） | `IconButton` | 白／on 时 accent | 非 on 时 hairline | **无**（on 态由 `MixerPanel::paintCardContent` 补画 glow） |
| 扇出／汇入 两个快捷按钮 | `IconButton` | 白／on 时 accent | 非 on 时 hairline | **无，且 on 态也没有 glow** ← 缺陷 |
| Open（顶部主操作） | `LookAndFeel::drawButtonBackground` | accent | 无 | **无** |
| 混音 M / S | 同上 | 白／on 时 mute 红 / solo 琥珀 | 非 on 时 hairline | **无** |
| 输出下拉、Loop/Fade 下拉 | `LookAndFeel::drawComboBox` | 白 | hairline | **无** |
| 滑杆拇指 / 旋钮 / 推子帽 / 勾选框 | `LookAndFeel` + `SoftShadow.h` | 白（推子帽为渐变） | hairline | **有** `controlShadow` |

由此暴露的 5 个问题：

1. **按钮无投影**，与滑杆/旋钮不一致（核心症状）。
2. **彩色 glow 是"父组件补画"的 ad-hoc 模式**：`TransportPanel` 与 `MixerPanel` 各自在 `paintCardContent()` 里判断 toggle 再画一次，并各自在状态变化时 `repaint(bounds.expanded(...))`；新增按钮时极易漏（两个快捷按钮已经漏了）。
3. **圆角规则三份且互不一致**：`IconButton` 固定 `buttonRadius=8`；`drawButtonBackground` 用 `高度<28 ? 0.2h : 8`；`drawComboBox` 硬编码 `6`；通道条输出行背景又用 `buttonRadius`。
4. **按下反馈只有亮度**（`darker(0.10)`），没有"抬升变化"，缺少物理感；悬停/按下逻辑在 `IconButton` 与 LookAndFeel 里各写一遍。
5. **完全没有 focus 视觉** —— 键盘用户无法看出焦点在哪个按钮上（macOS 有明确的 focus ring）。

---

## 3. 参考原则（macOS HIG 的取舍）

| HIG 概念 | 我们的用法 | 不能照搬的部分 |
|---|---|---|
| **Materials / Vibrancy** | 控件应"悬浮在内容之上"而不是贴在上面；用材质 + 阴影表达 | macOS 的**背景模糊**（backdrop blur）在 JUCE 与 VST3/AU 子视图里做不到，见 §4.1 |
| **Elevation（层级）** | 浅色模式下阴影**非常克制**：几 % 不透明度、小位移；层级越高阴影越大 | Apple 的具体数值不公开，下列数值为本项目自定 |
| **Prominence（主次）** | 主操作用强调色填充（Open、混音开关）；普通操作用中性材质；语义色只给状态（M 红 / S 琥珀） | — |
| **Corner radius** | 圆角与高度相关，保持"连续圆角"观感（同一行按钮圆角一致） | 不追求 squircle 数学曲线 |
| **Control size & hit target** | 尺寸分档固定；字号随高度（现有 `0.62 × h` 规则保留） | — |
| **States** | normal / hover / pressed / on / disabled / **focus** 六态齐全 | 不加入 macOS 的 focus ring 动画 |

---

## 4. 关键实现约束（决定规范如何落地）

### 4.1 没有真正的背景模糊

1. JUCE 没有 backdrop filter：`Component` 无法模糊"它下面已经画好的内容"；
2. VST3/AU 里插件是**宿主窗口的子视图**，即使平台原生毛玻璃（`NSVisualEffectView` / DWM）也只作用于窗口，不能作用于窗口内部的子控件；
3. 跨平台没有统一 API。

**结论**：用**三层合成**模拟"浮起"观感（按"简单优先、少占 GPU"定稿）：

```
① 单层柔和投影（blur 3、offsetY 1）        ← 主要来源：让按钮"离地"
② 平涂材质（纯白；填充按钮用调用方颜色）    ← 不做渐变，省一次画刷/色彩空间切换
③ 1px hairline 收边                        ← 浅色按钮在浅色卡片上的可读性
```

> 取舍记录：初稿曾设计"双层投影 + 渐变材质 + 顶部内高光"。按"简单优先、避免额外 GPU 开销"的要求**降级为单层 + 平涂** —— 这个尺寸下观感差别几乎不可见，而每去掉一层就少一次"生成图像 + 高斯模糊"（见 §4.4）。卡片本身是不透明白，所以材质的"透"本就只能靠描边与投影表达，这也是投影必须存在的原因。

### 4.2 组件被裁剪到自己的 bounds

JUCE 把每个子组件裁剪在自身 bounds 内 ⇒ **按钮要自己画投影，就必须在自己的 bounds 内预留空间**：

```cpp
inline constexpr int buttonShadowInset = 4;   // 见 §5
```

按钮的"可见形状" = `getLocalBounds().reduced(buttonShadowInset)`，投影在 inset 空间里绘制。这样：

- 任何父组件**都不需要**为按钮补画阴影，新增按钮零适配；
- 代价是按钮要多占 `2 × inset` 的空间（当前 4px）：**布局必须按"可见面 + 4"给尺寸**，否则按钮看起来就变小了 —— 这正是第一版把头部按钮从 22px 缩到 14px 的原因（见 §8 与实施记录）。

### 4.3 唯一的例外：彩色 glow（L2）

强调色 glow 的扩散半径（`accentGlowBlur = 12`）远大于 4px inset。两种处理：

| 方案 | 说明 | 评价 |
|---|---|---|
| A. 提高 inset 到 12 | 按钮自绘 glow，父组件完全不参与 | 按钮视觉缩到 10px，不可接受 |
| **B. 保持"父组件绘制"但统一 helper**（**建议**） | 卡片用一个 `paintFloatingGlow(button)` helper（内部判断 toggle + `repaint` 扩边），L2 按钮的 glow 全部走它 | 现状模式保留、但把 ad-hoc 收敛成一处，并覆盖漏掉的两个快捷按钮 |

> B 的规则写进规范：**"彩色发光由最近的卡片容器绘制；中性投影由按钮自绘"**。这条边界清楚，新按钮不会选错。

### 4.4 性能预算（"简单优先"的落地规则）

`juce::DropShadow::drawForPath()` **每次调用都会**：按"形状 + blur"建一张临时图像 → 做一次高斯模糊 → 再绘制。所以**投影层数 = 图像与模糊次数**。据此定下五条规则：

1. **只画一层投影**（`buttonShadowBlur = 3`：小半径 → 小图像）；
2. **不做渐变材质**，减少画刷与色彩空间切换；
3. 沿用 `drawSoftShadow()` 的**裁剪跳过**：重绘区域完全落在形状内、或与"形状 + 阴影"不相交时直接返回 —— 纯按钮重绘（状态变化、图标更新）几乎不付模糊代价；
4. **中性投影画在按钮自身 bounds 内** ⇒ 状态变化只重绘按钮自己，父组件零重绘（对比：彩色 glow 需父卡片按 `buttonGlowBounds()` 扩边重绘，仅少数按钮、且只在 on 时发生）；
5. 不使用 `setBufferedToImage`（小控件换来一张常驻图像，而每次状态变化都要重画它）。

---

## 5. Token（`Theme.h` 增量）

命名沿用现有风格（`inline` 函数 / `constexpr`）。

```cpp
// --- 材质（L1 中性）---
inline juce::Colour buttonMaterial (void)   { return juce::Colour (0xffffffff); }  // 平涂白
inline juce::Colour buttonHairline (void)   { return juce::Colour (0xffd0d0d8); }  // 收边
// --- 抬升（单层）---
inline juce::Colour buttonShadow (void)     { return juce::Colour (0x26000000); }  // ~15% 黑
inline constexpr int buttonShadowBlur     = 2;    // 紧贴的投影：可见面只能让出很少空间
inline constexpr int buttonShadowOffsetY  = 1;
inline constexpr int buttonShadowInset    = 2;    // 按钮自留的阴影空间（小控件按高度 15% 缩放）
// --- 状态系数 ---
inline constexpr float buttonHoverDarken   = 0.04f;
inline constexpr float buttonPressedDarken = 0.08f;
inline constexpr int buttonPressedBlurShift   = -2;   // 按下时投影收缩 = 位移感
inline constexpr int buttonPressedOffsetShift = -1;
inline constexpr float buttonDisabledAlpha     = 0.45f;
inline constexpr float buttonDisabledIconAlpha = 0.40f;
// --- 聚焦环（原来完全没有）---
inline juce::Colour focusRing (void)        { return juce::Colour (0x660a84ff); }  // accent 40%
inline constexpr float focusRingThickness   = 2.0f;
// --- 几何 ---
inline constexpr float iconInsetRatio = 0.28f;
inline float buttonRadiusFor (float height) { return juce::jmin (height * 0.2f, buttonRadius); }
```

**同时废止**：`drawComboBox` 里硬编码的 `6.0f`、`IconButton` 里固定用的 `buttonRadius`（随 M-UI.3 完成）。全部逻辑落在 `plugin/Source/ui/ButtonPaint.h`：`ButtonFace` / `drawButtonFace()` / `drawButtonGlow()` / `buttonGlowBounds()` / `buttonFaceShape()`。

---

## 6. 按钮分级

| 级别 | 用于 | 填充 | 边框 | 投影 | 文字/图标 |
|---|---|---|---|---|---|
| **L0 扁平** | 极低优先操作（未来的卡片内联操作、标题旁的弱按钮） | 无 | 无（hover 时 4% 黑） | 无 | secondaryText |
| **L1 标准**（默认） | 走带按钮、卡片头部图标按钮、输出下拉、Loop/Fade 下拉 | 白材质（平涂，§5） | hairline | 单层投影（§5） | primaryText |
| **L2 强调 / on** | Open、混音开关 on、Loop/Cursor on、M/S on、扇出/汇入 on | accent 或语义色（mute/solo）纯色 | 无 | **彩色 glow**（父卡片绘制，§4.3 B 方案） | 白（solo 用 primaryText，保持现状） |
| **L3 浮层** | PopupMenu、Tooltip 内的按钮 | 同 L1 | hairline | 仅 ambient（blur 2） | primaryText |

禁用态一律：填充与投影按 `buttonDisabledAlpha` 衰减、图标 `buttonDisabledIconAlpha`；**不使用灰色替换**（保持色相可辨）。

---

## 7. 状态与反馈

| 状态 | 填充 | 投影 | 说明 |
|---|---|---|---|
| normal | 100% | ambient 2 / key 6、offsetY 2 | 基准 |
| hover | `-buttonHoverDarken` | key blur +1 | 轻微"抬起" |
| pressed | `-buttonPressedDarken` | key blur −3、offsetY −1 | **投影收缩 = 按下去的位移感**（现在只有变暗） |
| on（toggle） | L2 纯色 | 彩色 glow | **切换按钮必须两者同时**（修掉快捷按钮缺 glow 的缺陷） |
| disabled | ×0.45 | ×0.45 | 图标 ×0.40 |
| focus | 不变 | 不变 | 形状外 `focusRingThickness` 圆环，距形状 `focusRingGap` |

---

## 8. 尺寸、圆角、图标

**尺寸档（沿用现值，规范只做"登记"）**

| 用途 | 尺寸（占位 / 可见面） | 位置 |
|---|---|---|
| 走带按钮 | 40 / **36**（`kButtonSize`） | `TransportPanel.cpp` |
| 卡片头部图标按钮 | 26 / **22**（`kHeaderHeight`） | `MixerPanel.cpp` |
| 混音 M/S | 28 / **24**（`kButtonHeight`） | `ChannelStrip.cpp` |
| 顶行主操作（Open） | 36 / **32**（`theme::topRowHeight`） | `PluginEditor.cpp` |
| 输出下拉 | 26 / **22**（`kOutputHeight`） | `ChannelStrip.cpp` |
| loop / fade 下拉（与音量同排） | 30 / **26**（设置行高） | `TransportPanel.cpp` |

> **占位 = 可见面 + 4**（`2 × buttonShadowInset`）：阴影空间由按钮自己保留，所以布局尺寸里要把它算进去 —— 第一版漏了这一步，头部按钮就从 22px 被挤成 14px。改按钮尺寸时请照此推算。

**圆角**：`buttonRadiusFor(height)`（§5），同一行内必须一致。
**图标**：图标区 = 形状 `reduced(宽 × iconInsetRatio, 高 × iconInsetRatio)`；描边类字形线宽 = 形状高 × 0.11（与 `Icons.cpp` 现状一致）。
**文本**：左右内边距 10；字号 `jlimit(10, 13.5, h × 0.62)`（保留现状）。

---

## 9. 组件映射与差距

| 组件 | 目标级别 | 需要的改动 |
|---|---|---|
| `IconButton`（走带 5 个） | L1 / on→L2 | 改用 `drawButtonFace()`：材质渐变 + 双层投影 + 内高光 + 顶部 inset；补 focus |
| `IconButton`（混音开关） | L1 / on→L2 | 同上；glow 改由统一 helper 绘制 |
| `IconButton`（扇出/汇入） | L1 / on→L2 | 同上 + **补上缺失的 on 态 glow** |
| `Open`（`TextButton` + accent） | L2 | `drawButtonBackground` 走同一 painter（纯色 + glow，不画材质/高光） |
| 混音 M/S | L1 / on→L2 | 同上；小尺寸走 `buttonRadiusFor` |
| 输出/loop/fade 下拉（`ComboBox`） | L1 | 圆角统一 + 材质/投影/内高光 |
| 滑杆/旋钮/推子/勾选框 | —（不在本次范围） | 保持，但 `controlShadow` 归入同一 token 家族命名 |

---

## 10. 实施计划（每步一提交、可独立回滚）

| 步 | 内容 | 验收 |
|---|---|---|
| **M-UI.1** | Token 增量（§5）+ 新 `ui/ButtonPaint.h`（`drawButtonFace(g, shape, level, state)`、`drawFloatingGlow(g, bounds, on)`）。**不改任何现有外观** | 编译通过、界面与之前逐像素一致（只加不换） |
| **M-UI.2** | `IconButton` 接入 painter（走带 5 个 + 卡片头部 3 个）→ **本次要解决的观感问题** | standalone 截图对照；hover/pressed/on/disabled/focus 六态肉眼确认 |
| **M-UI.3** | `LightLookAndFeel` 接入（`drawButtonBackground`、`drawComboBox`），圆角统一 | 同上 |
| **M-UI.4** | 收敛 ad-hoc glow 到 helper，修掉扇出/汇入缺 glow | 同上 |
| **M-UI.5** | 更新 `CODEBUDDY.md`、本文档补"实施记录" | 文档一致 |

**门禁（每步都要过）**：`vgm_mixer_test` 保持 **−1000.00 dB 逐样本一致**、`vgm_core_test` 全绿、VST3+standalone 构建与冒烟通过。UI 改动不碰音频路径，这两条正是防回归的保险。

### 实施记录

| 步 | 状态 | 说明 |
|---|---|---|
| M-UI.1 Token + `ButtonPaint.h` | ✅ | `Theme.h` 新增按钮 token（单层投影、平涂材质、聚焦环、`buttonRadiusFor`）；新文件 `ui/ButtonPaint.h` |
| M-UI.2 `IconButton` 接入 | ✅ | 走带 5 个 + 卡片头部 3 个统一走 `drawButtonFace()`；新增聚焦环（`focusGained/Lost` 触发重绘） |
| M-UI.4 收敛 glow | ✅ | `TransportPanel` / `MixerPanel` 改用 `drawButtonGlow()` + `buttonGlowBounds()`；**补上扇出/汇入一直缺失的 on 态发光** |
| M-UI.3 `LightLookAndFeel` 接入 | ✅ | `drawButtonBackground`（混音 M/S、Open）与 `drawComboBox`（输出/loop/fade）都改走 `drawButtonFace()`：填充色亮 → 中性材质、有色（accent/红/琥珀）→ 填充型；三份圆角规则合并为 `buttonRadiusFor()`；禁用态下拉的箭头也按规范衰减；布局同步补 4px（顶行 32→36、输出行 22→26、设置行 26→30、走带卡片 142、通道条 204、混音最小高 178） |
| M-UI.5 文档与视觉验收 | ⏳ | CODEBUDDY 更新、宿主内六态走查、可选的 `vgm_ui_test` 像素不变量 |

---

## 11. 验收

**手动清单**（standalone + 宿主各一遍）

1. 走带 5 个按钮：默认能看到底部柔和投影与上缘高光；鼠标悬停略变暗且投影略大；按下时按钮"陷下去"（投影收缩）；Loop/Cursor 打开时 accent 填充 + 彩色发光；
2. 卡片头部 3 个按钮同上一套表现；扇出/汇入打开时**也有发光**（当前缺陷）；
3. M/S：未按下为白材质浮起；按下为红/琥珀 + 发光；
4. Open：accent 填充 + 发光；
5. 下拉：圆角与相邻按钮一致、有投影；
6. 混音器关闭时（M/S 禁用）按钮明显变"淡"但仍可辨识；
7. Tab 键走查：每个按钮都能看到焦点环；
8. 宿主里 100%/150%/200% DPI 各看一次，投影不糊、不越界。

**可选自动化**（建议做）：`vgm_ui_test` —— 把按钮渲染进 `juce::Image` 做**像素不变量**断言：形状内部亮度 > 形状外紧邻像素（证明有投影）、四角透明（证明圆角与 inset）、pressed 态投影弱于 normal、disabled 态整体 alpha 更低。这类"阴影是否存在"的回归肉眼很难查，值得自动化。

---

## 12. 非目标

- 不做真正的背景模糊（§4.1）；
- 不做深色主题（浅色是本项目既定风格）；
- 不改布局结构、不改按钮尺寸（仅新增 `buttonShadowInset` 内缩）；
- 不引入动画/时长体系（状态切换仍为即时）。

---

## 13. 参考

- macOS Human Interface Guidelines — <https://developer.apple.com/design/human-interface-guidelines>（Buttons、Materials、Color、Layout 章节，概念参考）
- 本项目既有实现：`plugin/Source/ui/Theme.h`、`SoftShadow.h`、`Icons.cpp`、`LightLookAndFeel.cpp`
