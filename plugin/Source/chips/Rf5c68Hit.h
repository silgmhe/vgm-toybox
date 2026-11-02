#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace toybox::chips
{

/** Where one hit of an RF5C68/164 sample ends.

	That question has no answer inside the format, so it is worth saying why this one is picked.

	The chip plays whatever the driver put in a sixteen-bit address space. On `01 - Palmtree Panic
	Zone Past` the driver gives six channels six starts, eight kilobytes apart, and then triggers
	those slots **hundreds of times each at wildly different pitches** (one address is hit 299
	times with steps from 619 to 2503). So a slot is not an instrument and not a loop: it is a
	*place* where many hits were placed, back to back.

	The only edge the data offers is quiet. A hit's attack is loud, its tail decays, and the gap
	before the next hit is quieter than either - there is no `0xFF` between them, because on this
	chip `0xFF` only marks the top of RAM. So: find the loudest window, and cut at the first window
	that falls below a sixteenth of it (about -24 dB).

	Both readers call this - the export and `channelState` - because a row that draws a different
	region than the file it exports is the bug this branch has already paid for twice.
*/
template <typename ByteAt>
std::size_t rf5c68HitLength (std::uint32_t address, std::size_t span, ByteAt byteAt)
{
	constexpr std::size_t kWindow = 128;
	constexpr std::size_t kShortest = kWindow * 4;

	if (span <= kShortest)
	{
		return span;
	}

	std::size_t loudest = 0;
	std::vector<std::size_t> energy;
	energy.reserve (span / kWindow);

	for (std::size_t offset = 0; offset + kWindow <= span; offset += kWindow)
	{
		std::size_t sum = 0;

		for (std::size_t k = 0; k < kWindow; ++k)
		{
			// Sign and magnitude: the low seven bits are the distance from zero, and bit 7 is
			// only the direction. Using the raw byte would weight the two halves differently.
			const int magnitude = (int) (byteAt (address + (std::uint32_t) (offset + k)) & 0x7F);
			sum += (std::size_t) (magnitude * magnitude);
		}

		energy.push_back (sum);
		loudest = (std::max) (loudest, sum);
	}

	// Silence, not merely "quieter": a window whose every byte has magnitude 0..1. A relative
	// threshold was tried first and cut every slot at the same floor - the attack of these samples
	// is long enough that a sixteenth of the peak lands inside it, which measures nothing.
	constexpr std::size_t kSilenceEnergy = kWindow * 4;	// magnitude ≤ 2 throughout

	for (std::size_t i = 2; i < energy.size (); ++i)
	{
		if (energy[i] <= kSilenceEnergy)
		{
			return (std::max) (kShortest, i * kWindow);
		}
	}

	// No silence anywhere: the slot is one continuous sound, so it is the whole slot.
	return span;
}

} // namespace toybox::chips
