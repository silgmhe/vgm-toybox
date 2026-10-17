<h1 align="center">VGMToybox</h1>

<p align="center">
  <img src="plugin/assets/icon/vgmtoybox-256.png" width="96" alt="The VGMToybox icon">
</p>

<p align="center"><a href="README.zh-CN.md">简体中文</a> | English</p>

A chiptune music player as a **VST3 / AU plugin** and a standalone application, built on
[libvgm](https://github.com/ValleyBell/libvgm.git). It reads `.vgm`, `.vgz`, `.s98`, `.dro` and `.gym`
(plus `.m3u` playlists), lists the chips a song uses, and gives every chip channel its own mixer strip.
Up to 64 stereo output buses let a song be recorded as stems in one pass.

## Building

The third-party libraries are vendored as source under `libs/`, so there are no submodules to fetch.
The only thing the build downloads is zlib, which CMake fetches and compiles once for `.vgz`.

### Windows

Visual Studio 2019/2022/2026 with the **Desktop development with C++** workload, CMake and Ninja
(the copy bundled with CMake is found automatically).

```powershell
.\build.ps1
```

The script sets up the MSVC environment, configures with CMake + Ninja and copies everything into `dist\`:

| Artefact | What it is |
|---|---|
| `dist\VST3\VGMToybox.vst3` | the plugin |
| `dist\Standalone\VGMToybox.exe` | the same engine as a standalone application |
| `dist\AAX\VGMToybox.aaxplugin` | Pro Tools - unsigned, so developer builds only |
| `dist\VGMPlay64.exe` + `VGMPlay.ini` | the upstream command-line player |

`-NoPlugin` builds only the command-line player; `-Proxy <url>` / `-NoProxy` control how CPM reaches
GitHub. The AAX bundle and the bus count are build-time constants, so they are set in the cache once,
before the first build:

```powershell
cmake -S . -B build -DTOYBOX_BUILD_AAX=OFF -DTOYBOX_OUTPUT_BUSES=16
.\build.ps1
```

### macOS

The Xcode command line tools, CMake and Ninja:

```bash
xcode-select --install
brew install cmake ninja
```

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build --parallel
```

Drop `CMAKE_OSX_ARCHITECTURES` for a native build; the universal one is for handing the bundle to
somebody else. The floor is **macOS 10.15**, and 11.0 for the arm64 half - `CMAKE_OSX_DEPLOYMENT_TARGET`
in `CMakeLists.txt` sets it, because left unsaid it follows the SDK of whatever machine builds it.

The bundles land in `build/plugin/VGMToybox_artefacts/Release/`: `VST3/VGMToybox.vst3`,
`AU/VGMToybox.component` and `Standalone/VGMToybox.app`.

### Linux

JUCE needs these headers before it builds at all, and ALSA at runtime:

```bash
sudo apt install ninja-build libasound2-dev libx11-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxcursor-dev libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev
```

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The bundles land in `build/plugin/VGMToybox_artefacts/Release/`: `VST3/VGMToybox.vst3` and
`Standalone/VGMToybox`.

Build on the oldest distribution you care about - glibc is backwards compatible, so the version it
links against becomes the floor. The release packages come out of an `ubuntu:22.04` container for
exactly that reason.

### Tests

The console tests are `EXCLUDE_FROM_ALL`, so they are asked for by name:

```bash
cmake --build build --target vgm_core_test vgm_ui_test
./build/plugin/vgm_core_test
./build/plugin/vgm_ui_test
```

`vgm_mixer_test <file> [seconds]` is the sample-exact one: it renders a real song twice, so it needs a
song - it is not part of the default run for that reason.

## Using it

**Standalone.** `VGMToybox` plays the file it is started with, so anything that hands it one - a command
line, a shortcut, Explorer - loads it and starts the transport:

```powershell
.\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgm"
```

Windows association is `Open with` → `Always use this app`; the program never touches the registry, so
deleting the folder removes every trace of it.

**In a DAW.** Copy the bundle into the plugin folder and rescan:

| | Plugin folder |
|---|---|
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/Components/` |
| Linux | `~/.vst3/` |

If an older `VGMPlay.vst3` from this project is installed, delete it first: the plugin codes are
deliberately unchanged, so a host would see the two bundles as the same plugin.

**Separate outputs.** The plugin declares 64 stereo buses and only `Out 1/2` is active - enable the
others for the instance in the host, exactly like Kontakt. Then click *spread outputs* in the mixer
header (track 1 → `Out 1/2`, track 2 → `Out 3/4`, …) or set the target per strip. Every strip is its own
libvgm instance, so the stems add up to exactly the mix you would have heard.

**Mixer off** runs libvgm's plain mix; its unity-gain sum is bit-identical to the mixer's.

## Subscribe

If you like it, subscribe here:

1. MIDIshow: <https://www.midishow.com/u/Silgm>
1. Bilibili: <https://space.bilibili.com/249486555>
