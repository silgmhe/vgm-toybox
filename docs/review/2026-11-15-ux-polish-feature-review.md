# UX 打磨 feature 评审 · 及架构可持续性评估

**评审日期**：2026-11-15
**评审对象**：`feature/ux-polish`（相对 `master`：27 提交 / 67 文件 / +3 001 −364）
**评审基线**：`d25d75e`
**依据**：`.codebuddy/rules/code-quality.md`（13 条 + 提交前清单）

---

## 结论

**可以合并，但有 3 处必须在合并前还掉的债。**

三处有一个共同形状：**代码承诺了一件它做不到的事**。注释说"新增 kind 会编译失败"，实际是静默降级；排版说"居中，放不下换一档"，实际是被五种名字的宽度各撞一次；提交标题说 `refactor`，diff 里删掉了一条行为分支。它们都不会让 `54/54` 变红，这也正是它们的危险之处。

| 级别 | 数量 | 摘要 |
|---|---|---|
| **H（必须修）** | 3 | 编译期保护的承诺落空；排版逻辑的不可测已导致五次返工；`refactor` 夹带行为变更 |
| **M（应修）** | 5 | UI 缓存塞进播放器；SSG 规则两份实现；映射无断言；两次 revert；提交说明含未经证实的结论 |
| **L（可延后）** | 3 | 菜单与设置共用文案键的耦合未落笔；生成脚本无"忘跑"检查；两层状态转发只有一处调用 |

**对"架构是否可持续"的判断**：**主干是健康的，边缘在老化。**

- 健康的：`SyncedComboBox` 是这轮最好的抽象（把五个控件的同一个竞态收进一个类）；颜色零字面量；死代码删得干净；i18n 复用而非另起一套。
- 老化的：**新增一颗芯片现在要改 5 处，其中 3 处漏了不报错**；**UI 的需求正在往播放器里塞缓存**（`kindTable` + `kindsCollected`）。这两条都不紧急，但它们是下一轮 feature 的成本来源。

**一句话**：这一轮把"看得见的东西"做对了，代价是**验证方式仍是眼睛**——`ChannelStrip` 的排版改了五次，两次是 revert，因为没有任何断言能告诉作者"这一档什么时候生效"。

---

## 一、评审方法

| 项 | 做法 |
|---|---|
| 静态审查 | 逐文件读；27 个提交逐个看标题与 diff 的一致性 |
| 规范自检 | 跑 `code-quality.md` §7 的字面量颜色命令、`TEMP/TODO/FIXME` 全仓扫描 |
| 量化 | 改动行数、revert 次数、同一段代码的修改轮数、新增一颗芯片的改动点计数 |
| 运行验证 | `54/54`、`vgm_chip_test`、`vgm_ui_test`、`vgm_app_test` 截图 |
| 回溯 | 读提交说明与 revert 说明，判断"当时是否本可以一次做对" |

**自检结果**：字面量颜色 **0 处**（`Theme.cpp/h` 外），`TEMP-PROBE` **0 处**，`TODO/FIXME` **0 处**。这三项是干净的，值得点出。

---

## 二、必须修（H）

### H-1 `ChannelGlyphs.h` 承诺编译期保护，实际是静默兜底（规则 2、5）

头文件的注释写得很明确：

```43:46:plugin/Source/ui/ChannelGlyphs.h
	`delta` (the NES DMC) lands on `pcm`: it is a 1-bit sampled channel. `fmDrum` / `adpcmDrum` ride
	with `fm` / `pcm` because the channel name already says "drum". Exhaustive on purpose - no
	`default`, so adding a kind to the enum fails to compile here rather than drawing nothing.
```

**这句话一半是真的，一半是假的。**

真的部分：确实没有 `default`。
假的部分：**新增一个 `ChannelKind` 不会编译失败**。

```107:111:plugin/Source/ui/ChannelGlyphs.h
	case chips::ChannelKind::unknown:
		break;
	}

	return ChannelGlyph::pulse;		// unreachable; the switch is exhaustive
```

`-Wswitch` 只会产生**警告**，而本项目**没有 `/WX`**（`CMakeLists.txt`、`plugin/CMakeLists.txt` 均无 `WARNINGS_AS_ERRORS`）。于是新增 kind 时：编译通过 → 落到 `return ChannelGlyph::pulse` → **一个新芯片的所有通道都画成方波**，且注释还那句话还在说"会编译失败"。

那行注释 `// unreachable; the switch is exhaustive` 也是同一个问题：它在描述作者当时的心智状态，不是在描述代码。

**为什么算 H**：下一个给 `ChannelKind` 加值的人（很可能就是本项目的下一位 contributor）会相信这句注释，于是不去检查这个文件。规范 §5 说"已知缺陷不要用补丁掩盖"，这里更严重——**注释本身就是那个补丁**。

**修法**（二选一，都很小）：

1. 把兜底改成 `jassertfalse; return ChannelGlyph::pulse;`，并把注释改成"新增 kind 时这里会断言失败（Debug）或画成方波（Release），二者都需要你回来补一行"；
2. 或者给 `channelGlyphFor` 配一条测试，遍历 `ChannelKind` 的所有值并断言"不返回默认值"——但这条测试无法区分"故意映射到 pulse"与"漏了落到 pulse"，**所以方案 1 更好**。

顺带：`waveKindForChannelKind()`（`ChannelFromChip.cpp:11`）是同一形状的第二处。两处都改，或者都不改并写清"这里没有编译期保护"。

---

### H-2 `ChannelStrip` 的排版逻辑不可测，已经因此返工五次（规则 6、0）

当前 `paint()` 里的名字段是这个形状：

```565:620:plugin/Source/ui/ChannelStrip.cpp（节选）
	if (channelKind != chips::ChannelKind::unknown)
	{
		constexpr int kMarkSize = 16;
		constexpr int kMarkGap = 4;

		juce::GlyphArrangement arrangement;
		arrangement.addLineOfText (font, stripName, 0.0f, 0.0f);
		const float textWidth = arrangement.getBoundingBox (0, -1, true).getWidth ();

		const float textStart = nameBounds.getCentreX () - textWidth * 0.5f;
		const float markX = textStart - kMarkGap - kMarkSize;

		if (textWidth <= nameBounds.getWidth () && markX >= nameBounds.getX ())
			{ /* 图标贴文字 + 文字居中 */ }
		else
			{ /* 图标贴边 + 小一号 14px + 文字缩 */ }
	}
```

**三档排版**（整体居中 / 贴边 14px / 无图标）由两个浮点比较决定，而这两个比较**没有任何测试能调用**——它埋在 `paint()` 里，需要一个 `Graphics` 才能走进去。

代价是可以数出来的：

| 提交 | 做了什么 | 结果 |
|---|---|---|
| `8ee6aa3` | 图标 + 名字作为整体居中 | 名字偏离中线 |
| `e604752` | 改成"文字居中、图标贴文字左" | 放不下时丢图标 |
| `ed45b29` | 字号 10.5→11.5、图标 16→18 | 全部走兜底，五个 strip 无图标 |
| `79a8156` | **revert `ed45b29`** | 回到 `e604752` |
| `0f8f0ea` | 兜底也画图标，但 16px 把 `ADPCM-A 1` 截成 `ADPCM` | 编号丢失 |
| （当前） | 兜底用小一号 14px | — |

**六次提交、其中一次是撤销上一次，同一段代码。** 而 `code-quality.md` §6 的反例写的正是这件事：

> 音域跨度、行布局、波形合成曾埋在 private 嵌套类里，只能"画出来看"，**一晚因此返工 7 次**。

这一轮重演了，只是数字小一点。

**修法**：把"名字行怎么排"抽成一个纯函数，输入是**可传参的**，输出是几个矩形：

```cpp
struct ChannelNameLayout
{
	juce::Rectangle<float> mark;			// empty when there is no mark
	juce::Rectangle<float> text;
	float markSize = 0.0f;
	bool shrunk = false;					// true when the fallback took over
};

/** Pure: no Graphics, no Component. `textWidth` is measured by the caller, which is the only
	thing that needs a font. */
ChannelNameLayout layOutChannelName (juce::Rectangle<float> row, float textWidth, bool hasMark);
```

有了它，"`ADPCM-A 1`（约 55 px）在 76 px 行里应该走哪一档""短名字是否仍居中""mark 的右边缘是否恰好等于文字左边缘减 gap"全部可以断言。`paint()` 只剩 `g.drawText (layOut.text, ...)`。

**这条如果只能修一处，修它。** 它一次性消掉 H-2、M-3 与未来的第五次返工。

---

### H-3 `a68184c` 标题是 `refactor`，diff 里删掉了一条行为分支（规则 9）

```
refactor(menu): every Options category is a submenu, including the one with a single row
```

diff 删掉的是这段：

```cpp
// A submenu that opens to a single row is one click for nothing: in a plugin build only
// the paint-stats switch survives, so it moves up to the top level there.
if (general.children.size () > 1)        { options.entries.push_back (std::move (general)); }
else if (general.children.size () == 1)  { options.entries.push_back (std::move (general.children.front ())); }
```

**删掉它改变了运行时行为**：插件构建下，`General` 从"顶层平铺一项"变成"子菜单里一项"。

提交说明里确实解释了这件事（"a group that flattens out when this build happens to have one row would be a shape that changes with the build"），方向也是对的——**但标题是 `refactor`**。规范 §9 的原话：

> 标题写着 refactor / split / rename 的提交，**不允许同时改变运行时行为**。

以及那个真实事故：`cbaf588` 把掩码改动夹在 `refactor` 里，结果 YM2610 的 SSG 漏进 FM 1-3 轨道，**直到一位听众报"FM 上有 DPCM 串音"才被发现**。

**为什么这里更该算 H**：菜单结构是用户唯一能看见的功能，而 `refactor` 这个标签等于告诉评审"不用看行为"。这次它是好的改动，但**下一次不会**。

**修法**：标题改成 `feat(menu): every Options category is a submenu`，或拆成两个提交（先 `refactor` 抽 `addToggle`，再 `feat` 改结构）。历史已推，改标题需要 force-push——**不建议**。在合并说明里单列一段即可，同时把这条记进 `docs/memo/`。

---

## 三、应修（M）

### M-1 `kindTable` 是 UI 的缓存，塞进了播放器（规则 3、11）

```203:207:plugin/Source/backends/libvgm/LibvgmTrackPlayer.h
	mutable std::map<std::pair<uint32_t, bool>, std::vector<chips::ChannelKind>>
		kindTable;

	mutable bool kindsCollected = false;
```

三件事值得记下来：

1. **它是 UI 需求的产物**——为了在 strip 上画图标，把一个"所有设备的通道类型快照"缓存在播放器里；
2. **它 `mutable`**，因为 `channelKindFor()` 是 `const` 而它要写缓存——`const` 在这里不再表示"不修改状态"，只表示"不修改音频状态"；
3. **它永不失效**。`kindsCollected` 一旦为真，换歌也不会重建。

第 3 点现在**是安全的**，因为 `LibvgmSongInstance` 每首歌新建、其中的 `probe` 也随之新建。但**这是一个隐式契约**：任何一次"复用 player 以省一次构造"的优化，都会让图标静默停在上一首曲子的类型上，而且不会崩、不会红、只会画错。

**修法**（按代价排序）：

- 便宜：在 `setReplaySource()` 里 `kindsCollected = false; kindTable.clear();`——把隐式契约变成代码；
- 更好：把这张表挪到 `LibvgmSongInstance`（它拥有歌曲的生命周期），`channelKindFor` 改成一次性返回 `std::vector<chips::ChannelKind>`；
- 最好：`TrackInfo::channelKind` 既然是**枚举时**就要用的东西，就让**枚举**负责它——现在的"先建 player、推进两秒、再倒回"这套动作，发生在 `prepare()` 里，比播放器更早的那一层就能闭包。

---

### M-2 "三通道即 SSG" 这条规则有两份实现（规则 13）

```940:944:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
	if (device != nullptr && device->type == DEVID_YM2610
		&& instance.decoder->channelCount () == 3)
	{
		voiceType = DEVID_AY8910;
	}
```

```708:709:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
	const int count = instance.decoder->channelCount ();
	const bool isSsg = (instance.device->type == DEVID_YM2610 && count == 3);
```

同一句判断，两个地方。注释都写了理由（"libvgm 把它报成父设备的 id 和 type"），但**理由写了两遍**，而规范 §2 说"一个坑只解释一次"。

**这是怎么来的**：`channelKindFor` 是后加的，作者（本次的 AI）在写它时**没有先 grep 已有的 `voiceType` 判定**，于是把同一条硬件规则重新推了一遍。规范 §4 的"加东西应该只改一处"在这条上失效了——不是因为没有表，而是因为**规则本身没有被命名**。

**修法**：抽一个具名函数，两处都调它。

```cpp
/** **A device the song reports under its parent's identity.** libvgm gives a linked voice set
	(the YM2610's SSG) the parent's id *and* type; three channels is the only field that
	separates them. One definition, because getting it wrong draws the SSG as FM. */
bool isLinkedVoiceSet (const DeviceInstance& instance);
```

---

### M-3 41→7 的映射没有任何断言（规则 6、7.5）

`channelGlyphFor()` 是 `inline` 纯函数，**完全可测**，但全仓没有一条断言碰它。`UiButtonTest.cpp` 用了 `drawChannelGlyph` 和 `ChannelGlyph`，那是**画预览图**用的，不是验证映射。

于是：`c140Pcm` 是否真的落到 `pcm` 而不是 `wavetable`？`nesDmc` 是否落到 `pcm`？——目前唯一的验证方式是**找一首那个芯片的曲子，截图，用眼睛看**。这正是规范 §7.5 说的"核心逻辑必须有单元测试"的反面。

**修法**：一条表驱动的断言，五行代码：

```cpp
// The two that must never merge: a C140 drum is a recording, an SCC table is a cycle.
check (channelGlyphFor (chips::ChannelKind::c140Pcm) == ChannelGlyph::pcm);
check (channelGlyphFor (chips::ChannelKind::sccWave) == ChannelGlyph::wavetable);
```

更彻底的做法是遍历全部 41 项并断言"每一项都被显式列出"（配合 H-1 的 `jassertfalse` 兜底，这条测试才有意义）。

---

### M-4 两次 revert，都发生在"提交之后才发现"（规则 0）

| revert | 被撤销的内容 | 发现方式 |
|---|---|---|
| `79a8156` | `ed45b29` 把字号调大到 11.5/18 | 截图（用户看出"有的标题不居中了"） |
| `d25d75e` | `d168192` 给 SSG 加类型兜底 | 用户判断"每个都要处理太麻烦" |

第一条是**纯返工**：放大字号之前只要量一次 `88 − 12 − 18 − 4 = 54`，就会发现"居中排版装不下 18px 图标"，这个结论在写代码前就能得到。第二条则是**方向正确但被否决**——那不是错误，是产品判断，不算债。

所以真正的问题是第一条。规范 §0 说"时间是充足的，返工不是"，并且列了四件"全是因为写的时候图快"的事故。**这次是一件新的**：不是图快，而是**先动手再量尺寸**。

**修法**：不是流程，是**把尺寸算进注释**。`kMarkSize = 16` 旁边应当写"16+4 让名字剩 56px，20 会让 `drawText` 缩字"——现在这条理由是有的（提交说明里），但**没在代码里**。下一个想调到 20 的人仍会重走一遍。

---

### M-5 `9b9c81a` 的起因：提交说明写了未验证的结论（规则 12.4）

`673c897` 的说明里写了：

> the same missing flag means the N163 is classified as a sampled channel by `WaveExport::isSampledChannel`

**这是错的**。`isSampledChannel()` 的第一个条件就是 `sampleRate > 0.0f`，而 NSF 的 N163 从来没设过 `sampleRate`——短路之后根本走不到 `tableWave`。于是下一个提交（`9b9c81a`）专门去修一句**注释和提交说明**，代码零改动。

规范 §12 的第 4 条：

> **断言里只写"应当"，观测值先打印出来看**。曾经把 `length == 17` 和 "upper bits are dropped" 写进断言，等于给错误上了锁——`ctest` 一直是绿的，而功能是坏的。

这里是同一个病的另一种形态：**把推断当成了观测**。写那句话时，只要 grep 一次 `sampleRate` 的赋值点，就会发现 N163 那段根本没有它。

**修法**：已修（`9b9c81a`）。要记的是**判据**：说"某处受影响"之前，先找到那个 `if` 的第一行。

---

## 四、可延后（L）

### L-1 菜单与设置页共用 `settings.*` 文案键，是故意的，但只写在提交说明里

菜单项直接用 `settings.midi.bend`、`settings.general.reuseWindow`——**改一处措辞，两处同时变**，这是这轮最好的决定之一（避免第二套文案）。

但它带来一个约束：**菜单里永远不能比设置页更短**。`Settings...` 可以叫 `Settings...`，而设置页里那句"Transpose by the sample's root"在菜单里偏长，却改不了——改了设置页也得跟着改。

这个约束值得写进 `MainMenu.h` 的注释，现在它只存在于 `0aedd7a` 的提交说明里，而提交说明**在三个月后不会有人翻**。

### L-2 生成脚本没有"忘了重跑"的检查

`svg_to_channel_glyphs.py` 的头部说明写得很好（"The SVG is the design, this output is the drawing"）。但它没有任何机制告诉后来者"你改了 SVG 但没跑脚本，现在代码里的图标是旧的"。

**建议**：在 `ChannelGlyphs.generated.cpp` 的头部加一行构建时可见的标识——生成时间 + 源文件的 mtime 哈希，并让脚本在输出里做一次自检。成本十分钟，收益是"改了 SVG 却发现没生效"这类静默失败不再发生。

### L-3 两层状态转发只有一处调用

```
PluginEditor::updateTransportStatus → PlayView::setStatus → TransportPanel::setStatus → statusLabel
```

三层传一个字符串，只有一个调用方。`PlayView` 那一层现在只是转发。

**不是缺陷**——层次是对的（窗口不该知道 transport 在哪个卡片里）。但**如果三个月内没有第二个调用方**，它就是规范 §3 说的"跨模块只传值"的反面：为了省一次 `getTransportPanel()`，保留了一个永远转发的方法。可以留，记一笔账。

---

## 五、达标项（点名保留）

| 项 | 为什么值得记 |
|---|---|
| **`SyncedComboBox`** | 五个下拉框的同一个竞态收进一个类，`noteUserPick` / `syncTo` / `forceTo` / `holdOff` 四个动作把"谁赢"讲清楚了。时钟可注入 → 竞态可单测。**这是本轮唯一一次"抽象先于修复"** |
| **颜色零字面量** | 自检命令全仓 0 命中。`theme::primaryText ().withAlpha (0.75f)` 是派生色，合规 |
| **死代码删净** | `openButton`、`statusLabel`、`topRowHeight`、`getAudioDcBlock`、`dragFailedOverlay`（改名）——没有留"以后可能有用"的残骸 |
| **i18n 复用** | 菜单没有新增 `menu.options.*` 文案，全部引用 `settings.*`。少数一个文案库，也少见一种"两处措辞不一致" |
| **SVG → C++ 生成链路** | 设计源在 `assets/icon/`，产物提交进仓库，构建不需要 Python。改曲线 = 改 SVG + 跑脚本，diff 能看出哪条线动了 |
| **注释质量** | 绝大多数注释在写"为什么"。例如 `kNameHeight` 的 `// 20 px type mark + 2 px of air`、`ADPCM-A 1` 那条"loses the number"的说明——都是三个月后真正需要的句子 |
| **`GroupFrame`** | 虚线框 + 名牌抽成 `groupFrame::draw()`，MIDI/AUDIO 两个框共用。没有各自画一遍 |

---

## 六、架构可持续性评估

这是本次评审的核心问题。用两组**可数的事实**回答。

### 6.1 新增一颗芯片，现在要改几处

| # | 位置 | 漏了会怎样 |
|---|---|---|
| 1 | `chips/ChipTypes.h` 的 `ChannelKind` 枚举 | 编译失败（好） |
| 2 | 该芯片 decoder 的 `channelKind()` | 返回 `unknown`，strips 无图标 |
| 3 | `playback/ChannelFromChip.cpp` 的 `waveKindForChannelKind()` | **无警告**，键盘页波形画错 |
| 4 | `ui/ChannelGlyphs.h` 的 `channelGlyphFor()` | **无警告**，落到 `return pulse`，混音台图标错 |
| 5 | 若该芯片走 NSF：`NsfBackend.cpp` 的通道表 | 无图标 |

**5 处，其中 2 处漏了不报错。** 第 3、4 是同一形状的两个 switch，各自维护一份 41 项的列表。

这是**架构层面最该修的一条**，而且它有一个现成的解法：**让 `ChannelKind` 携带自己的族**。

```cpp
enum class ChannelKind : uint8_t { ... };

/** The family a kind belongs to. One function, two consumers - the waveform drawer and the
	mixer's mark - so a new kind is classified once. */
constexpr ChannelFamily familyOf (ChannelKind kind);
```

然后 `waveKindForChannelKind()` 变成 `familyOf()` 的一次查表，`channelGlyphFor()` 也变成一次查表。**新增 kind 只改 1 处**（那张表），两个 switch 彻底消失。

代价是要把现有两个 switch 的并集整理成一张 41 行的表——半天的活，换掉"以后每次加芯片都要记得两处"。

> **已实施（2026-11-15）**：`chips::familyOf()` + `ChannelFamily`，两个 41 项 switch 合并为一张表；
> `ChannelGlyph` 成为 `ChannelFamily` 的别名（生成代码零改动）。新增一颗芯片从 5 处（2 处静默）
> 降到 3 处，且漏项会 `assert`。`vgm_chip_test` 一条断言未改而全部通过——41 项映射被整段重写，
> 这是它没变的证据。详见 `docs/memo/2026-11-15-classify-the-kind-once.md`。

### 6.2 新增一条设置，现在要改几处

| # | 位置 |
|---|---|
| 1 | `settings/SettingsDefaults.h`（默认值与边界） |
| 2 | `settings/SettingsStore.h/.cpp`（getter/setter + key） |
| 3 | `ui/SettingsOverlay.cpp`（控件构造 + 布局 + `refresh` + `refreshLanguage`） |
| 4 | `assets/i18n/en.lang` |
| 5 | `assets/i18n/zh-Hans.lang` |
| 6 | 若进菜单：`ui/MainMenu.h` 一行 `addToggle` |

**6 处，其中 2 处是 i18n 双写。** 这是可接受的——设置项天然有 UI、有持久化、有多语言。规范 §4 的"加一行"在设置上不现实，能做到"加六行且顺序固定"就够了。

**这一项不需要改。** 但 `addToggle` 这个 lambda 的方向是对的：它把"布尔 → 菜单项"的五次重复收成了一处，下一次加开关确实只加一行。

### 6.3 总体判断

| 维度 | 评价 |
|---|---|
| 依赖方向 | **健康**。`ui/` 只依赖 `playback` 的值类型（`TrackInfo::channelKind`），不依赖后端；`chips/ChannelKind` 是纯枚举 |
| 所有权 | **一处松动**。`kindTable` 是 UI 需求驱动的播放器缓存（M-1）；其余（`TrackInfo`、`SettingsStore`、`SyncedComboBox`）都在正确的位置 |
| 可扩展性 | **芯片方向在老化**（5 处 / 2 处静默）；**设置方向健康**（6 处，顺序固定） |
| 可测试性 | **一半**。`SyncedComboBox`、`channelGlyphFor`、`Layout` 全是纯函数，可测；但**唯一没补测试的恰恰是改动最频繁的那段**（H-2） |
| 一致性 | **高**。颜色、文案、图标来源都收在单一点；没有第二套 |

**结论**：这一轮的代码**是优雅的**——名字清楚、注释写为什么、死代码删净、没有字面量颜色。它**不够可持续**的地方只有两处，都在"新增一颗芯片"上：两个 41 项 switch，和一条被推了两遍的硬件规则。两处都是可数的、可修的、各半天。

---

## 七、`code-quality.md` 检查清单逐条

| 条目 | 结论 |
|---|---|
| 有没有"为了快"写的？ | **有**：`ed45b29` 放大字号前没算宽度，导致 revert（M-4） |
| 三个月后能否只看函数名说出做什么？ | **能**。`channelGlyphFor`、`isSampledChannel`、`layOutChannelName`（建议的）都是好的形状 |
| 算术/映射有没有测试能直接调用？ | **映射没有**（M-3）；`SyncedComboBox` 有；排版没有（H-2） |
| 新增同类东西只改一处？ | **芯片方向不是**（5 处，2 处静默） |
| 有没有留下"先这样"的补丁？ | **H-1 的兜底注释**是一处：它把缺陷说成了保护 |
| 注释在重复代码已说清的事？ | 基本没有。但"三通道即 SSG"的解释写了两遍（M-2） |
| 有没有第二份表/常量/颜色？ | 颜色**没有**；表**有两份**（两个 41 项 switch） |
| 这份数据有几个写入点？ | `SettingsStore` 一个 ✅；`TrackInfo::channelKind` 两个后端各填一次（合理，因为一个枚举期、一个手写表）；SSG 规则两处**实现**（M-2） |
| 新颜色走四步？ | 本轮**没有新增颜色**，只用了 `primaryText().withAlpha()` |
| refactor 夹带行为变更？ | **有**（H-3） |
| 掩码/位域跨型号验证？ | 本轮未动掩码。但 SSG 的"三通道"判定横跨 YM2610 与 AY，**两处都验证过**（`c5c18ea`、`d168192`） |
| 断言容差小于缺陷？ | 不适用（本轮无数值断言） |
| 读的成员是否被 move 走？ | `PlaybackEngine::stateSnapshot` 有专门注释说明；无新增风险 |
| 界面读的是同一对象？ | ✅ `TrackInfo::channelKind` 与键盘页的 `ChannelState::channelKind` **曾是两份**，`673c897` 之后统一到 `ChannelKind`（这是本轮的正面修复） |
| 位宽是否在实现里确认？ | 本轮未动位宽 |
| 固定缓冲从取值范围推？ | 不适用 |
| 断言写"应当"还是观测现状？ | **M-5** 是反例：写了推断 |

---

## 八、建议的修复顺序

每个提交可独立验证，前三个合起来约半天。

| # | 提交 | 内容 | 验证 |
|---|---|---|---|
| 1 | `refactor(mixer): name-row layout as a pure function` | 抽 `layOutChannelName()`，`paint()` 只剩两次 `drawText`（H-2 的一半） | 新增断言：三种宽度 × 有无图标 = 六组矩形比较 |
| 2 | `test(mixer): the six cases the layout actually has` | 上一提交的断言（H-2 的另一半 + M-3） | `ctest` 变红再变绿 |
| 3 | `fix(glyphs): the exhaustive switch was not exhaustive` | 兜底改 `jassertfalse`，注释说实话（H-1） | Debug 下加一个 kind 会断言 |
| 4 | `refactor(playback): one definition of a linked voice set` | `isLinkedVoiceSet()`，两处调用（M-2） | 现有 NSF/VGM 截图不变 |
| 5 | `fix(playback): the kind table belongs to the song` | `kindsCollected` 在 `setReplaySource` 重置（M-1 的便宜版） | 连续加载两首歌，图标不串 |
| 6 | `docs(review): classify the channel kind once` | 为 §6.1 的 `familyOf()` 写一页设计，不实现 | — |

**H-3 无法用提交修**（历史已推），在合并说明里单列一段即可。

---

## 九、给下一轮的一句话

> 这一轮证明了"看得见的东西"可以做得很好；它没有证明"改了之后还对不对"是能被机器回答的。
>
> **下一次动 `paint()` 里的坐标之前，先把它变成一个能传参的函数。** 三次返工、两次 revert，全部发生在那 40 行里。

---

## 附：评审涉及的文件

| 文件 | 行数 | 角色 |
|---|---|---|
| `plugin/Source/ui/ChannelGlyphs.h` | 121 | 枚举 + 41→7 映射（H-1、M-3、§6.1） |
| `plugin/Source/ui/ChannelGlyphs.generated.cpp` | 315 | 脚本产物 |
| `plugin/tools/svg_to_channel_glyphs.py` | 398 | SVG → C++（L-2） |
| `plugin/Source/ui/ChannelStrip.cpp` | +103/−50 | 排版（H-2、M-4） |
| `plugin/Source/ui/MainMenu.h` | +150/−1 | Options 重建（H-3、L-1） |
| `plugin/Source/ui/SyncedComboBox.{h,cpp}` | +73/+99 | 本轮最好的抽象 |
| `plugin/Source/backends/libvgm/LibvgmTrackPlayer.{h,cpp}` | +27/+60 | `kindTable`（M-1、M-2） |
| `plugin/Source/ui/GroupFrame.{h,cpp}` | +59/+65 | 虚线框（达标项） |
| `plugin/Source/PluginEditor.cpp` | +72/−68 | 删顶部行、消息框（达标项） |
