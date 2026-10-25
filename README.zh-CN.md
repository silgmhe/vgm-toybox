<h1 align="center">VGMToybox</h1>

<p align="center">
  <img src="plugin/assets/icon/vgmtoybox-256.png" width="96" alt="VGMToybox 图标">
</p>

<p align="center"><a href="README.md">English</a> | 简体中文</p>

一个 chiptune 音乐播放器，既是 **VST3 / AU 插件**，也是独立程序，基于 [libvgm](https://github.com/ValleyBell/libvgm.git) 构建。可读 `.vgm`、`.vgz`、`.s98`、`.dro`、`.gym`（以及 `.m3u` 播放列表），会列出曲子用到的每一种芯片，并给每个芯片声道一条独立混音条。最多 64 路立体声输出总线，一首曲子可以一次录成分轨。

## 构建

第三方库以源码形式随仓库提供，放在 `libs/` 下，没有子模块需要拉取。JUCE 与 zlib 由 CPM 拉取，版本固定在 `plugin/CMakeLists.txt`；zlib 编译一次，用于 `.vgz` 解压。

### Windows

需要 Visual Studio 2019/2022/2026，安装 **使用 C++ 的桌面开发** 工作负载，另需 CMake 与 Ninja（CMake 自带的 Ninja 会被自动找到）。

**MinGW 编译不了。** JUCE 8 及以后的版本在 Windows 上只支持 MSVC，所以本项目同样不能用 MinGW 工具链编译 —— 报错出现在 JUCE 自己的源码里，还没走到我们的代码。

```powershell
.\build.ps1
```

脚本会配好 MSVC 环境、用 CMake + Ninja 构建，并把产物复制到 `dist\`：

| 产物 | 说明 |
|---|---|
| `dist\VST3\VGMToybox.vst3` | 插件 |
| `dist\Standalone\VGMToybox.exe` | 同一套引擎的独立程序 |
| `dist\AAX\VGMToybox.aaxplugin` | Pro Tools 格式 —— 未签名，只能在开发者版里加载 |

`-NoPlugin` 跳过 JUCE 插件，不再构建其它产物；`-Proxy <url>` / `-NoProxy` 控制 CPM 访问 GitHub 的方式。`-Clang` 不是构建而是检查：装了 clang-cl 时，用 MSVC 那次构建的同一条命令行把每个插件源码交给 clang 走一遍 `/Zs`（只做语法与类型检查，不产出 obj）。它针对的是「MSVC 收、clang 不收」的写法 —— Windows 绿、macOS 红就是它要拦的那种；没有 clang 时它打一行说明就跳过。

AAX 与总线数是构建期常量，第一次构建前写进缓存一次即可：

```powershell
cmake -S . -B build -DTOYBOX_BUILD_AAX=OFF -DTOYBOX_OUTPUT_BUSES=16
.\build.ps1
```

### macOS

需要 Xcode 命令行工具、CMake 与 Ninja：

```bash
xcode-select --install
brew install cmake ninja
```

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build --parallel
```

去掉 `CMAKE_OSX_ARCHITECTURES` 就是本机构建；通用包是为了把 bundle 交给别人用。最低版本是 **macOS 10.15**，arm64 那一半是 11.0 —— 由 `CMakeLists.txt` 里的 `CMAKE_OSX_DEPLOYMENT_TARGET` 指定，因为不写的话它会跟着构建机的 SDK 走。

产物落在 `build/plugin/VGMToybox_artefacts/Release/`：`VST3/VGMToybox.vst3`、`AU/VGMToybox.component` 与 `Standalone/VGMToybox.app`。

### Linux

JUCE 必须先有这些头文件才能编译，运行期还需要 ALSA：

```bash
sudo apt install ninja-build libasound2-dev libx11-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxcursor-dev libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev
```

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

产物落在 `build/plugin/VGMToybox_artefacts/Release/`：`VST3/VGMToybox.vst3` 与 `Standalone/VGMToybox`。

在你能接受的最老发行版上构建 —— glibc 向后兼容，链接它的版本就是下限。发布包正是因此在 `ubuntu:22.04` 容器里构建。

### 测试

控制台测试是 `EXCLUDE_FROM_ALL`，要按名字请求，运行器是 `ctest`：

```bash
cmake --build build --target vgm_core_test vgm_ui_test vgm_app_test vgm_mixer_test
ctest --test-dir build --output-on-failure
```

`plugin/CMakeLists.txt` 为每个程序和每个窗口脚本（`app.<脚本名>`，来自 `plugin/Tests/scripts/`）
各注册一项，带标签与超时，所以 `ctest -R app.` 只重跑窗口脚本。`vgm_mixer_test` 是做逐样本校验的
那个：它把一首真实的曲子渲染两遍，所以只有存在歌曲时才注册 —— 仓库自带
`plugin/Tests/assets/minimal.vgm`，`-DTOYBOX_TEST_SONG=<file>` 可换成更大的曲子。Windows 上
`.\build.ps1 -Test` 会先构建这四个程序，再跑同一份列表。

## 使用方法

### 独立程序

`VGMToybox` 会播放启动时交给它的文件，所以命令行、快捷方式、资源管理器，任何把文件递过去的方式都能加载并开始播放：

```powershell
.\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgm"
```

它的文件位于 `%APPDATA%\VGMToybox`（macOS 上是 `~/Library/Application Support/VGMToybox`，
Linux 上是 `~/.config/VGMToybox`）：`settings.xml` 存放与插件共用的偏好，
`standalone-state.xml` 存放只有独立程序才有的音频设备、窗口位置和最后一次插件状态。
旧版本留下的 `VGMToybox.settings` 会在首次启动时迁移为这个新文件名。

### 从资源管理器打开

用 Windows 自己的对话框把格式关联到这个程序一次即可。**程序从不写注册表** —— 没有安装器、没有 shell 扩展、没有辅助脚本 —— 关联完全由你自己建立、修改和撤销：

1. 右键一首曲子，选 **打开方式** → **选择其他应用**；
2. 选 `VGMToybox.exe`（第一次要靠 *更多应用* → *在这台电脑上查找其他应用* 找到它）；
3. 勾选 **始终使用此应用**；`.vgz`、`.s98`、`.dro`、`.gym` 想要的话照做一遍。

Windows 按用户保存这个选择，改在 *设置* → *应用* → *默认应用* 里 —— 删掉文件夹就什么都不剩。另有两点：关联记的是路径，移动或替换可执行文件后双击会失效（留一份、升级时换那个文件）；启动时用不上交给它的曲子，程序会往 `%TEMP%\vgmtoybox-startup.log` 写一行说明 —— 正常启动什么都不写。

默认情况下，双击的曲子会**交给已经打开的那个窗口**：它替换掉正在播的曲子，并把窗口提到前面，第二个进程直接退出、不画窗口。*设置* → *常规* → **复用已打开的窗口** 关掉之后，每次双击都会开自己的窗口 —— 想两首曲子对比时正是要这个。

- **macOS** 永远不会给一个正在运行的 app 开第二份：Finder 把文档交给已经打开的那个，所以那里没有可切换的东西，设置页会把这件事写出来而不是给一个假开关。`.app` 已经声明了五种格式，*打开方式* 里能看到它（未签名，第一次要从右键菜单选 *打开*）。
- **Linux** 没有接这条路径（JUCE 的跨进程投递在那里是空实现），所以设置项不显示，每次启动都是独立进程。
- 没能用上的投递会在 `%TEMP%\vgmtoybox-second-instance.log` 留一行，和上面那次启动日志同一个意图。

### 关闭到系统托盘

*设置* → *常规* → **关闭窗口后留在系统托盘** 改变窗口 × 的行为：窗口消失、曲子继续播放，通知区域里留下一个 VGMToybox 图标。左键点图标把窗口叫回来；右键菜单提供 Show Window、Play/Pause、Stop、Loop、Fade 和 Quit。右键菜单在 Windows 上是 Win32 系统菜单、macOS 上是 NSMenu；Linux 的托盘协议没有可挂接的原生菜单，因此仍用 JUCE 菜单。Quit 才是真正退出，保存状态的路径与原来点 × 相同。该开关默认关闭，升级不会改变现有行为；图标本身在打开开关后第一次点 × 时才出现。

Linux 上没有通知区域的桌面无法显示图标，而且 JUCE 的 X11 后端不会报告这种失败；设置页会用红色文字说明这一点，这个开关在 Linux 上属于尽力而为。

### 更改界面语言

*设置* → *语言* 在窗口和工具提示上切换内置的英文与简体中文，并列出放进
`%APPDATA%\VGMToybox\i18n` 的每个翻译文件（macOS 上是
`~/Library/Application Support/VGMToybox/i18n`，Linux 上是 `~/.config/VGMToybox/i18n`）。
**打开文件夹** 会创建并打开它；**重新扫描** 会读取窗口打开期间新增或修改的文件。
`id:` 与内置语言相同的文件会和内置语言并排出现，作为独立选项；它没有翻译的条目会先回退到
对应语言的内置包，再回退到英文 —— 所以一份翻译可以只从几行开始。

文件是 UTF-8，以两行元数据开头；键名与英文包相同（*设置* → *语言* 会显示还缺多少个）：

```text
language: 简体中文
id: zh-Hans

"menu.file.open" = "打开歌曲...";
"transport.play"  = "播放";
```

插件版与独立版共用同一个文件夹和同一个已选语言。

### 更改界面字体

*设置* → *外观* → **字体** 显示窗口当前使用的字体族。**选择...** 打开字体选择器，列出系统已安装的字体，
第一行保留 **系统 UI**；搜索框可过滤列表，上方的两行示例会跟随选中的字体。选择会保存；设置页会提示需要
重新打开窗口（独立版需重启）才能生效，因为 JUCE 已经用旧字体排好了当前的标签。保存的字体族被卸载后，
窗口会回退到系统 UI 字体，并在该行显示缺失的字体名。

### 弯音

芯片发出的细微音高——颤音、滑音、各通道之间的失谐——都会变成 MIDI 弯音，而一个弯音值只有在接收乐器的
弯音范围下才有意义。MIDI 没有查询这个范围的方式，所以在 *设置* → *MIDI* 里指定：每侧 1 到 24 个半音，
默认 12。引擎会在演奏前用 RPN 0 把这个值告诉要用到的每个通道（General MIDI 音源读取的那条消息），
并按它换算每一个弯音值。

关掉 **发送弯音** 就完全不发弯音：音符落在最近的半音上，一个 `0xE0` 都不会发出——给不认弯音轮的音源用。
范围一行会变灰但保留数值，开关打开时还是原来的值。播放中关掉开关时，引擎会给被自己带偏的通道补发一次
居中消息，免得这个模式把残留的颤音当成永久的失谐。

### 在 DAW 里

把 bundle 复制到插件目录并重新扫描：

| | 插件目录 |
|---|---|
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` 与 `~/Library/Audio/Plug-Ins/Components/` |
| Linux | `~/.vst3/` |

如果装过本项目早期的 `VGMPlay.vst3`，先删掉它：插件编号是刻意保持不变的，宿主会把两个 bundle 当成同一个插件。

### 把分轨录进 DAW

多输出路由就是为了这一件事：一次把每种芯片录到各自的轨道上，而不是逐个 solo。

![把 VGMToybox 的各路输出录进 DAW](docs/assets/multitrack-recording-in-daw.png)

1. **启用额外总线**：在宿主里为这个插件实例打开其余总线。插件声明 64 路立体声总线，默认只有 `Out 1/2` 是通的 —— 和 Kontakt 一样的那一步。
2. **分配通道条**：点混音条标题栏的 *spread outputs*（第 1 条 → `Out 1/2`，第 2 条 → `Out 3/4`，…），不合适的再逐条在输出选择器里改 —— 或者全部送到 master 总线，路由不动。
3. **每条总线在宿主里开一条轨，录制。** 每个芯片声道落到自己的轨道上，路由选择会被记成下次加载曲子时的默认值。

每条通道条都是独立的 libvgm 实例，所以分轨加起来正好等于你听到的混音 —— `vgm_mixer_test` 保证这个求和是逐样本一致的。

### 关闭 mixer

走的是 libvgm 原本的混音，其单位增益求和与 mixer 的逐样本一致。

## 许可证

VGMToybox 以 **GNU Affero 通用公共许可证 v3.0 或更高版本**（AGPL-3.0-or-later）发布，完整条款见 [LICENSE](LICENSE)。

选择 AGPL 的决定性原因是 JUCE 的双许可政策：JUCE 同时提供 AGPLv3 和商业许可证，本项目使用 AGPLv3 这一路径，因此插件与独立程序作为组合作品按 AGPL 分发。若取得 JUCE 商业许可证，才可以在其他许可证下分发。

### 上游项目

VGMToybox 建立在以下上游项目之上。它们各自的许可证适用于其代码；下表只是概览，不能替代各源码树中的许可证原文。

| 上游项目 | 提供什么 | 许可证 |
|---|---|---|
| [libvgm](https://github.com/ValleyBell/libvgm) | VGM/VGZ/S98/DRO/GYM 解码、芯片仿真与播放 | 按文件：BSD-3-Clause、GPL-2.0 / GPL-2.0-or-later、LGPL-2.1-or-later、MIT；见各文件头 |
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.9 | VST3/AU/AAX 与独立程序框架、UI 和宿主集成 | AGPLv3 或商业；本项目使用 AGPLv3 |
| [inih](https://github.com/benhoyt/inih) | 配置文件 INI 解析 | BSD-3-Clause |
| [wingetopt](https://github.com/alex85k/wingetopt) | MSVC 命令行构建使用的 getopt | ISC，含 BSD-3-Clause 部分 |
| [zlib](https://zlib.net/) 1.3.1 | `.vgz` 使用的 DEFLATE | zlib |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | 仅构建期依赖管理，不随二进制分发 | MIT |

libvgm、inih 和 wingetopt 以源码形式放在 `libs/` 下；JUCE 与 zlib 由 CPM 在配置阶段拉取。JUCE 的固定版本是 `plugin/CMakeLists.txt` 中的 `JUCE_TAG`。

## 关于作者

如果你喜欢，欢迎关注：

1. MIDIshow: <https://www.midishow.com/u/Silgm>
1. Bilibili: <https://space.bilibili.com/249486555>
