# Standalone：启动时恢复上次打开的歌曲

- **状态**：已实现（`plugin/Source/PluginEditor.cpp`、`plugin/Source/settings/SettingsStore.cpp`、
  `plugin/Source/ui/SettingsOverlay.cpp`、`plugin/Source/playback/PlaybackEngine.cpp`）
- **日期**：2026-10-18
- **分支**：`feature/standalone-last-song`

## 1. 问题

直接启动 Standalone（而不是双击一首歌）得到的永远是一个空窗口，即使上一秒还在听同一首曲子。
听一张专辑、改一点混音、关掉程序，再打开就要重新找文件——这是这个功能要消掉的重复动作。

它必须是一个**可关的开关**，因为不是所有会话都希望被接着：临时打开一首歌看一眼、随后退出，
下次启动不应该又把它端上来。开关放在设置面板的 General 页，和"复用运行中的窗口"放在一起，
因为它们回答的是同一类问题：**启动时发生什么**。

## 2. 先定下来的七件事

| # | 问题 | 决定 | 理由 |
|---|---|---|---|
| 1 | 默认值 | **开** | 新安装和 `Restore defaults` 之后都应该是"接着上次"，这才是启动的默认期待 |
| 2 | 什么算"上次的歌曲" | 每次**成功加载**后记录；`File > Close Song` **清空** | 关闭是用户主动说"不要这首"；崩溃或被强杀时没走到 close，记录还在，仍能恢复 |
| 3 | 恢复后是否播放 | **不播放**，停在开头 | 回来的是窗口，不是播放；一启动就出声（哪怕半秒）是这个"隐形"功能最响的部分 |
| 4 | 文件不存在 / 打不开 | 不存在：**静默跳过**；存在但加载失败：状态行报错，**记录保留** | 文件可能只是暂时没挂上（移动硬盘），一次失败就清掉会让下次也没得恢复 |
| 5 | 存在哪里、什么时候写 | 现有 `settings.xml`，键 `standalone.lastSong`；**加载成功即写**，不等退出 | 崩溃也不丢；不新开文件，见 §3 |
| 6 | 与显式传入的歌曲的优先级 | **显式传入优先**：命令行 / 第二实例交接 / macOS Finder 文档 > 恢复 | 用户的直接意图永远赢过默认行为 |
| 7 | 生效范围 | 三个平台的 Standalone 都生效；开关只在 Standalone 的 General 页出现，插件里没有这一行 | 插件的歌曲属于宿主工程，"上次应用打开时"这个概念不存在 |

**范围假设**（本次不做）：不恢复播放位置、循环、静音、混音、输出路由，不做"最近文件"列表，
不改双击打开与复用窗口的既有行为。只持久化**一个路径**。

## 3. 为什么仍然放进 `settings.xml`

曾考虑过单独的文件（`last-song.txt` / `session.xml`）、JUCE 的 `ApplicationProperties`
（`VGMToybox.settings`，存音频设备状态的那个）和系统"最近文档"。结论是留在 `settings.xml`：

| 方案 | 不采用的原因 |
|---|---|
| 单独的 `last-song.txt` | 只换来"重置设置不清它"一点；要重写一遍路径解析、原子写、失败上报、测试重定向 |
| `ApplicationProperties`（`VGMToybox.settings`） | 那是 JUCE 存音频设备状态的文件，没有测试重定向，也不属于我们的设置模型 |
| 系统最近文档 / 注册表 | 与 `SettingsStore` 明确"不进注册表"的设计冲突，三平台行为还不一致 |
| **`settings.xml`（采用）** | 已有 `save()` 失败 → `lastError()` → General 页显示这条链；`VGMTOYBOX_SETTINGS_DIR` 已能把整份设置指到临时目录，集成测试直接可用；键名与 `theme.mode`、`standalone.reuseWindow` 一致，缺键即默认值，不需要版本迁移 |

代价是 `Restore defaults` 会把"上次歌曲"一起清掉。这是可解释的：重置设置就是回到一个干净的
程序，而不是"设置清了、但它还偷偷记着上次的歌"。

文件里的样子（JUCE `PropertiesFile` 只读 `<VALUE>` 子元素，写在根元素属性上会被静默忽略）：

```xml
<PROPERTIES>
  <VALUE name="standalone.restoreLastSong" val="1"/>
  <VALUE name="standalone.lastSong" val="D:\Music\带 空格 的路径.vgm"/>
</PROPERTIES>
```

## 4. 实现

### 4.1 存储层：`SettingsStore`

| 成员 | 行为 |
|---|---|
| `getRestoreLastSong()` / `setRestoreLastSong()` | 缺省 **true**；写入随改随保存；`broadcastChange()` 通知所有窗口（和主题、复用窗口一样是偏好） |
| `getLastSong()` | 没有记录时返回默认构造的空 `juce::File`；调用方用 `getFullPathName().isEmpty()` 或 `existsAsFile()` 判断即可 |
| `setLastSong(file)` | 存绝对路径；路径为空或与已存值相同则**不写**；失败记进 `lastError()` |
| `clearLastSong()` | 没有记录时不写盘；有则删除键并立即保存 |
| `resetToDefaults()` | `file->clear()` 之后再写默认值，所以"上次歌曲"一并消失，开关回到 true |

`setLastSong()` / `clearLastSong()` **故意不广播**：这是会话状态，不是偏好。产生它的那次加载
本来就会刷新窗口，再广播一次只会在刷新之后又重绘一遍。

只写路径，不写别的：播放位置、循环、静音、混音一概不进 AppData。

### 4.2 记录：成功的那一刻

`ToyboxAudioProcessorEditor::loadFile(file, startPlaying)` 的异步回调里，`ok` 为真时：

```cpp
if (safe->processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
{
    toybox::settings::SettingsStore::getInstance().setLastSong (file);
}
```

- **在回调里写，不在退出时写**：崩溃、任务管理器结束进程、关机时被系统杀掉，都不会丢掉记录。
- **只在 Standalone 写**：插件里打开工程或换歌不会覆盖 Standalone 下次要打开的文件。这里用的是
  `wrapperType` 而不是 `toybox::standalone::available()`：后者回答的是"这个二进制里有没有一个
  应用填过 hooks 表"，前者回答的是"这是哪个包装"，而"有没有上一次会话"问的是后者。两者在正式
  的 Standalone 里同时为真，在测试 harness 里只有 `wrapperType` 被设置。
- 失败不写：记录保持上一个**能打开**的歌曲。所以"手动打开一个损坏文件 → 失败 → 退出 → 再启动"，
  恢复的仍是之前那首能用的。

`closeSong()`（`File > Close Song` 唯一入口，`MainMenu` 的 `file.close`）在 Standalone 里调用
`clearLastSong()`。加载中途关闭也安全：`unloadFile()` 会让在飞的结果被丢弃，不会在关闭之后
再写回一个"上次歌曲"。

### 4.3 恢复：首帧，只尝试一次，排在显式请求之后

恢复不在构造函数里，而在 `timerCallback()` 中，且排在交接与启动参数之后：

```cpp
if (const auto handedOver = processor.takeSongFromAnotherInstance(); handedOver.existsAsFile())
{
    loadFile (handedOver);
}

if (! startupHandled)
{
    openStartupSong();          // 只启动 worker，结果稍后由回调送达
}

restoreLastSongIfNeeded();      // 显式请求都没有发生，才轮到它
```

`restoreLastSongIfNeeded()` 的判据，按顺序：

1. `restoreAttempted` —— 整次启动只尝试一次，之后每帧直接返回；
2. `wrapperType == wrapperType_Standalone` —— 插件不做；
3. `startupSong.existsAsFile() || loadInFlight || engine.isLoaded()` —— 已经有人要了这首歌：
   命令行、第二实例交接、抢在首帧之前的拖拽。**显式请求优先，恢复只是兜底**；
   （`engine.isLoaded()` 是防御项：正常路径下首帧之前不可能已经装好一首歌，但万一以后有同步的
   加载入口，恢复也不该把它顶掉。）
4. 开关打开、`getLastSong()` 非空且文件存在；
5. `loadFile (remembered, false)` —— 走与拖拽、打开完全相同的加载路径，只是不自动播放。

**为什么不在构造函数里**：macOS 上 Finder 的文档事件（双击一首歌）在窗口创建**之后**才到达
`handOver()`。如果在构造函数里恢复，窗口会先加载（并安装走带）上次的歌，下一帧再被文档替换：
用户看到一帧错误的界面，若这首"上次的歌"已经装好走带，还会响一下。放在交接之后，优先级自然
成立，也没有双载。

`startupSong.existsAsFile()` 这一条不是多余的：`openStartupSong()` 在没有启动参数时会把
`startupHandled` 置真，但 `startupSong` 保持为空；而在有参数时，异步加载期间 `loadInFlight`
已经是真。两个条件合起来覆盖了"参数存在但还没来得及进回调"的窗口。

### 4.4 不自动播放：`loadFileAsync` 多一个参数

`PlaybackEngine::loadFileAsync (file, onDone, bool startPlaying = true)`，参数一路传到
`finishLoad()`，最后交给本来就有这个开关的 `installLoaded(..., startPlaying)`——所以引擎层
没有新行为，只是把"装好之后播不播"的选择露了出来。默认值保证所有既有调用（拖拽、打开、交接、
命令行）语义不变；只有恢复传 `false`。

### 4.5 设置页：一行开关

General 页新增（仅 `toybox::standalone::available()` 时创建）：

| componentID | 内容 |
|---|---|
| `settings.general.restoreLastSong` | `ToggleButton`，"Reopen the last song at startup"，点击即写 `SettingsStore` 并 `refresh()` |
| `settings.general.restoreLastSongHint` | 一行 12 pt 说明："Launching the application loads the song it last had open, paused at the start. File > Close Song forgets it." |

页标题下的副标题也改了：`How the application starts, and where these settings live.` —— 这一页
现在同时回答"启动行为"和"设置存在哪"。

插件的面板不会创建这一行（`available()` 为 false），所以插件用户不会看到一个点了没有意义的
开关；这比 `reuseWindow` 那行"Windows 插件里也显示"的旧行为更准确。

### 4.6 平台差异

三个平台的 Standalone 走的是同一条代码路径，没有 `#if`。差别只在"显式传入"的来源：

| | 显式传入的来源 | 与恢复的关系 |
|---|---|---|
| Windows | 命令行、第二实例经 `broadcastMessage` 交接命令 | 首帧先取交接，再恢复 |
| macOS | Finder 文档事件（同样走 `anotherInstanceStarted()`，晚于窗口创建） | 同上，这正是恢复必须放首帧的原因 |
| Linux | 命令行（`broadcastMessage` 在 Linux 是空实现，没有第二实例交接） | 首帧恢复 |

## 5. 诊断

- 设置文件里直接可见：`standalone.restoreLastSong` 是 `0/1`，`standalone.lastSong` 是绝对路径；
  路径里的空格、中文由 XML 转义处理，不需要额外编码。
- 手写或旧版本留下的文件：缺键读默认值（开关开、没有歌曲），`0` 读作关——`vgm_core_test` 的
  `[9]` 有一条照着这个格式写的检查。
- 恢复失败不弹窗：状态行照常显示加载失败的原因（`loadFile()` 的既有路径），设置页的错误行只
  反映**写盘**失败。

## 6. 验证

单元层（`plugin/Tests/PlaybackCoreTest.cpp`，`[9] the settings store`）：

- 开关默认开、可关、写盘无错；
- `lastSong` 的写入、读回（含带空格的路径）、`clearLastSong()` 之后键从文件消失；
- 文件不存在时路径**仍被保留**；
- 手写文件里的三个键一起被读出；
- `resetToDefaults()` 把开关恢复为开、并清掉记住的歌曲。

窗口层（`plugin/Tests/AppHarnessTest.cpp` + 脚本）需要"上一次会话留下的状态"，而脚本的第一条
命令运行时窗口早已构造完毕、首帧也已经跑过。所以 harness 增加了**构造之前的指令**：

```
# settings lastSong <path>          # 上一次会话留在 settings.xml 里的歌曲
# settings restoreLastSong on|off   # General 页的那个开关
```

它们在 `main()` 里、处理器与编辑器构造之前读取，`$VGM_TEST_SONG` 与脚本命令一样会先展开；
变量未设置时整个脚本报 `skipped` 并返回 0（与 `load` 命令的行为一致，机器上没有测试歌曲时
不会假失败）。

同时每个脚本都跑在自己的临时设置目录里（`%TEMP%/vgmtoybox-app-test-*`，通过
`VGMTOYBOX_SETTINGS_DIR` 指过去，进程结束时整个目录删除）。脚本会点开关、会写"上次歌曲"，
不能让它们改到开发机的真实设置。

两条脚本，缺一不可：

| 脚本 | 断言 | 证明什么 |
|---|---|---|
| `last-song.txt` | 等待首帧后状态行含 `Paused`、`0:06`（`minimal.vgm` 的长度），`engine position ~= 0` | 记住了的歌曲被加载，且**没有播放** |
| `last-song-off.txt` | 同样的"上次歌曲"，开关关闭 → 状态行含 `No file` | 开关真的起作用；只测前一条的话，"忽略开关"的 bug 也能通过 |

`settings-pages.txt` 另有：`settings.general.restoreLastSong` 存在（`enabled`）、默认 `checked`、
说明文字含 `Close Song`。

运行结果：`build.ps1 -Test` 之后 `ctest` **10/10 通过**（`core`、`ui`、`engine`、`app.daw-sync`、
`app.daw-sync-follow`、`app.last-song`、`app.last-song-off`、`app.load-and-play`、
`app.settings-pages`、`app.window-basics`）。

## 7. 已知限制

- **不恢复播放位置**：恢复的语义是"这首曲子还在"，不是"从 1:23 接着听"。播放位置也没有存进
  `settings.xml`（那会让每次退出都写一次高频变化的值，且与"用户偏好"的定位不符）。
- **只对 Standalone**：插件里既没有这一行，也不会写 `standalone.lastSong`；插件工程的歌曲由
  宿主工程负责（状态边界见 `docs/review/2026-10-17-structure-survey-and-design-questions.md`
  §6 H-1）。
- **记录不会被自动清理**：文件被删掉后，路径会一直留到下一次成功加载或 `Close Song`。这是有意
  的——外接盘、网络盘暂时不可用时，用户看不到任何提示，但下次插上还能恢复。
- **不追踪"最近多个文件"**：只保留一个路径；要做最近列表是另一套 UI 和另一份存储，不在本次范围。
