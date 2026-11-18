/**
	**A counting allocator, for asking where memory goes without a debugger.**

	Written because the question was "1.2 GB appeared in three seconds of playback" and every answer
	cost either a GUI session or a toolchain install.

	Two halves, because the two builds reach the same question from opposite ends:

	  **Release** replaces the global `operator new`. That covers every C++ allocation and nothing
	  else - which is exactly how the first gigabyte was ruled out: 174 MB here against 1 221 MB of
	  working set, so the rest is C code calling `malloc`.

	  **Debug** installs `_CrtSetAllocHook` instead, which sees `malloc` as well. It only exists when
	  `_DEBUG` is defined, which is why this file builds differently in the two configurations rather
	  than trying to do both at once - on a Debug build the C++ `operator new` already funnels into
	  `_malloc_dbg`, so hooking it separately would count those bytes twice.

	Both report the same three things, and in this order, because each narrows the next:

	  - which size band the bytes are in. A few large blocks means a `resize` or a buffer sized from
	    a header field; a flood of small ones means a loop allocating per iteration;
	  - how many allocations, not just how many bytes - one 400 MB block and 400 000 of 1 KB look
	    the same in a working-set graph and nothing alike here;
	  - the call sites of everything at or above the reporting floor, by return address. Those
	    addresses are resolved against `vgm_app_test.pdb`, which is why this file only ever links
	    into that test.

	**Neither half changes behaviour**: sizes pass through unchanged, and the bookkeeping is a handful
	of relaxed atomics. It is not a leak detector - it counts every allocation the run ever made.
*/

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef _DEBUG

#include <windows.h>

#include <crtdbg.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

#else

#include <intrin.h>
#include <malloc.h>

#endif

namespace
{

constexpr int kBuckets = 4;
constexpr int kSlots = 48;

/** Below 4 MB an allocation is not a call site worth naming here: the interesting blocks in this
	project are chip memories and audio buffers, and anything smaller would fill the table with
	string internals before the real block arrived. */
constexpr std::size_t kSlotFloor = 4 * 1024 * 1024;

std::atomic<std::uint64_t> gBucketBytes[kBuckets] {};
std::atomic<std::uint64_t> gBucketCount[kBuckets] {};
std::atomic<std::uint64_t> gSlotBytes[kSlots] {};
std::atomic<std::uint64_t> gSlotAddr[kSlots] {};
std::atomic<int> gSlotUsed { 0 };

void record (std::size_t bytes, void* caller)
{
	const auto size = (std::uint64_t) bytes;
	const int bucket = bytes < 1024 ? 0 : bytes < 65536 ? 1 : bytes < 1048576 ? 2 : 3;

	gBucketBytes[bucket].fetch_add (size, std::memory_order_relaxed);
	gBucketCount[bucket].fetch_add (1, std::memory_order_relaxed);

	if (bytes < kSlotFloor || caller == nullptr)
	{
		return;
	}

	const auto address = (std::uint64_t) (std::uintptr_t) caller;

	for (int i = 0; i < kSlots; ++i)
	{
		if (gSlotAddr[i].load (std::memory_order_relaxed) == address)
		{
			gSlotBytes[i].fetch_add (size, std::memory_order_relaxed);
			return;
		}
	}

	const int slot = gSlotUsed.fetch_add (1, std::memory_order_relaxed);

	if (slot < kSlots)
	{
		gSlotAddr[slot].store (address, std::memory_order_relaxed);
		gSlotBytes[slot].store (size, std::memory_order_relaxed);
	}
}

void report (void)
{
#ifdef _DEBUG
	// **Set up here, not in the hook.** `SymInitialize` allocates, and the hook runs inside
	// `malloc` - doing this there would re-enter the allocator while it is already busy.
	::SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	::SymInitialize (::GetCurrentProcess (), nullptr, TRUE);
#endif

	static const char* const names[kBuckets] = { "<1K", "<64K", "<1M", ">=1M" };
	std::uint64_t total = 0;

	for (int i = 0; i < kBuckets; ++i)
	{
		const auto bytes = gBucketBytes[i].load (std::memory_order_relaxed);
		total += bytes;

		std::fprintf (stderr, "[mem] %-6s count=%-9llu total=%8.1f MB\n", names[i],
					  (unsigned long long) gBucketCount[i].load (std::memory_order_relaxed),
					  (double) bytes / (1024.0 * 1024.0));
	}

	std::fprintf (stderr, "[mem] everything allocated over the run: %.1f MB\n",
				  (double) total / (1024.0 * 1024.0));

	const int used = (gSlotUsed.load (std::memory_order_relaxed) < kSlots)
						 ? gSlotUsed.load (std::memory_order_relaxed)
						 : kSlots;

	std::fprintf (stderr, "[mem] call sites for allocations of 4 MB or more: %d\n", used);

	for (int i = 0; i < used; ++i)
	{
		const auto address = (std::uint64_t) (std::uintptr_t) gSlotAddr[i].load (std::memory_order_relaxed);

		std::fprintf (stderr, "[mem]   0x%p %8.1f MB", (void*) (std::uintptr_t) address,
					  (double) gSlotBytes[i].load (std::memory_order_relaxed) / (1024.0 * 1024.0));

#ifdef _DEBUG
		// **Resolved at exit, not inside the hook.** `SymFromAddr` allocates, and the hook is called
		// from inside `malloc` - asking for a symbol there would re-enter itself.
		char symbolBuffer[sizeof (SYMBOL_INFO) + 512] = {};
		auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolBuffer);
		symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
		symbol->MaxNameLen = 512;

		DWORD64 displacement = 0;

		if (::SymFromAddr (::GetCurrentProcess (), (DWORD64) address, &displacement, symbol))
		{
			std::fprintf (stderr, "  %s+0x%llx", symbol->Name, (unsigned long long) displacement);
		}
		else
		{
			// Module + offset, so the address is still usable by hand.
			IMAGEHLP_MODULE64 module = {};
			module.SizeOfStruct = sizeof (module);

			if (::SymGetModuleInfo64 (::GetCurrentProcess (), (DWORD64) address, &module))
			{
				std::fprintf (stderr, "  %s+0x%llx", module.ModuleName,
							  (unsigned long long) (address - module.BaseOfImage));
			}
		}
#endif

		std::fprintf (stderr, "\n");
	}
}

/** Registered at load rather than at the end of `main`, which belongs to the test harness. */
struct Reporter
{
	Reporter (void) { std::atexit (report); }
};

Reporter gReporter;

} // namespace

#ifdef _DEBUG

namespace
{

/** **The frames this hook reports sit several levels up from the allocation.** A Debug-build `new`
	goes through `operator new` → `_malloc_dbg` → `malloc` → this hook, and the first three of those
	are all CRT; the count was found by running it, not by reasoning about it - three frames landed on
	`_malloc_dbg+0x2f`, which is the one address guaranteed not to be the caller.

	Skipping to the first frame in the executable would be more robust than a fixed count, but that
	needs `SymGetModuleInfo64` inside the hook, and the hook runs inside `malloc`. The count is a
	compromise that was verified instead.
*/
int __cdecl largeAllocHook (int allocType, void*, std::size_t size, int, long,
							const unsigned char*, int)
{
	if (allocType != _HOOK_ALLOC || size < kSlotFloor)
	{
		return 1;	// let it through unchanged
	}

	void* frames[16] = {};
	const auto captured = RtlCaptureStackBackTrace (8, 16, frames, nullptr);

	record (size, captured > 0 ? frames[0] : nullptr);

	return 1;
}

struct HookInstaller
{
	HookInstaller (void)
	{
		_CrtSetDbgFlag (_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
		_CrtSetAllocHook (largeAllocHook);
	}
};

HookInstaller gHookInstaller;

} // namespace

#else

#endif

#ifndef _DEBUG

void* operator new (std::size_t bytes)
{
	record (bytes, _ReturnAddress ());

	void* pointer = std::malloc (bytes != 0 ? bytes : 1);

	if (pointer == nullptr)
	{
		throw std::bad_alloc ();
	}

	return pointer;
}

void* operator new[] (std::size_t bytes)
{
	return ::operator new (bytes);
}

void* operator new (std::size_t bytes, const std::nothrow_t&) noexcept
{
	record (bytes, _ReturnAddress ());

	return std::malloc (bytes != 0 ? bytes : 1);
}

void* operator new[] (std::size_t bytes, const std::nothrow_t&) noexcept
{
	return ::operator new (bytes, std::nothrow);
}

void* operator new (std::size_t bytes, std::align_val_t alignment)
{
	record (bytes, _ReturnAddress ());

	void* pointer = _aligned_malloc (bytes != 0 ? bytes : 1, (std::size_t) alignment);

	if (pointer == nullptr)
	{
		throw std::bad_alloc ();
	}

	return pointer;
}

void* operator new[] (std::size_t bytes, std::align_val_t alignment)
{
	return ::operator new (bytes, alignment);
}

void operator delete (void* pointer) noexcept
{
	std::free (pointer);
}

void operator delete[] (void* pointer) noexcept
{
	std::free (pointer);
}

void operator delete (void* pointer, std::size_t) noexcept
{
	std::free (pointer);
}

void operator delete[] (void* pointer, std::size_t) noexcept
{
	std::free (pointer);
}

void operator delete (void* pointer, const std::nothrow_t&) noexcept
{
	std::free (pointer);
}

void operator delete[] (void* pointer, const std::nothrow_t&) noexcept
{
	std::free (pointer);
}

/** `_aligned_malloc` and `free` are not interchangeable, which is the one place a wrong pairing here
	would crash instead of miscounting. */
void operator delete (void* pointer, std::align_val_t) noexcept
{
	_aligned_free (pointer);
}

void operator delete[] (void* pointer, std::align_val_t) noexcept
{
	_aligned_free (pointer);
}

void operator delete (void* pointer, std::size_t, std::align_val_t) noexcept
{
	_aligned_free (pointer);
}

void operator delete[] (void* pointer, std::size_t, std::align_val_t) noexcept
{
	_aligned_free (pointer);
}

#endif	// ! _DEBUG
