#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace toybox::transport
{

/** The loop counts the transport panel and the tray menu offer, in display order.
	0 means "loop forever"; the other values are finite repeat counts. */
struct LoopPreset
{
	int count;
	const char* labelKey;
};

inline constexpr std::array<LoopPreset, 4> loopPresets {{
	{ 1, "transport.loop.once" },
	{ 2, "transport.loop.twice" },
	{ 3, "transport.loop.thrice" },
	{ 0, "transport.loop.forever" },
}};

/** The ending-fade times the transport panel and the tray menu offer, in display order. */
struct FadePreset
{
	double seconds;
	const char* labelKey;
};

inline constexpr std::array<FadePreset, 5> fadePresets {{
	{ 0.0, "transport.fade.none" },
	{ 1.0, "transport.fade.1s" },
	{ 3.0, "transport.fade.3s" },
	{ 5.0, "transport.fade.5s" },
	{ 10.0, "transport.fade.10s" },
}};

/** The row that represents @p count. Every value maps to exactly one row: an out-of-range
	count from an old session must not leave the combo box or the menu with nothing ticked.
	Negative counts are treated as "loop forever", which is what the libvgm backend does. */
inline int loopPresetIndex (int count)
{
	if (count <= 0)
	{
		return 3;
	}

	if (count == 1)
	{
		return 0;
	}

	if (count == 2)
	{
		return 1;
	}

	return 2;
}

/** The row nearest to @p seconds; ties choose the later row. This is the one rule the panel and
	the tray share for non-preset values, rather than a second threshold table. NaN and infinity
	map to "No fade" rather than to an arbitrary row. */
inline int fadePresetIndex (double seconds)
{
	if (! std::isfinite (seconds))
	{
		return 0;
	}

	int best = 0;
	double bestDistance = std::abs (seconds - fadePresets[0].seconds);

	for (std::size_t i = 1; i < fadePresets.size(); ++i)
	{
		const double distance = std::abs (seconds - fadePresets[i].seconds);

		if (distance <= bestDistance)
		{
			best = static_cast<int> (i);
			bestDistance = distance;
		}
	}

	return best;
}

} // namespace toybox::transport
