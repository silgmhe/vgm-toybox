# YM2608 可视化分支审查

> 分支 `issue/ym2608-visualisation`，11 个提交，起点 `54d23fb`。
>
> 代码改动 8 个文件、约 190 行插入（不含文档 660 行）。
>
> 结论：**可以合并，但先还三笔小债。** 逻辑是对的，注释质量高于本仓库的平均水平；问题集中在**格式噪音**和**同一语义的第二个来源**——后者是这个项目已经付过四次账的那一类。

## 一、审查范围

| 文件 | 变更 | 性质 |
|---|---|---|
| `LibvgmTrackPlayer.cpp` | +34 / −3 | `isLinkedVoiceSet` 改写 |
| `OpnVoiceDriver.cpp` | +12 | `0x29` 六通道模式 |
| `VgmReplayer.cpp` | +37 | rhythm ROM 注入 |
| `OpnDecoder.cpp` | +55 / −2 | 地址表、latch、`static_assert` |
| `OpnDecoder.h` | +13 / −3 | `kRequirements` 注释 |
| `KeyboardRowLayout.h` | +18 / −4 | 名字列与按钮尺寸 |
| `KeyboardLayoutTest.cpp` | +44 / −1 | 名字宽度断言 |
| `OpnCoreOracleTest.cpp` | +57 | 第二端口断言 |
| `CMakeLists.txt` | +6 / −7 | 移除多余的源 |

**未审查**：`docs/issue/*` 两篇文档（内容质量已单独确认）。

**方法**：逐 hunk 阅读 + 对每条结论回查被模拟的核心源码（`libs/libvgm/emu/cores/fmopn.c`、`fmopn_2608rom.h`）+ 核对断言是否真的会失败。

## 二、值得保留的做法

先记三条，因为它们容易被后续修复改掉：

1. **每个非显然的决定都带实测数字**。`peak 0.9971` / `0.0000`、`14080..14847` / `7040..7424`、`needs 56.3, has 54`——读的人不需要重跑就能判断。
2. **A/B 双向验证**。四条修复都做过"去掉它是否变红"，且 `OpnCoreOracleTest` 的新断言在去掉 `0x29` 后确实 `exit 1`。
3. **`static_assert` 锁住单位**（`OpnDecoder.cpp:179`）。这是本分支最便宜的一条防线：它把"表是字节还是 word"从"听一遍鼓声"变成"编译期"。

## 三、阻塞项（合并前修）

### P1-1 新增的测试函数多一级缩进

```515:515:plugin/Tests/KeyboardLayoutTest.cpp
	void testNameColumnFitsLongestChannelName (void)
```

同文件其余函数（`testRowChangeDecision`、`testRowLayout`）都在第 0 列。这个函数连同它上方 15 行注释一起被缩进了一个 tab——**是插入时带进去的，不是有意的嵌套**（文件里没有外层 `namespace`）。

**影响**：不改变行为，但它是"这段是后来贴上去的"的记号，而这种记号会让人以为它和周围不同。

**修法**：整块左移一个 tab。

### P1-2 临时打印清掉了，空行留下了

```921:924:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
		const bool hasDeclaration = device != nullptr && device->devDecl != nullptr;



		std::string chipName = "Chip";
```

```1253:1256:plugin/Source/chips/OpnDecoder.cpp
		state.gate = impl->now < impl->adpcmA[slot].end;



		// **The shape is the bytes the hit reads, decoded exactly as `fmopn.c` does.**
```

两处都是 `// TEMP` 打印被删后剩下的**三个**空行（`921-923` 与 `1253-1255`），已经进了提交（`3fbd007`、`489ceec`）。

**影响**：纯噪音。但它是**证据**——说明"清理临时打印"这一步没有 diff 自检就提交了。这个项目在 `2026-11-19` 那次评审里记过同样的形状（临时打印残留在代码里）。

**修法**：各删一行。**建议把"提交前 `git diff` 看一遍自己的删除"写进检查清单**——两条空行不会自己出现。

### P1-3 `CMakeLists.txt` 的 `add_executable` 整块重写

```849:857:plugin/CMakeLists.txt
```

实际只需要删掉 `Source/backends/libvgm/OpnVoiceDriver.cpp` 一行，diff 却显示 **6 行删除 + 6 行添加**，内容一模一样。

**影响**：评审时这 12 行要逐个比对才能确认"只是删了一个源"，而其中藏着什么改动是看不出来的。规则 §8 说"一个提交解释一件事"，而这是**同一件事被写成了两遍**。

**修法**：`git checkout` 该文件后只删那一行，重新提交。

## 四、重要项（下一次动这块之前）

### P2-1 `isLinkedVoiceSet` 用 `typeid` 做行为分派

```519:537:plugin/Source/backends/libvgm/LibvgmTrackPlayer.cpp
	template <typename Instance>
	bool isLinkedVoiceSet (const Instance& instance)
	{
		if (instance.device == nullptr || instance.decoder == nullptr)
		{
			return false;
		}

		// The parent test keeps a standalone AY8910 out: there the device type is the AY itself.
		const bool opnParent = instance.device->type == DEVID_YM2203
							   || instance.device->type == DEVID_YM2608
							   || instance.device->type == DEVID_YM2610;

		return opnParent
			   && instance.type == std::type_index (typeid (chips::Ay8910Decoder))
			   && instance.decoder->channelCount () == 3;
	}
```

**这是本次改动里唯一的结构性问题。**

它比原来那版（写死 `DEVID_YM2610`）正确得多，但仍然把"这个实例是不是联动设备"的**判定权留在了调用方**，而这个知识属于解码器：

- OPL4（`YMF278B`）联动一个 OPL3，通道数与主设备**相同**——`channelCount() == 3` 直接排除它；
- OPM、OPL 家族目前没有联动设备，但 `ChipPlans` 的注释已经预告过"linkIDs"这个概念；
- `Ay8910Decoder` 今天没有子类，明天有一个（比如立体声变体）`typeid` 精确比较就会静默返回 false，**症状与本次修的这个 bug 一模一样**。

**修法（推荐）**：把问题交给解码器自己回答。

```cpp
// IChipDecoder.h
/** Whether this decoder stands in for a linked voice set of the device it is reported under. */
virtual bool isLinkedVoiceSet (void) const { return false; }

// Ay8910Decoder.h
bool isLinkedVoiceSet (void) const override { return true; }
```

调用方退化成 `instance.decoder->isLinkedVoiceSet()`。新增谁，谁自己声明；`LibvgmTrackPlayer` 不再需要认识任何一个具体解码器类型，`typeid`、`<typeindex>`、`sys/...` 的三处 include 都可以去掉。

**为什么不是现在就做**：这是一个接口变更，值得单独一个提交，且需要为"独立 AY8910 不返回 true"补一条断言（见 P3-2）。**但它应该排在下一个 YM2608 相邻工作之前**——否则下一次加芯片会再复制一遍这段判定。

### P2-2 `0x29` 的型号列表是硬编码

```229:233:plugin/Source/backends/libvgm/OpnVoiceDriver.cpp
	if (patch.model == chips::ChipModel::ym2608 || patch.model == chips::ChipModel::ym2610)
	{
		core->write (0, 0x29, 0x9F);	// six FM channels, IRQ mask at the reset default
	}
```

**逻辑正确**（YM2612 天生六通道、YM2203 三通道，都不该写），但这是**又一份"哪些变体是六通道"的名单**。同一个文件里，这个名单的另一个副本是 `OpnDecoder::addressShiftFor`（OPNA/OPNB 走 288，其余 144）。

规则 §4 说"加东西应该只改一处"。加 YM2610B 时这份名单不用改（它走 `ym2610`），但加 YMF288（OPN3）时要改**两处**——而漏一处的症状分别是"半个芯片静音"和"整体低一个八度"，都不报错。

**修法**：放进已经存在的那张表旁边，或者给它一个名字：

```cpp
/** **The parts with six FM channels behind an `extend 3ch` switch.** A YM2612 has six without
    being told and a YM2203 has three, so neither is written to.
*/
constexpr bool hasSixChannelMode (chips::ChipModel model)
{
    switch (model)
    {
    case chips::ChipModel::ym2608:
    case chips::ChipModel::ym2610:
        return true;

    default:
        return false;
    }
}
```

**顺带**：`0x9F` 也是一个魔数。它由"bit 7 开六通道"和"低五位取复位默认的 `0x1F`"拼成，位含义写在注释里，但值是拼出来的。可以写成 `0x80 | 0x1F`，让两个来源在代码里可见。

### P2-3 `slotFlags != 2` 是枚举的第二个来源

```32:38:plugin/Source/backends/libvgm/VgmReplayer.cpp
	void attachRhythmRom (toybox::chips::IChipDecoder& decoder, uint8_t slotFlags)
	{
		if (slotFlags != 2 || typeid (decoder) != typeid (toybox::chips::OpnDecoder))
		{
			return;
		}
```

`2` 是"YM2608"在 `ChipConfig::flags` 里的序号，注释也这么写了。但 `OpnDecoder` 内部把它解释成 `Variant::ym2608`（`flags - 1`）——**同一个数字，两处语义，一个在 `VgmReplayer` 一个在 `OpnDecoder`**。

这正是规则 §13 判据说的："这份数据有几个写入点？"序号 `2` 的定义在 `VgmReplayer::adoptNewBuffers` 的 `switch` 里，而它的消费者要自己记住这个字面量。

**修法**：给序号一个具名常量（`constexpr uint8_t kOpnaSlotFlag = 2;`）放在 `VgmReplayer.cpp`，两边都引它；或者让 `attachRhythmRom` 收 `Variant` 而不是 `uint8_t`。

### P2-4 地址表的索引方式容易错位

```165:174:plugin/Source/chips/OpnDecoder.cpp
	constexpr uint32_t kOpnaRhythmAddress[2 * 6]
	{
		0x0000, 0x01bf,
		...
	};

	static_assert (kOpnaRhythmAddress[2 * 6 - 1] < 0x2000,
				   "...");
```

```915:925:plugin/Source/chips/OpnDecoder.cpp
				for (int channel = 0; channel < 6; ++channel)
				{
					...
					regs.latchStart = kOpnaRhythmAddress[channel * 2];
					regs.latchEnd = kOpnaRhythmAddress[channel * 2 + 1] + 1;
				}
```

三个小问题，合起来是一次"读错一行就换一个鼓"的机会：

1. `[channel * 2]` / `[channel * 2 + 1]` 是**扁平数组表达成对**，靠调用方记得乘二；
2. 循环上界 `6` 是字面量，而同一个文件里已经有 `kAdpcmAVoices`；
3. `static_assert` 只验了末项上界，**没验单调**——把 `0x1cff` 写成 `0x1bff` 编译照过。

**修法**：

```cpp
struct RhythmRange { uint32_t start, end; };

constexpr RhythmRange kOpnaRhythm[6]
{
    { 0x0000, 0x01bf },     // bass drum
    { 0x01c0, 0x043f },     // snare drum
    { 0x0440, 0x1b7f },     // top cymbal
    { 0x1b80, 0x1cff },     // high hat
    { 0x1d00, 0x1f7f },     // tom tom
    { 0x1f80, 0x1fff }      // rim shot
};

static_assert (kOpnaRhythm[5].end < 0x2000, "byte offsets into the 8 KB ROM");
static_assert (kOpnaRhythm[1].start == kOpnaRhythm[0].end + 1, "the samples are contiguous");
```

`channel < kAdpcmAVoices` 之后，索引是 `kOpnaRhythm[channel].start`——**"第几个通道的起止"不再需要读者做乘法**。

### P2-5 `buttonSize` 与 mixer 仍是两个来源

```40:45:plugin/Source/ui/KeyboardRowLayout.h
	/** **The mixer's button height and gap**, so the two views agree - which the comment here has
		claimed since the field was added, while the numbers had drifted to 30 and 8. ...
	*/
	int buttonSize = 28;
	int buttonGap = 6;
```

**注释诚实地记录了漂移，但漂移的原因还在**：`28` 和 `6` 仍然是从 `ChannelStrip.cpp` 的 `kButtonHeight`/`kButtonGap` **抄过来的两个数**。`KeyboardRowLayout.cpp` 与 `ChannelStrip.cpp` 之间没有任何编译期联系，下一次改 mixer 又会分叉。

`KeyboardLayoutTest` 里的 `check (row.mute.getHeight () == 28, ...)` 是**第三个副本**——它现在能抓住"键盘页改了但 mixer 没改"，但抓不住"两边一起偏"。

**修法**：把这两个数搬到一个共享头（例如 `ui/LayoutMetrics.h`），或把 `kButtonHeight`/`kButtonGap` 从 `ChannelStrip.cpp` 提到 `ChannelStrip.h` 并让 `RowMetrics` 引它。规则 §13 的判据对这条完全适用。

## 五、建议项（不阻塞）

| # | 位置 | 建议 |
|---|---|---|
| **P3-1** | `OpnDecoder.cpp:165` | `static_assert` 加一条"相邻段首尾相接"，把"写错一个数字"变成编译错误 |
| **P3-2** | `KeyboardLayoutTest.cpp` / `OpnCoreOracleTest.cpp` | `isLinkedVoiceSet` 与 `attachRhythmRom` **都没有单元测试**，只有截图验证。前者可以测（独立 AY8910 应当返回 false），后者可以测（`attachMemory` 后 `channelState(0).wave` 非空） |
| **P3-3** | `LibvgmTrackPlayer.cpp:519`、`VgmReplayer.cpp:34` | 两处 `typeid` 判定：一处用 `instance.type`（缓存的 `type_index`），一处用 `typeid (decoder)`（每次调用求值）。**同一语义两种写法**，P2-1 的接口化能一并消掉 |
| **P3-4** | `OpnDecoder.h:134` | 注释解释了"因为 `vgm_chip_test` 没有 libvgm 头路径，所以注入放在 `VgmReplayer`"。**这是构建缺陷被写进了领域代码的注释**。修构建更好，但当前写法可接受——只要这条注释指向的 issue 文档存在（`docs/issue/2026-11-21-the-drums-that-were-inside-the-chip.md` §四有完整解释） |
| **P3-5** | `VgmReplayer.cpp:32` | `attachRhythmRom` 的 `static const std::vector` 在首次调用时构造，是 C++11 的线程安全初始化 ✓。但它是**每个进程一份的 8 KB 副本**，而 `OpnDecoder` 收到后又 `= ` 复制一次——两个解码器同时存在时是 24 KB。当前无所谓，记一笔 |
| **P3-6** | 全部 | **没有任何一条断言覆盖"YM2608 的 ADPCM-A 有波形"**。这次的验证靠 `[adpcmwave] track=9 samples=1534` 的临时打印，删掉后就没有回归防线了。P3-2 那条测试正好补上 |

## 六、规范逐条对照

| 规则 | 结论 |
|---|---|
| §0 时间充足，返工不是 | ⚠️ P1-1/P1-2/P1-3 都是"顺手留下"的痕迹 |
| §1 先让人看懂 | ✅ 命名清楚；⚠️ `[channel*2]`、裸 `6`、`0x9F` |
| §2 注释写为什么，只写一次 | ✅ **本分支最好的一面**，每条都有实测和取舍 |
| §3 高内聚低耦合 | ⚠️ `LibvgmTrackPlayer` 认识 `Ay8910Decoder` 的具体类型（P2-1） |
| §4 加东西只改一处 | ❌ **P2-2、P2-3、P2-5 三处都是"再加一个芯片要改两处"** |
| §5 不留技术债 | ⚠️ 无 TODO 遗留，但 P3-2 的测试缺口未记录 |
| §6 可测试 | ⚠️ 新增的两个函数没有可调用断言（P3-2） |
| §7 颜色走主题 | ✅ 未涉及颜色 |
| §7.5 核心断言 + 界面截图 | ✅ 名字宽度走了断言（`needs 56.3, has 54` 能失败）；⚠️ `isLinkedVoiceSet` 只有截图 |
| §8 提交即最小单元 | ❌ P1-3（CMake 整块重写）、P1-1/P1-2（格式噪音） |
| §9 重构不夹带行为变更 | ✅ `b2671f5` 是纯构建修复，`83d553f` 是纯布局，两者分开 |
| §10 通道映射跨型号验证 | ✅ **本次做得很好**：`isLinkedVoiceSet` 的注释明确写了 YM2203 三通道 FM 的反例，`0x29` 明确写了 YM2612/YM2203 不写 |
| §11 读状态前先问所有权 | ✅ `static_assert` 那条是同一精神的另一种形式 |
| §12 位宽去实现里读 | ✅ **`kOpnaRhythmAddress` 的注释逐字引用了 `fmopn.c` 的单位推导**，`now_addr` 是 nibble 游标那句尤其关键 |
| §13 一份数据一个来源 | ❌ **P2-2、P2-3、P2-5 三处**；这是本分支最系统的欠债 |

**统计**：✅ 8 条、⚠️ 4 条、❌ 3 条。

## 七、架构优化建议

按收益/成本排序，都不必现在做：

### 1. 让解码器回答"我是什么"（承接 P2-1）

现在有两处代码在做这件事：

- `isLinkedVoiceSet`：`typeid(Ay8910Decoder)` + 通道数
- `attachRhythmRom`：`typeid(OpnDecoder)` + `slotFlags`

**它们问的是同一个问题的两个侧面**："这个实例需要什么特殊对待？" 答案是解码器自己的知识，不是播放器的。

如果 `IChipDecoder` 有

```cpp
virtual bool isLinkedVoiceSet (void) const { return false; }
virtual const uint8_t* builtinRom (std::size_t& size) const { return nullptr; }
```

那么 `LibvgmTrackPlayer` 与 `VgmReplayer` 都不再需要 `typeid`，也不再需要认识具体解码器——**加一颗芯片只写它自己的类**。这正是规则 §4 的目标。

**成本**：两个虚函数、两个 override、一处调用点替换。**收益**：`#include "chips/Ay8910Decoder.h"`、`#include "chips/OpnDecoder.h"`、`#include <typeindex>` 三行从播放器里消失。

### 2. 四套编号系统需要一张转换表

一个 OPN 设备在代码里有四种名字：

| 名字 | 例 | 定义处 | 消费者 |
|---|---|---|---|
| `DEVID_*` | `0x07` | libvgm | 路由、设备表 |
| `slotFlags` | `2` | `VgmReplayer::adoptNewBuffers` | `ChipConfig::flags` |
| `OpnDecoder::Variant` | `ym2608` | `OpnDecoder.h` | 布局、地址移位 |
| `ChipModel` | `ym2608` | `VoicePatch.h` | `OpnVoiceDriver` |

**四层转换，每层各写一次 switch**。这次的 `0x29` bug（`ChipModel` 兜底成 `ym2610`）和 P2-3（`slotFlags != 2`）都是这张网上的一个节点错了。

**建议**：一张 `constexpr` 表，四列一行，所有 switch 退化成查表。**不必现在做，但下次动这里时值得**——因为每加一颗芯片，现在要改四处。

### 3. `attachRhythmRom` 的位置

它在 `VgmReplayer.cpp` 的匿名命名空间里，`namespace toybox::backends::libvgm` 之外。技术上没问题（用了全限定名），但它**只服务这一类设备**，放在 `VgmReplayer` 内部更容易读。

**若采纳建议 1**，这个函数整个消失——ROM 由 `OpnDecoder::builtinRom()` 返回，注入点变成一句通用调用。

## 八、修复顺序

**合并前**（约 10 分钟）：

1. P1-1 左移那 16 行
2. P1-2 删两处双空行
3. P1-3 `CMakeLists.txt` 只删一行

**合并后第一件事**（约 40 分钟）：

4. P2-5 按钮尺寸收进共享常量（最小，且能防止下一次漂移）
5. P2-4 地址表改 `struct` + 加单调断言

**下次动 OPN 之前**：

6. P2-1 接口化（连带 P3-3、架构建议 1）
7. P2-2 型号名单收进具名函数
8. P2-3 序号具名

**验收标准**：`git diff` 里不再有无内容变化的行；`vgm_keyboard_test` 与 `vgm_opn_test` 各自至少有一条断言能在**功能被移除时变红**（现在 `OpnCoreOracleTest` 满足，`vgm_keyboard_test` 满足，`isLinkedVoiceSet` 不满足）。

---

**一句话**：这 11 个提交的**推理质量很高**——每个决定都有实测、每个坑都写清了为什么。欠的是**收尾**：三处格式噪音、三处"第二份来源"、两个没测的函数。前两笔十分钟能还完，第三笔值得排在下一颗芯片之前。
