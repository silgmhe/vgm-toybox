# standalone：系统原生窗口，Options 并入 Settings

> 状态：已实现（Windows 与 Linux 实测；macOS 仅由 CI 编译覆盖）
> 日期：2026-10-18
> 范围：`plugin/Source/Standalone/`、`plugin/Source/ui/SettingsOverlay.*`
> 相关：`standalone-single-instance.md`（同一个 standalone 改造的另一半）、`settings-and-theme-design.md`

## 0. 一句话

standalone 的窗口从 JUCE 自绘的无边框壳换成系统窗口，两行 JUCE 调用，没有一行平台分支；
原本画在 JUCE 标题栏上的 Options 按钮随之消失，它唯一有用的 Audio/MIDI 设置移进 Settings
覆盖层新增的 Audio 页，这一页在不在，由一张运行时能力表回答。

## 1. 原来的窗口为什么拿不到系统窗口管理

`juce::StandaloneFilterWindow` 是 `DocumentWindow`，而 `DocumentWindow` 默认自己画标题栏
（`useNativeTitleBar == false`）：标题栏、按钮、边框都是 JUCE 的组件，窗口没有非客户区。JUCE 那个
壳本身并不差（它连双击最大化都实现了），但窗口管理是系统的，系统看不见标题栏就管不了窗口：

| 拿不到的东西 | 原因 |
| --- | --- |
| 拖标题栏吸附到半屏 / Win11 悬停最大化按钮的排版面板 / `Win`+方向键 | 拖动是 `ResizableWindow::mouseDown` → `ComponentDragger`（`juce_ResizableWindow.cpp:618`）在应用内部挪自己，系统从没进入过它自己的拖动与吸附循环；也没有 caption 让系统接管拖动 |
| 最大化按钮 | `StandaloneFilterWindow` 只要了 minimise 和 close（`juce_StandaloneFilterWindow.h:665`），于是 `DocumentWindow::getDesktopWindowStyleFlags`（`:358`）不会置 `windowHasMaximiseButton`，Windows 上就没有 `WS_MAXIMIZEBOX` |
| Alt+Space 系统菜单、caption 动画 | `windowHasTitleBar` 只在原生标题栏时置位（`juce_TopLevelWindow.cpp:110`）；没有它，Windows 端走 `WS_POPUP` 分支。`WS_CAPTION` 位仍可能因为阴影被置上（`juce_Windowing_windows.cpp:2238`），但那只是一圈细边框，没有 caption 区域可点 |
| 双击标题栏、高 DPI 下非客户区随系统缩放、系统对"普通窗口"的其它处理 | 都挂在真正的非客户区上，而我们的窗口没有：双击只会去按一个不存在的最大化按钮（`juce_DocumentWindow.cpp:426`） |

这些不是 JUCE 的 bug，是"自己画标题栏"的必然代价。注意"没有非客户区"不等于"没有边框"：JUCE 给
窗口加了 `WS_THICKFRAME` 让它能缩放，也有阴影，看起来很像系统窗口——但系统仍然只把它当一个 popup。

## 2. 实现

| 文件 | 改动 |
| --- | --- |
| `plugin/Source/Standalone/StandaloneApp.cpp` | 窗口加两行：`setUsingNativeTitleBar (true)`、`setTitleBarButtonsRequired (allButtons, false)`；`ToyboxFilterWindow` 补上 Linux 缺的 `windowIsResizable`（2.5）；创建 holder 后先 `installHooks`，再构造窗口；`shutdown` 里逐字段清空能力表 |
| `plugin/Source/Standalone/StandaloneHooks.h`（新） | 运行时能力表 `toybox::standalone::hooks()` / `available()` |
| `plugin/Source/ui/SettingsOverlay.h` / `.cpp` | 新增 `AudioSettingsPage`；侧栏第三行按 `available()` 决定加不加 |
| `docs/desgin/native-window-and-audio-settings.md` | 本文 |

没有改 `plugin/CMakeLists.txt`，也没有新增依赖。

### 2.1 两行开关，顺序有讲究

```cpp
auto* window = new juce::StandaloneFilterWindow (getApplicationName(), backgroundColour,
                                                 std::move (holder));

window->setUsingNativeTitleBar (true);
window->setTitleBarButtonsRequired (juce::DocumentWindow::allButtons, false);
```

两行都在窗口上桌面之前（`initialise()` 里的 `setVisible(true)` 才真正建 peer），所以窗口是用这组
标志建出来的，不是建好之后再换壳。这条链是：

1. `setUsingNativeTitleBar` 只改成员并 `recreateDesktopWindow()`（此时不在桌面上，是空操作），
   作用在 `getDesktopWindowStyleFlags()` 里：`windowHasTitleBar`（`juce_TopLevelWindow.cpp:110`）。
2. `setTitleBarButtonsRequired` 存下按钮位（`juce_DocumentWindow.cpp:354`）。
3. `addToDesktop(styleFlags)` 时，平台后端把这两个标志翻成真窗框：Windows 的
   `computeNativeStyleFlags` 在有 maximize 时给出 `WS_MAXIMIZEBOX | WS_CAPTION | WS_SYSMENU`
   （`juce_Windowing_windows.cpp:2231`），而 `WS_CAPTION` 正是系统窗框的来源。

顺带得到一个副作用：原生模式下 `DocumentWindow::getTitleBarHeight()` 返回 0
（`juce_DocumentWindow.cpp:277`），编辑器拿到的是整个客户区——标题栏和边框（这台机器上 56 px）由
系统扣掉，不在我们的坐标里。我们的布局只吃 `getLocalBounds()`，不用改。

### 2.2 Options 按钮去哪了

它没有单独删掉——它跟着标题栏一起消失了。`StandaloneFilterWindow` 的构造函数把这个
`TextButton` 加进窗口（`juce_StandaloneFilterWindow.h:664`），`resized()` 里给它
`setBounds (8, 6, 60, getTitleBarHeight() - 8)`（`:810`）。原生标题栏下高度是 `-8`，而
`Component::setBounds` 把负尺寸夹成 0（`juce_Component.cpp:846`），于是它变成 60×0 的组件：
不绘制、不可点，也不会盖在我们的菜单栏上（实测截图里没有残影）。

它的四个菜单项的去处：

| Options 里的项 | 处理 |
| --- | --- |
| Audio/MIDI Settings... | 移到 Settings > Audio（新的 Audio 页，见 2.4） |
| Save current state / Load a saved state / Delete saved state | 不搬。standalone 的 `getStateInformation` 对 `wrapperType_Standalone` 直接清空返回（`PluginProcessor.cpp:164`：它没有工程可挂，曲子从命令行来，`setStateInformation` 也从不读回），点了只会写一个空文件，比没有按钮更糟 |

### 2.3 为什么是运行时能力表，不是编译期宏

`juce_add_plugin` 把编辑器编进一个共享静态库（target `VGMToybox`），standalone、VST3 和测试目标
都链接它；`JUCE_STANDALONE_APPLICATION` 只定义在 wrapper 那个 TU（`StandaloneApp.cpp`）里，
编辑器看不到它——这一点在 `compile_commands.json` 里可以直接确认。所以：

- `plugin/Source/Standalone/StandaloneHooks.h` 定义一个只有两个 `std::function` 的表；
- `StandaloneApp::createWindow` 在**构造窗口之前**填表，因为窗口的构造函数会创建编辑器，而编辑器
  构造设置覆盖层时就要问"有没有 Audio 页"；
- 没有窗口的路径（没有显示器）也填表，保证 `available()` 的语义是"这个进程是 standalone"，而不是
  "有窗口"；
- `shutdown()` 在 holder 销毁之后立刻逐字段清空（lambda 按引用捕获 holder，不清就是悬空引用）。
  逐字段而不是 `= {}`，因为 clang 对 `std::function` 的空花括号同样报拷贝/移动二义，与
  `juce::File` 那次是同一个坑。

用 `std::function` 而不是"带默认实现的接口"：整个进程只有一个 application，holder 活全程；一个
静默什么都不做的 stub 会在插件里长出一个"有按钮但按不动"的页面——宁可整页不存在。

`available()` 的答案是 `hooks().showAudioSettings != nullptr`，所以插件（VST3/AAX）里侧栏只有
Theme、General 两行，连 `AudioSettingsPage` 对象都不会被创建。

### 2.4 Audio 页

`AudioSettingsPage`（`ui/SettingsOverlay.h`）是一个普通 `View`，和 Theme、General 一样由
`ViewStack` 管：

- **设备行**：`<设备名> - <采样率, 一位小数> kHz - <缓冲> samples - <输出数> out`，来自
  `holder.deviceManager.getCurrentAudioDevice()`；没有设备时显示 "No audio device is open."。
  允许横向压缩到 0.7，因为驱动给的名字可以很长（`Voicemeeter Input (VB-Audio Voicemeeter VAIO)`）。
- **按钮**：`Audio/MIDI settings...`，调用 `StandalonePluginHolder::showAudioSettingsDialog()`。
  那个对话框是 JUCE 自己的（设备类型、输出设备、采样率、缓冲、MIDI 输入/输出），本来就是
  `DialogWindow` 加 `useNativeTitleBar = true`（`:313`）再 `launchAsync()`（`:316`），由
  `showAudioSettingsDialog()`（`:284`）发起；非阻塞，所以它开着的时候 Audio 页的定时器照跑。
- **刷新**：500 ms 定时器，`isShowing()` 才干活，读到的新行与旧行相同就什么都不做。这样在对话框里
  换设备、插拔耳机，回到页面上数字已经更新——对话框关闭时不会通知任何人。
- **componentID**：`settings.page.audio`、`settings.audio.device`、`settings.audio.open`、
  `settings.audio.hint`，沿用其它页的命名，方便窗口脚本以后按 id 找。

侧栏的行和页是一起加的（`SettingsOverlay.cpp` 构造函数里同一个 `if (hasAudio)`），注释写的就是
"侧栏不能给一个从没创建过的页留一行"。

### 2.5 Linux 上 JUCE 少给的那一位

`ResizableWindow::getDesktopWindowStyleFlags()`（`juce_ResizableWindow.cpp:82`）只在
`Desktop::supportsBorderlessNonClientResize()` 为真时置 `windowIsResizable`，而那个函数在 Linux 上
直接返回 false（`juce_Desktop.cpp:377`，函数体是 `#if JUCE_WINDOWS || JUCE_MAC`）。它问的是"这个平台
能不能做无边框窗口的非客户区缩放"，不是"这个窗口可不可以缩放"，但对 X11 来说这一位正是
`MWM_FUNC_RESIZE` / `MWM_DECOR_RESIZEH` 的来源（`juce_XWindowSystem_linux.cpp:3025`），而
`updateConstraints()` 更是在没有这一位时把 `WM_NORMAL_HINTS` 的 min 和 max 都写成当前尺寸
（`:1877`）——等于明确告诉窗口管理器"这窗口不能改大小"。

窗口管理器就照做了。openbox 实测：MWM hints 是 `0x3c / 0x7a`（没有 resize 位），
`_NET_WM_ALLOWED_ACTIONS` 里没有 `MAXIMIZE_HORZ / VERT`，`wmctrl` 发出的最大化请求被接受后又原样
忽略，窗口纹丝不动。Windows 和 macOS 因为那个函数返回 true，从来没暴露过这个问题。

`StandaloneApp.cpp` 的 `ToyboxFilterWindow` 只做一件事：拿到基类的答案，窗口确实可缩放时补上
`windowIsResizable`。补上之后 openbox 给出 `0x3e / 0x7e`、`RESIZE` 与 `MAXIMIZE_HORZ / VERT`，
最大化与拖边缩放都正常（实测 1600×981 与 1100×760）。另两个平台的基类本来就有这一位，OR 一下不变；
编辑器不可缩放时也不会凭空多出来。

## 3. 跨平台

### 3.1 代码层：零平台分支

功能只用 JUCE 公共 API（`setUsingNativeTitleBar`、`setTitleBarButtonsRequired`），没有
`#if JUCE_WINDOWS / JUCE_MAC / JUCE_LINUX`，没有 Win32 调用，也没有 `HWND`。能力表是纯 C++。
仓库里原有的平台分支（单实例、媒体控制）与这次无关。

### 3.2 三个平台各自把 `allButtons` 落到哪里

| 平台 | 谁画窗框 | `windowHasMaximiseButton` 的去处 | 预期看到什么 |
| --- | --- | --- | --- |
| Windows | DWM / 系统 | `WS_MAXIMIZEBOX \| WS_CAPTION \| WS_SYSMENU`（`juce_Windowing_windows.cpp:2231`） | 已实测：□ 按钮、Alt+Space 系统菜单、拖标题栏吸附半屏、Win+方向、系统阴影 |
| macOS | AppKit | 不改 `NSWindowStyleMask`（`Resizable` 已经带来绿色按钮），但 `windowHasMaximiseButton && windowIsResizable` 会把 collection behavior 设成 `NSWindowCollectionBehaviorFullScreenPrimary`（`juce_NSViewComponentPeer_mac.mm:483`），绿色按钮因此是原生全屏 | 交通灯在左（`setTitleBarButtonsRequired` 的第二个参数 `false` 在 macOS 无意义）、绿色按钮进全屏、双击标题栏按系统偏好缩放 |
| Linux | 窗口管理器（实测 openbox） | `_MOTIF_WM_HINTS`：functions `0x3e`（含 `MWM_FUNC_RESIZE`）、decorations `0x7e`，窗口管理器据此给出 `RESIZE / MAXIMIZE_HORZ / MAXIMIZE_VERT`（`juce_XWindowSystem_linux.cpp:3021`、`:3025`） | **实测**：openbox 画 20 px 标题栏，`_NET_FRAME_EXTENTS = 1,1,20,5`，最大化到 1600×981、缩到 1100×760；吸附、平铺、系统菜单归 WM，长什么样取决于主题。前提是 2.5 那个 resize 位——没有它 openbox 会忽略最大化 |

一个值得记住的点：macOS 上 `allButtons` 不是"多余的"——不加它，绿色按钮拿到的是
`FullScreenAuxiliary`，进不了全屏。三平台的语义不同，但恰好都对"要不要最大化"这事有正面作用。

Linux 补充：这份 JUCE（8.0.9）的 `juce_gui_basics/native/` 里没有 Wayland 后端，只有 X11；
Wayland 会话下应用走 XWayland，装饰仍由合成器/XWM 提供。

### 3.3 验证边界（说实话的部分）

| | 状态 |
| --- | --- |
| Windows 11 x64 | **实测**：`GetWindowLong(GWL_STYLE)` 读回 `WS_CAPTION / WS_THICKFRAME / WS_MAXIMIZEBOX / WS_MINIMIZEBOX / WS_SYSMENU` 全为真，非客户区 56 px；截图确认 caption、三页侧栏、设备行、Audio/MIDI 对话框 |
| Linux（WSL2 Ubuntu 22.04，Xvfb + openbox） | **实测**：`_MOTIF_WM_HINTS` 为 `0x3e / 0x7e`、`_NET_FRAME_EXTENTS = 1,1,20,5`、`_NET_WM_ALLOWED_ACTIONS` 含 `RESIZE / MAXIMIZE_HORZ / MAXIMIZE_VERT`；`wmctrl` 最大化到 1600×981、再缩到 1100×760，两次都截图确认布局跟着走。完整输出见第 5 节 |
| macOS | **只编译**：CI（`.github/workflows/GHA.yml`）在 macOS 上构建插件并跑控制台测试与窗口脚本，但到不了这里——`vgm_app_test` 不建 peer，构造不出窗口，所以 CI 绿不等于窗口行为被验证 |

也就是说：代码是跨平台的，Windows 和 Linux 已经验证到窗口属性这一层，macOS 还只有编译。macOS 上第一个
要看的是绿色按钮是否进全屏、标题是否显示 "VGMToybox"；这一条没有自动化，属于"谁先跑到谁补一句"的范围。

## 4. 已知取舍与风险

1. **标题栏颜色跟系统，不跟 Theme。** JUCE 8.0.9 从不调用 `DwmSetWindowAttribute`（整个
   `modules/` 里 0 处），所以不会有 `DWMWA_USE_IMMERSIVE_DARK_MODE`：系统浅色时深色界面配浅色
   标题栏。**决定不改**——一旦按 Theme 强制，系统设置就对标题栏失效，等于又把窗口管理拿回来一半。
   如果以后要改，位置在拿到 peer 之后、`#if JUCE_WINDOWS` 里调一次 Dwm 即可，代价是上面那句话。
2. **Options 按钮靠"高度被夹成 0"消失。** 这是 JUCE 8.0.9 的实现细节（`resized()` 里
   `getTitleBarHeight() - 8` + `setBounds` 夹负值），不是一个契约。JUCE 升级后如果它改成别的
   布局，症状是菜单栏左上角冒出一块 "Options"，而自动化测试抓不到（没有 peer）——只能靠真实窗口的
   冒烟。写在这里是为了下次升级 JUCE 时有人想起看它一眼。
3. **窗口在最大化状态下关闭**：JUCE 只保存 `windowX/windowY`（`juce_StandaloneFilterWindow.h:752`），
   最大化时 `getX()/getY()` 是屏幕左上角，于是下次启动窗口出现在那个角、用正常尺寸。窗口尺寸本来
   就不持久化，所以只是观感问题，不是这次引入的 bug，但以前没有最大化按钮，这个状态是新的。
4. **设置覆盖层在两个 wrapper 里不一样**（standalone 三行，插件两行）。装配这一半现在有自动化覆盖：
   `plugin/Tests/scripts/settings-pages.txt` 用首行的 `# hooks audio` 让 harness 在构造编辑器之前给
   hooks 表填两个假实现，于是三页侧栏、Audio 页的存在与设备行都能被断言（见 testing-architecture.md
   "运行器补强 5"）。剩下仍然手工的是控制台里做不了的部分：真实设备枚举、app-modal 的对话框本身。
5. **设备行是轮询**（500 ms），不是监听：`AudioDeviceManager` 的 change listener 也能做到，但
   那要求页面注册进一个生命周期更长的管理器；2 Hz 只在面板可见时跑，代价可以忽略。
6. **Linux 的可缩放靠 2.5 那一位，而且只在一个窗口管理器上试过。** openbox 认 `_MOTIF_WM_HINTS`，
   Mutter/KWin 的实现细节不同（它们对 `_NET_WM_ALLOWED_ACTIONS` 的处理并不一致），实测只覆盖了
   openbox + Xvfb。另外这版 JUCE 没有 Wayland 后端，Wayland 会话实际走 XWayland，缩放行为仍取决于
   合成器。如果哪天 JUCE 自己修了 `supportsBorderlessNonClientResize()`，`ToyboxFilterWindow` 可以
   删掉——只是那一行 OR 在修好之后也无害。

## 5. 验证记录

```
.\build.ps1 -Clang -Test        # 构建 + clang 检查 + 9 个测试全部通过
dist\Standalone\VGMToybox.exe   # 真实窗口
```

Win32 读回（一次性的 PowerShell + P/Invoke，脚本用完即删）：

```
WS_CAPTION     : True      WS_MAXIMIZEBOX : True
WS_THICKFRAME  : True      WS_MINIMIZEBOX : True
WS_SYSMENU     : True      非客户区高度 : 56 px
```

界面：标题栏是系统的（`VGMToybox` + − □ ×）；`Options > Settings…`（或 Ctrl+,）打开三行侧栏；
Audio 页显示 `Voicemeeter Input (VB-Audio Voicemeeter VAIO) - 48.0 kHz - 480 samples - 2 out`；
按钮打开 JUCE 的 Audio/MIDI Settings，主题跟着我们的 LookAndFeel。

Linux（WSL2，Ubuntu 22.04，Xvfb 1600×1000 + openbox；源码与 CPM 缓存都放在 ext4 上，`/mnt/h` 只用来
取一次源码）：

```
cmake -S ~/toybox -B ~/toybox-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCPM_SOURCE_CACHE=$HOME/.cpm_cache \
      -DJUCE_REPOSITORY=https://github.com/juce-framework/JUCE.git
cmake --build ~/toybox-build --target VGMToybox_Standalone -j 24
```

```
== WM ==
_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x...        # openbox 在，是它画窗框
== window ==
_MOTIF_WM_HINTS = 0x3, 0x3e, 0x7e, 0x0, 0x0                # functions / decorations 都带 resize
_NET_WM_ALLOWED_ACTIONS = _NET_WM_ACTION_MOVE, _NET_WM_ACTION_RESIZE,
                          _NET_WM_ACTION_MINIMIZE, _NET_WM_ACTION_MAXIMIZE_HORZ,
                          _NET_WM_ACTION_MAXIMIZE_VERT, _NET_WM_ACTION_FULLSCREEN,
                          _NET_WM_ACTION_CLOSE
_NET_FRAME_EXTENTS = 1, 1, 20, 5                            # 上 20 px 标题栏，下 5 px 缩放边
```

`wmctrl -b add,maximized_vert,maximized_horz` 之后 `_NET_WM_STATE_MAXIMIZED_HORZ/VERT` 生效，窗口
1600×981；再 `remove` 并缩到 1100×760，两次截图里侧栏、传送带、编辑器都跟着重排。

同一个二进制在打补丁之前是另一副样子，留作对照——这也正是 2.5 那一节的来历：

```
_MOTIF_WM_HINTS = 0x3, 0x3c, 0x7a, 0x0, 0x0                # 没有 resize
_NET_WM_ALLOWED_ACTIONS 里没有 MAXIMIZE_HORZ / VERT
wmctrl 最大化：接受，然后什么也没发生（几何、_NET_WM_STATE 都不变）
手工用 xprop 把 resize 位补上：同一条最大化请求立刻生效
```

WSLg 本身不能当证据：Weston 那边没有窗口管理器，`_NET_SUPPORTING_WM_CHECK` 是空的，窗口长什么样
取决于 Windows 端的 RDP 客户端，与 X11 的装饰无关。所以 Linux 的验证走 Xvfb + openbox。

一个跑测试时的环境细节：这台机器的桌面上有置顶的视频窗口，截图和模拟点击都会被它挡住，所以冒烟脚本
临时 `SetWindowPos(HWND_TOPMOST)` 把程序提到最前，结束时撤销。与代码无关，记在这里是因为第一次
截图拍到的全是那个窗口。

## 6. 否决的备选方案

| 方案 | 否决理由 |
| --- | --- |
| 自己实现吸附、最大化、系统菜单（`WM_NCHITTEST` / `WM_GETMINMAXINFO`） | 等于重写窗口管理，且只能覆盖 Windows；Win11 的 snap layouts 无论如何都拿不到（它要求真的 caption 和真的 maximize 按钮） |
| 保留 JUCE 标题栏，只把按钮补全 | caption 仍然是应用画的，吸附、系统菜单、DWM 动画一个都回不来，只是多了一个能点的 □ |
| 自定义标题栏 + `HTCAPTION` 命中测试（混合方案） | 半吊子：caption 还是我们画，DWM 的 caption 动画、系统阴影、Alt+Space 都要自己来；跨平台还得写三份 |
| 用编译期宏只给 standalone 加 Audio 页 | 做不到，编辑器编在共享静态库里，wrapper 的宏进不来（2.3） |
| 让 Audio 页在所有 wrapper 里都在，插件里置灰 | 在 DAW 里给一个永远按不动的按钮，比没有更糟 |
| 按 Theme 强制深色标题栏 | 决定不做：系统设置应当继续管标题栏，否则等于又把窗口的一部分收回应用里（见第 4 节第 1 条） |
