# VGMToybox

A chiptune music player as a **VST3 plugin** (plus a standalone application), built on
[libvgm](https://github.com/ValleyBell/libvgm.git). Load a `.vgm`/`.vgz` (or `.s98`, `.dro`,
`.gym`) file, play it on a DAW track and mix it like any other instrument.

> **Naming.** The fork's product is **VGMToybox** (`vgm-toybox`). The command-line player in the
> repository root keeps the upstream **VGMPlay** identity - name, `VGMPlay.ini`, `VGMPlay64.exe` and
> all - so the upstream tool and this plugin never get mixed up. The plugin's artefacts are
> `VGMToybox.vst3` and `VGMToybox.exe`.

## What it does

- **Plays the log formats libvgm covers** - `.vgm`, `.vgz`, `.s98`, `.dro`, `.gym`, with `.m3u`
  playlists - and enumerates every sound chip the song uses.
- **A DAW-style mixer** below the chip list: one channel strip per chip channel, with a pan knob,
  a vertical gain fader on a dB scale, mute/solo, meter graduations (0/6/12/24/48) and a gradient
  level meter that turns red from 0 dB up. The strips group by chip and scroll.
- **Multi-output routing** (the multi-out instrument model): the plugin declares 64 stereo output
  buses (`Out 1/2` … `Out 127/128`), each strip sends either to the internal **master** bus or
  straight to one of them, and the master bus picks its own destination. The count is a build-time
  constant (`TOYBOX_OUTPUT_BUSES`, default 64). Extra buses are **inactive by default** - enable
  the ones you need in the host, exactly like Kontakt.
- **The classic player path is still there**: switching the mixer off runs libvgm's plain mix, and
  its unity-gain sum is bit-identical to the mixer's (the acceptance test proves it).

## Building (Windows, MSVC)

```powershell
git submodule update --init --recursive
.\build.ps1
```

The script locates Visual Studio, configures with CMake + Ninja and copies everything into `dist/`:

| Artefact | What it is |
|---|---|
| `dist\VST3\VGMToybox.vst3` | the plugin |
| `dist\Standalone\VGMToybox.exe` | the same engine as a standalone application |
| `dist\VGMPlay64.exe` + `VGMPlay.ini`, `VGMPlay.txt` | the **upstream command-line player**, built from `vgmplayer/` |

Options: `-NoPlugin` (command-line player only), `-Proxy <url>` / `-NoProxy`. For a lighter build
with fewer output buses, configure with the cache variable first - it is a build-time constant
because a host fixes a plugin's bus layout when it loads it:

```powershell
cmake -S . -B build -DTOYBOX_OUTPUT_BUSES=16
.\build.ps1
```

### Tests

Both are console targets, built on demand and safe to run anywhere:

| Target | Covers |
|---|---|
| `vgm_mixer_test <file> [seconds]` | the libvgm path end to end: per-track isolation, the mixer parameters, the two volume stages, output routing - and the sample-exact gate "the unity-gain mixer mix equals the plain mix" (`-1000.00 dB`). |
| `vgm_core_test` | the playback core through the fake backend. It deliberately links **no libvgm code**, so a core that still depended on the format engine would not link. |

```bash
cmake --build build --target vgm_core_test
build/plugin/vgm_core_test
```

## Installing in a DAW

Copy `VGMToybox.vst3` into your VST3 folder and rescan. To use the separate outputs, enable the
extra buses for the plugin instance in the host (that step is the host's, not the plugin's).
If an older `VGMPlay.vst3` from this project is installed, **delete it first**: the plugin codes
are deliberately unchanged, so a host would see the two bundles as the same plugin.

## Licence and attribution

This project stands on other people's work and keeps the notices visible:

- **libvgm** - the emulation and playback library the plugin links against (git submodule,
  https://github.com/ValleyBell/libvgm.git), by Valley Bell and contributors.
- **VGMPlay** - the command-line player whose sources sit in `vgmplayer/`; its name, files and
  documentation are untouched.
- The emulator cores under `libvgm/emu/cores/` carry their own licence headers; they are the reason
  this tree is not simply "one author's code".

The plugin links libvgm statically, so it is distributed under the same GNU General Public License
terms as the library and the player it comes from - see the licence headers in the sources for the
exact wording. Keep those notices when you redistribute.

## Documentation

| Document | Content |
|---|---|
| `CODEBUDDY.md` | architecture, threading and real-time rules, build/test conventions |
| `docs/playback-backend-design.md` | the playback port every format engine plugs into (the standard, the ADRs, the migration) |
| `docs/mixer-milestones.md` | mixer requirements, design and milestones |
| `docs/multitrack-output-design.md` | the multi-output routing design (buses, targets, ADRs) |
| `VGMPlay.txt` | the upstream command-line player's readme (keys, formats) |
