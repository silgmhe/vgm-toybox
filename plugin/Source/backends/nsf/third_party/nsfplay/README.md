# NSFPlay chip cores (vendored)

Source: NSFPlay (`reference/nsfplay`), directory `xgm/`. Copied here because `reference/` is
git-ignored: it is the upstream reference tree, not part of this repository, so anything the build
depends on has to live under `plugin/`.

## What is included

The whole `xgm/devices` tree plus the two headers its `device.h` needs (`xtypes.h`, `debugout.h`).
Copying the tree rather than a hand-picked file list is deliberate: the headers include each other
relatively, and a missing one shows up as a build error far from its cause. The extra chip cores
that come along are headers and sources the build does not compile.

Compiled today (see `plugin/CMakeLists.txt`):

```
xgm/devices/Sound/nes_apu.cpp     2A03 pulse, triangle, noise
xgm/devices/Sound/nes_dmc.cpp     DPCM
xgm/devices/CPU/nes_cpu.cpp       required by nes_dmc (cycle stealing)
```

Not yet compiled but present for the backend work: `nes_vrc6`, `nes_n106`, `nes_vrc7`, `nes_fds`,
`nes_mmc5`, `nes_fme7`.

## What is different from upstream

Nothing inside `xgm/`: these are unmodified copies, so they can be diffed against `reference/` and
re-copied when NSFPlay is updated. The only additions are this file and `LICENSE-nsfplay-xgm.txt`.

`plugin/Source/backends/nes/NsfPlayStubs.cpp` supplies four symbols that live in NSFPlay's CPU/NSF
player and are unreachable when a chip is driven by register writes rather than by an NSF file.

## Licence

See `LICENSE-nsfplay-xgm.txt` (the `xgm` readme, which is where the project states its terms) and
`xgm/devices/CPU/km6502.txt` for the 6502 core. Those terms are understood to be GPL-compatible;
this project is AGPL-3.0, so including them is compatible. If that reading is ever disputed, the
API does not change: any NES-family core with a `Write(address, value)` interface can replace these.
