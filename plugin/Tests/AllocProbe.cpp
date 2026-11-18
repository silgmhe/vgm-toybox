/**
	**A counting `operator new`, for asking where memory goes without a debugger.**

	Written because the question was "1.2 GB appeared in three seconds of playback" and every answer
	cost either a GUI session or a toolchain install. This is neither: it replaces the global allocator
	with one that records and forwards, and prints a summary at exit.

	Three things it answers, in the order they narrow the search:

	  - which size band the bytes are in. A few large blocks means a `resize` or a buffer sized from a
	    header field; a flood of small ones means a loop allocating per iteration, and only that case
	    needs call stacks;
	  - how many allocations, not just how many bytes - one 400 MB block and 400 000 of 1 KB look the
	    same in a working-set graph and nothing alike here;
	  - the call sites of everything at or above 64 KB, by return address. Those addresses are resolved
	    against `vgm_app_test.pdb`, which is why this file only ever links into that test.

	**It changes no behaviour**: sizes pass through `malloc` unchanged, and the bookkeeping is a handful
	of relaxed atomics. It is not a leak detector either - it counts every allocation the run ever
	made, which is the number that was missing.
*/

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <intrin.h>
#include <malloc.h>

namespace
{

constexpr int kBuckets = 4;
constexpr int kSlots = 48;

/** Below 64 KB an allocation is not a call site worth naming - there would be thousands of them,
	and the slot table would fill with `std::string` internals before the real block arrived. */
constexpr std::size_t kSlotFloor = 65536;

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

	if (bytes < kSlotFloor)
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

	std::fprintf (stderr, "[mem] call sites for allocations of 64 KB or more: %d\n", used);

	for (int i = 0; i < used; ++i)
	{
		std::fprintf (stderr, "[mem]   0x%p %8.1f MB\n",
					  (void*) (std::uintptr_t) gSlotAddr[i].load (std::memory_order_relaxed),
					  (double) gSlotBytes[i].load (std::memory_order_relaxed) / (1024.0 * 1024.0));
	}
}

/** Registered at load rather than at the end of `main`, which belongs to the test harness. */
struct Reporter
{
	Reporter (void) { std::atexit (report); }
};

Reporter gReporter;

} // namespace

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
