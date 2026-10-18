#include "VisualiserView.h"

#include "SoftShadow.h"
#include "Theme.h"

using namespace toybox::ui;

namespace
{
	// What this page is for, in the order a player meets it. The list is a signpost, not a
	// feature: every line of it disappears as the panel behind it arrives. The keys are stable;
	// the text is looked up when the page paints, so a language switch needs only a repaint.
	const char* const kPlannedPanelKeys[] =
	{
		"visualiser.item.waveform",
		"visualiser.item.spectrum",
		"visualiser.item.midi",
		"visualiser.item.chip",
	};

	constexpr int kHeaderHeight = 62;
	constexpr int kLineHeight = 27;
	constexpr int kFooterHeight = 30;
	constexpr int kInset = 20;
}

juce::Rectangle<int> VisualiserView::cardBounds (void) const
{
	const int wanted = kHeaderHeight + kLineHeight * (int) juce::numElementsInArray (kPlannedPanelKeys) + kFooterHeight;
	return getLocalBounds().removeFromTop (juce::jmin (getHeight(), wanted));
}

void VisualiserView::paintCardShadows (juce::Graphics& g) const
{
	drawCardShadow (g, cardBounds().toFloat());
}

void VisualiserView::paint (juce::Graphics& g)
{
	const auto card = cardBounds();

	// the same card the rest of the window uses; its shadow is painted by the window
	// (View::paintCardShadows), because it reaches outside this page's bounds
	g.setColour (theme::card());
	g.fillRoundedRectangle (card.toFloat(), theme::cardRadius);
	g.setColour (theme::cardBorder());
	g.drawRoundedRectangle (card.toFloat().reduced (0.5f), theme::cardRadius, 1.0f);

	auto inner = card.reduced (kInset, 0);
	auto text = inner.withTrimmedTop (16);

	g.setColour (theme::primaryText());
	g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
	g.drawText (toybox::i18n::tr ("visualiser.title"), text.removeFromTop (22),
				juce::Justification::centredLeft, false);

	g.setColour (theme::secondaryText());
	g.setFont (juce::Font (juce::FontOptions (12.5f)));
	g.drawText (toybox::i18n::tr ("visualiser.subtitle"),
				text.removeFromTop (20), juce::Justification::centredLeft, false);

	g.setFont (juce::Font (juce::FontOptions (12.5f)));

	for (const auto* key : kPlannedPanelKeys)
	{
		auto row = text.removeFromTop (kLineHeight);

		// a small accent dot instead of a bullet character: it survives every platform font
		g.setColour (theme::accent());
		g.fillEllipse (juce::Rectangle<float> ((float) row.getX() + 3.0f,
											   (float) row.getCentreY() - 2.5f, 5.0f, 5.0f));
		row.removeFromLeft (16);

		g.setColour (theme::primaryText());
		g.drawText (toybox::i18n::tr (key), row, juce::Justification::centredLeft, false);
	}

	g.setColour (theme::secondaryText());
	g.setFont (juce::Font (juce::FontOptions (12.0f)));
	g.drawText (toybox::i18n::tr ("visualiser.footer"),
				text.removeFromTop (kFooterHeight), juce::Justification::centredLeft, false);
}
