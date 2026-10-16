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
| `vgm_ui_test` | the button faces, rendered into images: the shadow under a face, the rounded corner where the hairline stops, the tighter shadow while pressed, the accent ring on focus and the glow a container paints behind a toggled button (see `docs/desgin/ui-button-design.md`). |

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

To make Explorer do that, associate the formats with the application once, using Windows' own
dialog. **The program itself never touches the registry** - no installer, no shell extension, no
helper script - so the association is entirely yours to create, change and undo:

1. right-click a song and choose **Open with** -> **Choose another app**;
2. pick `VGMToybox.exe` (*More apps* -> *Look for another app on this PC* finds it the first time);
3. tick **Always use this app**. Repeat for `.vgz`, `.s98`, `.dro` and `.gym` if you want those too.

Windows keeps that choice per user, and *Settings* -> *Apps* -> *Default apps* is where it is
edited - so nothing this project ships has to be uninstalled or cleaned up afterwards: deleting the
folder removes all of it.

Two things worth knowing:

- A recorded association stores a path. If the copy you associated is later moved or deleted, a
  double click can do nothing at all while the very same program started from a command line works
  perfectly. Keep one copy of the application and replace that file when upgrading.
- If a launch cannot use the song it was handed - a typo in the command line, or a file the
  backends do not play - the application writes one line about it to
  `%TEMP%\vgmtoybox-startup.log`. A launch that works writes nothing anywhere, and the plugin
  writes nothing at all.

## Licence and attribution

**VGMToybox is licensed under the GNU Affero General Public License, version 3 or later** -
[`LICENSE`](LICENSE) holds the full text, unmodified. In practice: use it, read it, learn from it,
make music with it. If you distribute a modified build - or run one as a network service - the
source comes with it, under the same licence. Songs you make with it are yours; a DAW hosting the
plugin is not a derivative work of it.

Why AGPL rather than something permissive: **JUCE** is dual-licensed as AGPLv3 or commercial
(`LICENSE.md` line 7), so the free path fixes the licence of everything that links it. Turning this
into an MIT project would take a commercial JUCE licence, not a different line in this file.

The one part that is not ours to license is the command-line player in `vgmplayer/`: it is a fork of
Valley Bell's VGMPlay and keeps its upstream terms.

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

**libvgm is the one dependency whose terms are not written down in a file.** Neither Valley Bell
project ships a licence file - not in the vendored trees, and not on GitHub - so this is recorded
here rather than assumed: on 2026-10-17 the author confirmed on the forum that use is fine as long
as the project stays open source, which it does (AGPL-3.0-or-later, see above). Keep that thread;
confirm it again before syncing the library, and ask for a licence file when the chance comes up,
because a file is sturdier than a forum reply.

Where the sources carry their own header, that header is the authority: the plugin links libvgm
statically, and `libs/libvgm/emu/cores/` mixes emulator cores that each carry their own (mostly
permissive) notice. Keep every one of them intact.

## Documentation

| Document | Content |
|---|---|
| `CODEBUDDY.md` | architecture, threading and real-time rules, build/test conventions |
| `docs/desgin/playback-backend-design.md` | the playback port every format engine plugs into (the standard, the ADRs, the migration) |
| `docs/desgin/mixer-milestones.md` | mixer requirements, design and milestones |
| `docs/desgin/multitrack-output-design.md` | the multi-output routing design (buses, targets, ADRs) |
| `docs/desgin/ui-button-design.md` | the button face every control draws through |
| `docs/desgin/testing-architecture.md` | how the window is verified as text (and what the pixel path is still for) |
| `docs/desgin/maintenance-policy.md` | the legacy front-end, libvgm updates, and the licence |
| `docs/review/` | code reviews and their repair records |
| `VGMPlay.txt` | the upstream command-line player's readme (keys, formats) |
