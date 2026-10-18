# Standalone：双击一首曲子是"复用一个窗口"还是"再开一个"

- **状态**：已实现（`plugin/Source/Standalone/StandaloneApp.cpp`）
- **日期**：2026-10-18

## 1. 问题

双击一首关联了本程序的曲子，每次都会开一个**新的独立进程**。听歌时这是多余的：用户想要的是
"歌换掉，窗口一个"。而对比两首曲子时又需要两个窗口。所以这是一个**用户可选的行为**，放在
设置页里。

## 2. 现状为什么是"每次新开"

JUCE 给插件生成的 standalone 应用（`juce_audio_plugin_client_Standalone.cpp`）把两个开关写成常量：

```cpp
bool moreThanOneInstanceAllowed() override           { return true; }   // 允许第二个进程
void anotherInstanceStarted (const String&) override {}               // 收到了也不做事
```

而 JUCE 本身已经实现了机制（`juce_ApplicationBase.cpp`）：

- `InterProcessLock`（跨平台）判断"是否已有实例"；
- 抢不到锁的进程把**自己的命令行**通过 `MessageManager::broadcastMessage()` 发给已在运行的那个，
  然后 `initialiseApp()` 直接返回 false —— 第二个进程连窗口都不建；
- 已在运行的实例在 `anotherInstanceStarted(commandLine)` 里收到那条命令行。

所以需要做的只有两件事：把第一个开关接到设置上，并在第二个回调里把歌交给现有窗口。

## 3. 实现

| 位置 | 作用 |
|---|---|
| `plugin/Source/Standalone/StandaloneApp.cpp` | 自定义 `JUCEApplication`；`CMakeLists.txt` 里 `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1` 只给 `VGMToybox_Standalone` 目标 |
| `playback/StartupSong.h` | 新增 `parseCommandLine()` / `chooseSongFromInstance()`：`anotherInstanceStarted()` 给的是**字符串**，引号里的空格不能切（和启动参数是同一条教训） |
| `PluginProcessor` | `openSongFromAnotherInstance()` 存下文件；`takeSongFromAnotherInstance()` 取走 |
| `PluginEditor` | 定时器里取走并走**同一个 `loadFile()`**，所以行为和拖拽完全一致（异步加载、加载期间旧曲子继续播、完成后替换） |
| `settings/SettingsStore` | `standalone.reuseWindow`，缺省 **true**；随改随写（下一次启动在任何窗口存在之前就要读它） |
| `ui/SettingsOverlay` | 常规页那一行：Windows 是开关，macOS 是陈述，Linux 不显示 |

第二实例把歌交出去后，`mainWindow->toFront(true)` 把窗口提到前面——Windows 上没人会替你抬窗口。

## 4. 平台差异（这是本功能唯一需要解释的地方）

| | 能否"复用" | 能否"每次新开" | 说明 |
|---|---|---|---|
| Windows | ✓ `WM_COPYDATA` | ✓（默认的进程模型） | 两个方向都是真的 |
| macOS | ✓ 分布式通知 + `openFile:` Apple Event | **✗ 做不到** | Finder 从不给运行中的 app 开第二份，文档总是交给已运行的那个；只有 `open -n` 能强制新进程 |
| Linux | ✗ `broadcastMessage` 是空实现 | ✓ | 所以 Linux 上不显示这个设置 |

macOS 因此没有"关掉开关 = 新窗口"这回事，设置页写一行事实而不是给一个点了没用的复选框。

macOS 的另一半是**让 Finder 知道这个 app 能打开这些文件**：`plugin/Source/Standalone/Info.plist`
通过 `juce_add_plugin(... PLIST_TO_MERGE ...)` 声明 `CFBundleDocumentTypes` 与五个
`UTExportedTypeDeclarations`（vgm/vgz/s98/dro/gym）。没有它，`打开方式` 里根本看不到这个 app。

## 5. 诊断

- `%TEMP%\vgmtoybox-second-instance.log`：每次投递写一行（交给谁 / 为什么没交成 + 当前的设置值 +
  设置文件路径）。这是这个功能唯一的"外部视角"，和启动路径的 `vgmtoybox-startup.log` 同一个意图。
- 设置文件是 `<VALUE name="standalone.reuseWindow" val="0"/>` —— JUCE 的 `PropertiesFile` 只读
  `<VALUE>` 子元素，写在根元素上的属性会被**静默忽略**（`vgm_core_test` 的 `[9]` 有一条照着这个
  写的检查）。

## 6. 验证

- `vgm_core_test` `[12]`：命令行解析（裸路径、带引号的空格路径、选项在前、macOS 多文档），
  `[9]`：`reuseWindow` 的默认值、写入文件、手写文件的读取；
- 真机（Windows）：默认设置下第二次启动 **48 ms 退出、进程数保持 1**、截图确认第一个窗口换成了
  第二首曲子；把设置改成 `0` 后**两个进程同时存在**、没有投递日志；
- macOS 的 `.app` 声明与事件路径无法在本地验证（没有 Mac），留给 CI 的 macOS 作业编译与首次发布
  时的 `打开方式` 实测。
