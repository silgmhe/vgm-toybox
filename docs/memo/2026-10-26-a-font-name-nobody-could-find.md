# 一个字体名，没有人认识它

> 起因：0.6.0 发布后，KDE 桌面上程序窗口一切正常——按钮在、卡片在、颜色对、布局对——
> **一个字都没有**。0.2.1 在同一台机器上完全正常。

## 一、症状为什么容易看错

**不是"界面对了"，是"界面只剩形状"。**

一排按钮有圆角、有阴影、有悬停区，点亮时会变色；卡片有边框和背景；禁用态是灰的。**这些全都不需要字形**。缺的只有字符本身，而字符是最后才被注意到的部分——人眼先看到"这个按钮在不在"。

所以第一反应会是字体没装、freetype 版本不对、或者打包漏了资源文件。**三个都是错的。**

## 二、根因

`plugin/Source/ui/UiFont.cpp` 决定界面用哪个字体族，它有**两条路径，只有一条做了检查**：

```cpp
// custom：查到了才用
for (const auto& installed : juce::Font::findAllTypefaceNames())
    if (installed.equalsIgnoreCase (requested)) { return installed; }
return systemUiFamily();

// 系统默认：拿到什么用什么          ← 问题在这里
if (auto typeface = juce::Typeface::findSystemTypeface())
{
    const auto name = typeface->getName();
    if (name.isNotEmpty()) { return name; }        // 没有验证
}
```

然后：

```cpp
juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName (family);
```

**`setDefaultSansSerifTypefaceName()` 收到一个查不到的族名时不会回退**——它找不到 typeface，于是每个字形都画成空。**这个 API 的失败模式是静默的**：不抛异常，不打日志，不警告。它就是什么都不画。

## 三、用户的机器上发生了什么

```
fc-match sans-serif  →  NotoSansCJK-Regular.ttc: "Noto Sans CJK SC"
fc-list : family     →  C059, DejaVu Sans, DejaVu Sans Mono, Droid Sans Fallback, IBM Plex Sans, ...
```

**两条命令给出的是同一个概念的两个名字，而它们对不上。**

`fc-match` 返回的是**一个 `.ttc` 集合里某个 face 的名字**；`fc-list` 列的是**族名**。`typeface->getName()` 拿到前者，`setDefaultSansSerifTypefaceName()` 按后者去查。**查不到，全空白。**

## 四、为什么三个平台只有 KDE 出问题

| 环境 | `fc-match sans-serif` | 结果 |
|---|---|---|
| Windows | `Segoe UI` | 既是 typeface 名也是族名 → 正常 |
| WSL / Debian | `DejaVuSans.ttf: "DejaVu Sans"` | 同上 → 正常 |
| **KDE（用户）** | `NotoSansCJK-Regular.ttc: "Noto Sans CJK SC"` | **名字对不上** → 空白 |

**所以这不是"Linux 的问题"，是"字体配置恰好不同的那一个桌面的问题"。** WSL 里无论怎么测都不会复现——那里的 `fc-match` 给出的是个 `.ttf`。

**这也是为什么它躲过了 CI**：三个平台的 runner 都装了普通的 `.ttf` 字体。

## 五、为什么测试没抓到——最值得记的一条

`app.window-basics` 里有这么一行：

```
assert editor.status  text contains "No file"
```

**它断言的是组件的 `text` 属性，也就是字符串内容。不是"这个字符串被画出来了"。**

`text` 属性和字形渲染是两件事：

| 层 | 谁负责 | 测试覆盖 |
|---|---|---|
| 字符串赋值 | 应用代码 | ✅ 断言了 |
| 布局与可见性 | JUCE | ✅ 部分断言了 |
| **字形查找与绘制** | **LookAndFeel + FreeType** | ❌ **没有任何断言** |

**45 个测试全绿，和一个字都画不出来，完全不矛盾。** 测试问的是"这个控件里存了什么字"，屏幕上发生的是"这些字有没有被画出来"。

## 六、0.2.1 为什么正常

**它根本没有 `UiFont.cpp`**——那个文件是 0.5.0「选择界面字体」时加的。

0.2.1 从不调用 `setDefaultSansSerifTypefaceName`，用 JUCE 自己的默认，而那条路总是能解析。**一个功能加进来，把一条一直安全的路径换成了自己解析的路径。**

## 七、根子上的问题：这一层本不该存在

```cpp
FontMode::systemUI  →  systemUiFamily()
                    →  Typeface::findSystemTypeface()->getName()
                    →  setDefaultSansSerifTypefaceName(那个名字)
```

**"使用系统默认"被实现成了"查一个名字，再把名字设进去"。**

而 **JUCE 本来就有默认，不设置就是系统默认**。这一层多余的自解析没有换来任何东西——它只是把"JUCE 知道怎么解析"换成了"我们自己猜一个名字"。

## 八、改成了什么

两条路径都验证，都不匹配就返回空；`applyStoredSelection()` 只在非空时设置：

```cpp
if (family.isNotEmpty())
    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName (family);
```

**空 = 什么都不设置，让 JUCE 用自己的默认。**

顺带删掉了原来的回退 `Font::getSystemUIFontName()`——它上面的注释自己就写着「LookAndFeel 可能不认识这个名字」，**而设置一个不认识的名字正是通向同一张空白窗口的另一条路**。

## 九、规则

**1. 一个"使用默认"的选项，不该有实现。**

如果某个选项的含义是"让库自己决定"，那么正确的实现通常是**什么都不做**。任何"查一下、再设回去"的写法都在增加一个可能出错的环节，而它换来的信息量为零。

**2. 同一个文件里两条路径，要检查同样多。**

`custom` 分支验证了，`systemUI` 分支没有——**这是"同一件事写两遍"的又一个实例**（参见 `2026-10-25-the-second-list.md`），只不过这次两遍在同一份文件、相隔二十行。

**3. 断言字符串，不等于断言它被画出来。**

`text contains "No file"` 是有效的测试，**但它测的不是渲染**。任何"文字/图标/颜色不可见"的缺陷都不在这类断言的能力范围内。

要覆盖这一层只有两条路：**截图比对**（`ui-visual-check.md` 那条规则），或者**直接断言字体解析的结果**——比如"`resolvedFamily()` 返回的名字必须在 `findAllTypefaceNames()` 里，或者为空"。后者更便宜，且能进单元测试。

**4. 平台差异只在"和别的平台不一样"的地方暴露。**

CI 跑三个平台不等于覆盖了真实桌面。**同一个 Linux，KDE 和 Debian 容器的 `fc-match` 给出的东西不一样**——而缺陷恰好住在这个差别里。
