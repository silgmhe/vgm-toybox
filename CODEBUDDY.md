# CODEBUDDY.md
This file provides guidance to CodeBuddy when working with code in this repository.

## Overview

VGMPlay is a player for video game music log formats: `.vgm`/`.vgz` (Video Game Music), `.s98`, `.dro` (DOSBox RAW OPL), `.gym` (Genecyst), plus `.m3u` playlists. It is a C++ frontend around the **libvgm** library, which lives in this repository as a git submodule (`libvgm/`, https://github.com/ValleyBell/libvgm.git). `libs/inih` and `libs/wingetopt` are also submodules. Application version is in `version.h` (`APP_NAME`, `VGMPLAY_VER_STR`, `VGM_VER_STR`); changelog is in `Updates.txt`; `VGMPlay.txt` is the user readme.

The repository builds two front-ends on top of the same libvgm libraries:

1. **`VGMPlay64.exe` / `vgmplay`** — the classic command-line player (root directory sources, `PLAYSTATE_*`-driven render loop in `playctrl.cpp`).
2. **`plugin/`** — a JUCE-based GUI plugin (VST3 + standalone) with a transport UI, seekable progress bar and per-chip mute buttons. See "GUI plugin" in the architecture section.

Because there is no README at the repository root, the two authoritative docs are `VGMPlay.txt` (keys, supported formats) and `libvgm/Compiling.txt` (build notes for the library).

## Build commands

Fetch the submodules first — `libvgm`, `libs/inih` and `libs/wingetopt` are pinned git submodules:

```bash
git submodule update --init --recursive
```

Everything is built in **one CMake tree**: the root project adds the libvgm submodule with `add_subdirectory(libvgm)` (static libraries `vgm-utils`, `vgm-audio`, `vgm-emu`, `vgm-player`), and libvgm fetches + adds zlib itself (`libvgm/cmake/CPM.cmake`, used in `libvgm/utils/CMakeLists.txt`). `find_package(libvgm)` is only a fallback for when the submodule is not checked out. `.github/workflows/GHA.yml` still builds an installed libvgm first, which the app now simply ignores.

```bash
# Windows: MSVC + Ninja + static runtime, output in .\dist (+ clangd database)
.\build.ps1                      # options: -Arch x86 | -Config Debug | -Clean | -NoPlugin | -Proxy <url>
build.bat -Clean                 # wrapper for cmd.exe
build.bat -NoPlugin              # command-line player only (skips the JUCE download/build)

# manual equivalent (any platform/toolchain)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build              # or: cmake --build build --target VGMPlay64
```

`build.ps1` locates Visual Studio (`vswhere` + `vcvars64.bat`), configures and builds, then copies `VGMPlay*.exe` + `VGMPlay.ini` + the two text docs into `.\dist\` and writes `compile_commands.json` to the repository root (CMake writes it to `build/`; the root copy lets clangd resolve flags for *all* sources: VGMPlay, libvgm and zlib).

The executable is `VGMPlay64.exe` (x64 MSVC), `VGMPlay.exe` (Win32 MSVC) or `vgmplay` (other toolchains) inside the build directory.

Static runtime: the root `CMakeLists.txt` sets `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (`/MT`) for every target, so the executable does not require the VC++ Redistributable, a UCRT DLL or `zlib1.dll` — only Windows system DLLs (check with `dumpbin /dependents`). Pass `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL` to use the runtime DLLs instead.

zlib is compiled from source into the package, so no zlib DLL is needed. Related cache variables: `LIBVGM_ZLIB_FROM_SOURCE` (default `ON`; `OFF` uses a system zlib via `find_package(ZLIB)`), `LIBVGM_ZLIB_TAG` (`v1.3.1`), `LIBVGM_ZLIB_MIRRORS` / `LIBVGM_ZLIB_REPOSITORY` (the build probes GitHub first, then the Gitee mirror, because GitHub is not always reachable), `CPM_SOURCE_CACHE` (source cache, e.g. `H:/cmake_cache`).

The **JUCE plugin** is part of the default build (`VGMPLAY_BUILD_PLUGIN`, set to `OFF` by `-NoPlugin`). `plugin/CMakeLists.txt` fetches JUCE with CPM (`JUCE_TAG`, default `8.0.9`; `JUCE_MIRRORS` / `JUCE_REPOSITORY` work like the zlib ones) and builds a VST3 + a standalone application. `build.ps1` exports the Windows system proxy (`HTTP(S)_PROXY`) before configuring, because git/CPM do not read the Windows proxy settings — use `-Proxy <url>` to override or `-NoProxy` to disable.

Notable CMake cache variables: root project `MEDIA_CONTROLS` = `OFF`/`WIN_KEYS`/`WIN_SMTC`/`DBUS` (default `WIN_KEYS` on Windows, `DBUS` on Linux when DBus1 is found, otherwise `OFF`); libvgm-side `BUILD_LIBAUDIO`, `BUILD_LIBEMU`, `BUILD_LIBPLAYER`, `BUILD_TESTS`, `BUILD_PLAYER`, `BUILD_VGM2WAV`, `LIBRARY_TYPE` (`STATIC` default), `SNDEMU__ALL` plus per-chip `SNDEMU_<CHIP>_*` and `AUDIODRV_*` switches. Set `SNDEMU__ALL=OFF` (then enable individual `SNDEMU_<CHIP>_ALL` or `SNDEMU_<CHIP>_<CORE>` options) to compile only selected sound cores.

The root `CMakeLists.txt` pre-sets the in-tree libvgm defaults (overridable): static libraries, `BUILD_TESTS`/`BUILD_PLAYER`/`BUILD_VGM2WAV` off, and on Windows `UTIL_CHARCNV_WINAPI=ON` + `UTIL_CHARCNV_ICONV=OFF` (the libiconv bundled in `libvgm/libs/iconv` is a DLL import library and would add a runtime dependency).

## Running

```bash
dist/VGMPlay64.exe song.vgm [song2.vgz ...]     # Windows (copied there by build.ps1)
build/VGMPlay64.exe song.vgm                    # straight from the build tree
build/vgmplay song.vgm                          # Linux/macOS
```

With no file argument the player prompts for a file name. Command-line options (see `main.cpp`, `OPT_LIST_ARR`): `-h/--help`, `-v/--version`, `-L/--lib-info` (supported formats + sound cores), `-w/--dump-wav`, `-W/--dump-path <path>`, `-d/--output-device <id>`, `-c/--config section.key=Data`, `-C/--cfg-file <path>`.

Runtime keys (from `VGMPlay.txt`): Space/P pause, ESC/Q quit, Left/Right seek 5 s (Ctrl = 60 s), 0–9 jump to 0–90 %, PgUp/B previous, PgDn/N next, R restart, F fade out, Up/Down volume ±0.2 dB (Ctrl ±3 dB), `[`/`]` speed ±1/16, `{`/`}` halve/double speed, Ctrl+X quit after song, Ctrl+P pause after song. All keys work during silent WAV logging too.

## Tests and validation

There is no automated test suite and no linter setup in this repository. CI (`.github/workflows/GHA.yml`) only builds the CMake flow above on Windows (MSVC Win32, MSYS2/MinGW), macOS and Ubuntu, in Release and Debug.

libvgm ships manual test programs behind `-DBUILD_TESTS=ON` (`audiotest`, `emutest`, `audemutest`, `vgmtest`). The closest thing to a smoke test is the VGM renderer test, which takes a **gz-compressed** VGM only (it also exercises the in-tree zlib):

```bash
# in the main build tree (the root CMakeLists disables BUILD_TESTS by default)
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build --target vgmtest
build/vgmtest song.vgz
```

For end-to-end verification, play a file (or dump audio with `-w`) and compare while listening; WAV dumps written by the app use the same render path as live output, so they are the practical regression artifact.

The plugin's playback engine can be tested without a DAW: `plugin/Tests/VgmEngineTest.cpp` renders a file offline with exactly the same `VgmEngine` class that the VST3/standalone plugin uses. It is not part of the default build and has to be built explicitly:

```bash
cmake --build build --target vgm_engine_test
build/plugin/vgm_engine_test song.vgz 5        # renders up to 5 s, writes song.rendered.wav
```

It prints the tags, the chip list, the final position and the peak/RMS of the rendered audio (a peak of ~0.15 is expected for the bundled test tone); exit code 2 means the output was silent.

Linux build dependencies: `libinih-dev` (otherwise the bundled `libs/inih` submodule is used), `zlib1g-dev`, and for the audio drivers `libasound2-dev` / `libpulse-dev` / `libao-dev`; `libdbus-1-dev` for MPRIS media controls. MSVC needs no extra packages (`libs/wingetopt` supplies `getopt_long` on MSVC; MinGW/Linux use the system implementation).

## Architecture

### Two layers, built in a single CMake tree

The repository is not one program but a library + application pair:

1. **`libvgm/` (submodule)** — four libraries: `vgm-utils` (data loaders with zlib/`.vgz` support, OS thread/mutex abstraction, charset conversion), `vgm-audio` (audio output drivers + `AudioStream` abstraction), `vgm-emu` (sound chip emulation cores + resampling), `vgm-player` (format playback engines). It also still ships a CMake package config (`libvgm::` namespace) and pkg-config files for out-of-tree consumers.
2. **Repository root** — the VGMPlay CLI application (`main.cpp`, `playctrl.cpp`, `playcfg.cpp`, `config.cpp`, `mediainfo.cpp`, `m3uargparse.cpp`, `utils.cpp`, `mediactrl*.cpp`).

The root project builds libvgm in-tree: `add_subdirectory(libvgm)` creates the four static targets (`vgm-utils`, `vgm-audio`, `vgm-emu`, `vgm-player`) in the same build tree and links them directly, so one configure + build produces the executable. Two constraints inside `libvgm/` exist because of this and must be kept in mind:

- `libvgm/utils/CMakeLists.txt` fetches zlib with CPM and adds it via `add_subdirectory`, then links it as `$<BUILD_INTERFACE:$<TARGET_FILE:zlibstatic>> $<INSTALL_INTERFACE:ZLIB::ZLIB>`. Referring to the zlib target by name would make libvgm's `export()`/`install(EXPORT)` rules fail ("target … is not in any export set"), because the zlib target is not part of libvgm's export set; the same reason applies to the `$<BUILD_INTERFACE:…>` wrapping of zlib's include directories. Only zlib symbols may be taken from that target — do not "simplify" this back to `find_package(ZLIB)`.
- The submodule is a pinned upstream commit: committing libvgm changes means committing inside the submodule and recording the new revision in the parent repository (that is how the zlib/CPM change is stored). The MSVC builds of the whole tree must share one runtime: the root sets `CMAKE_MSVC_RUNTIME_LIBRARY=/MT`, which the libvgm and zlib sub-projects inherit.

### Root application data flow

`main.cpp` is startup only: it initializes search paths, loads the INI, merges command-line overrides, parses file/playlist arguments, then calls `PlayerMain()` (implemented in `playctrl.cpp`).

**Configuration**: `Configuration` (`config.hpp/cpp`) is a generic `map<section, CfgSection>` where each section keeps unordered entries plus an *ordered* list used for channel `Mute*`/`Pan*` keys where order matters. Two sources are built as `Configuration` objects and merged with `operator+=`; command line wins over INI (`playerCfg += argCfg`). `playcfg.cpp` then converts the merged config into typed structures: `GeneralOptions` (sample rate/bits, volume, loops, fade/pause times, resampler mode, WAV logging mode and path, log levels, audio driver/device selection, UI options) and `ChipOptions[0x300]` (per-chip core choice, per-channel mute and pan, extra options). The `CFG_CHIP_LIST` table maps libvgm `DEV_ID` values to INI section names (`[YM2612]`, `[NES APU]`, ...); `[Chip]` applies to every instance of that chip, while `[Chip-0]`/`[Chip-1]` target a single instance (stored in slots `0x100 + devID*2 + inst`). `ApplyCfg_General`/`ApplyCfg_Chip` push the result into the `PlayerA` facade via `SetConfiguration`/`SetDeviceOptions`.

**INI search order** (`InitAppSearchPaths` in `main.cpp`): Unix install datadir, executable directory, the directory the app was invoked from, per-user config dir (`%USERPROFILE%/.vgmplay/` on Windows, `$XDG_CONFIG_HOME|~/.config` + `/vgmplay/` elsewhere), then `./`. First `VGMPlay.ini`/`vgmplay.ini` found is loaded; `-C` overrides the search entirely. `VGMPlay.ini` in the repo root is the default template and is installed next to the binary.

**Song list**: `m3uargparse.cpp` expands command-line arguments into `songList` (one entry per playable file, with optional playlist back-reference) and `plList` (playlist files); `.m3u` files are recursively read and must only reference supported formats.

### Playback runtime (`playctrl.cpp`, `mediainfo.cpp`)

`PlayerMain()` is the program's main loop and the densest file in the repo. Sequence:

1. `InitAudioSystem()` — `Audio_Init()`, enumerate drivers, choose an *output* driver (`adOut`, default: first driver on Windows / last on Linux) and a *disk writer* (`adLog`, WaveWriter) according to `General.pbMode` (play / log / play+log). `InitAudioDriver()` adds driver-specific setup (e.g. DSound window handle, Pulse stream name).
2. `StartAudioDevice()` — pushes sample rate/channels/bits and buffer settings into the driver, sizes the local render buffer, and calls `PlayerA::SetOutputSettings(sampleRate, channels, bits, smplAlloc)`.
3. Register the four playback engines on the single `PlayerA` instance: `new VGMPlayer`, `S98Player`, `DROPlayer`, `GYMPlayer` (kept alive for the whole run so per-chip options survive track changes). Event/file-request/log callbacks are installed, then configuration is applied.
4. Per song: `OpenFile()` (UTF-8 path → `FileLoader`/`FileLoader_InitW` via `CPCONV`) → `PlayerA::LoadFile(DATA_LOADER*)` → `PreparePlayback()` (loop count, fade/end-silence samples) → `Start()` → `EnumerateChips()` (must run *after* `Start()` to learn which cores were actually created) → `Render(0, NULL)` once (consumes the init block, gives the real start offset) → print song info → optional disk writer → `PlayFile()`.
5. `PlayFile()` loops until `PLAYSTATE_END`, refreshing the status line, reading the keyboard, and draining `MediaInfo::_evtQueue` through `HandleCtrlEvent()`. Audio is produced by the audio driver calling `FillBuffer()`, which locks `renderMtx` and calls `PlayerA::Render()`; when no output driver is available, `manualRenderLoop` makes the main loop render into `audioBuf` itself.

**Threading model**: the audio driver's callback thread renders; the main thread handles display/keys/events; `renderMtx` (an `OS_MUTEX`) guards every player mutation (volume, seek, fade, reset, pause). `MediaInfo::_evtQueue` is the thread-safe-ish hand-off used by media controls to inject commands (see `mediactrl_*.cpp` calling `Event()` from other threads or signals).

**`MediaInfo`** (`mediainfo.hpp/cpp`) is the shared state hub: the `PlayerA` instance, `GeneralOptions`/`ChipOptions`, play-state bit flags, current song/playlist paths, song tags, used-chip list, code-page converters, signal callbacks (`MI_SIG_*`, notified via `Signal()`) and the event queue (`MI_EVT_*` with parameters like `MIE_CTRL_*`, `MIE_PS_*`, `MIE_PL_*`). Media controls subscribe to signals (metadata/play-state/position) and push events back; `playctrl.cpp` translates both directions for keyboard and MPRIS/SMTC.

**Media controls** (`mediactrl.hpp` + `GetMediaControl(sig)`): `MediaControl` is a small base class; implementations are `mediactrl_WinKeys.cpp` (Windows `RegisterHotKey`), `mediactrl_WinSMTC.cpp` (Windows Runtime `SystemMediaTransportControls`, requires `ENABLE_WINRT`), and `mediactrl_dbus.cpp` (Linux MPRIS over DBus). Selected at build time through the `MEDIA_CONTROLS` variable (defines `MEDIACTRL_WINKEYS`/`MEDIACTRL_SMTC`/`MEDIACTRL_DBUS`) and at runtime by `General.MediaKeys` (`Default`/`None`/`WinKeys`/`SMTC`/`DBus`).

**Windows specifics**: the entry point is `wmain` (`USE_WMAIN`); wide `argv` is converted to UTF-8 up front, and all user-facing strings are UTF-8 (`u8printf` in `utils.cpp`). `utils.hpp` `#undef`s `GetFileTitle`/`GetFileExtension` to avoid clashing with the Windows API macros.

### Inside libvgm (read these before touching the library)

- `libvgm/utils/` — `DataLoader.h` (abstract data source), `FileLoader.c`/`MemoryLoader.c` (zlib decompression for `.vgz`), `OSMutex`/`OSThread`/`OSSignal` (POSIX/Win split), `StrUtils.h` (codepage conversion, implemented via iconv or WinAPI).
- `libvgm/audio/` — `AudioStream.h` (`AUDDRV_INFO`, `AudioDrv_*`, `Audio_*`, `AERR_*`) with one file per driver under `audio/drivers/` (WaveWriter is the file logger), plus data forwarding so output and logging drivers can be chained (`AudioDrv_DataForward_Add`, used for WAV dumping).
- `libvgm/emu/` — `SoundEmu.h` (`SndEmu_GetDevDecl`, `SndEmu_Start2`, `SndEmu_Stop`, ...), `EmuStructs.h` (`DEV_DECL`, `DEV_INFO`, `RWF_*` flags), `SoundDevs.h` (all `DEVID_*` IDs), `Resampler.c` (`Resmpl_*`, modes `RSMODE_*`, output format is `WAVE_32BS`). Chip cores are a flat `emu/cores/` directory: an interface/register file per chip (`*intf.c`) plus one or more emulation implementations (e.g. Nuked-OPN2, MAME, EMU cores). Adding a core means adding files there and wiring `SNDEMU_*` options in `emu/CMakeLists.txt`.
- `libvgm/player/` — `PlayerBase` (`playerbase.hpp`) is the abstract engine (`LoadFile`/`Start`/`Stop`/`Seek`/`Render`, `PLREVT_*` event and `PLRLOG_*` log levels, `PLR_SONG_INFO`/`PLR_DEV_INFO`/`PLR_DEV_OPTS` structures). `PlayerA` (`playera.hpp`) is the front-end facade that owns registered engines, sets output settings/volume/fade/loop/speed, and dispatches rendering to whichever engine loaded the file. Format engines: `VGMPlayer`, `S98Player`, `DROPlayer`, `GYMPlayer`; `playerbase.cpp` provides format detection (`PlayerCanLoadFile`). Note this is a C++ class API — there is no flat C `PLR_*` functional API in this version, and `stdtype.h`/`common_def.h` live in the libvgm root.

### GUI plugin (`plugin/`, JUCE)

`plugin/` is a JUCE plugin target (`juce_add_plugin`, VST3 + Standalone) that links the same in-tree libvgm libraries as the command-line player — `vgm-player`, `vgm-emu`, `vgm-utils`, but **not** `vgm-audio`, because a plugin must never open an audio device itself. JUCE is fetched with CPM (`plugin/CMakeLists.txt`), the plugin is enabled by the root option `VGMPLAY_BUILD_PLUGIN`.

Files and responsibilities:

- `Source/vgm/VgmEngine.{h,cpp}` — the whole libvgm integration. Owns one `PlayerA` with all four format engines, loads files through `MemoryLoader` (handles `.vgz` gzip containers), applies the player `Config` (master volume, loop count, fade, **`endSilenceSmpls = 0`** so a DAW does not get trailing silence) and enumerates tags/chips for the UI.
- `Source/PluginProcessor.{h,cpp}` — `AudioProcessor` with one stereo output bus, `processBlock` forwards to `VgmEngine::render`, and the file path plus the transport settings are saved with the host session in `getStateInformation`. The **standalone ignores restored state on purpose** (`wrapperType == wrapperType_Standalone` makes `setStateInformation` return immediately), so double-clicking `VGMPlay.exe` always starts with an empty player; only a plugin inside a host restores its session.
- `Source/PluginEditor.{h,cpp}` — the GUI: file chooser + drag & drop, an info card (title/tags/file name/position), play/pause/stop plus a **loop-forever toggle button**, a draggable position slider, loop/fade/volume controls and one checkbox per chip (ticked = audible). All colours come from the nested `LightLookAndFeel` (macOS-like light theme: white cards on `#f2f2f7`, hairline borders, `#0a84ff` accent); the palette helpers are at the top of the `.cpp`. Note that the loop button and the loop combo box are two views of the same value (`VgmEngine::getLoopCount()`) and `updateControls()` keeps them in sync. Trap: a slider's text box gets its colours and its text formatting **baked in when `setTextBoxStyle()` creates the label** (`LookAndFeel_V2::createSliderTextBox`), and at that point the slider has no parent yet, so the lookup falls back to the *default* (dark) `LookAndFeel_V4` - white text on the white card. Therefore `volumeSlider` gets its `Slider::textBox*` colours, its suffix and its `textFromValueFunction` *before* `setTextBoxStyle()`, plus an `updateText()` afterwards.
- `Tests/VgmEngineTest.cpp` — offline renderer used to verify the engine (see "Tests and validation").

Threading contract: the audio thread calls `render()`, which uses a **try**-lock on the engine mutex and outputs silence while a file is being loaded (so the GUI never blocks the audio callback for long). The message thread uses a normal lock for loading, seeking, muting and configuration. Never call into libvgm from the message thread without that lock.

Three traps that already caused bugs — keep them in mind when changing the render path:

- `PlayerA::Render(bufSize, data)` takes a **byte** count and **returns the number of bytes written**, not samples (`bytes / (bits/8 * channels)` gives samples). `SetOutputSettings(rate, 2, 32, maxBlockSize)` selects 32-bit integer stereo output; libvgm packs 24-bit internal samples by shifting them left by 8, so the float conversion factor is `1.0f / 2147483648.0f`.
- `PlayerA::SetSampleRate()` **skips the player that is currently playing**, so a song that is already loaded keeps running at the old rate when the output sample rate changes (audible as wrong pitch and playback speed). `VgmEngine::prepare()` therefore re-loads the current file when the rate or the block size changes, and a file must never be loaded before the first `prepare()` call.
- libvgm mixes all chips into one stereo buffer (`Resmpl_Execute` *accumulates* into the destination). There is no per-chip output API yet, which is why the plugin currently has a single stereo bus — see the per-chip multi-out plan below.

Roadmap for the DAW multi-out feature: each chip can already be muted/disabled from outside (`SetDeviceMuting` with `PLR_DEV_OPTS.muteOpts.disable` skips the chip's resampler and therefore its emulation), and `GetSongDeviceInfo` lists the chips with their `PLR_DEV_ID`. A real per-chip stem output should be added in libvgm as a small render tap (a per-device output buffer used in the `_devices` loop of the four `*Player::Render` functions, next to the existing `Resmpl_Execute(&clDev->resmpl, smplStep, &data[curSmpl])`), because rendering the song once per chip (N player instances with solo-muting) would cost N times the CPU and memory.

### Conventions

- Style is C++98-era by design (the codebase still contains MSVC6 compatibility paths): use `std::string`/`std::vector`/`std::map`, the `UINT8/UINT16/UINT32/INT32` typedefs from `stdtype.h`, tabs for indentation, and keep new libvgm code C++98/C90-clean (`REALLY_NEED_CXX14` exists as an opt-in). Avoid C++11+ features in libvgm; the root app is also written without them in active code.
- Error convention is `UINT8`/`UINT32` return codes where the high bit (`& 0x80`) means failure and `0x00`/`AERR_OK` means success; libvgm uses `0xF0`-style codes. Log output goes through `PlayerLogCallback` (`playctrl.cpp`) honoring `General.LogLevelFile`/`LogLevelEmu`.
- Code that must run on old MSVC uses `stricmp`/`strnicmp`/`snprintf` shims defined at the top of the affected `.cpp` files — follow the existing `#ifdef _MSC_VER` pattern rather than introducing new portability helpers.
