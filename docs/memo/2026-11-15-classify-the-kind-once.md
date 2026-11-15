# 一颗芯片，改几处？

**日期**：2026-11-15
**背景**：`docs/review/2026-11-15-ux-polish-feature-review.md` §6.1 的整改
**提交**：`refactor(chips): one table classifies a channel kind`

---

## 一、问题：同一份 41 项的判断，写了两遍

给通道名加类型图标时，需要回答"这个通道是什么声音"。答案在 `chips::ChannelKind`（41 个值）里，但两个消费者各自把它折成 7 类：

```cpp
// playback/ChannelFromChip.cpp —— 键琴页和示波器画什么形状
int waveKindForChannelKind (chips::ChannelKind kind);

// ui/ChannelGlyphs.h —— 混音台和键盘页画哪个标记
inline ChannelGlyph channelGlyphFor (chips::ChannelKind kind);
```

两份 `switch`，各 41 个 `case`，各自分组。**加一颗芯片要改两处**，而且：

- 项目**没有 `/WX`**，漏一个 `case` 只是 `-Wswitch` 警告；
- 两份 switch 都有静默兜底（`Kind::none` / `ChannelGlyph::pulse`）；
- 于是漏掉的那一处**编译通过、测试全绿、图标画错**。

这不是假想的风险：N163 的 `tableWave` 缺失、SSG 的 kind 被 FM 实例回答，两次都是"某一处没跟上，没有任何东西报错"。

## 二、解法：把"是什么"从"画成什么"里拆出来

新增一个**族**枚举，成为唯一的分类表：

```cpp
enum class ChannelFamily : uint8_t
{
	none = 0, pulse, triangle, saw, noise, fm, wavetable, pcm
};

constexpr ChannelFamily familyOf (ChannelKind kind);		// 41 项，唯一的表
```

两个消费者都从它派生：

```cpp
// 绘制形状
switch (chips::familyOf (kind)) { case pulse: return Kind::pulse; ... }

// 类型标记
using ChannelGlyph = chips::ChannelFamily;			// 不再有自己的枚举
```

**`wavetable` 与 `pcm` 在这一层就分开了**，而这正是当初写两份 switch 的动机——`waveform::Kind` 把两者并成 `stored`（对画波形而言它们都是"从内存读"），但读 strip 的人想知道"这是合成器还是采样"。族在他们的上游做这个区分，下游各自取用。

## 三、一个不对齐的地方，和它的处理

`nesDmc` 是唯一"族"与"形状"不一致的：

| 问题 | 答案 |
|---|---|
| 它是采样吗？ | 是，1-bit 差分采样 → 族 = `pcm` |
| 画成什么？ | 阶梯（`delta`）——那是 DMC 实际产生的波形 |

所以 `waveKindForChannelKind()` 保留了**一个显式特例**，其余全部由族推出：

```cpp
// The one kind whose drawn shape is not its family.
if (kind == chips::ChannelKind::nesDmc)
	return (int) Kind::delta;
```

一处特例 + 一张 7 项表，比两份 41 项表容易核对。

## 四、收益：从 5 处（2 处静默）到 3 处（0 处静默）

| # | 位置 | 漏了会怎样 |
|---|---|---|
| 1 | `ChipTypes.h` 的 `ChannelKind` | 编译失败 |
| 2 | decoder 的 `channelKind()` | 返回 `unknown`，行无图标 |
| 3 | **`familyOf()` 的一个 `case`** | **`assert` 失败（Debug）** |

`familyOf` 的兜底是 `assert(false && "ChannelKind has a new value; add it to familyOf()")`，而不是静默返回一个值。这是评审 H-1 的同一个结论：**一个"穷举"的 switch，只有在漏项会响的时候才是穷举的**。

## 五、代价与取舍

**`ChannelGlyph` 保留为别名，没有删掉。**

生成代码（`ChannelGlyphs.generated.cpp`）由 `svg_to_channel_glyphs.py` 产出，里面写着 `case ChannelGlyph::pulse:`。改成 `ChannelFamily::pulse` 只是重跑一次脚本，但会让 315 行生成文件整体 churn，而别名让这件事零成本：

```cpp
using ChannelGlyph = chips::ChannelFamily;
```

别名不是第二份数据——它是**同一个类型的另一个名字**，两者不可能分叉。

**`familyOf` 放在 `chips/ChipTypes.h` 而不是 `playback/`。**

它回答的是"这个通道是什么"，与芯片本身同层；放在 `playback/` 会让 `ui/` 依赖播放层才能知道族的名字，方向是反的。

## 六、验证

| 检查 | 结果 |
|---|---|
| `ctest` | **54/54** |
| `vgm_chip_test` | 通过——**41 项波形映射被整段重写，断言一条没改**，这是最强的回归证据 |
| 截图 | 2A03 的 `∏ ∧ ∿ ⊔` 与 N163 的波表标记与重写前逐像素一致 |

## 七、一句话

> 同一个判断被两个消费者需要时，**把判断本身变成一个类型**，而不是让两边各抄一遍。
>
> 抄的那份不会报错——它只会在某一天画错，而那天没有人会想起它。
