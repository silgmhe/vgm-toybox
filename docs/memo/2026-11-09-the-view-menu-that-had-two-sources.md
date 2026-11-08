# 一个菜单，两个填写者：语言切换后 View 只剩 "Play"（2026-11-09）

**现象**：设置里切换语言后，`View` 菜单只剩一项 `Play`；切换前正常（三项：Mixer / Keyboard /
Oscilloscope）。退出重进即恢复，因此它可以一直藏着不被发现。

---

## 一、分析过程

1. **先看谁在填这个菜单。** `menu.viewNames` 是 `MainMenu` 的数据源，`grep viewNames` 命中两处：

   | 位置 | 来源 | 内容 |
   |---|---|---|
   | `PluginEditor.cpp` 构造函数 | `contentViews()`（`ui/ContentSlot.cpp` 的注册表） | `Mixer / Keyboard / Oscilloscope` |
   | `PluginEditor.cpp` 设置变化回调 | `viewStack.getViewNames()`（窗口的**页面**） | `Play`（只有一页） |

2. **为什么看起来都对。** 两者都返回 `juce::StringArray`，都叫 "View names"，都紧跟着一句
   `menu.viewIds = ...`。只有读它们的**语义**才能看出区别：一个说"内容区能显示什么"，一个说
   "窗口有几页"。构造函数注释甚至明确写了 "not pages of the window"，而回调里的赋值正好违反了
   它自己的注释。

3. **为什么只有切换语言后才坏。** 构造走第一条路（正确），语言切换走第二条路（错误），所以症状
   与"运行时重新填充"绑定；重启回到构造路径，于是"重进就好"。

4. **用 harness 复现**：`menu invoke options.settings` → `set settings.language.select 1` →
   `menu`。修复前三项变一项；修复后仍是三项，且标签变成 `混音器 / 键盘 / 示波器`。

## 二、根因

**同一份数据（View 菜单的内容）有两个写入点，语义却不同。**

- `menu.viewNames` 只有一个字段，却由两个地方按两套规则填写；
- 第二处填写用的 `ViewStack::getViewNames()` 本来是为"页面"设计的，名字与被填字段几乎一样，
  于是"看起来能编译、跑起来也像对的"；
- 两处的注释还都**反过来**描述了事实（`ViewStack` 类注释说"View 菜单由 `getViewNames()` 构建"，
  `MainMenu` 字段注释说"来自窗口的 ViewStack"），下一次修改只会照着错注释走。

这不是"漏了一个 `menuItemsChanged()`"这一类小疏忽，而是**数据所有权不清**：菜单的内容属于
`contentViews()` 这张表，`ViewStack` 的页面名是另一件事，两者没有从属关系。

## 三、解决方案

1. **把填写收进一个函数**：`PluginEditor::rebuildViewMenu()`，内容、id、两个回调（`getCurrentView`
   与 `onSelectView`）都在里面；构造函数与语言切换都只调它。于是"填什么"只有一个定义。
2. **删掉另一个来源**：`ViewStack::getViewNames()` 在修复后没有读者，删除；留下的 `getViewIds()`
   只服务"哪个页面在屏幕上"这一件事，注释也改成这个意思。
3. **订正两处反向注释**：`ViewStack` 类注释、`MainMenu` 字段注释，都写清"菜单列的是内容视图，
   不是页面"，并记下这次事故。
4. **加回归断言**：`Tests/scripts/language.txt` 中英各断言三项存在。并验证断言真的会失败——
   `assert menu.view.nothere ...` 报 `no menu entry`，说明它查的是真实菜单项而不是空过。

## 四、验证

```
启动            view.mixer / view.keyboard / view.oscilloscope
切到简体中文     混音器 / 键盘 / 示波器        ← 仍是三项
切回英文         Mixer / Keyboard / Oscilloscope
断言有效性       menu.view.nothere → FAIL: no menu entry
46/46 · 芯片断言 PASS
```

## 五、教训：一份数据只能有一个来源

这次与项目里已发生过的几件事是**同一个形状**：

| 事故 | 两个来源 |
|---|---|
| View 菜单只有 `Play`（本文件） | 构造函数读内容视图注册表，语言切换读页面名 |
| ADPCM-A 六行画斜坡、导出 0（`2026-11-09-the-image-that-was-too-small.md`） | 解码器要求与导出上限各写一份 `8 MB` |
| 键盘页行与 Mixer 不对应（`2026-11-08-a-row-needs-an-identity.md`） | `tracks` 与 `replayer->instances()` 各枚举一次 |

三条共用一个判据：**同一语义若有第二个写入点，就一定有一个停在旧值**，而且它通常"能编译、像对的、
只在某条路径上错"。

**动手前的三个问题**：

1. 这份数据（菜单、行、上限、颜色、文案键）**有几个写入点**？
2. 第二个写入点的**语义与第一个相同吗**？还是只是名字像？
3. 我能不能让第二个消失——收进一个函数/一个常量/一张表，并删掉另一条路径？

配套的规则条目：`.codebuddy/rules/code-quality.md` 第 13 条。
