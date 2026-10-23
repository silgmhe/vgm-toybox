// The four symbols NSFPlay's DMC core references but that belong to its CPU and NSF players.
//
// Linking nes_dmc.cpp drags in NES_CPU, which drags in CPULogger, NES_MEM and NSF2_IRQ - the whole
// NSF player, none of which is used when a chip is driven by register writes instead of by an NSF
// file. Rather than vendor that stack, the unreachable paths are stubbed: the DMC calls them when it
// steals cycles from a running CPU and when it raises an IRQ, and there is no CPU here to steal
// from and nothing to interrupt.
//
// The includes are spelled out relative to this file on purpose. NSFPlay's headers include each
// other relatively, so no include directory is needed to parse them; writing the paths here keeps
// the file analysable on its own (an editor that has no compilation database entry for it still
// resolves them) instead of failing with "nes_cpu.h not found".
//
// This file exists only because the linker needs the symbols. If the backend later grows a real NSF
// path, it should link the real implementations and this file should go.

#include "third_party/nsfplay/xgm/devices/CPU/nes_cpu.h"
#include "third_party/nsfplay/xgm/devices/Misc/nsf2_irq.h"

namespace xgm
{

void CPULogger::Init (UINT8 /*cpu*/, UINT8 /*apu*/) {}

void CPULogger::Play () {}

void NSF2_IRQ::Clock (UINT32 /*clocks*/) {}

} // namespace xgm
