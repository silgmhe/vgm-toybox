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

**MinGW will not work.** JUCE 8 and later support MSVC on Windows and nothing else, so this project
cannot be built with a MinGW toolchain either - the errors start inside JUCE's own sources, before the
compiler ever reaches ours.

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

### The standalone

`VGMToybox` plays the file it is started with, so anything that hands it one - a command line, a
shortcut, Explorer - loads it and starts the transport:

```powershell
.\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgm"
```

### Opening songs from Explorer

Associate the formats with the application once, through Windows' own dialog. **The program never
touches the registry** - no installer, no shell extension, no helper script - so the association is
yours to create, change and undo:

1. right-click a song and choose **Open with** → **Choose another app**;
2. pick `VGMToybox.exe` (*More apps* → *Look for another app on this PC* finds it the first time);
3. tick **Always use this app**, and repeat for `.vgz`, `.s98`, `.dro` and `.gym` if you want those too.

Windows keeps the choice per user, and *Settings* → *Apps* → *Default apps* is where it is edited -
deleting the folder removes all of it. Two things worth knowing: a recorded association stores a
path, so moving or replacing the executable breaks the double click (keep one copy and replace that
file when upgrading); and a launch that cannot use the song it was handed writes one line to
`%TEMP%\vgmtoybox-startup.log` - a launch that works writes nothing.

By default the song goes into the window that is already open: the running window replaces what it
is playing and comes to the front, and the second process exits without drawing anything. *Settings*
→ *General* → **Reuse the running window** turns that off, and then every double-click starts a
window of its own - which is what you want when two songs are meant to be compared.

- **macOS** never starts a second copy of a running application: the Finder hands the document to
  the one that is open, so there is nothing to switch and the page states the behaviour instead. The
  `.app` declares the five formats, so *Open With* offers it (unsigned, so the first launch needs
  *Open* from the context menu).
- **Linux** is not wired up - JUCE's cross-process hand-over has no implementation there - so the
  setting is not shown and every launch is its own process.
- A hand-over that could not be used leaves one line in `%TEMP%\vgmtoybox-second-instance.log`, in
  the same spirit as the startup log above.

### In a DAW

Copy the bundle into the plugin folder and rescan:

| | Plugin folder |
|---|---|
| Windows | `C:\Program Files\Common Files\VST3\` |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` and `~/Library/Audio/Plug-Ins/Components/` |
| Linux | `~/.vst3/` |

If an older `VGMPlay.vst3` from this project is installed, delete it first: the plugin codes are
deliberately unchanged, so a host would see the two bundles as the same plugin.

### What a project keeps

Since 0.1.0 the plugin's state carries the song **as data**, not as a path: the file is embedded in
the project (compressed), so the project opens on a machine that never saw it, together with the
mixer settings, the per-track values and the routing. The transport is not part of it: a project
always opens **stopped at the top of the song**, so opening one never makes sound on its own. Two
limits are worth knowing:

- a song over 16 MB is not embedded - the project still restores every setting and the status line
  says the song has to be loaded again;
- projects saved with 0.0.5 or older used a different format (a path, no snapshot). This version
  refuses that format rather than guessing: the instance comes up empty and says so in the status
  line, and the song is loaded once more by hand.

### Recording stems into a DAW

The multi-output routing exists for one workflow: record every chip on its own track in a single pass
instead of soloing chips one by one.

![Recording the output buses of VGMToybox into a DAW](docs/assets/multitrack-recording-in-daw.png)

1. **Enable the extra buses** for the plugin instance in the host. The plugin declares 64 stereo buses
   and only `Out 1/2` is active until you do - the same step Kontakt users know.
2. **Route the strips**: click *spread outputs* in the mixer header (track 1 → `Out 1/2`, track 2 →
   `Out 3/4`, …), then fix individual strips in their output selector where that does not fit - or
   send everything to the master bus and leave the routing alone.
3. **Arm one host track per bus and record.** Each chip channel lands on its own track, and the routing
   is remembered as the default for the next song you load.

Every strip is a separately rendered libvgm instance, so the stems add up to exactly the mix you would
have heard - `vgm_mixer_test` proves that sum is sample-exact.

### Mixer off

Runs libvgm's plain mix; its unity-gain sum is bit-identical to the mixer's.

## Subscribe

If you like it, subscribe here:

1. MIDIshow: <https://www.midishow.com/u/Silgm>
1. Bilibili: <https://space.bilibili.com/249486555>
