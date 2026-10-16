# VGMToybox

<img src="plugin/assets/icon/vgmtoybox-256.png" width="96" alt="The VGMToybox icon">

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

Everything the build needs is in the repository: the third-party libraries are vendored as source
under `libs/` (see "Licence and attribution"), so there are no submodules to fetch.

```powershell
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

All three are console targets, built on demand and safe to run anywhere:

| Target | Covers |
|---|---|
| `vgm_mixer_test <file> [seconds]` | the libvgm path end to end: per-track isolation, the mixer parameters, the two volume stages, output routing - and the sample-exact gate "the unity-gain mixer mix equals the plain mix" (`-1000.00 dB`). |
| `vgm_core_test` | the playback core through the fake backend. It deliberately links **no libvgm code**, so a core that still depended on the format engine would not link. |
| `vgm_ui_test` | the button faces, rendered into images: the shadow under a face, the rounded corner where the hairline stops, the tighter shadow while pressed, the accent ring on focus and the glow a container paints behind a toggled button (see `docs/ui-button-design.md`). |

```bash
cmake --build build --target vgm_core_test   # or vgm_mixer_test / vgm_ui_test
build/plugin/vgm_core_test
```

## Installing in a DAW

Copy `VGMToybox.vst3` into your VST3 folder and rescan. To use the separate outputs, enable the
extra buses for the plugin instance in the host (that step is the host's, not the plugin's).
If an older `VGMPlay.vst3` from this project is installed, **delete it first**: the plugin codes
are deliberately unchanged, so a host would see the two bundles as the same plugin.

## Recording stems into a DAW

The multi-output routing exists for one workflow: record every chip on its own track in a single
pass instead of soloing chips one by one.

![Recording the output buses of VGMToybox into a DAW](docs/assets/multitrack-recording-in-daw.png)

1. **Enable the extra buses** for the plugin instance in the host - only `Out 1/2` is active until
   you do (the same step Kontakt users know).
2. **Route the strips**: click *spread outputs* in the mixer card header (track 1 -> `Out 1/2`,
   track 2 -> `Out 3/4`, …) and then adjust individual strips in their output selector where that
   does not fit - or send everything to the master bus and leave the routing alone.
3. **Arm one host track per bus and record.** Each chip channel lands on its own track, and the
   routing choice is remembered as the default for the next song you load.

Every strip is a separately rendered libvgm instance, so the stems add up to exactly the mix you
would have heard - `vgm_mixer_test` proves that sum is sample-exact.

## Opening songs from Explorer

The standalone application plays the song it is started with, so anything that hands it a file -
a command line, a shortcut, Explorer - loads it and starts the transport:

```powershell
.\dist\Standalone\VGMToybox.exe "C:\music\Some Song.vgm"
```

To do that from Explorer, register the application as a handler for the formats it plays. It is per
user, needs no administrator, and is reversible:

```powershell
cd dist
.\RegisterFileTypes.cmd              # adds VGMToybox to the "Open with" list
.\RegisterFileTypes.cmd /default     # also makes it the default for .vgm .vgz .s98 .dro .gym
.\RegisterFileTypes.cmd /unregister  # undoes it, leaving other handlers alone
```

Or the plain Windows way: right-click a song, **Open with** -> **Choose another app** ->
`VGMToybox.exe`, tick *Always use this app*. Either way the writes go to
`HKCU\Software\Classes` only, so uninstalling stays "delete the folder".

## Licence and attribution

This project stands on other people's work and keeps the notices visible. The libraries are
**vendored as source under `libs/`** rather than pulled in as git submodules, so they can be
patched in place:

| Vendored | What it is | Licence |
|---|---|---|
| `libs/libvgm/` | the emulation and playback library the plugin links against - upstream `ValleyBell/libvgm`, master `915e928` plus the in-tree build/zlib patch | no licence file ships with it (see the note below); the emulator cores under `libs/libvgm/emu/cores/` carry their own headers |
| `libs/inih/` | the INI reader the command-line player uses - upstream `benhoyt/inih`, `4e618f7` | New BSD (BSD-3-Clause), © 2009 Ben Hoyt; `libs/inih/LICENSE.txt` is kept |
| `libs/wingetopt/` | `getopt_long` for MSVC - upstream `alex85k/wingetopt`, `e8531ed` | permissive ISC-style terms (Todd C. Miller / The NetBSD Foundation); `libs/wingetopt/LICENSE` is kept |

**VGMPlay**, the command-line player in `vgmplayer/`, comes from upstream
`ValleyBell/vgmplay-libvgm`; its name, files and documentation are untouched.

One note before redistributing widely: **neither Valley Bell project ships a licence file** - not
in the vendored trees, and not on GitHub - so the exact terms are worth confirming with the author.
Meanwhile the sources' own headers are the authority: the plugin links libvgm statically, and
`libs/libvgm/emu/cores/` mixes emulator cores that each carry their own (mostly permissive)
notice. Keep every one of them intact.

## Documentation

| Document | Content |
|---|---|
| `CODEBUDDY.md` | architecture, threading and real-time rules, build/test conventions |
| `docs/playback-backend-design.md` | the playback port every format engine plugs into (the standard, the ADRs, the migration) |
| `docs/mixer-milestones.md` | mixer requirements, design and milestones |
| `docs/multitrack-output-design.md` | the multi-output routing design (buses, targets, ADRs) |
| `VGMPlay.txt` | the upstream command-line player's readme (keys, formats) |
