<h1 align="center">VGMToybox</h1>

<p align="center">
  <img src="plugin/assets/icon/vgmtoybox-256.png" width="96" alt="VGMToybox 图标">
</p>

<p align="center"><a href="README.md">English</a> | 简体中文</p>

一个 chiptune 音乐播放器，既是 **VST3 / AU 插件**，也是独立程序，基于 [libvgm](https://github.com/ValleyBell/libvgm.git) 构建。可读 `.vgm`、`.vgz`、`.s98`、`.dro`、`.gym`（以及 `.m3u` 播放列表），会列出曲子用到的每一种芯片，并给每个芯片声道一条独立混音条。最多 64 路立体声输出总线，一首曲子可以一次录成分轨。

## 构建

第三方库以源码形式随仓库提供，放在 `libs/` 下，没有子模块需要拉取。构建时唯一要下载的是 zlib，由 CMake 拉取并编译一次，用于 `.vgz` 解压。

### Windows

需要 Visual Studio 2019/2022/2026，安装 **使用 C++ 的桌面开发** 工作负载，另需 CMake 与 Ninja（CMake 自带的 Ninja 会被自动找到）。

```powershell
.\build.ps1
```

脚本会配好 MSVC 环境、用 CMake + Ninja 构建，并把产物复制到 `dist\`：

| 产物 | 说明 |
|---|---|
| `dist\VST3\VGMToybox.vst3` | 插件 |
| `dist\Standalone\VGMToybox.exe` | 同一套引擎的独立程序 |
| `dist\AAX\VGMToybox.aaxplugin` | Pro Tools 格式 —— 未签名，只能在开发者版里加载 |
| `dist\VGMPlay64.exe` + `VGMPlay.ini` | 上游的命令行播放器 |

`-NoPlugin` 只构建命令行播放器；`-Proxy <url>` / `-NoProxy` 控制 CPM 访问 GitHub 的方式。AAX 与总线数是构建期常量，第一次构建前写进缓存一次即可：

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

控制台测试是 `EXCLUDE_FROM_ALL`，要按名字请求：

```bash
cmake --build build --target vgm_core_test vgm_ui_test
./build/plugin/vgm_core_test
./build/plugin/vgm_ui_test
```

`vgm_mixer_test <file> [seconds]` 是做逐样本校验的那个：它会把一首真实的曲子渲染两遍，所以要给它一首曲子 —— 默认不跑也是这个原因。

## 使用方法

**独立程序。** `VGMToybox` 会播放启动时交给它的文件，所以命令行、快捷方式、资源管理器，任何把文件递过去的方式都能加载并开始播放：

```powershell
.\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgm"
```

Windows 上的关联是 `打开方式` → `始终使用此应用`；程序从不写注册表，删掉文件夹就什么都不剩。

**在 DAW 里。** 把 bundle 复制到插件目录并重新扫描：

| | 插件目录 |
|---|---|
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` 与 `~/Library/Audio/Plug-Ins/Components/` |
| Linux | `~/.vst3/` |

如果装过本项目早期的 `VGMPlay.vst3`，先删掉它：插件编号是刻意保持不变的，宿主会把两个 bundle 当成同一个插件。

**分离输出。** 插件声明 64 路立体声总线，默认只有 `Out 1/2` 激活 —— 在宿主里为这个实例启用其余总线，和 Kontakt 一样。然后在混音条标题栏点 *spread outputs*（第 1 条 → `Out 1/2`，第 2 条 → `Out 3/4`，…），或逐条设置目标。每条混音条都是独立的 libvgm 实例，所以分轨加起来正好等于你听到的混音。

**关闭 mixer** 走的是 libvgm 原本的混音，其单位增益求和与 mixer 的逐样本一致。

## 关于作者

如果你喜欢，欢迎关注：

1. MIDIshow: <https://www.midishow.com/u/Silgm>
1. Bilibili: <https://space.bilibili.com/249486555>
