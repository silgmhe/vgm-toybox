#include "xgm/devices/Misc/log_cpu.h"

/**
	The CPU core references the logger on paths that never run when none is installed, but the
	symbols still have to exist at link time. The real implementation lives with the parts of
	NSFPlay this project does not build - it writes a trace file and needs the module model - so
	the two members the core touches are defined here as no-ops.
*/
namespace xgm
{

void CPULogger::Init (UINT8, UINT8) {}
void CPULogger::Play () {}

} // namespace xgm
