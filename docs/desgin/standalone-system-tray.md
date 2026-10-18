# Standalone：关闭到系统托盘

- **状态**：已实现（`plugin/Source/Standalone/TrayController.*`、`TrayMenu.*`、`TrayIcon.*`、
  `TrayIcon_windows.cpp`、`TrayIcon_mac.mm`、`TrayIcon_linux.cpp`、
  `plugin/Source/Standalone/StandaloneApp.cpp`、`plugin/Source/settings/SettingsStore.*`、
  `plugin/Source/ui/SettingsOverlay.*`、`plugin/CMakeLists.txt` 的图标资源）
- **日期**：2026-10-18
- **分支**：`feature/standalone-tray`

## 1. 问题

独立程序按窗口的 × 就是退出，正在听的曲子、走带位置、混音状态一起消失。后台听一张专辑时，
用户要的是"把窗口收起来"，而不是"结束进程"：托盘图标负责把窗口叫回来，右键菜单负责在窗口
不出现时完成最常用的走带操作，Quit 才是真正的退出。

它必须是一个**可关的开关**：一个关掉窗口后还在运行的进程，不能是用户没有要求过的行为。
默认关闭；开关放在 General 页，和其他"启动/运行方式"的偏好放在一起，并持久化到
`settings.xml`。

## 2. 先定下来的七件事

| # | 问题 | 决定 | 理由 |
|---|---|---|---|
| 1 | 默认值 | **关** | 关闭窗口一直等于退出；让进程活下来必须由用户明确要求 |
| 2 | 什么时候读设置 | **每次关闭时**读，不在构造控制器时读 | 在设置页打开开关后，下一次关闭立即生效，不需要重启 |
| 3 | 图标什么时候出现 | **第一次被吞掉的关闭**时创建；之后一直留到退出 | 设置关闭时绝不创建；窗口重新显示后图标仍在，应用"还在运行"才是可见的 |
| 4 | 左键 | 显示窗口并置前 | 与任务栏点击的期待一致 |
| 5 | 右键 | 上下文菜单：Show Window、Play/Pause、Stop、Loop、Fade、Quit | 菜单就是走带控制的一个子集，不新造概念 |
| 6 | 托盘 Quit | 走 `systemRequestedQuit()` | 和系统退出同一条路径：保存插件状态、关掉模态面板、再退出 |
| 7 | 图标创建失败（可检测的） | **不隐藏窗口**，关闭继续按原来的方式退出 | 没有图标还能把窗口藏起来，是唯一无法恢复的失败。Linux 没有通知区域时 JUCE 不报错，属于不可检测的失败，见 §6 |

**范围假设**（本次不做）：不改变最小化行为（最小化仍然只是最小化），不做"启动时最小化到托盘"，
不新增全局快捷键，不在托盘菜单里放音量/播放起始点这类需要滑杆或模式解释的控件，也不把走带状态
写进设置文件。托盘只是窗口隐藏期间的一个入口，不是第二套完整的界面。

## 3. 存储：`SettingsStore`

| 成员 | 行为 |
|---|---|
| `getCloseToTray()` | 缺键 **false**；`standalone.closeToTray` |
| `setCloseToTray(bool)` | 值不变不写盘；变化则立即保存并 `broadcastChange()` |
| `resetToDefaults()` | 写回 **false** |

```xml
<PROPERTIES>
  <VALUE name="standalone.closeToTray" val="1"/>
</PROPERTIES>
```

和 `standalone.reuseWindow`、`standalone.restoreLastSong` 同一个文件、同一套失败上报
（写盘失败 → `lastError()` → General 页的错误行），因此 `VGMTOYBOX_SETTINGS_DIR` 仍然能把
测试的整份设置指到临时目录。缺键读 false，不需要版本迁移。

**不在设置变化时立刻创建/销毁图标**：图标是"关闭被吞掉"的产物，不是设置项的预览。设置关掉后
如果图标已经存在，它会留到进程退出（下一次关闭会真正退出）；这样不会出现"点一下开关，托盘图标
闪一下又消失"的噪音，也避免控制器去监听每一次设置广播。

## 4. 实现

### 4.1 三层，GUI 只占最外面一层

| 层 | 文件 | 职责 |
|---|---|---|
| 菜单模型 | `Standalone/TrayMenu.*` | 把引擎状态变成 `TrayMenuModel`（纯数据），`invoke(id)` 执行动作；不依赖 GUI |
| 关闭策略 | `Standalone/TrayController.*` | 读设置、决定关闭是否被吞、创建/销毁图标、把左键接到显示窗口；通过 `TrayIconPresenter` 接口隔离真实桌面 |
| 桌面适配 | `Standalone/TrayIcon.*` | 唯一包含 `juce_gui_extra` 的一层：`juce::SystemTrayIconComponent`；Windows 调 Win32 菜单，macOS 调 NSMenu，Linux 用 JUCE 菜单 |

`TrayMenu` / `TrayController` 因此被编进 `vgm_core_test`，可以在没有桌面的机器上测试关闭策略和
菜单内容（`plugin/Tests/PlaybackCoreTest.cpp` 的 `[9b]`）。`TrayIcon.cpp`、平台菜单文件和被选中的
平台适配文件只编进 Standalone 目标（`plugin/CMakeLists.txt`）。

### 4.2 关闭：`ToyboxFilterWindow::closeButtonPressed()`

`StandaloneApp.cpp` 里的窗口子类在调用 JUCE 基类之前先问控制器：

```cpp
void closeButtonPressed (void) override
{
    if (closeRequested != nullptr && closeRequested())
    {
        return;                         // 吞掉：图标已就位，窗口已隐藏
    }

    juce::StandaloneFilterWindow::closeButtonPressed();   // 原来的保存并退出
}
```

控制器返回 true 的条件只有两个，缺一不可：

1. `settings.getCloseToTray()` 为真；
2. 图标 `show()` 成功。

第 2 条是安全阀，但它只挡得住**能检测到**的失败：工厂创建不出 presenter、或适配器明确
`show()` 返回 false 时，`closeRequested()` 返回 false，窗口照常关闭并退出。

Linux 是这条规则的例外：JUCE 的 X11 托盘实现只是向 `_NET_SYSTEM_TRAY_Sn` 发 dock 请求，没有
通知区域时静默什么也不做，`show()` 仍然返回 true。控制器无法区分"图标已上屏"和"请求被忽略"，
所以 Linux 上没有通知区域的桌面仍可能在关闭后找不到窗口 —— 这正是 General 页在 Linux 上多一行
红色警告的原因（§4.5），也是 §6 如实记录的限制。**不要把这条安全阀理解成三平台都能保证有图标。**

图标创建一次后由控制器持有；`activate()` 只做 `setVisible(true)` + `toFront(true)`，不销毁图标。
所以窗口叫回来之后，用户再次点 × 仍然会收回托盘。

### 4.3 菜单：每次右键重建

`TrayMenu::build()` 每次都从引擎重新读状态，不缓存：

| 分组 | 条目 | 启用条件 / 勾选 |
|---|---|---|
| 顶层 | **Show Window** | 始终可用 |
| 顶层 | **Play / Pause**（同一行，标签随 `isPlaying()` 变） | 已加载歌曲 |
| 顶层 | **Stop** | 已加载歌曲 |
| Loop 子菜单 | `1x` / `2x` / `3x` / `Loop forever` | 勾选当前 `getLoopCount()`；0 表示无限 |
| Fade 子菜单 | `No fade` / `1 s` / `3 s` / `5 s` / `10 s` | 勾选当前 `getFadeSeconds()`（±0.001 s） |
| 顶层 | **Quit** | 始终可用；先取图标，再 `systemRequestedQuit()` |

没有歌曲时 Play、Stop 都灰掉，`invoke()` 对灰掉的条目返回 false —— 菜单点击和测试调用走的是
同一套判断。5 秒跳转不放进托盘菜单：在那里操作没有实际意义，走带面板仍然是它的入口。

Loop 只给次数，不给循环点：循环点由歌曲自己决定，程序里没有编辑入口。Fade 的五档和走带面板
一致（`0/1/3/5/10` 秒），不是另一套值。

`TrayIcon::showMenu()` 把模型翻译成平台菜单。Windows 上是真正的 Win32 菜单：

```cpp
CreatePopupMenu / AppendMenuW
TrackPopupMenuEx (menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ...)
```

`TPM_RETURNCMD` 让选择结果直接返回，不会走 `WM_COMMAND`；先 `DestroyMenu()` 再执行对应的
`std::function`，所以 Quit 执行时菜单已经消失，销毁图标不会把菜单从点击下面抽走。系统负责字体、
阴影、暗色模式和 DPI 缩放，代码里没有任何自绘。

macOS 走同一条"选择后再执行"的顺序，只是菜单换成 `NSStatusItem` 的 `NSMenu`：`NSMenuItem` 的
target/action 只记录选中的 tag，`popUpStatusItemMenu:` 返回后才执行 `TrayMenuModel` 里的动作，
所以 Quit 也不会在菜单还开着时销毁图标。Linux 没有可直接挂接的系统菜单 API，仍走
`juce::PopupMenu`（见 §6）。

### 4.4 左键、右键与平台

| | 左键 | 右键 | 菜单 | 图标 |
|---|---|---|---|---|
| Windows | `mouseDown` 且非 popup → 显示窗口 | `mouseDown` 且 `isPopupMenu()` → 菜单 | **Win32 原生菜单** | 应用图标的 16 px 或 256 px 版本，按 `SM_CXSMICON` 选择 |
| macOS | 同上 | 同上 | **NSMenu 原生菜单** | 从同一张应用图标提取的白色模板；菜单栏按系统颜色渲染 |
| Linux | 同上 | 同上 | JUCE 菜单（见 §6） | 应用图标；工具提示在 JUCE 的 Linux 后端里未实现 |

图标的源文件就是 `ICON_BIG` / `ICON_SMALL` 使用的那两张 PNG：`plugin/CMakeLists.txt` 用
`juce_add_binary_data()` 把它们编进 Standalone，`TrayIcon::makeAppIcon()` 按目标像素尺寸选择
16 px 或 256 px。托盘和窗口不再是两个图案。

Linux 的图标走 X11 的 `_NET_SYSTEM_TRAY_Sn` 协议；没有通知区域（部分 Wayland 会话、精简桌面）
时图标不会出现，而 JUCE 不会把这件事报告给调用方（§4.2 的安全阀因此管不到它），所以 General
页在 Linux 上多一行红色警告。

### 4.5 设置页

General 页新增（仅 `toybox::standalone::available()` 时创建；插件没有自己的窗口可关）：

| componentID | 内容 |
|---|---|
| `settings.general.closeToTray` | `ToggleButton`，"Keep running in the system tray"；点击即写 `SettingsStore` 并 `refresh()` |
| `settings.general.closeToTrayHint` | 12 pt 说明："The close button hides the window instead of quitting. Use the tray icon to reopen it, or Quit in its menu to exit." |
| `settings.general.closeToTrayWarning` | **仅 Linux**：红色加粗，说明没有通知区域时不会出现图标，关闭的窗口只能靠结束进程恢复 |

设置面板的默认高度从 430 调到 500：General 页多了两行（开关 + 说明），窗口缩到最小时也要容得下。
面板本身仍然随窗口收缩（`panelBounds()` 的 `jmin`），不是固定 500。

### 4.6 与单实例/文件交接的关系

双击一首歌交给已运行的实例时（`handOver()`），窗口会被 `setVisible(true)` + `toFront(true)`：
隐藏到托盘的窗口也需要被这个动作拉回来。这条路径在托盘功能之前就存在，本次只确认它覆盖了
"窗口隐藏"这一新状态，没有新增分支。

托盘 Quit 与系统退出共用 `systemRequestedQuit()`：保存插件状态 → `cancelAllModalComponents()`
→（100 ms 后）`quit()`。直接调用 `quit()` 会跳过保存和模态清理，所以没有这么做。

### 4.7 暂停灯只有一个状态源

以前 `TransportPanel` 自己保存 `paused`，托盘直接调 `engine.pause()`，面板下次刷新时只看到
"引擎不在播放"，于是把 Stop 点亮。现在 `PlaybackEngine` 持有 `paused`：

- `play()`、`stop()`、`load`、播放到结尾都清掉它；
- `pause()` 置上它；
- 面板的 Pause/Stop 灯在 `refresh()` 里读 `engine.isPaused()`，面板不再保存副本。

这样从托盘暂停、从按钮暂停、从测试里直接调 `engine.pause()`，走的是同一个状态。`[9b]` 和
`load-and-play.txt` 都加了对应断言，后者专门从按钮之外暂停再检查灯。

## 5. 验证

### 5.1 核心测试（`vgm_core_test`，无需桌面）

`[9] the settings store` 新增：

- `getCloseToTray()` 默认 false；
- `setCloseToTray(true)` 后读回 true，且 `settings.xml` 里出现 `closeToTray`；
- `resetToDefaults()` 后回到 false。

`[9b] the tray and what a close does` 用 `FakeTrayIcon`（`TrayIconPresenter` 的测试实现，记录
`show/hide` 并保存回调）驱动：

| 断言 | 证明 |
|---|---|
| 开关开 → `closeRequested()` true，`show()` 被调用，窗口回调执行一次 | 关闭被吞、窗口被藏、图标就位 |
| 第二次关闭仍返回 true，图标仍然可见 | 关闭可以重复，图标不会因再次关闭而消失 |
| 调 `activate` 回调 → 显示窗口回调执行，图标仍可见 | 左键能把窗口叫回来，图标不消失 |
| 菜单第一行是 Show Window；Play 的标签随播放状态变 Pause | 菜单按当前状态重建 |
| Loop 2x / Fade 3 s 的勾选跟随引擎 | 勾选不是另一份副本 |
| `invoke("tray.playPause")` 让引擎播放；`tray.loop.1` / `tray.fade.0` 到达引擎 | 菜单动作不是空壳 |
| 菜单中没有 `tray.back` / `tray.forward` | 5 秒跳转不会被重新加回托盘 |
| 空引擎：Play 灰掉，`invoke` 返回 false | 没有歌曲时不会假装能播 |
| `invoke("tray.quit")` 执行退出回调，图标被取走 | Quit 真的走退出路径 |
| `engine.pause()` 后 `isPaused()` true，菜单 Play 后 false，Stop 后 false | 暂停状态归引擎所有，不归某个窗口 |
| `show()` 返回 false → `closeRequested()` false，窗口不隐藏，不声称图标可见 | 能检测到的失败不会藏窗口（Linux 的不可检测失败见 §6） |

### 5.2 窗口/设置测试（`vgm_app_test`，`settings-pages.txt`）

- `settings.general.closeToTray` 存在且启用；
- 默认 `checked == false`；
- 说明文字含 "tray"；
- `toggle` 之后 `checked == true`；
- `load-and-play.txt` 先让窗口暂停，再通过测试命令 `engine pause` 从按钮之外暂停，断言
  `btn.pause` 亮、`btn.stop` 不亮。

### 5.3 真实桌面冒烟（Windows，手动）

用 Win32 消息驱动真实的 `VGMToybox.exe`（设置目录指向临时目录，文件里 `standalone.closeToTray=1`）：

1. `WM_CLOSE` 发给主窗口 → 进程仍在，主窗口 `IsWindowVisible == false`，进程多出一个无标题的
   托盘窗口；
2. 向托盘窗口投递 `WM_TRAYNOTIFY (WM_USER+100)`，`lParam = WM_LBUTTONDOWN` → 主窗口重新可见；
3. 再次 `WM_CLOSE` → 又隐藏；
4. 右键消息 → 出现窗口类为 `#32768` 的可见窗口，`MN_GETHMENU` 取到 12 个顶层项，第一项文字
   为 "Show Window"；按 Esc 关闭后进程仍在。

第 4 步的 `#32768` 是 Win32 菜单窗口的固定类名；旧的 JUCE 菜单不会产生这个窗口。这条路径不在
ctest 里（需要真实桌面），但它验证了 `TrackPopupMenuEx` 的菜单确实上屏、模型顺序与顶层分隔符
正确。

macOS 的 NSMenu 代码无法在 Windows 开发机上运行，需要在 Mac 上确认：右键图标出现系统菜单、
Loop/Fade 子菜单正常展开、勾选状态正确、选 Quit 后进程退出。Linux 的托盘外观和 JUCE 菜单仍需
在对应桌面上人工确认。

## 6. 已知限制

- **Linux 没有通知区域就没有图标，而且检测不到**：JUCE 走 X11 的 `_NET_SYSTEM_TRAY_Sn`，
  Wayland 会话或没有托盘宿主的桌面不会显示，`show()` 却仍然成功。此时关闭窗口会把它藏起来，
  只能结束进程恢复；设置页的红色警告说的就是这件事。要彻底消除它，需要在 Linux 上直接查询
  X11 selection owner（并处理 XWayland/Wayland 的差异），不在本次范围内。
- **Linux 工具提示不显示**：JUCE 的 Linux 实现里 `setIconTooltip()` 是空函数。
- **Linux 的右键菜单仍是 JUCE 菜单**：Linux 没有统一的系统菜单 API；真正的桌面原生菜单要走
  StatusNotifierItem + DBusMenu，并把 JUCE 的 X11 托盘后端换掉，本轮不做。GTK 菜单只是另一套
  toolkit 菜单，在 KDE 上也不会变成系统样式，因此没有采用。
- **macOS 原生菜单尚未在 Mac 上实机验证**：代码已经按 `NSStatusItem` + `NSMenu` 实现，Windows
  构建只验证了共享部分；macOS 的菜单弹出、子菜单和 Quit 仍需在 Mac 上跑一次。
- **macOS 使用模板图标**：模板从应用图标里提取白色芯片和引脚，蓝色底块不进入菜单栏；菜单栏
  按系统主题着色，这是平台规范，不是缺失。
- **图标一旦出现就留到退出**：即使设置随后被关掉，图标也不会立即消失；下一次关闭会退出。
- **不拦截系统退出**：托盘只接管窗口的关闭按钮/`WM_CLOSE`；macOS 的 Cmd+Q、任务管理器的
  结束进程仍然是真正的退出。
- **不保存"隐藏"状态**：重启后照常显示窗口；没有"启动时最小化到托盘"。
- **托盘不提供音量、播放起始点、进度条**：这些控件需要滑杆或模式说明，放进右键菜单会让它变成
  第二套走带面板；窗口叫回来后它们仍在原位。
