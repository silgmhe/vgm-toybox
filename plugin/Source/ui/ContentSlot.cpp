#include "ContentSlot.h"

#include "MixerPanel.h"
#include "Theme.h"
#include "i18n/LanguageManager.h"
#include "playback/PlaybackEngine.h"
#include "playback/WaveExport.h"

#include <cmath>
#include <functional>

using namespace toybox::ui;

namespace
{
	constexpr int kInset = 20;
	constexpr int kHeader = 40;
	constexpr int kKeyboardHeight = 62;
	constexpr int kRowHeight = 30;
	constexpr int kRefreshHz = 30;

	// C1 to B5: five octaves cover every chip in the corpus, and a wider range would make the
	// keys too narrow to read at this height.
	constexpr int kFirstNote = 24;
	constexpr int kOctaves = 5;

	// Where each pitch class sits among the white keys; -1 for the ones that are black.
	constexpr int kWhiteIndex[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

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

	/** One key per semitone, white behind black, with the notes being played lit. */
	void drawKeyboard (juce::Graphics& g, juce::Rectangle<int> area,
					   const toybox::playback::ChipStateSnapshot& snapshot)
	{
		if (area.getWidth () < 40 || area.getHeight () < 10)
		{
			return;
		}

		const float whiteWidth = (float) area.getWidth () / (float) (kOctaves * 7);
		const float blackWidth = whiteWidth * 0.62f;
		const float blackHeight = (float) area.getHeight () * 0.6f;

		const auto keyAt = [&] (int note, float& x, float& width, bool& isBlack)
		{
			const int offset = note - kFirstNote;

			if (offset < 0 || offset >= kOctaves * 12)
			{
				return false;
			}

			const int octave = offset / 12;
			const int pitchClass = offset % 12;

			if (kWhiteIndex[pitchClass] >= 0)
			{
				x = (float) area.getX () + whiteWidth * (float) (octave * 7 + kWhiteIndex[pitchClass]);
				width = whiteWidth;
				isBlack = false;
			}
			else
			{
				const int leftWhite = kWhiteIndex[pitchClass - 1];
				x = (float) area.getX () + whiteWidth * (float) (octave * 7 + leftWhite + 1) - blackWidth * 0.5f;
				width = blackWidth;
				isBlack = true;
			}

			return true;
		};

		// which semitones are sounding, and how many channels share them
		int lit[128] = {};

		for (const auto& channel : snapshot.channels)
		{
			if (channel.active && channel.note >= 0 && channel.note < 128)
			{
				++lit[channel.note];
			}
		}

		const auto paintKey = [&] (int note, bool isBlack)
		{
			float x = 0.0f, width = 0.0f;
			bool black = false;

			if (! keyAt (note, x, width, black) || black != isBlack)
			{
				return;
			}

			const float height = isBlack ? blackHeight : (float) area.getHeight ();
			const juce::Rectangle<float> key (x + 0.5f, (float) area.getY (), width - 1.0f, height);

			if (lit[note] > 0)
			{
				g.setColour (theme::accent ().withAlpha (isBlack ? 0.95f : 0.85f));
			}
			else
			{
				g.setColour (isBlack ? theme::primaryText ().withAlpha (0.55f)
									 : theme::secondaryText ().withAlpha (0.14f));
			}

			g.fillRoundedRectangle (key, 2.0f);

			if (! isBlack && lit[note] > 1)
			{
				// two channels on one key: a second tick, because a shared note is information
				g.setColour (theme::onAccent ());
				g.fillEllipse (key.getCentreX () - 2.0f, key.getBottom () - 8.0f, 4.0f, 4.0f);
			}
		};

		for (int octave = 0; octave < kOctaves; ++octave)
		{
			for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
			{
				if (kWhiteIndex[pitchClass] >= 0)
				{
					paintKey (kFirstNote + octave * 12 + pitchClass, false);
				}
			}
		}

		for (int octave = 0; octave < kOctaves; ++octave)
		{
			for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
			{
				if (kWhiteIndex[pitchClass] < 0)
				{
					paintKey (kFirstNote + octave * 12 + pitchClass, true);
				}
			}
		}
	}
}

//==============================================================================
/** The chip tab of the old page, minus the page: keyboard, rows, and the export button. */
class ContentSlot::ChipStatePanel : public juce::Component
{
public:
	ChipStatePanel (juce::String& statusToUse)
		: exportStatus (statusToUse)
	{
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		exportButton.setColour (juce::TextButton::buttonColourId, theme::card ());
		exportButton.setColour (juce::TextButton::textColourOffId, theme::primaryText ());
		exportButton.onClick = [this] { if (onExport) { onExport (); } };
		addAndMakeVisible (exportButton);
	}

	void setSnapshot (const toybox::playback::ChipStateSnapshot& snapshotToUse)
	{
		snapshot = snapshotToUse;
	}

	void refreshLanguage (void)
	{
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		repaint ();
	}

	void paint (juce::Graphics& g) override
	{
		auto area = getLocalBounds ().reduced (kInset, 0);
		auto header = area.removeFromTop (kHeader);

		g.setColour (theme::primaryText ());
		g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
		g.drawText (toybox::i18n::tr ("visualiser.chips"), header.removeFromLeft (200).withTrimmedTop (10),
					juce::Justification::centredLeft, false);

		if (exportStatus.isNotEmpty ())
		{
			g.setColour (theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (11.5f)));
			g.drawText (exportStatus, header.withTrimmedRight (124),
						juce::Justification::centredRight, false);
		}

		if (snapshot.channels.empty ())
		{
			g.setColour (theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (13.0f)));
			g.drawText (toybox::i18n::tr ("visualiser.chipsUnavailable"),
						area.withTrimmedTop (30), juce::Justification::centredTop, false);
			return;
		}

		drawKeyboard (g, area.removeFromTop (kKeyboardHeight), snapshot);
		area.removeFromTop (12);

		for (const auto& channel : snapshot.channels)
		{
			auto row = area.removeFromTop (kRowHeight);

			if (row.getY () + row.getHeight () > getLocalBounds ().getBottom ())
			{
				break;
			}

			g.setColour (channel.active ? theme::primaryText () : theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (12.0f)));
			g.drawText (juce::String (channel.name), row.removeFromLeft (70),
						juce::Justification::centredLeft, false);

			g.setColour (theme::secondaryText ());
			g.drawText (noteName (channel.note), row.removeFromLeft (48), juce::Justification::centredLeft, false);

			// level as a bar: a number changes too slowly to read at a glance
			auto bar = row.removeFromLeft (70).reduced (2, 11);
			g.setColour (theme::cardBorder ());
			g.fillRoundedRectangle (bar.toFloat (), 2.0f);
			g.setColour (theme::accent ().withAlpha (0.85f));
			g.fillRoundedRectangle (bar.toFloat ().withWidth (bar.getWidth () * juce::jlimit (0.0f, 1.0f, channel.volume)),
									2.0f);

			row.removeFromLeft (12);

			if (channel.wave.size () < 2 || row.getWidth () < 8)
			{
				continue;
			}

			// Stepped, not interpolated. A chip channel holds each sample for a full period and
			// then jumps; joining the points with straight lines would draw a ramp the hardware
			// never produced - which matters here, because the shape is the thing being shown.
			juce::Path wave;
			const auto count = (int) channel.wave.size ();
			const float half = (float) row.getHeight () * 0.5f - 4.0f;
			const float span = (float) row.getWidth () / (float) count;

			for (int i = 0; i < count; ++i)
			{
				const float level = juce::jlimit (-1.0f, 1.0f, channel.wave[(std::size_t) i]);
				const float y = (float) row.getCentreY () - level * half;
				const float x0 = (float) row.getX () + span * (float) i;

				if (i == 0)
				{
					wave.startNewSubPath (x0, y);
				}
				else
				{
					wave.lineTo (x0, y);		// the vertical edge between levels
				}

				wave.lineTo (x0 + span, y);		// the hold
			}

			g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.3f));
			g.strokePath (wave, juce::PathStrokeType (1.1f));
		}
	}

	void resized (void) override
	{
		exportButton.setBounds (getWidth () - kInset - 112, 8, 112, 24);
	}

	std::function<void ()> onExport;

private:
	playback::ChipStateSnapshot snapshot;
	juce::String& exportStatus;
	juce::TextButton exportButton { "export" };
};

//==============================================================================
class ContentSlot::ScopePanel : public juce::Component
{
public:
	void paint (juce::Graphics& g) override
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawText (toybox::i18n::tr ("visualiser.scopePlaceholder"), getLocalBounds (),
					juce::Justification::centred, false);
	}
};

//==============================================================================
ContentSlot::ContentSlot (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	mixer = std::make_unique<MixerPanel> (engine);

	chipStatePanel = std::make_unique<ChipStatePanel> (exportStatus);
	chipStatePanel->onExport = [this]
	{
		// The export lives with the panel that owns the waves, but the file chooser is the
		// slot's: one chooser per page rather than one per view.
		const int writable = (int) chipState.channels.size ();

		if (writable == 0)
		{
			exportStatus = toybox::i18n::tr ("visualiser.exportEmpty");
			chipStatePanel->repaint ();
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
			chipStatePanel->repaint ();
		});
	};

	scopePanel = std::make_unique<ScopePanel> ();

	addAndMakeVisible (*mixer);
	addChildComponent (*chipStatePanel);
	addChildComponent (*scopePanel);

	startTimerHz (kRefreshHz);
}

ContentSlot::~ContentSlot (void)
{
	stopTimer ();
	engine.setStateCaptureWanted (false);
}

void ContentSlot::setView (ContentView view)
{
	if (view == current && (mixer->isVisible () || chipStatePanel->isVisible () || scopePanel->isVisible ()))
	{
		return;
	}

	current = view;

	mixer->setVisible (view == ContentView::mixer);
	chipStatePanel->setVisible (view == ContentView::chipState);
	scopePanel->setVisible (view == ContentView::oscilloscope);

	// The capture is only worth running for the panel that is showing.
	engine.setStateCaptureWanted (view == ContentView::chipState);

	if (view == ContentView::chipState)
	{
		lastChannelCount = -1;		// force a relayout of the panel on the next tick
	}

	resized ();
	repaint ();
}

void ContentSlot::refresh (void)
{
	if (mixer != nullptr)
	{
		mixer->refresh ();
	}
}

void ContentSlot::refreshLanguage (void)
{
	if (mixer != nullptr) { mixer->refreshLanguage (); }
	if (chipStatePanel != nullptr) { chipStatePanel->refreshLanguage (); }
	if (scopePanel != nullptr) { scopePanel->repaint (); }
}

void ContentSlot::songChanged (void)
{
	if (mixer != nullptr) { mixer->refresh (); }

	chipState.channels.clear ();
	chipState.valid = false;
	lastChannelCount = -1;
	exportStatus.clear ();

	if (chipStatePanel != nullptr)
	{
		chipStatePanel->repaint ();
	}
}

void ContentSlot::paint (juce::Graphics& g)
{
	// The mixer is a CardComponent and paints its own rounded card, border and shadow included.
	// Drawing one here as well put a card inside a card, which reads as a doubled outline.
	if (current == ContentView::mixer)
	{
		return;
	}

	// The other two are contents only, so the slot provides the card they sit in.
	g.setColour (theme::card ());
	g.fillRoundedRectangle (getLocalBounds ().toFloat (), theme::cardRadius);
	g.setColour (theme::cardBorder ());
	g.drawRoundedRectangle (getLocalBounds ().toFloat ().reduced (0.5f), theme::cardRadius, 1.0f);
}

void ContentSlot::resized (void)
{
	// The panels sit inside the card's rounded corners; the mixer brings its own card, so it takes
	// the whole slot rather than an inset copy of it.
	const auto inner = getLocalBounds ().reduced (2);

	if (mixer != nullptr) { mixer->setBounds (getLocalBounds ()); }
	if (chipStatePanel != nullptr) { chipStatePanel->setBounds (inner); }
	if (scopePanel != nullptr) { scopePanel->setBounds (inner); }
}

void ContentSlot::timerCallback (void)
{
	if (current != ContentView::chipState || ! chipStatePanel->isVisible ())
	{
		return;
	}

	if (! isShowing ())
	{
		engine.setStateCaptureWanted (false);
		return;
	}

	engine.setStateCaptureWanted (true);

	if (engine.captureChipState (chipState))
	{
		chipStatePanel->setSnapshot (chipState);

		const int rows = (int) chipState.channels.size ();

		if (rows != lastChannelCount)
		{
			lastChannelCount = rows;
			chipStatePanel->resized ();
		}

		if (isShowing ())
		{
			chipStatePanel->repaint ();
		}
	}
}
