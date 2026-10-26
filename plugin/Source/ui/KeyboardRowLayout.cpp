#include "KeyboardRowLayout.h"

namespace toybox::ui::keyboard
{

Layout layoutRows (const std::vector<std::string>& chipNames,
				   int width,
				   int scrollOffset,
				   int top,
				   const RowMetrics& metrics)
{
	Layout layout;

	if (chipNames.empty ())
	{
		return layout;
	}

	const int cardX = metrics.cardInset;
	const int cardWidth = juce::jmax (200, width - 2 * metrics.cardInset - metrics.reservedRight);

	int y = top - scrollOffset;
	juce::String group;

	for (std::size_t i = 0; i < chipNames.size (); ++i)
	{
		const auto chip = juce::String (chipNames[i]);

		if (chip != group)
		{
			group = chip;

			Row header;
			header.header = true;
			header.title = chip;

			// How many channels this chip has. Counted here, where the run of names is already being
			// walked, rather than once per frame in paint(): the answer cannot change until the song
			// does, and the panel was rebuilding a juce::String per comparison to get it.
			for (std::size_t j = i; j < chipNames.size () && chipNames[j] == chipNames[i]; ++j)
			{
				++header.chipCount;
			}

			header.area = { cardX, y, cardWidth, metrics.groupHeader };
			layout.rows.push_back (header);

			y += metrics.groupHeader + metrics.afterHeader;
		}

		Row row;
		row.channel = (int) i;
		row.area = { cardX, y, cardWidth, metrics.rowHeight };

		// Inside the card: the columns are inset, and the keyboard is the last thing between them.
		auto columns = row.area.reduced (metrics.rowInsetLeft, 0);
		columns.removeFromRight (metrics.rowInsetRight);

		row.name = columns.removeFromLeft (metrics.nameWidth);

		columns.removeFromLeft (metrics.nameToButton);
		row.mute = columns.removeFromLeft (metrics.buttonSize)
						  .withSizeKeepingCentre (metrics.buttonSize, metrics.buttonSize);
		columns.removeFromLeft (metrics.buttonGap);
		row.solo = columns.removeFromLeft (metrics.buttonSize)
						  .withSizeKeepingCentre (metrics.buttonSize, metrics.buttonSize);

		columns.removeFromLeft (metrics.buttonToNote);
		row.note = columns.removeFromLeft (metrics.noteWidth);
		columns.removeFromLeft (metrics.noteToWave);

		row.wave = columns.removeFromLeft (metrics.waveWidth)
						  .withSizeKeepingCentre (metrics.waveWidth, metrics.waveHeight);
		columns.removeFromLeft (metrics.waveToKeys);

		row.meter = columns.removeFromRight (metrics.meterWidth)
						   .withSizeKeepingCentre (metrics.meterWidth, metrics.meterHeight);
		columns.removeFromRight (metrics.meterGap);

		// The keyboard is centred in what is left rather than filling it: one that reached the
		// row's edges stuck out of the card on both sides.
		row.keys = columns.withSizeKeepingCentre (columns.getWidth (), metrics.keyHeight);

		layout.rows.push_back (row);

		y += metrics.rowHeight + metrics.rowGap;

		// A gap between groups, once the next channel belongs to another chip.
		const bool lastOfGroup = (i + 1 >= chipNames.size ()) || chipNames[i + 1] != chipNames[i];

		if (lastOfGroup)
		{
			y += metrics.groupGap;
		}
	}

	// Measured with the scroll offset taken back out: a content height that shrank as the view
	// scrolled made the scroll range collapse under the user's wheel.
	layout.contentHeight = y + scrollOffset - top;

	return layout;
}

int clampScroll (int scrollOffset, int contentHeight, int viewportHeight)
{
	return juce::jlimit (0, juce::jmax (0, contentHeight - viewportHeight), scrollOffset);
}

} // namespace toybox::ui::keyboard
