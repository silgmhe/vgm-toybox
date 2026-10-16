# 架构与代码审查（2026-10-16）

> **审查对象**：`99aa57ab4ebbcc8d86fec8a44432f25c0b53470d`
> （短号 `99aa57a`，2026-10-16，"keep the View tick on the page that is actually showing"）
> **分支**：`feature/ui-refactor`（远端已同步 `99aa57a`）
> **范围**：`plugin/Source/**`（7156 行 / 55 文件）、`plugin/Tests/**`（1096 / 3）、`vgmplayer/**`（6451 / 13）、`build.ps1`、`.github/workflows`、`docs/**`
> **手段**：全量阅读关键路径（引擎、后端、UI 装配）+ 结构计量（脚本见 §8，可复现）+ 一次独立结构检索
> **没做的**：没有运行 SonarQube / CodeQL / Semgrep / Snyk（本机没有这些工具，CI 里也没有接入）。静态检查的实际水平 = **MSVC /W4 干净**（`juce_recommended_config_flags`）+ 结构计量 + 人工阅读。本文不声称有覆盖扫描或依赖漏洞扫描的结果。
>
> 文档用途：**反思当前设计 → 改进设计**。因此每条问题都给出"为什么这么设计会变坏"和"改成什么样"，而不只是"改一下"。

---

## 1. 一页结论

| 维度 | 评价 | 依据（详见后文） |
|---|---|---|
| 分层与依赖方向 | **好** ✓ | `ui → playback` 单向；`playback` 对 `ui`/`backends` **零反向依赖**；具体后端在顶层注册（§3.1） |
| 模块化 / 可扩展 | **好，且刚变好** ✓ | 后端=策略（加后端=注册表一处）；页面=View（加页面=一个类）；按钮=统一绘制器 |
| 复杂度 | **偏高，集中在 3 处** ⚠ | `PlaybackEngine.cpp` 866 行、`render()` 214 行、`MixerPanel::refresh()` 134 行（§2.2） |
| 可维护性 | **中上** ✓ | 注释质量高、无 TODO 遗留（0 处）、文档与代码同仓；但面板刷新是"每帧全量重写"（§4 M-2） |
| 可测试性 | **两极** ⚠ | 引擎/混音/像素测试堪称模范（含"不链接 libvgm"的机械证明）；**UI 与进程装配层 0 覆盖**，而最近的 bug 全在那里（§4 H-2） |
| 工程化 / CI | **有短板** ✗ | CI **不编译插件**（`-DTOYBOX_BUILD_PLUGIN=OFF`），也**不跑任何测试**（§4 H-3） |
| 实时安全 | **好，但有一处时间问题** ⚠ | 渲染路径零分配/零 I/O ✓；但加载**全程持锁**（含文件 I/O + 解压 + libvgm 初始化）→ 音频线程 `TryLock` 失败即整块静音（§4 H-1） |
| 技术债总量 | **可控** | 无 TODO 堆积、无死代码堆积；债集中在"加载事务化""UI 刷新""CI"三处 |

**如果只做三件事**：

1. **H-1 加载事务化**（把文件 I/O、解压、libvgm 初始化移出锁；锁只保护"指针换入"）——它是唯一会**被用户听见**的问题（加载瞬间掉音），也是后续一切"重载/热切换"的前置。
2. **H-3 CI 真的编译插件并跑三个测试**——现在 7156 行（含全部 UI）在流程上**零验证**。
3. **H-2 让启动/菜单这类"进程装配层"可测**——最近三个 bug（带空格路径 ✗、勾选不跟随 ✗、关联指向旧副本 ✗）两个落在这里，而它们都不是"难"，是"没有测试"。

---

## 2. 事实基线（可核查）

### 2.1 规模

| 层 | 行数 | 文件数 | 说明 |
|---|---|---|---|
| `plugin/Source/ui` | **3631** | 33 | 最大的一层（占 `Source` 的 51%）——符合"这是个看重界面的播放器"，但也是复杂度主要来源 |
| `plugin/Source/playback` | 1505 | 6 | 引擎核心 |
| `plugin/Source/backends` | 1284 | 12 | libvgm + fake |
| `plugin/Source`（含以上） | 7156 | 55 | 插件 + standalone |
| `plugin/Tests` | 1096 | 3 | 三个控制台测试目标 |
| `vgmplayer` | **6451** | 13 | 旧版独立前端，**不共享 `playback/`**（§4 M-6） |

其他：`build.ps1` 345 行（唯一构建入口）；设计文档 1240 行（4 篇）；README 155 行；CODEBUDDY 194 行。

### 2.2 最长的函数（结构检索结果，已逐处核对）

| 位置 | 函数 | 行数 |
|---|---|---|
| `playback/PlaybackEngine.cpp:651` | `PlaybackEngine::render` | **214** |
| `backends/libvgm/LibvgmSongInstance.cpp:135` | `enumerate` | 154 |
| `ui/MixerPanel.cpp:110` | `MixerPanel::refresh` | 134 |
| `ui/TransportPanel.cpp:38` | 构造函数 | 95 |
| `PluginEditor.cpp:22` | 构造函数 | 95 |
| `playback/PlaybackEngine.cpp:466` | `createPlayers` | 85 |
| `ui/TransportPanel.cpp:341` | `refresh` | 82 |
| `ui/ChannelStrip.cpp:29` | 构造函数 | 70 |
| `ui/IconButton.cpp:10` | `paintButton` | 69 |
| `ui/MixerPanel.cpp:288` | `layoutStrips` | 59 |
| `ui/MainMenu.h:55` | `getMenuForIndex` | 58 |

> 判读：**"构造函数 70~95 行"在这一版是合理的**（控件装配 + 回调接线，本来就是长的，而且它把"接线"集中在一处 ✓）；真正该拆的是 `render`(214)、`enumerate`(154)、`MixerPanel::refresh`(134) —— 前者是"每块都在跑的热路径"，后两者是"每帧/每首都在跑的全量重建"。

### 2.3 依赖方向（证据）

- `ui/ → playback/`：7 处（`TransportPanel.cpp:9`、`ChipPanel.cpp:5`、`SongInfoPanel.cpp:6-7`、`ChannelStrip.h:8`、`PlayView.cpp:6`、`MixerPanel.h:10`、`DropHintOverlay.cpp:3`）；**`ui` 中没有任何文件 include `backends/` 或 `PluginProcessor.h`** ✓
- `playback/ → ui|backends|PluginProcessor`：**0 处** ✓（方向干净）
- `backends/ → playback/`：只 include `SongBackend.h` / `BackendRegistry.h` ✓
- 具体后端的注册在**顶层**（`PluginProcessor.cpp:14` 调 `registerBuiltInBackends()`）✓ —— 依赖倒置的标准形状
- 唯一双向：`PluginEditor.h → PluginProcessor.h` 与 `PluginProcessor.cpp → PluginEditor.h`（宿主要素：`createEditor`）——**可接受**（JUCE 的固定形状），但意味着 `PluginProcessor.h` 不宜再变大。

### 2.4 线程与锁（关键结论）

- 一把锁：`PlaybackEngine.h:177` `juce::CriticalSection lock`。
- **音频线程**：仅 `render()`（`:700`）用 `ScopedTryLock`；**失败即整块静音**（这是**有意的**：宁可静音也不产生 glitch，见 `:41-45`、`:683-704` 的注释 ✓ 设计正确）。
- **消息线程**：`loadFile`(`:62`)、`unloadFile`(`:99`)、`prepare`(`:25`)、`set*` 系列、`setDefaultRouting`(`:630`)。
- **`loadFile` 全程持锁**，锁内包含：`loadFileAsData`（文件 I/O + 分配）→ `createPlayers`(`:466`，注册表查找、vgz 解压、libvgm 初始化、每轨 `createPlayer` + `start`）。→ 见 §4 H-1。
- 锁可重入被**隐式依赖**：`rebuildPlayers`(`:557`，注释要求持锁) 内部调用 `setPositionSeconds`(`:574`，再次取锁)。→ 见 §4 M-5。
- 渲染路径**零分配、零文件 I/O** ✓（`render` 只写 `prepare` 预分配的缓冲；后端写预分配的 `renderBuffer`）✓ —— 这是这个代码库最值得保持的纪律之一。

### 2.5 测试的形状

| 目标 | 覆盖 | 亮点 |
|---|---|---|
| `vgm_core_test` | 混合器数学（对已知 RMS 断言）、两级音量、seek/曲终、**多总线路由**全矩阵、纯播放器回退 | **不链接 libvgm** → "核心仍依赖格式引擎"就链接失败 ✓ 机械证明解耦 |
| `vgm_mixer_test` | 真实 .vgm/.vgz：加载与元数据、**unity 混音 == 纯播放（−1000.00 dB 逐样本）**、分轨隔离、seek/loop、96 kHz、输出 WAV | "−1000.00 dB" 是**不可绕过**的回归门 |
| `vgm_ui_test` | 按钮像素不变量（投影/圆角/hairline/按下收缩/聚焦环/发光范围/窄道衰减） | 把"肉眼才看得见"的回归变成断言 ✓ |

**覆盖缺口（明确回答）**：`ToyboxAudioProcessor::getStartupFile()`、启动重试与面包屑、`ui/MainMenu.h`、`ui/ViewStack.h`、`PluginEditor` 的装配与命令 —— **无任何测试**（`plugin/Tests` 全目录 grep `startup|MainMenu|ViewStack` 无命中；三个目标也不编译这些源文件）。

### 2.6 CI 的真实覆盖

`.github/workflows/GHA.yml`（64 行，"Build Tests"）以 **`-DTOYBOX_BUILD_PLUGIN=OFF`** 配置（`:45`），且**没有**任何 `vgm_mixer_test` / `vgm_core_test` / `vgm_ui_test` / `build.ps1` 的调用。
→ 结论：**CI 既不编译插件（含全部 UI），也不跑任何测试** ✗。

---

## 3. 做得对的地方（请保持）

1. **依赖方向**：`playback` 不认识 UI 与具体后端；后端在顶层注册。加一个后端 = 实现 `SongBackend` + 注册表一行 ✓（`backends/BuiltInBackends.cpp:26`）。
2. **实时纪律**：`render` 无分配、无 I/O；音频线程用 `TryLock` 且**失败时选择静音而不是 glitch** ✓（这是正确的权衡）。
3. **两处"结构性防错"**（比"小心一点"更可靠）：
   - `ui/ViewStack.h`：窗口=外壳、页面=View → 加页面不需要动窗口/菜单；
   - `ui/ButtonPaint.h`：一个绘制器 + `buttonShadowInset` 约定 → "按钮变小/漏投影"这类问题在结构上被封住。
4. **测试里的"机械证明"**：core test 不链接 libvgm；mixer test 的 −1000.00 dB；ui test 的像素不变量。三者都比"再多几个单元测试"更有价值。
5. **文档与代码同仓**：`docs/desgin/`（4 篇，1240 行）+ `CODEBUDDY.md`（写给下一位改代码的人）+ README。**提交信息质量很高**（近 20 条能从标题读出"为什么"）。
6. **无 TODO 堆积**（全仓 0 处 `TODO/FIXME/HACK`）——债务是"被写下来的"，不是"被藏起来的"。

---

## 4. 问题清单（按严重度）

> 严重度定义：**H** = 用户能感知或流程上无保护；**M** = 增加后续改动成本/风险；**L** = 一致性与整洁。
> 本程序是**本地桌面应用、无网络、无特权操作、输入只来自用户选择的歌曲文件**，因此不存在 OWASP 意义上的入口面；下面没有 CRITICAL 安全项（唯一相关提醒见 M-7）。

### H-1 加载全程持锁 → 加载瞬间掉音 ⚠（最大的一笔债）

- **位置**：`playback/PlaybackEngine.cpp:62-95`（锁内做 `loadFileAsData` + `createPlayers:466` + vgz 解压 + libvgm 初始化 + 每轨 `start`）；音频侧 `:700` `TryLock` 失败即静音。
- **现象**：打开一首较大的歌（或压缩 vgz）时，消息线程持锁跨越毫秒级到秒级的 I/O 与初始化；期间每次音频回调都可能拿到 0 锁 → 连续静音块 = **可听见的掉音**。
- **为什么这么写会变坏**：锁的语义变成了"保护整个加载过程"，而这恰恰是最长的一段工作；将来任何"重新加载 / 切换后端 / 热重载"都会在同一把锁上放大。
- **建议（"准备-换入"，两阶段提交）**：
  1. 在**锁外**构建一个完整的、不可变的"歌曲会话"（后端实例、轨道播放器、预分配缓冲、元数据）；
  2. 取锁只做**指针换入 + 少量状态赋值**（微秒级）；
  3. 释放后再做"旧会话"的回收（放到锁外/或交给后台线程）。
  这样 `TryLock` 几乎不会再失败，且语义更清晰：**锁保护的是"当前会话指针"，不是"加载动作"**。
- **工作量**：中（1~2 天，含把 `createPlayers` 改成返回会话对象）。
- **顺带收益**：这正是 §5 里"可视化数据通路"的第一个落点——会话对象天然携带"上一首歌"的清理与"新歌"的元数据。

### H-2 进程装配层 0 覆盖，而最近的 bug 都落在那里 ⚠

- **位置**：`PluginProcessor.cpp:17-55`（`getStartupFile`）、`PluginEditor.cpp:302-353`（启动加载/重试/面包屑）、`ui/MainMenu.h`（勾选与可用性）、`ui/ViewStack.h`（当前页）。
- **证据**：近三个真实缺陷 —— `67af914`（带空格路径分不出参数 ✗）、`99aa57a`（View 勾选不跟随 ✗）、关联指向旧副本（✗，这一条属于外部状态）—— **没有一个能被现有测试发现**。
- **建议**：
  1. 把"从参数数组里挑出歌曲"抽成**纯函数**（例如 `juce::String chooseSongArgument (const juce::StringArray&, std::function<bool (const juce::File&)> canOpen)`），放进 `playback/`（或 `plugin/Source/Startup.h`）并**加进 `vgm_core_test`**：覆盖带空格、带引号、无效路径、多参数、无参数；
  2. 给 `ViewStack` 写一个"无窗口"测试（它只依赖 `juce::Component`，可在 `vgm_ui_test` 里建两个假 View，断言"当前页索引 + 可见性 + `songChanged` 广播 + `handleKey` 只到当前页"）。
- **工作量**：小（2~4 小时）。
- **为什么值得**：把"装配层"从"只能靠手动点"变成"可断言"，之后每次加页面/加命令都受益。

### H-3 CI 不编译插件、不跑测试 ✗

- **位置**：`.github/workflows/GHA.yml:45`（`-DTOYBOX_BUILD_PLUGIN=OFF`）；全流程无测试调用。
- **影响**：7156 行（含全部 UI 与插件装配）在主流程上**零验证**；三个测试目标从上线起就没有被自动执行过。
- **建议**（按代价从小到大）：
  1. 加一个 `windows-latest` job：跑 `build.ps1`（或拆出 `configure/build/package` 三个函数后单独调用）+ 依次跑 `vgm_core_test` / `vgm_ui_test`（不需要素材）与 `vgm_mixer_test`（需要一首小素材；可用仓库里已有的 `docs/assets` 之外另存一个极小的测试 vgz，或该 job 跳过它）；
  2. 至少让 `-DTOYBOX_BUILD_PLUGIN=ON` 在 Windows job 上**能编译过**——"UI 编译失败"目前在 CI 上不可见。
- **工作量**：小到中（半天~1 天，主要是把 `build.ps1` 的步骤拆开以便复用）。

### M-1 `PlaybackEngine` 一类的四份职责

- **位置**：`playback/PlaybackEngine.h/.cpp`：866 行 .cpp、228 行 .h、26 个方法；`render()` 214 行；`createPlayers()` 85 行。
- **现状判读**：类本身**是内聚的**（都围绕"当前会话的播放"），但已承担 4 件事：会话加载（`loadFile`/`createPlayers`）、路由策略（`applyDefaultRouting`/`setDefaultRouting`）、走带状态（loop/fade/position）、以及**实时渲染**（`render` 内混轨、混总线、清总线、master 音量、位置推进）。
- **建议**（渐进，不要一次性重构）：
  - 触碰加载时 → 抽出 `SongSession`（配合 H-1）；
  - 触碰 `render` 时 → 抽出 `mixTracks()` / `mixBuses()` / `advancePosition()`，让 `render` 变成 ~40 行的"装配顺序"；
  - 触碰路由时 → 把"路由决策"抽成纯数据 `RoutingPlan`（可单测：给一组轨道与目标 → 期望的连线）。
- **工作量**：中（分散在若干次改动里完成）。

### M-2 面板刷新是"每帧全量重建"

- **位置**：`ui/MixerPanel.cpp:110` `refresh()` 134 行（每 15 Hz 重建/更新整条通道列表与标签）、`ui/TransportPanel.cpp:341`（82 行）。
- **影响**：热路径上的浪费（即便 JUCE 有 early-out，也是每帧上百次比较/可能的布局）；更麻烦的是**改动风险**——"每帧都重写一切"意味着任何一处状态漏同步都不会暴露，直到用户看见闪烁。
- **建议**：
  1. 拆成两层：`syncStructure()`（歌曲/路由/可见性变化时调用）+ `refreshValues()`（每帧只写变化的值）；
  2. "只在变化时"模式已在 `ChannelStrip::setValues()` 里用过（M/S 翻转才 `repaint`）——把它推广成项目惯例，并写进 `docs/desgin/`。
- **工作量**：小（2~4 小时）。

### M-3 卡片套路重复三份

- **位置**：`kHeaderHeight` 定义在 `MixerPanel.cpp:13`、`ChipPanel.cpp:13`、`VisualiserView.cpp:20`；"`reduced(cardPadding, 8)` + `headerBounds = inner.removeFromTop(kHeaderHeight)`"在 `MixerPanel.cpp:265-267` 与 `ChipPanel.cpp:68-69` 逐字重复；表头文字绘制三处同形（`MixerPanel.cpp:366-369`、`ChipPanel.cpp:90-93`、`MixerPanel.cpp:374-378`）。
- **影响**：即将新增的**可视化面板**会照着复制第四份 ✗ —— 这正是"加功能越来越慢"的典型路径。
- **建议**：把 header 行提到 `CardComponent`（`getCardHeaderBounds()` + `paintCardHeader(title, subtitle)`），常量进 `Theme.h`（`theme::cardHeaderHeight`）。
- **工作量**：小（2~3 小时），**收益随页面数量线性增长**。

### M-4 同一份"格式列表文案"派生 5 次

- **位置**：唯一来源 `backends/libvgm/LibvgmBackend.cpp:11`；`BackendRegistry.cpp:34/46/57` 拼 3 种字符串；句式再现于 `PluginEditor.cpp:52`、`:252`（"Open a … file"）、`ui/DropHintOverlay.cpp:20`、`ui/SongInfoPanel.cpp:47`。
- **建议**：把"面向用户的句子"收进 `ui/Format.h`（已有 `formatTime`）或 `BackendRegistry` 的少量方法（`getOpenPrompt()` / `getDropPrompt()`）；文案改动只在一处 ✓。
- **工作量**：trivial（30 分钟）。

### M-5 锁可重入的隐式契约

- **位置**：`PlaybackEngine.cpp:557` `rebuildPlayers`（要求持锁）→ `:574` `setPositionSeconds`（再次取锁）；另一处 `:630 setDefaultRouting` → 无锁的 `applyDefaultRouting`。
- **风险**：`juce::CriticalSection` 在 Windows/POSIX 上都是可重入的 ✓，所以今天不会死锁；但这是**隐式契约**：任何"换锁/加锁顺序/改成 `std::mutex`"的动作都会瞬间变成死锁 ✗。
- **建议**：把需要持锁的内部动作命名为 `*Locked()` 私有方法（不取锁），公开方法只取一次锁并调用它；`rebuildPlayers` 明确标注"必须持锁调用"并只被这样的方法调用。
- **工作量**：小（1~2 小时，纯机械）。

### M-6 两套前端长期并存

- **事实**：`vgmplayer/`（6451 行、13 文件）**不共享** `playback/`；它产出 `dist/VGMPlay64.exe`。同时 `plugin/Source` 是重写的第二套前端（7156 行）。
- **影响**：libvgm 用法、设置、结尾行为等修一处要修两处；新人也需要先搞清"改哪一套"。
- **建议**：**写一份一句话决策**（不必写代码）：例如"`vgmplayer/` 冻结为遗留 CLI，只接受必要的安全/正确性修复；所有新功能只进 `plugin/`"，或"计划把 `vgmplayer` 迁到 `playback/` 之上（分阶段：先共享后端注册与加载）"。把它写进 `docs/desgin/`，避免每次都要重新讨论。
- **工作量**：决策（0 代码）。

### M-7 依赖外部解析器（提醒，不是缺陷）

- 歌曲文件由 libvgm 解析；恶意/损坏文件属于"唯一的外部输入面"。当前防护是"只加载用户显式选择的文件" + 定期更新 vendored 库。**建议**：在 `docs/desgin/` 里记录"libvgm 更新策略"（何时同步上游、如何回归），把这件事从"临时决定"变成"流程"。
- **工作量**：文档（30 分钟）。

### L-1 文档搬家后引用失效（11 处）

- **位置**：`README.md:63`、`CODEBUDDY.md:84,163`、`ui/ButtonPaint.h:11`、`ui/ChannelStrip.cpp:267`、`ui/IconButton.cpp:12`、`ui/LightLookAndFeel.cpp:63`、`ui/MixerPanel.cpp:12,144`、`ui/Theme.h:80`、`Tests/UiButtonTest.cpp:3` —— 全部仍指向 `docs/ui-button-design.md`（已移动到 `docs/desgin/`）。
- **建议**：机械替换为 `docs/desgin/ui-button-design.md`（§8 给了命令）。
- **工作量**：trivial。

### L-2 缺少工程化小配置

- `.editorconfig`（缩进=Tab、编码、行尾 ✓）、`.clang-format`（可选：现有代码风格已稳定，加它是为了"以后不用在 PR 里争格式"）——两者目前都**不存在**（`CMakePresets.json`、`.clang-tidy` 同样没有）。
- **建议**：先加 `.editorconfig`（零风险）；`.clang-tidy` 建议**暂缓**（需要一套取舍，否则会淹没在噪音里）。
- **工作量**：trivial。

### L-3 测试内部的重复

- 三个测试各自定义 `check()`（`PlaybackCoreTest.cpp:31`、`PlaybackEngineTest.cpp:169`、`UiButtonTest.cpp:35`）；`renderSeconds()` 在 `PlaybackCoreTest.cpp:54` 与 `PlaybackEngineTest.cpp:83` 重复。
- **建议**：抽 `plugin/Tests/TestSupport.h`（不含 JUCE UI 依赖的公共部分）。

### L-4 头内实现与 .cpp 实现的混用

- `ui/MainMenu.h`（58 行 `getMenuForIndex` 内联）、`ui/ViewStack.h`、`ui/View.h` 是头内实现；`ui/PlayView.*`、`ui/TransportPanel.*` 等是 .h/.cpp 分离。
- **判读**：**不是问题**（小类头内是合理选择 ✓），但建议把判据写进 CODEBUDDY（"超过 ~120 行或需要被多个目标链接 → 拆 .cpp"），免得下一个人犹豫。

### L-5 债务标记没有约定

- 全仓 0 处 `TODO/FIXME` ✓（好事），但也意味着"已知的将就"只存在于散文注释里 ✗ —— 机器找不到。
- **建议**：**本文档即债务登记册**（§6）；需要临时标记时用统一前缀 `// DEBT:`（便于 grep 与定期清理）。

---

## 5. 设计模式机会（具体到文件）

| 机会 | 用在哪 | 解决什么 | 备注 |
|---|---|---|---|
| **两阶段构造 / "准备-换入"** | `PlaybackEngine::loadFile` + `createPlayers` | H-1（锁的语义扩张） | 会话对象不可变、锁只换指针 |
| **策略：混音阶段** | `render()` 内部 | M-1（214 行） | 后端已是策略 ✓；把"混音阶段"也策略化（直出 / 总线路由 / 未来的效果） |
| **拉模型 + 无锁环形缓冲（SPSC）** | 新增 `playback/ScopeTap.h` | 可视化页的数据通路（波形/频谱的前置） | **不要**在音频线程回调 UI；UI 每帧"拉"最近 N 个样本 + 一个写指针 |
| **中介者** | `ui/ViewStack.h` ✓（已具备） | 加页面 = 一个类 | 新面板只需实现 `View` |
| **模板方法** | `ui/CardComponent` | M-3（卡片头套路） | 把 header 行与内边距上提，面板只实现内容 |
| **观察者（谨慎）** | 引擎 → UI | 只在**非实时线程**用（例如"加载完成"通知） | 热路径用拉模型（上一条），别混 |
| **命令模式** | 菜单/快捷键 | 目前**不要**（5 个菜单项用 lambda 合适） | 触发条件：菜单动作 >10 项，或需要"快捷键表/撤销/可配置"时再上 `ApplicationCommandManager` |
| **服务定位器 → 显式依赖** | `BackendRegistry`（12 处 `getInstance`） | 目前可接受 ✓ | 触发条件：若要"每首歌可选不同后端"或"并行测试多套后端"，就改成显式注入 |

**反模式提醒**：`BackendRegistry` 是**全局可变单例**（写一次、读多次）。当前用法是有节制的 ✓；请在 CODEBUDDY 里写一句"注册只发生在启动装配处"作为纪律，避免它逐步变成"什么都往里塞"的上帝对象。

---

## 6. 技术债登记册

| ID | 债务 | 影响 | 成本 | 优先级 | 建议时机 |
|---|---|---|---|---|---|
| H-1 | 加载全程持锁（含 I/O 与初始化） | 用户可听见掉音 | 中 | **P0** | 现在（下次碰引擎时） |
| H-3 | CI 不编译插件、不跑测试 | 7156 行零验证 | 小~中 | **P0** | 现在 |
| H-2 | 启动/菜单/视图栈 0 覆盖 | bug 只能靠用户发现（已发生 2 次） | 小 | **P1** | 现在（下一个功能前） |
| M-2 | 面板每帧全量刷新 | 热路径浪费 + 改动风险 | 小 | P1 | 做可视化页之前（它会复用） |
| M-3 | 卡片套路三份 | 加页面越加越慢 | 小 | P1 | 做可视化页之前 |
| M-1 | `PlaybackEngine` 四职责 | 阅读/改动成本 | 中 | P2 | 渐进（触碰即拆） |
| M-5 | 可重入锁隐式契约 | 换锁即死锁 | 小 | P2 | 下次碰 `rebuildPlayers` 时 |
| M-4 | 格式文案派生 5 处 | 文案不一致 | trivial | P2 | 随手 |
| M-6 | 两套前端并存 | 修两处 | 决策 | P2 | 有空时写决策 |
| M-7 | libvgm 更新策略未文档化 | 长期安全/正确性 | 文档 | P2 | 有空时 |
| L-1..L-5 | 引用失效 / 缺配置 / 测试内重复 / 风格判据 / 无标记约定 | 整洁度 | trivial | P3 | 随手 |

**有意的负债（不打算还，也请写下来）**：
- **单锁 + 失败静音**：以极简换取实时安全 ✓（`TryLock` 失败静音是正确取舍）；
- **小类头内实现**：减少 .cpp 往返 ✓；
- **无安装程序、无注册表写入**：这是产品原则 ✓（见 README"绿色"一节）；
- **`vgmplayer` 并存**：历史资产，未共享代码 ✓（需决策，不必立刻统一）；
- **无 `.clang-tidy`**：避免噪音淹没真问题 ✓。

---

## 7. 下一步路线（与"可视化页"对齐）

1. **地基（P0）**：H-1 加载事务化 + H-3 CI 真正编译并跑测试。→ 之后每加一个功能都受益。
2. **可测（P1）**：H-2 把"参数→歌曲"抽成纯函数并进 `vgm_core_test`；给 `ViewStack`/`MainMenu` 加无窗口测试。
3. **铺路（P1）**：M-2 刷新分层 + M-3 卡片头提到 `CardComponent`。→ 可视化页将直接复用这两件。
4. **可视化第一步**：新增 `playback/ScopeTap.h`（SPSC 环形缓冲 + 写指针；UI 侧"拉"）：波形 View → 频谱 View（FFT）→ MIDI/键盘 → FM 参数。
5. **纪律**：每个可视化面板都带一条可自动化的断言（像素不变量或数据断言）——沿用 `vgm_ui_test` 的思路：**"改坏了能发现"是交付的一部分**。

---

## 8. 附录：本次审查的复现方式

```powershell
# 规模与结构（本文 §2.1/§2.2 的数字来源）
Get-ChildItem -Recurse -File -Include *.h,*.cpp -Path plugin\Source | ForEach-Object { (Get-Content $_.FullName).Count }

# 依赖方向（§2.3）
Select-String -Path plugin\Source\ui\*.h,plugin\Source\ui\*.cpp -Pattern 'playback/|backends/|PluginProcessor.h'

# 锁与线程（§2.4）
Select-String -Path plugin\Source\playback\PlaybackEngine.cpp -Pattern 'ScopedLock|ScopedTryLock'

# 覆盖缺口（§2.5）
Select-String -Path plugin\Tests\*.cpp -Pattern 'startup|MainMenu|ViewStack'

# CI 的真实覆盖（§2.6）
Select-String -Path .github\workflows\*.yml -Pattern 'BUILD_PLUGIN|vgm_.*_test|build.ps1'

# 文档引用失效（§L-1）
Select-String -Path README.md,CODEBUDDY.md,plugin\Source\ui\*.h,plugin\Source\ui\*.cpp,plugin\Tests\*.cpp -Pattern 'docs/ui-button-design'

# 一次性修复 §L-1（替换后请重跑上面的检查确认）
Get-ChildItem -Recurse -File -Include *.md,*.h,*.cpp -Path README.md,CODEBUDDY.md,plugin | ForEach-Object {
    $t = [System.IO.File]::ReadAllText($_.FullName)
    if ($t.Contains('docs/ui-button-design.md')) {
        [System.IO.File]::WriteAllText($_.FullName, $t.Replace('docs/ui-button-design.md', 'docs/desgin/ui-button-design.md'))
        'fixed: ' + $_.Name
    }
}
```

**审查边界声明**：本文没有覆盖 `libs/`（vendored 上游）与 `vgmplayer/` 的内部实现质量，只在"与 `plugin/` 的关系"层面提及；也没有做性能剖析（未跑 profiler）——文中关于性能的判断均基于"代码结构（锁范围、分配位置、刷新频率）"这一可核查的事实。

---

## 附：复核记录（2026-10-16）

写完之后回读，发现**两处判断需要修正** ✓。原文保留不动，差异记在这里 —— "当时怎么判断、之后怎么改"本身就是这份文档的用途 ✓。（代码基线 `99aa57a` 之后只改过文档与构建脚本 ✓，所以技术结论仍成立 ✓。）

### R-1 `H-1` 的触发条件写得过宽

原文："它是唯一会**被用户听见**的问题（加载瞬间掉音）" ✓。准确说法：**冷加载**（当时没在播放）**听不见** ✓ —— 那几块静音本来就该是静音 ✓；**播放中切歌 / 再加载**才听得见 ✓；另外 **seek**（`setPositionSeconds` 同样取全锁 ✓）属同族 ✓，原文未提 ✓。**H 级维持**（切歌是常见操作 ✓），但触发条件已按此写明 ✓。

### R-2 `M-2`（面板每帧全量刷新）描述有误 —— 已重定义

原文说 `MixerPanel::refresh()`"每帧全量重建" ✗。读代码后确认**它本来就是分层的** ✓：
- `MixerPanel.cpp:115-120`：只在**文件变了或轨道数变了**时才 `rebuildStrips()` ✓；
- `MixerPanel.cpp:162-209`：输出选择器列表也只在**宿主总线布局变了或选择变了**时重建 ✓（注释原文就写着"每帧重建 65 个 combo item 是无意义的开销" ✓）。

即：引用没错，**结论夸大了** ✗。M-2 的实际内容因此不是性能修复 ✗，而是**把这条契约写明** ✓ —— 已随 A#4 完成：`refresh()` 拆为 `syncStructure()`（结构 ✓）与 `refreshValues()`（值 ✓），两处都写了"每帧路径只做值更新"的理由 ✓。

### R-3 A 部分的条目现在是什么状态

| review 条目 | 现状 | 证据 |
|---|---|---|
| **H-2** 装配层 0 覆盖 | **完成** ✓ | 纯函数 `playback/StartupSong.h` + `vgm_core_test` 的 `[5]` 共 7 项断言（含"路径含空格"回归 ✓）；`vgm_app_test` 可无窗口构造**真实** editor ✓（`3c7c36a`、`d9587f8` ✓） |
| **H-3** CI 不编译插件、不跑测试 | **完成** ✓ | `build.ps1` 增加 `-Target`/`-Test`/`-TestSong` ✓ + job `plugin-tests`（`windows-latest` ✓）+ 触发分支扩到 `feature/**` ✓（`3e2778e` ✓；CI 首次真实运行待观察 ✓） |
| **M-2** 刷新分层 | **已重定义并完成** ✓ | 见 R-2 ✓ |
| **M-3** 卡片头套路 ×3 | **完成** ✓ | `CardComponent::{getCardHeaderArea, getCardInnerArea, paintCardHeaderRow}` ✓；`MixerPanel` / `ChipPanel` 不再各写一份 ✓；视觉零回归（截图核对 ✓） |
| **H-1** 加载事务化 / **M-5** 锁契约 | 未动 ✓ | 计划见 `docs/desgin/testing-architecture.md` §4 的 P5 ✓ |
