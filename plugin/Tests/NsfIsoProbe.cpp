// Isolation spike with a handler: the crash reports its own address and stack.
//
// Built with /MAP so the addresses can be turned back into function names without a debugger,
// because this machine has no cdb and no lldb.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <windows.h>

#include "Nsf_Emu.h"

namespace
{

void report (EXCEPTION_POINTERS* info)
{
	const auto* record = info->ExceptionRecord;
	const auto base = (std::uintptr_t) GetModuleHandleW (nullptr);

	std::printf ("EXCEPTION code=0x%08lX address=0x%p rva=0x%llX\n",
				 (unsigned long) record->ExceptionCode,
				 record->ExceptionAddress,
				 (unsigned long long) ((std::uintptr_t) record->ExceptionAddress - base));

	void* frames[24] = {};
	const unsigned short count = RtlCaptureStackBackTrace (0, 24, frames, nullptr);

	for (unsigned short i = 0; i < count; ++i)
	{
		std::printf ("  frame %2d rva=0x%llX\n", i,
					 (unsigned long long) ((std::uintptr_t) frames[i] - base));
	}

	std::fflush (stdout);
}

} // namespace

long __stdcall filter (EXCEPTION_POINTERS* info)
{
	report (info);
	return EXCEPTION_EXECUTE_HANDLER;
}

int main (int argc, char** argv)
{
	SetUnhandledExceptionFilter (filter);

	{
		if (argc < 2) { std::printf ("usage: probe <file.nsf>\n"); return 1; }

		std::ifstream file (argv[1], std::ios::binary);
		const std::vector<char> data ((std::istreambuf_iterator<char> (file)),
									  std::istreambuf_iterator<char> ());

		Nsf_Emu emu;
		emu.set_sample_rate (44100);
		emu.load_mem (data.data(), (long) data.size());
		std::printf ("voices=%d\n", emu.voice_count());

		emu.start_track (0);
		std::printf ("started\n");

		emu.enable_isolation_();
		std::printf ("isolated\n");

		std::vector<short> buffer (512 * 2, 0);
		emu.set_voice_mask_ (1u);

		for (int i = 0; i < 20; ++i)
		{
			if (emu.play (512, buffer.data()) != nullptr) { std::printf ("play failed\n"); return 1; }
		}

		std::printf ("played peak=%d\n", (int) buffer[0]);
	}

	return 0;
}
