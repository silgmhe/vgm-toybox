#include "VisualiserView.h"

#include "SoftShadow.h"
#include "Theme.h"
#include "playback/PlaybackEngine.h"
#include "playback/WaveExport.h"

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
	constexpr int kScopeHeight = 190;
	constexpr int kPlansHeight = 150;
	constexpr int kChipsHeader = 58;
	constexpr int kRowHeight = 26;
	constexpr int kChipsPadding = 18;

	constexpr int kRefreshHz = 30;

	juce::String noteName (int note)
	{
		static const char* const names[] = { "C", "C#", "D", "D#", "E", "F",
											 "F#", "G", "G#", "A", "A#", "B" };

		if (note < 0)
		{
			return "-";
		}

		return juce::String (names[note % 12]) + juce::String (note / 12 - 1);
	}
}

VisualiserView::VisualiserView (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	// A scope has nothing to show between notes, but it must keep drawing while silence is
	// playing through - the line drifting back to zero is information too.
	startTimerHz (kRefreshHz);

	exportButton.onClick = [this] { exportWaves (); };
	exportButton.setColour (juce::TextButton::buttonColourId, theme::card ());
	exportButton.setColour (juce::TextButton::textColourOffId, theme::primaryText ());
	exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
	addAndMakeVisible (exportButton);
}

VisualiserView::~VisualiserView (void)
{
	stopTimer ();
	engine.setStateCaptureWanted (false);
}

VisualiserView::Areas VisualiserView::layoutAreas (void) const
{
	Areas areas;

	auto all = getLocalBounds ();

	areas.scope = all.removeFromTop (juce::jmin (all.getHeight (), kScopeHeight));
	all.removeFromTop (kCardGap);

	const int rows = juce::jmax (1, (int) chipState.channels.size ());
	const int wanted = kChipsHeader + rows * kRowHeight + kChipsPadding;
	const int ceiling = juce::jmax (0, all.getHeight () - kPlansHeight - kCardGap);

	areas.chips = all.removeFromTop (juce::jmin (all.getHeight (), juce::jmin (wanted, ceiling)));
	all.removeFromTop (kCardGap);
	areas.plans = all.removeFromTop (juce::jmin (all.getHeight (), kPlansHeight));

	return areas;
}

void VisualiserView::paintCardShadows (juce::Graphics& g) const
{
	const auto areas = layoutAreas ();

	drawCardShadow (g, areas.scope.toFloat ());
	drawCardShadow (g, areas.chips.toFloat ());
	drawCardShadow (g, areas.plans.toFloat ());
}

void VisualiserView::resized (void)
{
	// The button lives in the chip card's header, which moves when the channel count changes -
	// so its bounds are recomputed with everything else rather than once.
	const auto areas = layoutAreas ();
	auto header = areas.chips.reduced (kInset, 0).withHeight (kChipsHeader);

	exportButton.setBounds (header.removeFromTop (26).removeFromRight (112).withTrimmedTop (12).reduced (0, 1));

	repaint ();
}

void VisualiserView::refreshLanguage (void)
{
	exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
	repaint ();
}

void VisualiserView::exportWaves (void)
{
	int writable = 0;

	for (const auto& channel : chipState.channels)
	{
		if (channel.wave.size () >= 2)
		{
			++writable;
		}
	}

	if (writable == 0)
	{
		exportStatus = toybox::i18n::tr ("visualiser.exportEmpty");
		repaint ();
		return;
	}

	waveChooser = std::make_unique<juce::FileChooser> (toybox::i18n::tr ("visualiser.export"),
													   juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
													   juce::String (), true);

	waveChooser->launchAsync (juce::FileBrowserComponent::openMode
							  | juce::FileBrowserComponent::canSelectDirectories,
							  [this] (const juce::FileChooser& chooser)
	{
		const auto folder = chooser.getResult ();

		if (folder == juce::File () || ! folder.isDirectory ())
		{
			return;
		}

		int written = 0;

		for (std::size_t i = 0; i < chipState.channels.size (); ++i)
		{
			const auto& channel = chipState.channels[i];

			if (channel.wave.size () < 2)
			{
				continue;
			}

			const auto name = juce::File::createLegalFileName (juce::String (channel.name)
															   + " " + juce::String ((int) i + 1))
							  + ".wav";

			if (playback::writeCycleWav (channel, folder.getChildFile (name).getFullPathName ().toStdString ()))
			{
				++written;
			}
		}

		exportStatus = juce::String (written) + " WAV";
		repaint ();
	});
}

void VisualiserView::timerCallback (void)
{
	// A page that is not on screen still ticks otherwise, and repainting an invisible component
	// costs the same as repainting a visible one. The capture is told to stop with it: an engine
	// that keeps building snapshots for nobody is exactly the kind of cost that shows up later
	// as an unexplained load.
	if (! isShowing ())
	{
		engine.setStateCaptureWanted (false);
		return;
	}

	engine.setStateCaptureWanted (true);

	if (engine.captureChipState (chipState))
	{
		const int rows = (int) chipState.channels.size ();

		// The card grows with the channel list, so the layout is rebuilt when that changes - not
		// on every frame, which is what this used to do.
		if (rows != lastChannelCount)
		{
			lastChannelCount = rows;
			resized ();
		}
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

void VisualiserView::paintChips (juce::Graphics& g, juce::Rectangle<int> area)
{
	if (area.getHeight () < kChipsHeader)
	{
		return;
	}

	auto inner = area.reduced (kInset, 0);

	g.setColour (theme::primaryText ());
	g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
	auto titleRow = inner.removeFromTop (26).withTrimmedTop (12);
	g.drawText (toybox::i18n::tr ("visualiser.chips"), titleRow,
				juce::Justification::centredLeft, false);

	if (exportStatus.isNotEmpty ())
	{
		g.setFont (juce::Font (juce::FontOptions (11.5f)));
		g.drawText (exportStatus, titleRow.withTrimmedRight (120),
					juce::Justification::centredRight, false);
	}

	if (chipState.channels.empty ())
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawText (toybox::i18n::tr ("visualiser.chipsUnavailable"),
					inner.removeFromTop (24), juce::Justification::centredLeft, false);
		return;
	}

	inner.removeFromTop (8);

	for (const auto& channel : chipState.channels)
	{
		auto row = inner.removeFromTop (kRowHeight);

		if (row.getY () + row.getHeight () > area.getBottom ())
		{
			break;
		}

		g.setColour (channel.active ? theme::primaryText () : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.0f)));
		g.drawText (juce::String (channel.name), row.removeFromLeft (66), juce::Justification::centredLeft, false);

		g.setColour (theme::secondaryText ());
		g.drawText (noteName (channel.note), row.removeFromLeft (44), juce::Justification::centredLeft, false);

		// volume as a bar: a number changes too slowly to read at a glance
		auto bar = row.removeFromLeft (64).reduced (2, 9);
		g.setColour (theme::cardBorder ());
		g.fillRoundedRectangle (bar.toFloat (), 2.0f);
		g.setColour (theme::accent ().withAlpha (0.85f));
		g.fillRoundedRectangle (bar.toFloat ().withWidth (bar.getWidth () * juce::jlimit (0.0f, 1.0f, channel.volume)),
								2.0f);

		row.removeFromLeft (10);

		// the waveform, one period of whatever the chip is reading
		if (channel.wave.size () >= 2 && row.getWidth () > 8)
		{
			juce::Path wave;
			const auto count = (int) channel.wave.size ();
			const float half = (float) row.getHeight () * 0.5f - 3.0f;
			const float span = (float) row.getWidth () / (float) count;

			// Stepped, not interpolated. A chip channel holds each sample for a full period and
			// then jumps; joining the points with straight lines would draw a ramp the hardware
			// never produced - which matters here, because the shape is the thing being shown.
			for (int i = 0; i < count; ++i)
			{
				const float level = juce::jlimit (-1.0f, 1.0f, channel.wave[(std::size_t) i]);
				const float y = (float) row.getCentreY () - level * half;
				const float x0 = (float) row.getX () + span * (float) i;
				const float x1 = x0 + span;

				if (i == 0)
				{
					wave.startNewSubPath (x0, y);
				}
				else
				{
					wave.lineTo (x0, y);		// the vertical edge between levels
				}

				wave.lineTo (x1, y);			// the hold
			}

			g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.35f));
			g.strokePath (wave, juce::PathStrokeType (1.1f));
		}
	}
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

	const auto areas = layoutAreas ();

	// --- the scope ---
	drawCard (g, areas.scope);

	auto scopeInner = areas.scope.reduced (kInset, 0);
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

	// --- what the chips are doing ---
	drawCard (g, areas.chips);
	paintChips (g, areas.chips);

	// --- what is still to come ---
	drawCard (g, areas.plans);

	auto inner = areas.plans.reduced (kInset, 0);
	auto text = inner.withTrimmedTop (14);

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
		auto row = text.removeFromTop (22);

		// a small accent dot instead of a bullet character: it survives every platform font
		g.setColour (theme::accent ());
		g.fillEllipse (juce::Rectangle<float> ((float) row.getX () + 3.0f,
											   (float) row.getCentreY () - 2.5f, 5.0f, 5.0f));
		row.removeFromLeft (16);

		g.setColour (theme::primaryText ());
		g.drawText (toybox::i18n::tr (key), row, juce::Justification::centredLeft, false);
	}
}
