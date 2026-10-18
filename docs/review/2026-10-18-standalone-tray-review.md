# Standalone 系统托盘 feature 评审（`feature/standalone-tray`）

| 项目 | 内容 |
| --- | --- |
| 评审对象 | `feature/standalone-tray` @ `5d5affc`（`standalone: close to the system tray and control playback from its menu`） |
| 基线 | `c3e714c`（`merge the last-song restore into master`） |
| 规模 | 30 个文件，+2196 / −15 |
| 门禁 | 评审时 `.\build.ps1 -Test`：10/10；修复后重新运行 `.\build.ps1 -Test`：10/10（2026-10-18） |
| 方法 | 逐文件读实现；对 Win32、NSMenu、JUCE 8.0.9 的托盘/菜单路径做源码核对；核对测试与设计文档 |
| 限制 | 未在 macOS 实机运行；未重跑真实桌面冒烟；原评审只读，修复改动与验证见 §10 |
| 状态 | 本文所列 M/L 问题已在工作区修复（未提交）；剩余限制见 §10.2 |
| 级别 | H = 用户可感知或流程无保护；M = 增加后续成本/风险，或边界条件下行为不一致；L = 一致性、整洁或局部资源问题 |

## 1. 一页结论（评审时）

| 维度 | 结论 | 依据 |
| --- | --- | --- |
| 功能正确性 | 未发现当前数据下的已知错误 | 核心模型、关闭策略、图标失败回退均有测试；评审时 10/10 通过 |
| 分层与可测试性 | 好 | `TrayMenu` / `TrayController` 不依赖 GUI，核心测试可在无桌面环境运行 |
| 可维护性 | 中 | 三个平台适配器各遍历一次模型；菜单模型依赖空 `title` 和位置聚合；Loop/Fade 预设表有两份 |
| 跨平台一致性 | 中 | 菜单外壳三平台一致；图标与勾选策略在 Windows/JUCE/macOS 间不一致，当前数据避开了冲突 |
| 状态一致性 | 中 | Play/Pause 标签、图标和 action 分别读状态；Fade 非预设值会出现面板与托盘勾选不一致 |
| 测试 | 逻辑层好，边界层不足 | 替身 `show()` 的合同不严；关闭开关、菜单 Show Window、空引擎 Stop/loop/fade、图标不变量缺测 |
| 文档 | 详细但有三处漂移 | 顶层项数量、`handOver()` 分支、`没有自绘` 与实现不符 |

一句话：**当前提交可以保留；没有 H 级阻塞项。建议下一轮优先做 M-1（统一菜单遍历/分发）、M-4（预设表与勾选统一）和 M-7（修替身、补边界测试），其余随下一次触碰该区域处理。**

> 以上是评审时的结论。本轮已按 §7 的顺序完成 M-1 ~ M-7、L-1 ~ L-3 的修复，当前门禁 10/10；逐项记录和剩余限制见 §10。

## 2. 级别分布

| 级别 | 含义 | 本次数量 |
| --- | --- | --- |
| H | 用户可感知或流程无保护 | 0 |
| M | 增加后续改动成本/风险，或边界条件下行为不一致 | 7 |
| L | 一致性、整洁或局部资源问题 | 3 |

## 3. 问题总表

| ID | 级别 | 维度 | 问题与影响 | 位置 | 建议 |
| --- | --- | --- | --- | --- | --- |
| M-1 | M | 架构/平台适配 | 三个平台适配器各自遍历模型，并各自实现 `title`、分隔符、`enabled`、`ticked`、ID→action 映射；生产路径直接执行捕获的 `action`，`TrayMenu::invoke` 只被测试调用。当前行为一致，但加/改一项菜单要改三处，测试覆盖不到真实分发路径。 | `TrayIcon.cpp:65-163`、`TrayIcon_windows.cpp:147-237`、`TrayIcon_mac.mm:94-195`、`TrayMenu.cpp:108` | 抽平台无关的菜单 renderer：只输出 `beginSubmenu / addItem / addSeparator / endSubmenu` 事件；适配器返回选中的 ID，由 `TrayController` 统一调用 `menu.invoke(id)`。增加 fake renderer 测试。 |
| M-2 | M | 菜单模型 | 空 `title` 同时表示"顶层命令组"和"子菜单边界"；注释说 "an item of the top level"，但 transport group 含两条命令。构造大量使用位置聚合，插入字段或调序会继续编译但语义静默错位。 | `TrayMenu.h:57-62`、`TrayMenu.cpp:40-42,63,81,99,103` | 用命名工厂替代位置初始化：`topLevel(entries)`、`submenu(title, icon, entries)`；增加显式 `isSubmenu()`。有预算时改为 `std::variant<Command, Submenu, Separator>`。同步修正注释。 |
| M-3 | M | 图标/勾选契约 | `ticked` 与 `icon` 可以同时设置，但三平台处理不同：Windows 同时加 `MF_CHECKED` 和带勾角标；JUCE 同时设 `isTicked` 和带勾 image；macOS 只画原生勾选。当前模型避开了冲突，未来同时设置会出现双重标记。未使用的 glyph 与当前模型下不可达的角标分支成为待清理资产。 | `TrayMenuIcons.h:16-18`、`TrayIcon_windows.cpp:161-172`、`TrayIcon.cpp:109-110`、`TrayIcon_mac.mm:53,112`、`TrayMenuIcons.cpp:181-184`、`TrayMenuIconsSvg.h:1-31` | 明确一条策略：原生勾选优先，或统一自绘优先。把策略写进模型不变量和测试；删除未使用的 back/forward/loop1-3/loopForever/fadeOff glyph 与角标分支，或给它们真实消费者。 |
| M-4 | M | 预设/状态 | Loop/Fade 的数值与标签在 `TrayMenu` 和 `TransportPanel` 各写一份。Fade 勾选算法不同：面板按区间（2.5 s 映射到 3 s 档），托盘按精确比较（2.5 s 不勾任何项）。外部或旧会话可写入任意 `double`，因此不是纯理论情况。 | `TrayMenu.cpp:66-97`、`TransportPanel.cpp:37-40,315-321`、`PlaybackEngine.cpp:545-551`、`SessionState.cpp:106` | 抽共享的 `TransportPresets`：数值、标签、`loopPresetIndex()`、`fadePresetIndex()`、非预设值归一化策略集中一处。面板、托盘和测试共用同一份表，并增加 2.5 s 的回归断言。 |
| M-5 | M | 状态一致性 | Play/Pause 的标签、图标和 action 三次读 `isPlaying()`。菜单打开期间播放状态变化时，标签和点击后的动作可能相反；标签和图标也可能来自不同快照。 | `TrayMenu.cpp:47-59` | 在 `build()` 时只读一次 `const bool wasPlaying`，标签、图标、action 共用该快照；或拆成明确的 `play` / `pause` 两个命令。若产品要求"点击时以当前状态为准"，需要明确记录，并处理菜单打开期间标签过期的问题。 |
| M-6 | M | 文档/验证说明 | 手动冒烟文档写"12 个顶层项"，当前 Win32 顶层是 6 条命令/子菜单 + 4 个分隔符 = 10。`handOver()` 文档说"没有新增分支"，但本次提交新增了 `setVisible(true)`。`没有自绘` 与 `TrayMenuIcons` 的自绘 glyph 矛盾。 | `docs/desgin/standalone-system-tray.md:130-132,170-172,233`、`StandaloneApp.cpp:378-383` | 按当前实现更新文档：12→10；补上 `handOver()` 的 `setVisible(true)` 分支；把"没有自绘"限定为"菜单外壳由系统绘制，item glyph 由应用绘制"。 |
| M-7 | M | 测试 | `FakeTrayIcon::show()` 在返回 `showResult` 之前就把 `visible` 设为 `true`，没有 `showCalls`。第二次 close 只断言 controller 的 `isIconVisible()`，即使重复调用 `show()` 也能通过。关闭开关路径、菜单 Show Window、空引擎 Stop/loop/fade、图标/勾选不变量缺测；`TrayMenuIcons` 未编入 core 目标。 | `PlaybackCoreTest.cpp:114-136,942-947,959,967,1011-1029`、`CMakeLists.txt:307-318` | fake 改为 `visible = showResult; ++showCalls; return showResult;`；补 `showCalls == 1`、`! failingIcon->visible`、`setCloseToTray(false)`、`tray.show`、空引擎 Stop/loop/fade 和图标不变量测试。删除遗留的 `setPositionSeconds(0.25)`；把 "stopped engine" 改成 `stop()` 或改成 "paused engine"。 |
| L-1 | L | 注释/API 描述 | 注释与实现不一致：`TrayController.cpp` 说菜单在 `showMenuAsync` 回调后执行，只有 Linux/JUCE 路径异步；`TrayMenu.h` 说"every row has one"图标，子菜单行没有；`TrayIcon.h` 说选中的行交回模型，生产路径直接执行 action；`TrayController.h` 说 fake 把菜单交回测试，实际保存的是 builder。 | `TrayController.cpp:23-25`、`TrayMenu.h:25-26`、`TrayIcon.h:18`、`TrayController.h:18-20` | 按平台分别描述关闭顺序；把"every row"改成"顶层和子菜单父项有图标"；按实际调用路径描述 selected row / builder。 |
| L-2 | L | 测试 API/头文件层次 | `TrayController::getMenu()` 是 public 非 const，只为测试存在；`TrayIconPresenter` 定义在 `TrayController.h`，`TrayIcon.h` 为继承它而 include 整个 controller，间接带入 `SettingsStore`、`PlaybackEngine` 和菜单模型依赖。 | `TrayController.h:23-36,75-76`、`TrayIcon.h:7` | `getMenu()` 返回 `const TrayMenu&`；把 `TrayIconPresenter` 拆到独立 `TrayIconPresenter.h`，让平台适配层只依赖接口。6 个构造参数可收拢为 `Callbacks` 结构（可选）。 |
| L-3 | L | Windows 资源/默认项 | `CreateDIBSection` 返回了非空 `bitmap` 但 `pixels == nullptr` 时直接返回，未 `DeleteObject(bitmap)`。`SetMenuDefaultItem(menu, 1, FALSE)` 硬编码 ID 1，当前第一项固定是 Show Window 才成立。 | `TrayIcon_windows.cpp:60-65,208-209` | 失败路径补 `DeleteObject(bitmap)`；默认项从 `tray.show` 对应的命令 ID 计算，或从 actions 中记录 Show 的 ID。 |

## 4. 测试覆盖

| 层/行为 | 覆盖 | 证据或缺口 |
| --- | --- | --- |
| 菜单模型的 ID、顺序、enabled、ticked、action | 有 | `PlaybackCoreTest.cpp:1015-1119`：经 `icon->menuCallbacks` 的 build/invoke 进入同一菜单，覆盖 show/play/pause/loop/fade/stop/quit |
| presenter 回调装配 | 有 | `PlaybackCoreTest.cpp:1015-1038`：断言 controller 把 build/invoke 交给 presenter，并用它们构建菜单、触发 Show Window |
| 菜单遍历（顺序、自动分隔符、子菜单嵌套） | 有 | `TrayMenuRenderer` + `RecordingTrayMenuRenderer`，`PlaybackCoreTest.cpp:1042-1057` 断言完整事件序列 |
| 关闭到托盘（开关打开） | 有 | `PlaybackCoreTest.cpp:987-1003`：隐藏窗口、显示图标、左键恢复 |
| 关闭到托盘（开关关闭） | 有 | `PlaybackCoreTest.cpp:967-980`：不隐藏、不创建图标、close 继续退出 |
| 图标无法显示时的回退 | 有 | `PlaybackCoreTest.cpp:1164-1185`；fake 的 `visible` 跟随返回值和 `showCalls` |
| 第二次 close 不重复创建图标 | 有 | `PlaybackCoreTest.cpp:1000-1001`：`showCalls == 1` |
| 菜单 Show Window 动作 | 有 | `PlaybackCoreTest.cpp:1037-1038`：经 presenter 的 invoke 回调，与左键走同一控制器 |
| Play/Pause 陈旧点击 | 有 | `PlaybackCoreTest.cpp:1109-1115`：暂停后旧的 `tray.pause` 被拒绝，不会反向播放 |
| 空引擎时 Stop/loop/fade 的 enabled 状态 | 有 | `PlaybackCoreTest.cpp:1132-1150`：Play/Stop 灰掉，Loop/Fade 仍可选 |
| 非预设 Loop/Fade 的勾选 | 有 | `PlaybackCoreTest.cpp:1062-1077`：2.5 s 只勾最近档，5 次循环仍勾一行 |
| 图标/勾选不变量 | 模型层有，栅格层无 | `PlaybackCoreTest.cpp:1082-1101` 锁住"子菜单行不带 glyph、ticked 不与 icon 并存"；`TrayMenuIcons.cpp` 的实际像素仍不在 `vgm_core_test` |
| 设置页与开关的装配 | 部分 | `settings-pages.txt` 覆盖页面存在与可见；controller 层已有开关测试，UI 点击链路仍未单独覆盖 |
| 平台适配器 | 手工 | 设计文档 §5.3 记录 Windows 真实桌面冒烟；macOS 未在 Windows 开发机运行；本轮未重跑真实桌面 |
| 整体门禁 | 通过 | 修复后 `.\build.ps1 -Test`：10/10 |

## 5. 文档/注释同步明细（评审时）

| 文档或注释 | 评审时写法 | 评审时的问题 | 评审时的建议 |
| --- | --- | --- | --- |
| `README.md` / `README.zh-CN.md` | 新增"关闭到系统托盘"一节：开关、左右键、Quit、Linux 尽力而为 | 与实现一致 | 保持 |
| `docs/desgin/standalone-single-instance.md:38-42` | 交接后 `setVisible(true)` + `toFront(true)`，并解释单独 `toFront()` 不够 | 与本次实现一致 | 保持 |
| `docs/desgin/standalone-system-tray.md:233` | "`MN_GETHMENU` 取到 12 个顶层项" | 当前顶层是 Show、Play/Pause、Stop、Loop、Fade、Quit 六条，加 4 个分隔符（Play 前、Loop 前、Fade 前、Quit 前）共 10 项 | 改为 10；或改为"6 条命令 + 4 个分隔符"，避免数字再次漂移 |
| `docs/desgin/standalone-system-tray.md:170-172` | "这条路径在托盘功能之前就存在，本次只确认……没有新增分支" | 本次提交在 `StandaloneApp.cpp:378-383` 新增了 `mainWindow->setVisible(true)` | 写明这是本次新增的分支，并解释 `toFront` 单独调用无法拉回隐藏窗口 |
| `docs/desgin/standalone-system-tray.md:130-132` | "系统负责字体、阴影、暗色模式和 DPI 缩放，代码里没有任何自绘" | `TrayMenuIcons.cpp` 自绘菜单 glyph 和勾选角标 | 限定为"菜单外壳由系统绘制；item glyph 由应用绘制" |
| `TrayController.cpp:23-25` | 菜单在 `showMenuAsync` 的结果回调后执行 action | 只有 Linux/JUCE 路径异步；Windows `TrackPopupMenuEx` 返回后执行，macOS `popUpStatusItemMenu:` 返回后执行 | 改成"菜单关闭后执行"；平台差异单独说明 |
| `TrayMenu.h:25-26` | 每一行都有图标 | 子菜单行 `TrayMenuIcon::none`，只有顶层行和子菜单父项有图标 | 改成"顶层行和子菜单父项有图标；子菜单行保留系统勾选" |
| `TrayIcon.h:18` | 选中的行交回模型 | 生产适配器直接执行 `entry.action`；`TrayMenu::invoke` 只有测试调用 | 按实际路径描述，或先完成 M-1 的统一分发再改注释 |
| `TrayController.h:18-20` | fake 记录回调并把菜单交回测试 | fake 保存的是 `menuBuilder`，测试调用 `controller.getMenu()` | 改成"保存 builder，测试通过 controller 取模型" |
| `PlaybackCoreTest.cpp:959` | `engine.setPositionSeconds(0.25)` | 已删除 seek 测试的遗留，没有断言依赖它 | 删除 |
| `PlaybackCoreTest.cpp:967` | "a stopped engine shows Play" | 实际调用 `engine.pause()`，不是 `stop()` | 改成 `stop()`，或把断言文案改成 paused |

## 6. 做得好的地方

| 方面 | 证据 |
| --- | --- |
| 纯逻辑与 GUI 解耦 | `TrayMenu`、`TrayController` 不依赖桌面；核心测试在无托盘环境也能跑，关闭策略和菜单模型都可测 |
| 图标失败的安全阀 | `TrayController.cpp:103-114`：`show()` 失败就释放 presenter，close 继续退出，不会把窗口藏到没有图标可恢复的状态 |
| 菜单关闭后再执行动作 | Windows 用 `TPM_RETURNCMD`，先 `DestroyMenu` 再执行 action；macOS 在 `popUpStatusItemMenu:` 返回后执行；Quit 不会在菜单还开着时销毁图标 |
| 范围控制 | 设置只在 standalone 出现；Linux 明确提示没有通知区域时图标可能不出现；默认关闭，符合"进程活得比窗口久必须是用户要求" |
| 菜单语义 | Play/Pause 共用一行，标签表达点击后的动作；Loop 控制循环次数而不是循环点；Fade 明确是歌曲结尾淡出 |
| 已有测试 | 评审时 10/10 通过；核心模型覆盖顺序、enabled、ticked、invoke 和关闭策略 |

## 7. 建议的处理顺序（本轮已按此顺序完成）

| 顺序 | 动作 | 对应 ID | 验收标准 |
| --- | --- | --- | --- |
| 1 | 抽平台无关的菜单 renderer，统一 `id → invoke` 分发 | M-1 | 三平台适配器只负责平台 API；fake renderer 能断言顺序、分隔符、enabled、ticked 和选中 ID |
| 2 | 抽 `TransportPresets`，统一 Loop/Fade 数值、标签和勾选算法 | M-4 | 预设表只有一份；2.5 s 在面板和托盘中都勾选同一档 |
| 3 | 修 `FakeTrayIcon` 合同，补关闭开关/Show Window/空引擎/图标不变量测试 | M-7 | `showCalls == 1`；`setCloseToTray(false)` 有测试；空引擎 Stop/loop/fade 有断言 |
| 4 | 用命名工厂和 `isSubmenu()` 显式化菜单模型 | M-2 | 不再依赖位置聚合；插入字段不会静默错位；结构测试通过 |
| 5 | 定图标/勾选策略，清理死 glyph 和角标分支 | M-3 | 三平台行为一致；模型不变量测试锁住"ticked + icon"组合 |
| 6 | 更新文档和注释 | M-6 / L-1 | 12→10；`handOver()` 分支；自绘范围；`showMenuAsync` 等描述与代码一致 |
| 7 | 收尾测试 API、头文件层次和 Windows 小问题 | L-2 / L-3 | `getMenu()` 返回 const；Presenter 独立头文件；失败路径无 GDI 泄漏；默认项不依赖字面量 1 |

这个顺序的依据：第 1 项决定下一次加菜单项是“改一处”还是“改三处”；第 2 项是评审时唯一能让用户看到不一致的边界行为，所以排在测试和模型整理之前。

## 8. 产品语义：本轮决定

| # | 问题 | 本轮决定 | 验证 |
| --- | --- | --- | --- |
| 1 | 空引擎时 Loop/Fade 是否应该置灰 | 保持 enabled，作为下一首歌的默认值；Play/Stop 灰掉 | `PlaybackCoreTest.cpp:1132-1150` |
| 2 | Play/Pause 以"菜单打开时"还是"点击时"的状态为准 | 标签/glyph 用打开时状态；id 区分 `tray.play` / `tray.pause`。动作是明确的，若状态已变，旧 id 被拒绝，不会反向执行 | `PlaybackCoreTest.cpp:1109-1115` |
| 3 | 子菜单行是否允许同时有图标和勾选 | 不允许：子菜单行只用平台原生勾选，glyph 只给顶层命令和子菜单父项 | 模型不变量 `PlaybackCoreTest.cpp:1082-1101` |
| 4 | 顶层项数量是否要作为稳定契约 | 不把 10 当公式；文档写成"6 条命令/子菜单 + 4 个分隔符"，并保留结构测试 | 设计文档 §5.3；`RecordingTrayMenuRenderer` 事件序列 |
| 5 | 非预设 Fade/Loop 如何选行 | Fade 选最近档；Loop 按 1x/2x/3x/forever 归一到最近语义行；整数值 5 次循环显示 3x（不显示 forever） | `TransportPresets.h`；`PlaybackCoreTest.cpp:1062-1077` |

## 9. 附录：评审证据与限制

| 项目 | 结果 |
| --- | --- |
| `git rev-parse HEAD` | `5d5affc` |
| `git rev-parse HEAD^` | `c3e714c` |
| `git diff --shortstat HEAD^ HEAD` | 30 files changed, 2196 insertions(+), 15 deletions(-) |
| 工作区 | 原评审时干净；本轮修复后为未提交改动（见 §10） |
| 测试 | 评审时 `.\build.ps1 -Test`：10/10；修复后同一门禁：10/10 |
| Windows 手动冒烟 | 设计文档 §5.3 有记录；本轮未重跑 |
| macOS | 未实机运行、未编译；结论来自 NSMenu 代码与 JUCE 源码 |
| 未做 | 未做 macOS 视觉检查、未做三平台菜单截图对比、未重跑真实桌面冒烟 |

## 10. 本轮修复记录（工作区，未提交）

### 10.1 逐项状态

| ID | 状态 | 改动 | 验证 |
| --- | --- | --- | --- |
| M-1 | 已修 | 新增 `TrayMenuModel.h` 的 `TrayMenuRenderer` / `renderTrayMenu()`；Win32、NSMenu、JUCE 三个适配器只翻译遍历事件。生产路径不再直接执行 `entry.action`：`TrayIconPresenter.h` 的 `TrayMenuCallbacks` 带 `build` + `invoke`，适配器把选中 id 交回 `TrayMenu::invoke()`，enabled 判断和分发只有一处。 | `PlaybackCoreTest.cpp:1042-1057` 用 `RecordingTrayMenuRenderer` 断言完整事件序列；测试还经 presenter 的 build/invoke 回调驱动菜单；`.\build.ps1 -Test` 10/10 |
| M-2 | 已修 | `TrayMenuGroup::topLevel()` / `submenu()` 命名工厂、`isSubmenu()`；`TrayMenuModel.h` 把模型数据、renderer 接口和菜单行为拆开。没有引入 `std::variant`：当前三组模型不需要按类型访问，命名工厂已消除位置初始化。 | 编译通过；菜单结构测试和事件序列测试 |
| M-3 | 已修 | 明确"原生勾选优先"：`makeTrayMenuIcon()` 删除 `ticked` 参数和角标绘制；子菜单行不再带 glyph；删除 `back` / `forward` / `loop1` / `loop2` / `loop3` / `loopForever` / `fadeOff` 等无消费者资源。 | 模型不变量 `PlaybackCoreTest.cpp:1082-1101`；三平台适配器统一走同一份数据 |
| M-4 | 已修 | 新增 `playback/TransportPresets.h`：Loop/Fade 数值、标签、`loopPresetIndex()`、`fadePresetIndex()`；走带面板和托盘共用。非预设 Fade 选最近档（2.5 s → 3 s），非预设 Loop 也总能选中一行。 | `PlaybackCoreTest.cpp:1022-1035,1062-1077`；面板编译通过 |
| M-5 | 已修 | 不再用同一个 `tray.playPause` 同时表示两种动作：菜单按打开时的 `isPlaying()` 生成 `tray.play` 或 `tray.pause`，标签、glyph、id 共用一次快照。菜单打开期间状态若改变，旧 id 不在重建模型中，点击被拒绝，不会反向执行。 | `PlaybackCoreTest.cpp:1109-1115` 专门覆盖陈旧 `tray.pause` |
| M-6 | 已修 | 设计文档：12 → "10 个顶层项（6 条命令/子菜单 + 4 个分隔符）"；补上 `handOver()` 新增 `setVisible(true)` 的说明；把"没有自绘"限定为"菜单外壳系统绘制、item glyph 应用绘制"；同步 `playPause` → `play` / `pause` 和新的 renderer/invoke 路径。 | 在 `docs/desgin/standalone-system-tray.md` 中搜索无旧的 "12 个顶层""playPause""没有新增分支" 表述（本文 §2/§6 保留评审时的历史引用） |
| M-7 | 已修（模型层） | `FakeTrayIcon` 现在 `visible = showResult`、`showCalls++`；新增关闭开关关、Show Window、空引擎 Stop/Loop/Fade、`show()` 失败、图标/勾选不变量、非预设预设值等测试；删除遗留的 `setPositionSeconds(0.25)`，把 "stopped engine" 文案改成 paused。 | `.\build.ps1 -Test` 10/10；实际 `TrayMenuIcons.cpp` 的像素仍不在 `vgm_core_test`，见 §10.2 |
| L-1 | 已修 | `TrayController.cpp` 的 Quit 注释改成"三个适配器都在菜单关闭后调用"；`TrayIcon.h` 说明适配器只映射 id、由 controller 分发；`TrayMenu.h` 删除 "every row" 的错误说法；`TrayController.h` 的 test 说明与只读 `getMenu()` 一致。 | 代码注释核对 |
| L-2 | 已修 | `TrayIconPresenter` 拆到独立 `TrayIconPresenter.h`；`TrayMenuIcons.h` 改为只包含 `TrayMenuModel.h`，平台适配层不再通过图标头文件间接带入 `PlaybackEngine` / `SettingsStore`；`getMenu()` 改为 `const TrayMenu&`。6 参构造函数收拢为 `Callbacks` 的可选重构未做。 | Standalone 目标编译通过；`TrayIcon.h` 只包含 presenter，`TrayMenuIcons.h` 只包含 model |
| L-3 | 已修 | `CreateDIBSection` 的 `pixels == nullptr` 分支补 `DeleteObject(bitmap)`；默认项按 `renderer.defaultItemId()` 中 `tray.show` 的命令 id 计算，不再硬编码 1。 | Windows 适配器编译通过；默认项逻辑随 `TrayMenuModel` 的 show id 移动 |

### 10.2 剩余限制与有意保留

| 项目 | 状态 |
| --- | --- |
| macOS 实机 / 编译 | 未做。`TrayIcon_mac.mm` 只做了源码核对；Windows 开发机无法链接 Cocoa/AppKit |
| 真实 Windows 桌面冒烟 | 未重跑；`.\build.ps1 -Test` 只证明编译和逻辑测试通过 |
| 托盘 glyph 的实际像素 | 模型策略已测，`TrayMenuIcons.cpp` 的栅格结果仍未编入 `vgm_core_test`；需要 GUI 目标或新增像素测试才能覆盖 |
| `TrayMenuGroup` 的 `std::variant` | 未做。命名工厂 + `isSubmenu()` 已覆盖当前需求；等出现第三种组形态再重构 |
| `TrayController` 6 参构造 | 未收拢为 `Callbacks`。不影响正确性，留作低优先级整洁项 |
| 非预设 Fade 的映射变化 | 从旧的 `<0.5 / <2 / <4 / <8` 阈值改为最近档；2.5 s 仍是 3 s，但 1.5 s 现在选 3 s、7.5 s 现在选 10 s。这是为了托盘和面板共用一条规则而做的有意行为变更 |
| 空引擎的 Loop/Fade | 仍保持 enabled，作为下一首的默认值；Play/Stop 灰掉 |
| 顶层项数量 | 文档改成结构描述；没有把总数写成公式，避免再加一项时再次漂移 |

### 10.3 本轮验证

| 命令 | 结果 |
| --- | --- |
| `.\build.ps1 -Test` | 构建 standalone + 测试目标；10/10 通过 |
| `git diff --check` | 无空白错误 |
| 实际桌面托盘 | 未重跑（见 §10.2） |

> 最终结论：原评审的 0 个 H、7 个 M、3 个 L 均已处理；剩余项是平台实机验证和可选的整洁性重构，不阻塞当前功能。
