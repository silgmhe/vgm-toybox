# 架构与代码审查（2026-10-24）：NSF 支持、键盘视图与芯片静音

分支：`review/keyboard-and-chip-mute`
评审范围：`93a3245..632e7c8`（上次评审之后到今天合入 master 的全部改动）
日期：2026-10-24

---

## 1. 一页结论

**能发布，但不适合在这个形状上继续加格式。**

这三件事做得好：功能是真的（不是占位）、每次改动都有对应测试或截图、文档与提交信息写得能读懂。这些比大多数项目在同阶段的状态要好。

问题集中在**一处**：`ContentSlot.cpp` 一个 1381 行的文件里，塞了一个 1090 行的面板类，它同时负责布局、绘制、输入、音域算法和波形合成。由此派生出的三个后果——领域逻辑无法单测、新增芯片类型必须改 UI、返工率 37%——都不是"再多写点代码"能解决的，而是**边界放错了位置**。

而"以后要支持更多格式"这件事，恰好会从这个边界上撞过来。

| 维度 | 评价 |
|---|---|
| 功能完整性 | ✅ 可用，已合入 master |
| 测试 | ⚠️ 36 个用例，但新逻辑 0 单测；1 个"测试"是纯截图 |
| 可读性 | ✅ 注释质量高于平均，命名清楚 |
| 可演进性 | ❌ UI 里带着芯片知识，新增格式要动 UI |
| 仓库卫生 | ⚠️ 3 个 317 KB 的测试产物被提交 |

---

## 2. 事实基线（可核查）

### 2.1 规模

```
72 files changed, 7316 insertions(+), 270 deletions(-)
82 个产品提交（非合并 103 个，含 5 个 diag、1 个 wip、5 次回退/重做）
距上次评审 14 小时 40 分
```

改动最集中的文件：

| 行数 | 文件 |
|---|---|
| +1381 | `plugin/Source/ui/ContentSlot.cpp`（新文件） |
| +676 | `plugin/Source/backends/nsf/NsfPlayer.cpp` |
| +331 | `plugin/Source/playback/PlaybackEngine.cpp` |
| +309 | `plugin/Source/backends/nsf/NsfBackend.cpp` |
| +282 | `plugin/Source/ui/SettingsOverlay.cpp` |

### 2.2 最长的一段

`ContentSlot.cpp` 1381 行，其中 `ContentSlot::KeyboardPanel` 从第 78 行延伸到约第 1170 行，**约 1090 行**，内含：

```
 93  resetRange          音域状态
100  setSnapshot         每帧 30 Hz 的快照摄入 + 熄灭延迟
143  refreshLanguage     文案
333  Row / buildRows     布局（枚举行、组头、可滚动内容）
422  paintEmptyState     空态
448  paintGroupHeader    组头
468  paintChannelRow     一行五列
505  paintToggle         M/S 按钮
525  paintShiftControls  标题栏 ◀ ▶ 与起始音
575  paintWave           波表窗口 + 波形
721  KeySpan / octaveFloor / noteForWhite / baseNote / spanFor   音域算法
833  paintKeyboard       88 键的绘制、按压态、八度标签、越界箭头
1094 shiftOctaves        手动平移
1115 paintMeter          力度条
```

一个类里同时有：**领域算法**（音名、八度映射、音域自适应）、**渲染**（五种绘制）、**输入状态**（滚动、平移、悬停）。

### 2.3 返工率

```
触及 ContentSlot.cpp 的提交：19
其中 fix 类：7（37%）
```

同一文件在一晚之内被修了 7 次（meter 高度 0、键盘溢出、白键不可见、琴床宽度、黑键配色、深色主题回退……）。**这不是手滑**，是一个 1090 行、无单测的类只能靠"截图 → 发现 → 再改"来验证的必然结果。

### 2.4 测试的形状

```
ctest 36 个
  app.*（无头 harness）10 个，其中 app.keyboard-shot 是纯截图脚本，0 条 assert
```

`app.keyboard-shot` 与其它 `app.*` 并列出现在 `ctest -N` 里，会永远"通过"，因为它只执行 `size / load / menu invoke / shot`，没有任何断言。**它拍的照片有价值，它作为测试的价值是零。**

### 2.5 新逻辑的覆盖

对这批新增的纯函数，测试里的引用次数：

| 符号 | 测试引用 |
|---|---|
| `octaveFloor` | 0 |
| `noteForWhite` | 0 |
| `spanFor` / `baseNote` / `canShift` | 0 |
| `synthesisedCycle`（四种波形合成） | 0 |
| `noteName` | 0 |
| `keyboard.*`（设置项） | 0 |

这些都是**纯函数**：给定输入有确定输出，本该是最便宜的测试对象。现在它们全是 `private` 嵌套类的成员，写在 `.cpp` 里，测试根本够不到。

---

## 3. 做得对的地方（请保持）

这几条是有意为之的设计，值得写下来以免后人改坏：

1. **快照是拷贝，不是视图。** `ChipState.h` 的注释讲清楚了理由：音频线程不能把状态借出去，读活的寄存器数组是"带漂亮界面的数据竞争"。每块渲染结束时抓一份，UI 读副本。
2. **`waveKind` 是后端词汇，不是芯片名字。** 面板不写 `if (chip == "2A03")`，而是按 `waveKind`（1 表 / 2 方波 / 3 三角 / 4 噪声 / 5 增量）画。这是正确的方向，虽然还差半步（见 M-5）。
3. **能力位终于被消费了。** `canMuteChips` 之前只有两个后端在写、没有一处读，导致"勾了没反应"的卡片一直存在于界面上。现在由 `installSong()` 统一记录、`PlayView` 据此决定是否建卡。
4. **文档先行且留痕。** 五份设计稿（keyboard-view / low-range / labels-and-range / visual-refinement / view-switching）都写了取舍与评审结论，`docs/review` 也有本次的延续。
5. **无头截图 + 规则。** `vgm_app_test` 的 `shot` 命令配合 `.codebuddy/rules/ui-visual-check.md`，把"改完要看一眼"变成了流程。这一晚抓到的界面问题里，没有一个是被单元测试发现的。

---

## 4. 问题清单（按严重度）

### H-1 `KeyboardPanel` 一个类五种职责，且领域逻辑不可测 ⚠

`ContentSlot.cpp:78-1170`。音名、八度映射、音域自适应（`octaveFloor` / `noteForWhite` / `spanFor` / `baseNote` / `canShift`）与波形合成（`synthesisedCycle`）是**纯逻辑**，却被埋在 `private` 嵌套类里，只能通过"画出来再看"来验证。

后果已经在数据里：返工率 37%（§2.3），且这 7 次修复里有 4 次是**算术错误**（高度算成 0、键数取整、溢出、居中偏移），全是可以被一个 20 行单测挡住的。

**建议**：抽 `ui/KeyboardRange.{h,cpp}`（音名、八度、音域、平移）+ `playback/WaveShape.{h,cpp}`（四种波形合成），剩下 `KeyboardPanel` 只做布局与绘制。预计净减 200 行，同时换来可测性。

### H-2 测试假覆盖：截图脚本被注册成 ctest ⚠

`plugin/Tests/scripts/keyboard-shot.txt` 没有一条 `assert`，却以 `app.keyboard-shot` 出现在测试列表里。任何人看 `ctest` 输出都会以为键盘视图被覆盖了。

**建议**：二选一——把截图脚本从 `add_test` 里移除（它属于工具，不属于测试），或在脚本里补上真实断言（`assert chip.0 visible`、`assert view.keyboard …`、`dump` 关键组件）。同时为 H-1 抽出来的纯逻辑补单测。

### H-3 仓库被测试产物污染 ⚠

```
plugin/Tests/assets/.dac.rendered_temp2a160f85.wav   317,564 B
plugin/Tests/assets/.psg.rendered_temp425914c2.wav   317,564 B
plugin/Tests/assets/.psg.rendered_temp59a8389c.wav   317,564 B
```

合计约 **950 KB**，文件名带 `rendered_temp`，显然是渲染测试的中间产物，没有 `.gitignore` 规则挡住。

**建议**：加 `.gitignore` 规则（`*.rendered_temp*.wav`），`git rm --cached` 这三个文件；检查产生它们的测试是否该写到临时目录（`juce::File::getSpecialLocation(tempDirectory)`）而不是仓库内。

### M-1 视图映射靠裸 cast + 硬编码长度

```cpp
// PluginEditor.cpp:136
playView->setContentView ((ContentView) index);
// SettingsStore.cpp
return juce::jlimit (0, 2, file->getIntValue (kContentViewKey, 0));   // ← 2 是枚举长度
```

菜单索引直接强转枚举，两者"顺序一致"是**隐式约定**；`jlimit(0, 2)` 把枚举长度复制了一份。这不是假设性风险——`Ctrl+1/2/3` 失效那次，就是"键对了、目标错了"。

**建议**：单一映射表 `{ id, labelKey, ContentView }`，菜单、快捷键、持久化都从它派生；长度用 `juce::jlimit (0, kContentViewCount - 1, …)` 或 `static_cast` 前校验。

### M-2 音名表写了两份

```
ContentSlot.cpp:54        const char* const names[12] = { "C", "C#", … };
SettingsOverlay.cpp:824   static const char* const names[] = { … }        // 同一个表
```

设置页为了给"起始音"下拉生成标签，把音名表抄了一份。两份将来会漂移（比如某天要支持 `H` 记法或降号）。

**建议**：与 H-1 一起抽到 `playback/NoteNames.h`。

### M-3 残留文案键与过期注释

```
"view.visualiser" = "Visualiser";   ← 代码里已无引用，只剩注释还在举例
```

`view.play` 仍在用（`PlayView::getViewName`），不能删；`view.visualiser` 随可视化页一起失去了调用方。同时 `MainMenu.h:47`、`View.h:27` 的注释仍以它作为示例，会误导后来者以为还有这一页。

**建议**：删掉两份 `view.visualiser`，把注释里的示例换成现有的 `view.keyboard`。

### M-4 `captureState` 149 行、47 个字面量

`NsfPlayer.cpp:362-510`。可读性在边界上：`(i * 4)`、`maskFor (5, 8)`、`0x03`、`duty` 阈值这类数字直接出现在逻辑里，没有命名常量。它目前是对的（有探针验证），但任何一次改动都需要重新确认这些数字。

**建议**：把 `i * 4`（寄存器步长）、各组掩码的 base/count 提成 `constexpr`，并为 N163 的 8 通道与 2A03 的 5 通道各写一个"通道数 = 常量"的断言。

### M-5 波形合成放在 UI 侧：新增格式要改 UI

`waveKind` 是好的抽象，但**把"怎么画出这种形状"留给了面板**。今天只有 NES 的五种，明天加 OPN 的 FM、OPL 的正弦/波形选择表，就要在 `ContentSlot.cpp` 里新增 `case`——UI 开始随格式增长。

**建议**：合成下沉到后端（它本来就知道自己的芯片），`ChannelState.wave` 永远非空；面板只画 `wave`，`waveKind` 仅用于"这是不是可描述的波形"。这样面板对格式彻底无感。

### L-1 命名不一致：`"scope"` vs `oscilloscope`

菜单 id 是 `"scope"`，枚举是 `ContentView::oscilloscope`，文案键是 `view.scope`。三处指同一个东西、两个名字。建议统一为 `oscilloscope`（或反过来都叫 scope）。

### L-2 加一个设置要动四处

`SettingsStore.h`（声明）→ `.cpp`（key 字符串、getter、setter）→ `SettingsOverlay`（控件、布局、语言、读写）→ 两份 `.lang`。这次加了 4 个键（`keyboard.startNote` / `rangeHints` / `noteHoldMs` / `middleCOctave`），重复了 4 遍同一套模板。

**建议**：若设置项继续增长，引入一张 `{ key, type, default, min, max }` 的表 + 一个通用的 `PropertyPanel` 绑定，把"加一项"收敛到一处。现在还不急，但趋势明显。

### L-3 注释密度不均

`KeyboardPanel` 内部的注释质量很高（解释了"为什么"而不是"是什么"），但 `Theme.h` 新增的四个调色板角色 `keyBed/keyWhite/keyBlack/edgeFade` 只有一行总注释，没有说明每个角色在深浅两套主题里的取值原则——而这次恰恰因为"两套主题用同一套调色逻辑"翻了车（深色下黑键比琴床还黑，键盘成了一个黑洞）。

**建议**：在 `Theme.h` 里补一句每个角色的语义（"琴床永远比卡片深；浅色下黑键比白键暗，深色下相反"）。

---

## 5. 面向"更多格式（对标 MDPlayer）"的演进建议

如果目标是 MDPlayer 那种规模（几十种芯片、多种可视化），当前架构需要三次收敛：

| # | 收敛点 | 现在 | 应该 |
|---|---|---|---|
| 1 | **领域逻辑出 UI** | 音名/音域/合成在 `ContentSlot.cpp` | `playback/` 下的可测模块（H-1、M-5） |
| 2 | **能力描述进数据** | `waveKind` 一个字段 + 若干布尔 | `ChannelState` 带 capability 段：有无波表、可否静音、是否支持 MIDI |
| 3 | **视图注册表** | 枚举 + index cast + 三个面板指针 | 一张注册表，新增视图 = 新增一行（M-1） |

另外建议给每个后端加**一致性测试套件**（conformance）：同一组断言（能报告芯片名、能静音、快照字段合法）跑遍所有后端。这次 `canMuteChips` 声明了却没人读、OPL4 静音失效，都是因为**没有一处强制后端兑现自己的声明**。

---

## 6. 优先级

| 优先级 | 项 | 为什么现在做 | 预估 |
|---|---|---|---|
| **P0** | H-3 清掉 `.wav` 产物 | 950 KB 且会随每次渲染增长 | 10 分钟 |
| **P0** | H-2 截图脚本不要算测试 / 补断言 | 假覆盖会掩盖真实回归 | 30 分钟 |
| **P1** | H-1 抽 `KeyboardRange` + `WaveShape` 并补单测 | 返工率 37% 的根因；也是后续格式的基础 | 半天 |
| **P1** | M-1 视图注册表 + 长度不硬编码 | 已经出过一次事故 | 2 小时 |
| **P2** | M-2 音名表归一、M-3 残留文案清理、L-1 命名统一 | 低成本，防止漂移 | 1 小时 |
| **P2** | M-5 合成下沉后端 | 决定"加格式要不要改 UI" | 与 H-1 同批 |
| **P3** | M-4 常量命名、L-2 设置表化、L-3 注释补充 | 长期可维护性 | 视情况 |

---

## 7. 结论

这一批改动**功能上可以留在 master**，测试全绿、文档齐全、界面可用。

但**在开始支持第二种格式之前**，P1 的两项必须做完：把领域逻辑从 UI 里拿出来并给它单测，把视图映射从裸 cast 换成注册表。否则每加一种格式，`ContentSlot.cpp` 会再长一次，返工率会再翻一次——而它现在已经是全仓最大的单个文件了。

---

## 8. 处理记录（评审当日完成）

| 编号 | 提交 | 结果 |
|---|---|---|
| **H-3** | `15e2ac6` | `.gitignore` 增加 `*.rendered_temp*.wav`，三个 317 KB 文件移出版本控制 |
| **H-2** | `15e2ac6` | `keyboard-shot` 从 ctest 移除并在配置时说明原因；测试数 36 → 35，随后因新增用例回到 36 |
| **H-1** | `cc8e762` `f61828e` | 音名/八度/音域 → `ui/KeyboardLayout`；四种波形合成 → `ui/WaveShape`。新增测试目标 `vgm_keyboard_test`（20 条断言）。`ContentSlot.cpp` 1381 → 1252 行 |
| **M-1** | `d8fae29` | 视图注册表替代两个手写数组与裸 cast；`SettingsStore` 不再硬编码视图数量 |
| **M-2** | `cc8e762` | 音名表归一为 `midiNoteName()`，设置页不再复制一份 |
| **M-3** | `7e82da7` | 删除 `view.visualiser`，注释示例改用存在的视图 |
| **L-1** | `d8fae29` | 视图 id `scope` → `oscilloscope`，与枚举一致 |
| **L-3** | `7e82da7` | `Theme.h` 写明四个键盘调色板角色必须保持的关系（含深色下黑键应更亮） |
| **M-5** | `f61828e` | 合成本身已移出 UI；"由后端提供 `wave`"这一步留待第一种新格式接入时再做 |
| 后端一致性 | `fb6ac17` | `[4.8]` 在套件加载的每个文件上校验快照形状；无快照的后端明确跳过 |

### 新发现（评审时未列入）

`spanFor` 的八度公式**永远为最后一个不完整八度加满 7 个白键**，因此可以描述出 C9 这种不存在的键；面板一直在绘制循环里用 `if (keyNote > kLastMusicalNote) continue;` 掩盖它。新单测 `ui.keyboard-layout` 第一次运行就报了两条失败，现已把不变量移入 `spanFor` 本身。

这条值得单独记：**它是"逻辑埋在私有嵌套类里"的第二个代价**——缺陷不会被测试发现，只会被一个恰好写在那里的 `continue` 掩盖。

### 仍未做

| 编号 | 原因 |
|---|---|
| **M-4** | `captureState` 的 149 行与 47 个字面量；建议在 `[4.8]` 这类一致性断言稳定后再动 |
| **L-2** | 设置项表化；等键盘设置增长到 8 项以上再抽象，现在做属于过度设计 |
