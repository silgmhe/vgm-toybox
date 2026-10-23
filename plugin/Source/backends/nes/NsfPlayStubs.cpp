// The four symbols NSFPlay's DMC core references but that belong to its CPU and NSF players.
//
// Linking nes_dmc.cpp drags in NES_CPU, which drags in CPULogger, NES_MEM and NSF2_IRQ - the whole
// NSF player, none of which is used when a chip is driven by register writes instead of by an NSF
// file. Rather than vendor that stack, the unreachable paths are stubbed: the DMC calls them when it
// steals cycles from a running CPU and when it raises an IRQ, and there is no CPU here to steal
// from and nothing to interrupt.
//
// This file exists only because the linker needs the symbols. If the backend later grows a real NSF
// path, it should link the real implementations and this file should go.

#include "nes_cpu.h"
#include "nes_mem.h"
#include "nsf2_irq.h"

namespace xgm
{

void CPULogger::Init (UINT8 /*cpu*/, UINT8 /*apu*/) {}

void CPULogger::Play () {}

bool NES_MEM::WriteReserved (UINT32 /*adr*/, UINT32 /*data*/) { return false; }

void NSF2_IRQ::Clock (UINT32 /*clocks*/) {}

} // namespace xgm
