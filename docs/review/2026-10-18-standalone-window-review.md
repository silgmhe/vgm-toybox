# Standalone 单实例、原生窗口与 Audio 页评审（`feature/standalone-single-instance`）

- **评审对象**：`feature/standalone-single-instance` @ `5107956`，4 个提交（`2adccf3` 单实例交接 / `7b63a52` clang 二义修复 / `2c664e1` `-Clang` 门禁 / `5107956` 原生窗口 + Audio 页）
- **状态**：已快进合并进 `master`（`master == 5107956`），工作区干净
- **范围**：相对 `b0b35a6`（release 0.2.1）19 个文件，+1470 / −9
- **门禁**：`.\build.ps1 -Clang -Test` → clang 检查 34 个 TU 全过，9 个测试目标通过（2026-10-18 复跑，修复后）
- **方法**：逐文件读实现；命令行交接的每一跳对 JUCE 8.0.9 源码核过（`juce_ApplicationBase.cpp`、`juce_Messaging_windows.cpp`、`juce_DocumentWindow.h`）；平台结论对照仓库里的实测记录与 `%TEMP%` 下的交接日志
- **读法**：§2 ~ §8 是评审当时的判断，不随修复改写；哪些改掉了、改成了什么，见 §9 处理记录

## 1. 一页结论

| 维度 | 结论 | 依据 |
| --- | --- | --- |
| 单实例交接 | 好 | Windows 实测：文件名带空格的 `.vgz` 被正确交接；每一跳与 JUCE 源码一致（见 3.2） |
| 分层 | 好 | hooks 是运行时空表，插件里连页面都不创建；解析逻辑进 `playback/StartupSong.h`，可单独测 |
| 可维护性 | 中上 | 侧栏页码是手写整数；hooks 逐字段清空；三处注释还停在"两页" |
| 可测试性 | 纯逻辑好，装配层空白 | `[12]` 覆盖了解析与选择；设置面板与 Audio 行零脚本覆盖 |
| 文档同步 | 好，四处滞后 | README 双语、两份设计文档、`Info.plist` 都对得上；滞后项见 L-2、L-5 |
| 跨平台 | 认真 | Linux 明确不做交接并写了原因；macOS 的 Finder 语义单独说明；X11 窗口标志有实测值 |
| 工程化 | 好 | `-Clang` 进了 `build.ps1`，且它抓到的第一件事（`{}` 二义）当场修掉 |

一句话：**可以合（事实上已经合了）；L-1 建议本轮就改，M-1 ~ M-3 排进下一次触碰这个区域的提交。**

## 2. 问题清单

| ID | 级别 | 一句话 | 位置 |
| --- | --- | --- | --- |
| L-1 | L（建议本轮改） | 同一条命令行规则在 26 行内给出两种相反说法，前一种与 JUCE 实际行为相反 | `playback/StartupSong.h:20-21` vs `:46-49` |
| M-1 | M | 侧栏页码是手写 0/1/2，与 `addView` 顺序隐式耦合，加一页要改三处 | `ui/SettingsOverlay.cpp:473-483`、`:495-502` |
| M-2 | M | hooks 逐字段清空，以后加字段忘了清就是悬空 lambda | `Standalone/StandaloneApp.cpp:171-176` |
| M-3 | M | 设置面板与 Audio 页没有自动化覆盖，装配层行为只靠手测 | `plugin/Tests/scripts/*.txt`、`Tests/AppHarnessTest.cpp` |
| L-2 | L | "一个设置页就是一次 `addSubItem`"、"list of two pages" 已过期 | `ui/SettingsOverlay.h:151-152`、`.cpp:468-469` |
| L-3 | L | Windows 的按钮与 macOS 的说明文字共用一个 componentID，没注释说明这是刻意跨平台对齐 | `ui/SettingsOverlay.cpp:217`、`:227` |
| L-4 | L | `handOver()` 直接 `static_cast` 处理器，"窗口必带 processor"的契约没有写下来，也没有空检查 | `Standalone/StandaloneApp.cpp:297-303` |
| L-5 | L | `CODEBUDDY.md` 的 `options:` 示例行没跟上 `-Clang` / `-Test` / `-Target` 等开关 | `CODEBUDDY.md:25` |
| L-6 | L | 两处各拼一份 `%TEMP%` 日志，格式不同、目的相同 | `StandaloneApp.cpp:312`、`PluginProcessor.cpp:19` |
| L-7 | L | 文件头多一个空行；Audio 页的 500 ms 定时器从构造起常开，靠 `isShowing()` 自己收敛 | `ui/SettingsOverlay.cpp:1`、`:398` |

## 3. 事实基线

### 3.1 四个提交

| 提交 | 标题 | 规模 | 层次 |
| --- | --- | --- | --- |
| `2adccf3` | a double-clicked song goes into the running window | 15 文件 +749/−1 | 交接链路、hooks 表、`reuseWindow` 设置 |
| `7b63a52` | spell out `juce::File()` where the braces are ambiguous to clang | 2 文件 +6/−2 | 质量修复（`{}` 二义） |
| `2c664e1` | build.ps1: `-Clang`, a second opinion on what MSVC accepts | 3 文件 +133/−4 | 门禁与文档 |
| `5107956` | the standalone window is the system's, and its Audio page is the real one | 6 文件 +588/−8 | 原生窗口收尾、三页侧栏、`Info.plist` |

### 3.2 一次双击的完整链路（Windows）

1. 资源管理器起第二个进程，argv 带文件路径。`moreThanOneInstanceAllowed()` 返回 `!getReuseWindow()`（`StandaloneApp.cpp:108-113`；Linux 恒为 `true`）。
2. 返回 `false` 时 JUCE 取命令行文本广播给首实例。Windows 这一截是 `GetCommandLineW()` 去掉 exe 之后的**原样尾部**（`findEndOfToken`），引号保持资源管理器写下的样子；macOS/Linux 则由 `getCommandLineParameterArray()` 重新加引号拼接——两条路径都没有反斜杠转义。
3. 首实例 `anotherInstanceStarted()` → `chooseSong()`：`parseCommandLine`（引号是分隔符、不进入值）→ `chooseStartupSong`（第一个"存在且 `canOpen`"的参数）。
4. 窗口已在 → `handOver()`：写交接日志、把文件塞进 processor、`toFront(true)`。窗口还没建好（macOS 常见）→ 存 `pendingSong`，`initialise()` 里补一次。
5. processor 只存一格 `instanceSong`（消息线程写、消息线程读，不需要锁）；编辑器定时器取走 → `loadFile()`。
6. `loadFile()` 会置 `startupHandled`，因此 argv 里的启动曲不再播——**后到的交接优先**。这个抢占语义目前只体现在代码顺序里，注释和设计文档都没写。

手工证据（`%TEMP%\vgmtoybox-second-instance.log`，本轮复核仍在）：

```
Another instance started with: G:\VGM\Metal_Slug_X_(SNK_Neo_Geo)\04 Judgment -X-.vgz
Handed to the running window.
reuse setting: on
```

要点：第一行是 `handOver()` 里 `file.getFullPathName()` 的结果，按构造不带引号，**不能**拿它推断原始字符串。真正的证据是"文件名里有空格，却交接成功了"——`parseCommandLine` 在不带引号时会把这一行切成 3 段，三段都不是文件，结果只会是 `No argument was a supported song file.`。所以原始字符串一定带着引号，且引号前没有反斜杠。

### 3.3 四层边界

| 层 | 只做 | 不碰 |
| --- | --- | --- |
| `StandaloneApp` | 进程/窗口生命周期、装配能力表、交接入口 | 解析规则、播放 |
| `playback/StartupSong.h` | 纯规则：从 argv 或命令行字符串挑第一首能播的 | 实例、窗口、设置 |
| `PluginProcessor` | 存一格 `instanceSong` | 加载、开窗 |
| `PluginEditor` | 定时器取走并 `loadFile`；启动曲只播一次 | 第二实例的存在 |
| `SettingsOverlay` | 按 `available()` 决定第三行；显示 hooks 给的设备描述 | 平台判断 |

### 3.4 测试覆盖

| 层 | 状态 | 证据 |
| --- | --- | --- |
| 解析与选择规则 | 有 | `PlaybackCoreTest` `[12]`：引号、空格、多参数、无参数、不支持的扩展名、空 `canOpen` |
| `reuseWindow` 设置往返 | 有 | `PlaybackCoreTest` `[9]` |
| 窗口行为 | 有 | 5 条脚本：`daw-sync` / `daw-sync-follow` / `load-and-play` / `settings-pages` / `window-basics` |
| 设置面板与 Audio 页 | 评审时**无**，现在有 | 评审时脚本里没有一处 `settings`（M-3）；修复后由 `settings-pages.txt` 覆盖装配（§9） |
| 第二实例端到端 | 手工 | 上面的 `%TEMP%` 日志 |
| clang 第二意见 | 有 | `-Clang` 检查 34 个 TU |

### 3.5 文档同步

| 文档 | 状态 |
| --- | --- |
| `README.md` / `README.zh-CN.md` | 一致：参数、双击关联、单实例说明都写了 |
| `CODEBUDDY.md` | 评审时 `options:` 示例行漏了新开关（L-5）；修复后已补 `-Clang` / `-Test` / `-Target` |
| `docs/desgin/standalone-single-instance.md` | 一致：六节（问题 / 现状 / 实现 / 平台差异 / 诊断 / 验证），macOS Finder 与 Linux 不交接都写了 |
| `docs/desgin/native-window-and-audio-settings.md` | 评审时一致：三页、componentID、手工验证结论都在；修复后 §4.4 改为指向新的 `settings-pages.txt` |
| `docs/desgin/settings-and-theme-design.md` | 评审时 `:340` 写"`ViewStack` 两页"（L-2）；修复后加了指向 Audio 页的注 |
| `plugin/Source/Standalone/Info.plist` | 一致：5 个导出 UTI（vgm/vgz/s98/dro/gym）、`LSHandlerRank: Alternate`、`CFBundleTypeRole: Viewer` |

## 4. 做得好的地方

1. **交接链路的每一跳都对得上 JUCE 源码**，平台差异是查出来的而不是猜的：`moreThanOneInstanceAllowed()` 的三平台差异、`anotherInstanceStarted()` 拿到的字符串形状、macOS 的 Finder open-document 语义，注释和设计文档里都有出处。
2. **用运行时空表代替 `#ifdef`**。插件里 `Available()` 恒为 `nullptr`，所以 `SettingsOverlay` 连 Audio 页都不构造——宿主里不可能出现"选音频设备"这种属于应用的事。`StandaloneHooks.h` 的注释还解释了为什么不能用宏（`JUCE_STANDALONE_APPLICATION` 只在 wrapper 的 TU 里定义）。
3. **规则搬进 `playback/` 并真的被测到**。`parseCommandLine` 的注释直说"这个缺陷以前无法被测试"，`[12]` 把它钉住——这正是把"一行没法调用的代码"变成可测单元的标准做法。
4. **clang 第二意见形成了闭环**：先手工发现 `= {}` 的二义（`7b63a52`），再把检查做成开关（`2c664e1`），并在代码里写明 MSVC 为什么不报。门禁不是摆设，它抓到的第一件事当场就修了。
5. **"不做"也写清楚**。Linux 没有 `broadcastMessage`，所以不做交接，只保证每个进程吃自己的 argv；X11 的窗口标志补的是实测值（`0x3c/0x7a` → `0x3e/0x7e`），没有拍脑袋。
6. **交互细节到位**：`toFront(true)` 把窗口带到前台；交接和启动两条路径都写 `%TEMP%` 面包屑；`reuse off` 时新进程直接读 argv。
7. **文档与代码同步率高**：三份设计文档、双语 README、`CODEBUDDY.md` 都跟上了，测试编号 `[9]` / `[12]` 在设计文档里能查到。

## 5. 问题详述

### L-1 同一条命令行规则，26 行内两种相反说法

`StartupSong.h:20-21`：

```20:21:plugin/Source/playback/StartupSong.h
	The raw command line *string* is re-quoted for display and must never be split by hand: on
	Windows a path with spaces arrives there with backslashes in front of the quotes.
```

`StartupSong.h:46-49`：

```46:49:plugin/Source/playback/StartupSong.h
	`JUCEApplication::anotherInstanceStarted()` hands over the *command line string* rather than
	the array above: on Windows it is the raw tail of the second process's command line (a path
	with spaces keeps the quotes Explorer put around it), on macOS JUCE joins one or more quoted
	paths with spaces.
```

后一种是对的。依据有两条：

- JUCE 8.0.9 源码：Windows GUI 分支的 `getCommandLineParameters()` 是 `findEndOfToken(GetCommandLineW(), ...)` 取 exe 之后的原样尾部；广播走 `WM_COPYDATA`，接收侧不做改写。全程只在 `CommandLineToArgvW` 那一侧才会**吃掉**转义，不会**添加**反斜杠。
- 仓库里的实测：文件名带空格的 `.vgz` 被成功交接（3.2 节）。若真如 20-21 行所说带反斜杠，`parseCommandLine` 会把 `\` 留在值里，`File` 路径不存在，结果只会是 `No argument was a supported song file.`。

风险不在运行时，在误导：下一个改这里的人可能照着第一段去写"先 unescape `\"`"的处理，而 `parseCommandLine` 的引号是分隔符、不进入值，两层反斜杠处理会再引入一个新的空格路径 bug。建议把 20-21 行的第二句删掉，改成指向下面的 `parseCommandLine()`：

```text
`arguments` must be the platform's own argv - `JUCEApplicationBase::getCommandLineParameterArray()`.
The command line *string* a second instance hands over has a different shape (see
parseCommandLine() below) and must never be split by hand.
```

### M-1 侧栏页码是手写整数，和 `addView` 顺序隐式耦合

```473:483:plugin/Source/ui/SettingsOverlay.cpp
		rootItem->addSubItem (new CategoryItem ("Theme", 0, [this] (int page) { pages.setCurrentView (page); }));
		rootItem->addSubItem (new CategoryItem ("General", 1, [this] (int page) { pages.setCurrentView (page); }));
		...
		rootItem->addSubItem (new CategoryItem ("Audio", 2, [this] (int page) { pages.setCurrentView (page); }));
```

```495:502:plugin/Source/ui/SettingsOverlay.cpp
		pages.addView (themePage);
		pages.addView (generalPage);
		...
		pages.addView (audioPage);
```

页码、`addView` 顺序、构造顺序三处必须一致，编译器不查。在 General 和 Audio 之间插一页，侧栏能正常显示，点进去却是错页面。建议用一个 `enum Section { theme = 0, general = 1, audio = 2 };` 同时喂 `CategoryItem` 和 `addView`，在 `refreshFromStore()` 末尾加一句 `jassert (pages.getNumViews() == audio + 1)`，插页漏改会当场断言。

### M-2 hooks 逐字段清空，加字段即埋悬空 lambda

```171:176:plugin/Source/Standalone/StandaloneApp.cpp
		// The hooks hold onto the holder, which has just gone. Cleared field by field: `= {}` on a
		// pair of std::functions is the same ambiguity clang reports for juce::File (copy and move
		// assignment both fit an empty braced list).
		auto& table = standalone::hooks();
		table.describeAudioDevice = std::function<juce::String (void)>();
		table.showAudioSettings = std::function<void (void)>();
```

注释说的二义是真的（`7b63a52` 就是在修这个），但清空逻辑离字段定义太远：以后在 `Hooks` 里加第三个字段，改的人只看到结构体，不会想到 300 行外还有一份必须同步的清单。漏了不会编译错——`SettingsOverlay` 的两处调用只判空，判不出悬空。建议把这段搬进 `StandaloneHooks.h`：

```cpp
void clear (void) noexcept
{
    // Typed, because `= {}` on std::function is ambiguous to clang (see commit 7b63a52).
    describeAudioDevice = std::function<juce::String (void)>();
    showAudioSettings = std::function<void (void)>();
}
```

`shutdown()` 里只剩 `standalone::hooks().clear();`，字段和清空在同一屏里。

### M-3 设置面板与 Audio 页没有自动化覆盖

四条窗口脚本里搜不到一处 `settings`；`vgm_ui_test` 只覆盖按钮这类不依赖插件的组件。设计文档把 Audio 页记作"手工验证"，理由是"控制台进程没有音频设备、对话框是 app-modal"——这个理由对**真实设备枚举和对话框**成立，但对**装配**不成立：第三行加不加、点了会不会切页，都只取决于 `available()` 这一个布尔值，完全可以用假 hook 测。

现在做不到的原因是时序：harness 在解析脚本**之前**就构造了 editor，而 `available()` 是在 `SettingsOverlay` 构造时读的。harness 已经有 `# wrapper vst3` 这种"必须在 processor 之前决定"的先例，加一个同形的 `# hooks audio`（约 10 行）即可。配套要做一件小事：给三个 `CategoryItem` 加 componentID（`settings.tree.theme` / `.general` / `.audio`），否则脚本只能断言默认页，点不到第三行。之后一条脚本就能覆盖：

```text
# hooks audio
menu invoke options.settings
assert settings.page.theme visible true
click settings.tree.audio
assert settings.page.audio visible true
assert settings.audio.device text "Fake device"
```

这同时是 M-1 的安全网：插页插错，脚本当场红。

### L-2 三处注释还停在"两页"

- `SettingsOverlay.h:151-152`：说加设置页就是"one `addSubItem`"。实际是三件事：页面类、`addSubItem`、`pages.addView`，外加 M-1 的页码。
- `SettingsOverlay.cpp:468-469`："list of two pages wants"。现在最多三页。
- `docs/desgin/settings-and-theme-design.md:340`："`ViewStack` 两页"。这是上一轮的历史记录，但 `SettingsOverlay.h` 仍把它当 §6 引用，读起来像当前状态；加一句"（第三页见 native-window-and-audio-settings.md）"即可。

### L-3 跨平台共用一个 componentID，没有注释

`SettingsOverlay.cpp:217` 与 `:227` 让 Windows 的 `reuseWindowButton` 和 macOS 的 `reuseWindowNote` 共用 `settings.general.reuseWindow`。两行分别在 `#if JUCE_WINDOWS` / `#elif JUCE_MAC` 的分支里，一个二进制里只有一个，所以共用是安全的；而且这让"这一行存在"的断言在两个平台都成立，这个意图是好的。问题只是没有一行注释说明它是故意的：扫一眼头文件会看到两个声明、同看代码会看到两个相同的 ID，第一反应是复制粘贴错误。写一行"deliberately the same ID on both platforms"即可。

### L-4 `handOver()` 的契约没有写下来

```297:303:plugin/Source/Standalone/StandaloneApp.cpp
	void handOver (const juce::File& file)
	{
		...
		static_cast<ToyboxAudioProcessor*> (mainWindow->getAudioProcessor())
			->openSongFromAnotherInstance (file);
```

当前恒成立：窗口是 `ToyboxFilterWindow`，processor 一定是我们自己的。但这是全特性里唯一没有空检查的解引用，而"窗口必带 processor"只存在于作者脑子里。建议：

```cpp
if (auto* processor = dynamic_cast<ToyboxAudioProcessor*> (mainWindow->getAudioProcessor()))
{
    processor->openSongFromAnotherInstance (file);
}
```

频率是"每次双击一次"，`dynamic_cast` 的开销无关紧要；顺带把 `mainWindow == nullptr` 也挡掉（`initialise()` 里 `createWindow()` 失败时）。

### L-5 `CODEBUDDY.md` 的开关示例行没跟上

`CODEBUDDY.md:25` 现在是：

```text
# options: -Arch x86 | -Config Debug | -Clean | -NoPlugin | -Proxy <url>
```

本轮新增的 `-Clang` 不在里面，`-Test` / `-Target` / `-TestSong` / `-NoProxy` 也没有。这一行是示例不是清单，但它是 `CODEBUDDY.md` 里唯一讲开关的地方，容易被当成全集。补成 `-Clang | -Test | -Target <t>` 即可。

### L-6 两份 `%TEMP%` 日志，两段相似的拼装

`StandaloneApp::writeBreadcrumb`（`:312`）写 `vgmtoybox-second-instance.log`（"Another instance started with: …" + 结果 + `reuse setting`）；`PluginProcessor::getStartupFile`（`:46-49`）写 `vgmtoybox-startup.log`（"Started with: …" + 结果）。目的相同、格式不同、各自拼两行。出现第三个入口（文件关联、拖到 EXE 上）时会抄第三遍。可提一个 `standalone::writeBreadcrumb(file, line)`，不做也行。

### L-7 整洁性

- `SettingsOverlay.cpp:1` 是空行，`#include` 之间还夹着空行。
- `SettingsOverlay.cpp:398` 的 `startTimer(500)` 从构造起常开，`timerCallback` 靠 `isShowing()` 自己收敛。2 次/秒可忽略；按显示/隐藏开关更合语义，不做也行。

## 6. 扩展性预演

| 变化 | 现在要改几处 | 采纳 M-1 / M-2 后 |
| --- | --- | --- |
| 加第四页（比如 MIDI） | 页面类 + `addSubItem` + `addView` + 手写页码，漏一处静默错页 | enum 加一项，漏了 `jassert` 会响 |
| 换掉占位 UI、接真设备 | 只动 `AudioSettingsPage` 与 hooks | 不变 |
| `Hooks` 加第三个字段 | 字段 + 300 行外的清空，漏了变悬空 | `clear()` 就在字段下面 |
| 插件里出现 Audio 页 | 需要新的平台宏，设计上不允许 | 不变 |
| 给设置页写脚本 | 无假 hook 可用（M-3）；TreeView 行不是 Component，`click` 按组件 ID 也点不到 | 假 hook 已有（`# hooks audio`）；脚本断言"页在不在"而不是"点了换不换页"，后者要真实的树坐标 |

## 7. 建议的处理顺序

1. **L-1 注释**（5 分钟）——本轮就改，它是唯一一处"照着做会写错代码"的文档。
2. **L-2 / L-3 / L-7 注释与空行**（20 分钟）——随下一次触碰这个目录的提交捎带。
3. **M-2 `clear()` + L-4 空检查**（15 分钟）——同一提交，都是"把隐含契约写进代码"。
4. **M-1 enum + `jassert`**（30 分钟）——纯重构，行为不变，门禁能验证。
5. **M-3 harness `# hooks audio` + 行 ID + `settings-pages.txt`**（半天）——价值最高，也最容易被跳过；做完 M-1 的安全网才真正闭合。
6. L-5 / L-6 顺手。

## 8. 待拍板

- **L-1 的改法**：保留"两种形状"的正面描述 + 把规则指向 `parseCommandLine()`。~~删掉第二句~~——第二段本身是对的，删的是与它矛盾的第一段。
- **M-1 用 enum 还是 `pages.getNumViews()`**：用 enum。`getNumViews()` 只是让数字消失，顺序耦合仍在；enum 把"第几个"写在名字里，`jassert` 兜住两边长度。
- **M-2 `clear()` 放 header**：接受。`= {}` 的二义只在空 braced list，typed 赋值没有二义，clang 检查通过。
- **M-3 是否为测试给三行加 componentID**：**不加**。`CategoryItem` 是 `TreeViewItem`，不是 `Component`；脚本的 `click` 按组件 ID 找 Component，给了 ID 也点不到树行（要真实屏幕坐标）。脚本改为断言"页存在/可见/设备行文本"，装配层正是这次要覆盖的部分。
- **`StartupSong.h` 是否移到 `app/`**：不动。设计文档 `standalone-single-instance.md` 明确放在 `playback/`，本轮没有功能收益。

## 9. 处理记录（2026-10-18，同一分支）

| ID | 状态 | 改法 |
| --- | --- | --- |
| L-1 | ✅ 已修 | `StartupSong.h` 的第一段改成"argv 是平台自己的数组；第二实例给的是另一种形状（见 `parseCommandLine()`）"，删掉"Windows 会在引号前加反斜杠"那句——JUCE 8.0.9 不加。第二段（真实形状）保持不变 |
| M-1 | ✅ 已修 | `SettingsOverlay.cpp` 匿名命名空间里加 `enum PageIndex`（`themePageIndex` / `generalPageIndex` / `audioPageIndex`），侧栏行与 `addView` 都用它；构造函数末尾 `jassert (pages.getNumViews() == ... + 1)` |
| M-2 | ✅ 已修 | `Hooks::clear()` 写在字段正下方（typed 赋值 + 为什么的注释），`shutdown()` 只剩 `standalone::hooks().clear();` |
| M-3 | ✅ 已修 | harness 加 `# hooks audio`：`scriptAsksForAudioHooks()` 读首行（与 `# wrapper` 同一个位置、同一个原因），在构造处理器之前填两个假 hook；新增 `plugin/Tests/scripts/settings-pages.txt`。**没有**给侧栏行加 componentID，理由见 §8 |
| L-2 | ✅ 已修 | `SettingsOverlay.h` 的"加一页"改成四件事并指向 `PageIndex`；`.cpp` 的 "list of two pages" 改 "list of a few pages"；`settings-and-theme-design.md:340` 加注指向 Audio 页 |
| L-3 | ✅ 已修 | 共用 ID 处加三行注释：一个二进制里只有一个分支，脚本不必知道哪个平台 |
| L-4 | ✅ 已修 | `handOver()`：`mainWindow == nullptr` 直接返回，再用 `dynamic_cast<ToyboxAudioProcessor*>` 取处理器；不是我们的窗口就丢掉这首歌 |
| L-5 | ✅ 已修 | `CODEBUDDY.md:25` 补 `-Clang` / `-Test` / `-Target` / `-NoProxy`；同文件的 "Four scripts" 改 "Five scripts" 并写明 `# hooks audio` |
| L-6 | ⏸ 不改 | 两处 `%TEMP%` 日志保持现状：评审结论本身就是"出现第三个入口再说"，为两处调用加抽象不划算 |
| L-7 | 无需改动 / 保留 | `SettingsOverlay.cpp:1` 当前就是 `#include`，评审时看到的空行不在文件里；Audio 页定时器保留 `isShowing()` 自收敛的写法——500 ms 只做两次字符串比较，改成 show/hide 驱动要重写生命周期，收益为零 |

### 9.1 修复后的门禁

```
.\build.ps1 -Clang -Test
  → clang: 34 file(s) accepted by clang
  → vgm_core_test / vgm_ui_test / vgm_app_test / vgm_mixer_test + 5 条窗口脚本
    = 9 个测试目标通过
```

`settings-pages.txt` 的实际输出：

```
> menu invoke options.settings
     OK  menu invoke options.settings
> assert settings.close      visible == true
     OK  settings.close.visible == 'true'   (was: true)
> assert settings.page.theme visible == true
     OK  settings.page.theme.visible == 'true'   (was: true)
> assert settings.page.audio enabled == true
     OK  settings.page.audio.enabled == 'true'   (was: true)
> assert settings.page.audio visible == false
     OK  settings.page.audio.visible == 'false'   (was: false)
> assert settings.audio.device text contains "Fake device"
     OK  settings.audio.device.text contains 'Fake device'   (was: Fake device)
Result    : all checks passed
```

反向对照是免费的：第一次运行时脚本漏写了首行的 `# hooks audio`，harness 按插件的两页装配构造 → 三条 Audio 断言全部报 `no component with the id 'settings.page.audio'` 并 exit 1。也就是说"应用形状"和"插件形状"确实被脚本区分开了，而不是两条路都能过。
