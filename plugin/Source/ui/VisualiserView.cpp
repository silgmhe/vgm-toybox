#include "VisualiserView.h"

#include "SoftShadow.h"
#include "Theme.h"
#include "playback/PlaybackEngine.h"

#include <cmath>

using namespace toybox::ui;

namespace
{
	// What this page is for, in the order a player meets it. The list is a signpost, not a
	// feature: every line of it disappears as the panel behind it arrives. The keys are stable;
	// the text is looked up when the page paints, so a language switch needs only a repaint.
	const char* const kPlannedPanelKeys[] =
	{
		"visualiser.item.spectrum",
		"visualiser.item.midi",
		"visualiser.item.chip",
	};

	constexpr int kInset = 20;
	constexpr int kCardGap = 12;
	constexpr int kScopeHeight = 210;
	constexpr int kPlansHeader = 62;
	constexpr int kLineHeight = 27;
	constexpr int kPlansFooter = 30;

	constexpr int kRefreshHz = 30;
}

VisualiserView::VisualiserView (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	// A scope has nothing to show between notes, but it must keep drawing while silence is
	// playing through - the line drifting back to zero is information too.
	startTimerHz (kRefreshHz);
}

VisualiserView::~VisualiserView (void)
{
	stopTimer ();
}

juce::Rectangle<int> VisualiserView::scopeBounds (void) const
{
	return getLocalBounds().removeFromTop (kScopeHeight);
}

juce::Rectangle<int> VisualiserView::plansBounds (void) const
{
	const int wanted = kPlansHeader + kLineHeight * (int) juce::numElementsInArray (kPlannedPanelKeys)
					   + kPlansFooter;

	auto all = getLocalBounds();
	all.removeFromTop (kScopeHeight + kCardGap);

	return all.removeFromTop (juce::jmin (all.getHeight (), wanted));
}

void VisualiserView::paintCardShadows (juce::Graphics& g) const
{
	drawCardShadow (g, scopeBounds().toFloat());
	drawCardShadow (g, plansBounds().toFloat());
}

void VisualiserView::resized (void)
{
	repaint ();
}

void VisualiserView::timerCallback (void)
{
	// A page that is not on screen still ticks otherwise, and repainting an invisible component
	// costs the same as repainting a visible one.
	if (! isShowing ())
	{
		return;
	}

	repaint ();
}

void VisualiserView::paintScope (juce::Graphics& g, juce::Rectangle<int> area)
{
	if (area.getWidth () < 8 || area.getHeight () < 8)
	{
		return;
	}

	// Two samples per pixel is enough for the shape of a waveform and keeps the path short
	// enough to redraw thirty times a second on a single core.
	scopeSamples.resize ((std::size_t) juce::jmax (2, area.getWidth () * 2));

	const int count = engine.readScope (scopeSamples.data (), (int) scopeSamples.size ());

	float peak = 0.0f;
	double sum = 0.0;

	for (int i = 0; i < count; ++i)
	{
		const float value = scopeSamples[(std::size_t) i];
		peak = juce::jmax (peak, std::fabs (value));
		sum += (double) value * (double) value;
	}

	scopePeak = peak;
	scopeRms = count > 0 ? (float) std::sqrt (sum / (double) count) : 0.0f;

	// the centre line, so silence is visibly a line and not an empty box
	g.setColour (theme::cardBorder ().withAlpha (0.5f));
	g.drawHorizontalLine (area.getCentreY (), (float) area.getX (), (float) area.getRight ());

	if (count < 2)
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawText (toybox::i18n::tr ("visualiser.scopeIdle"), area, juce::Justification::centred, false);
		return;
	}

	const float half = (float) area.getHeight () * 0.5f - 4.0f;
	juce::Path path;

	for (int i = 0; i < count; ++i)
	{
		const float x = (float) area.getX () + (float) i / (float) (count - 1) * (float) area.getWidth ();
		const float y = (float) area.getCentreY ()
						- juce::jlimit (-1.0f, 1.0f, scopeSamples[(std::size_t) i]) * half;

		if (i == 0)
		{
			path.startNewSubPath (x, y);
		}
		else
		{
			path.lineTo (x, y);
		}
	}

	g.setColour (theme::accent ());
	g.strokePath (path, juce::PathStrokeType (1.2f));

	// peak and rms, because a single line at this zoom cannot tell a quiet passage from a loud one
	g.setColour (theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (11.5f)));
	g.drawText ("peak " + juce::String (scopePeak, 3) + "   rms " + juce::String (scopeRms, 3),
				area.removeFromTop (16), juce::Justification::topRight, false);
}

void VisualiserView::paint (juce::Graphics& g)
{
	const auto drawCard = [] (juce::Graphics& target, juce::Rectangle<int> bounds)
	{
		target.setColour (theme::card ());
		target.fillRoundedRectangle (bounds.toFloat (), theme::cardRadius);
		target.setColour (theme::cardBorder ());
		target.drawRoundedRectangle (bounds.toFloat ().reduced (0.5f), theme::cardRadius, 1.0f);
	};

	// --- the scope ---
	const auto scope = scopeBounds ();
	drawCard (g, scope);

	auto scopeInner = scope.reduced (kInset, 0);
	auto scopeText = scopeInner.withTrimmedTop (14);

	g.setColour (theme::primaryText ());
	g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
	g.drawText (toybox::i18n::tr ("visualiser.scope"), scopeText.removeFromTop (22),
				juce::Justification::centredLeft, false);

	g.setColour (theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.5f)));
	g.drawText (toybox::i18n::tr ("visualiser.scopeHint"), scopeText.removeFromTop (20),
				juce::Justification::centredLeft, false);

	paintScope (g, scopeInner.withTrimmedTop (58).withTrimmedBottom (12));

	// --- what is still to come ---
	const auto plans = plansBounds ();
	drawCard (g, plans);

	auto inner = plans.reduced (kInset, 0);
	auto text = inner.withTrimmedTop (16);

	g.setColour (theme::primaryText ());
	g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
	g.drawText (toybox::i18n::tr ("visualiser.title"), text.removeFromTop (22),
				juce::Justification::centredLeft, false);

	g.setColour (theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.5f)));
	g.drawText (toybox::i18n::tr ("visualiser.subtitle"), text.removeFromTop (20),
				juce::Justification::centredLeft, false);

	for (const auto* key : kPlannedPanelKeys)
	{
		auto row = text.removeFromTop (kLineHeight);

		// a small accent dot instead of a bullet character: it survives every platform font
		g.setColour (theme::accent ());
		g.fillEllipse (juce::Rectangle<float> ((float) row.getX () + 3.0f,
											   (float) row.getCentreY () - 2.5f, 5.0f, 5.0f));
		row.removeFromLeft (16);

		g.setColour (theme::primaryText ());
		g.drawText (toybox::i18n::tr (key), row, juce::Justification::centredLeft, false);
	}

	g.setColour (theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.0f)));
	g.drawText (toybox::i18n::tr ("visualiser.footer"), text.removeFromTop (kPlansFooter),
				juce::Justification::centredLeft, false);
}
