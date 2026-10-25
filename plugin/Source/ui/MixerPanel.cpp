#include "MixerPanel.h"

#include "ButtonPaint.h"
#include "SoftShadow.h"
#include "Theme.h"

#include "i18n/LanguageManager.h"

namespace toybox::ui
{
namespace
{
	// The header row is as tall as the buttons in it. Each button carries its own shadow room, so
	// this is the face (22, as before they were given a shadow) plus 2 * buttonShadowInset - see
	// docs/desgin/ui-button-design.md §8.
	constexpr int kHeaderButtonSize = 30;		// the same size as the keyboard's mute and solo, so the
										// two views really do share one set of buttons
	constexpr int kGroupHeaderHeight = 16;		// painted above each row of strips
	constexpr int kStripWidth = 88;				// meter + graduations + fader, see ChannelStrip
	constexpr int kStripGap = 6;
	constexpr int kGroupGap = 10;
	constexpr int kScrollBarWidth = 12;			// the card's own, at the far right of the mixer
	constexpr float kMeterDecay = 0.82f;		// per refresh tick while playing (~0.3 s falloff)
	constexpr float kMeterStopDecay = 0.55f;	// while the transport is stopped they drop fast

	/** Peak decay for the meters: rise instantly, fall smoothly and settle exactly at zero, so
		the strips stop repainting (they skip a frame when the level does not move). */
	float decayPeak (float peak, float previous, float decay)
	{
		const float decayed = juce::jmax (peak, previous * decay);
		return (decayed < 0.0005f) ? 0.0f : decayed;	// -66 dB is silence on a meter
	}

	bool deviceIsMuted (const std::vector<toybox::ChipEntry>& chips, toybox::playback::DeviceId deviceId)
	{
		for (const auto& chip : chips)
		{
			if (chip.deviceId == deviceId)
			{
				return chip.muted;
			}
		}
		return false;
	}
}

MixerPanel::MixerPanel (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	mixerToggle.onClick = [this]
	{
		engine.setMixerEnabled (! mixerToggle.getToggleState());
		refresh();
	};
	mixerToggle.setComponentID ("mixer.enable");
	addAndMakeVisible (mixerToggle);
	addAndMakeVisible (dcBlockButton);

	dcBlockButton.setClickingTogglesState (true);
	dcBlockButton.onClick = [this]
	{
		engine.setDcBlockEnabled (dcBlockButton.getToggleState ());
		dcBlockButton.setTooltip (i18n::tr (dcBlockButton.getToggleState ()
											? "mixer.dcBlockOn" : "mixer.dcBlockOff"));
	};

	masterStrip.setup (i18n::tr ("mixer.master"), true);
	// the master fader is the mixer's own master gain - a stage of its own, in front of the
	// transport card's output volume (neither control drives the other)
	masterStrip.onGainChanged = [this] (float gain) { engine.setMixerMasterGain (gain); };
	// the master strip's M silences the master bus (tracks sent to real buses keep playing) and
	// its selector picks the bus the master mix goes to - never the master itself
	masterStrip.onMuteChanged = [this] (bool muted) { engine.setMasterMuted (muted); };
	masterStrip.onOutputChanged = [this] (playback::OutputTarget target) { engine.setMasterOutput (target); };
	masterStrip.onMidiChannelChanged = [this] (int channel) { engine.setMasterMidiChannel (channel); };
	masterStrip.setMidiState (engine.getMasterMidiChannel(), playback::MidiCapability::melodic);

	// The master sits in a clip of its own: it is pinned to the right of the scrolling rows, but it
	// scrolls with them, so something has to cut it off at the card's edge.
	masterStrip.setComponentID ("mixer.master");	// for the window scripts (AppHarnessTest)
	addAndMakeVisible (masterClip);
	masterClip.addAndMakeVisible (masterStrip);

	// the two routing shortcuts in the card header
	spreadOutputsButton.onClick = [this] { spreadTrackOutputs(); };
	addAndMakeVisible (spreadOutputsButton);

	mergeToMasterButton.onClick = [this] { sendTrackOutputsToMaster(); };
	addAndMakeVisible (mergeToMasterButton);

	addAndMakeVisible (viewport);
	viewport.setViewedComponent (&holder, false);

	// The vertical scrollbar is the card's own, not the viewport's: the viewport's would sit
	// between the strips and the master, which reads as "this scrolls the strips" while it also
	// drags the master along. This one is placed at the far right of the card and drives both.
	viewport.setScrollBarsShown (false, true, true, true);
	verticalBar.setAutoHide (false);
	verticalBar.addListener (this);
	addAndMakeVisible (verticalBar);

	refreshLanguage();
	refresh();	// hides the card while nothing is loaded
}

void MixerPanel::refreshLanguage (void)
{
	mixerToggle.setName (i18n::tr ("mixer.toggleName"));
	mixerToggle.setTooltip (i18n::tr ("mixer.toggleTooltip"));
	spreadOutputsButton.setName (i18n::tr ("mixer.spreadName"));
	spreadOutputsButton.setTooltip (i18n::tr ("mixer.spreadTooltip"));
	mergeToMasterButton.setName (i18n::tr ("mixer.mergeName"));
	mergeToMasterButton.setTooltip (i18n::tr ("mixer.mergeTooltip"));

	masterStrip.setStripName (i18n::tr ("mixer.master"));
	masterStrip.refreshLanguage();

	for (auto* strip : strips)
	{
		strip->refreshLanguage();
	}

	repaint();
}

void MixerPanel::rebuildStrips (void)
{
	strips.clear();
	holder.groupHeaders.clear();

	const auto& tracks = engine.getTracks();
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		auto* strip = new ChannelStrip();
		strip->setup (tracks[i].name, false, (int) i);

		const size_t index = i;
		strip->onGainChanged = [this, index] (float gain) { engine.setTrackGain (index, gain); };
		strip->onPanChanged = [this, index] (float pan) { engine.setTrackPan (index, pan); };
		strip->onMuteChanged = [this, index] (bool muted) { engine.setTrackMute (index, muted); };
		strip->onSoloChanged = [this, index] (bool soloed) { engine.setTrackSolo (index, soloed); };
		strip->onOutputChanged = [this, index] (playback::OutputTarget target) { engine.setTrackOutput (index, target); };
		strip->onMidiChannelChanged = [this, index] (int channel) { engine.setTrackMidiChannel (index, channel); };
		// A backend without a sequence offers no MIDI, whatever a track's voice could do: the
		// capability describes the voice, this describes what the song can send.
		strip->setMidiState (engine.getTrackMidiChannel (i),
							 engine.canEmitMidi () ? tracks[i].midiCapability
												   : playback::MidiCapability::unavailable);

		strips.add (strip);
		holder.addAndMakeVisible (strip);
	}

	meterLeft.assign (tracks.size(), 0.0f);
	meterRight.assign (tracks.size(), 0.0f);

	// Only ever hides itself. The content slot decides whether the mixer is on screen at all, and
	// a panel that could show itself would draw over whichever view the user picked - which is
	// exactly what happened the first time this ran: the chip panel and the mixer were both up.
	if (tracks.empty ())
	{
		setVisible (false);
	}
	layoutStrips();
	repaint();
}

void MixerPanel::refresh (void)
{
	if (dcBlockButton.getToggleState () != engine.isDcBlockEnabled ())
	{
		dcBlockButton.setToggleState (engine.isDcBlockEnabled (), juce::dontSendNotification);
	}

	// Which strips exist changes only with the song, so that part is a check rather than work: the
	// path the timer calls 10-15 times a second stays value-only, because anything else would cost
	// more than the meters it is there to show.
	syncStructure();

	if (engine.getTracks().empty())
	{
		return;
	}

	refreshValues();
}

void MixerPanel::syncStructure (void)
{
	const juce::String file = engine.getFile().getFullPathName();

	if (file != loadedFile || (int) engine.getTracks().size() != loadedTrackCount)
	{
		loadedFile = file;
		loadedTrackCount = (int) engine.getTracks().size();
		rebuildStrips();
	}
}

void MixerPanel::refreshValues (void)
{
	const auto& tracks = engine.getTracks();

	// The header buttons follow the engine. A flip has to repaint the area around the button as
	// well, because the accent glow the card paints (paintCardContent()) reaches past the button
	// bounds.
	if (mixerToggle.getToggleState() != engine.isMixerEnabled())
	{
		mixerToggle.setToggleState (engine.isMixerEnabled(), juce::dontSendNotification);
		repaint (buttonGlowBounds (mixerToggle.getBounds()));
	}

	// the routing shortcuts are only meaningful while the mixer routes per track (with the mixer
	// off every track ends up on the master path anyway - R8)
	const bool canRoute = engine.isMixerEnabled() && ! engine.getTracks().empty();
	spreadOutputsButton.setEnabled (canRoute);
	mergeToMasterButton.setEnabled (canRoute);

	// They behave like a radio pair for the current *default*: the active scheme lights up, so
	// a glance tells which routing a newly loaded song will start with. Both get the same glow
	// as the mixer toggle - it used to be missing here (docs/desgin/ui-button-design.md §2).
	const bool spreadDefault = (engine.getDefaultRouting() == playback::DefaultRouting::spreadAcrossBuses);
	if (spreadOutputsButton.getToggleState() != spreadDefault)
	{
		spreadOutputsButton.setToggleState (spreadDefault, juce::dontSendNotification);
		repaint (buttonGlowBounds (spreadOutputsButton.getBounds()));
	}
	if (mergeToMasterButton.getToggleState() == spreadDefault)
	{
		mergeToMasterButton.setToggleState (! spreadDefault, juce::dontSendNotification);
		repaint (buttonGlowBounds (mergeToMasterButton.getBounds()));
	}

	// A stopped (or paused) transport means silence, whatever the last rendered block left
	// behind, and the meters drop quickly - they must never freeze at the last level.
	const bool playing = engine.isPlaying();
	const float decay = playing ? kMeterDecay : kMeterStopDecay;

	// --- the output selectors -----------------------------------------------------------------
	// The item list is rebuilt only when it can have changed (the host's bus layout) or when a
	// strip shows something else than the engine - rebuilding 65 combo items per strip on every
	// tick would be pointless work.
	std::uint64_t connectivity = 0;
	for (int bus = 0; bus < playback::kOutputBusCount; ++bus)
	{
		if (engine.isOutputBusConnected (bus))
		{
			connectivity |= (std::uint64_t) 1 << bus;
		}
	}

	const bool routesChanged = (connectivity != lastOutputConnectivity);
	lastOutputConnectivity = connectivity;

	// A strip needs its list rebuilt when the host layout changed *or* when it shows a different
	// target than the engine (the user just picked one). Both cases fill the lists - handing an
	// empty list to the selector would leave it without items.
	bool selectionChanged = false;
	for (size_t i = 0; i < tracks.size() && i < (size_t) strips.size(); ++i)
	{
		selectionChanged = selectionChanged
						   || strips[(int) i]->getShownOutput() != engine.getTrackOutput (i);
	}
	selectionChanged = selectionChanged || masterStrip.getShownOutput() != engine.getMasterOutput();

	std::vector<ChannelStrip::OutputOption> trackOutputs;
	std::vector<ChannelStrip::OutputOption> masterOutputs;

	if (routesChanged || selectionChanged)
	{
		ChannelStrip::OutputOption masterOption;
		masterOption.target = playback::kMasterOutput;
		masterOption.name = i18n::tr ("mixer.masterBus");
		trackOutputs.push_back (masterOption);		// a track may send to the master bus …

		for (int bus = 0; bus < playback::kOutputBusCount; ++bus)
		{
			ChannelStrip::OutputOption option;
			option.target = bus;
			option.name = playback::outputBusName (bus);
			option.connected = engine.isOutputBusConnected (bus);

			trackOutputs.push_back (option);		// … or go straight to a real bus
			masterOutputs.push_back (option);		// the master has no "Master" entry (R3)
		}
	}

	const bool mixerActive = engine.isMixerEnabled();
	const auto& chips = engine.getChips();

	for (size_t i = 0; i < tracks.size() && i < (size_t) strips.size(); ++i)
	{
		meterLeft[i] = decayPeak (playing ? engine.getTrackPeak (i, 0) : 0.0f, meterLeft[i], decay);
		meterRight[i] = decayPeak (playing ? engine.getTrackPeak (i, 1) : 0.0f, meterRight[i], decay);

		strips[(int) i]->setValues (engine.getTrackGain (i), engine.getTrackPan (i),
									engine.isTrackMuted (i), engine.isTrackSoloed (i),
									meterLeft[i], meterRight[i],
									deviceIsMuted (chips, tracks[i].deviceId) ? 0.45f : 1.0f);
		strips[(int) i]->setMidiState (engine.getTrackMidiChannel (i),
									   engine.canEmitMidi () ? tracks[i].midiCapability
															 : playback::MidiCapability::unavailable);

		if (routesChanged || strips[(int) i]->getShownOutput() != engine.getTrackOutput (i))
		{
			strips[(int) i]->setOutputOptions (trackOutputs, engine.getTrackOutput (i));
		}
		strips[(int) i]->setMixerActive (mixerActive);
	}

	masterPeakL = decayPeak (playing ? engine.getOutputPeak (0) : 0.0f, masterPeakL, decay);
	masterPeakR = decayPeak (playing ? engine.getOutputPeak (1) : 0.0f, masterPeakR, decay);
	// the master strip is never dimmed: its gain is applied in both modes and its output
	// selector works with the mixer on and off (switched off it is the only path)
	masterStrip.setValues (engine.getMixerMasterGain(), 0.0f, engine.isMasterMuted(), false,
						   masterPeakL, masterPeakR, 1.0f);
	masterStrip.setMidiState (engine.getMasterMidiChannel(), playback::MidiCapability::melodic);

	if (routesChanged || masterStrip.getShownOutput() != engine.getMasterOutput())
	{
		masterStrip.setOutputOptions (masterOutputs, engine.getMasterOutput());
	}
	masterStrip.setMixerActive (true);
	// ... but its MIDI row is not: with the mixer off no track is rendered separately, and the
	// engine sends no MIDI at all in that mode (PlaybackEngine::emitMidiForBlock), so a live
	// selector there would promise something that does not happen
	masterStrip.setMidiActive (mixerActive);
}

void MixerPanel::spreadTrackOutputs (void)
{
	// the engine applies the scheme to the loaded song *and* remembers it for the next one
	engine.setDefaultRouting (playback::DefaultRouting::spreadAcrossBuses);
	refresh();		// the strips' selectors follow right away
}

void MixerPanel::sendTrackOutputsToMaster (void)
{
	engine.setDefaultRouting (playback::DefaultRouting::master);
	refresh();
}

void MixerPanel::resized (void)
{
	if (getLocalBounds().isEmpty())
	{
		return;		// the editor had no room for the card - keep the children as they are
	}

	headerBounds = getCardHeaderArea (kHeaderButtonSize);
	auto inner = getCardInnerArea (kHeaderButtonSize);
	inner.removeFromTop (4);

	// the header's right edge: the mixer toggle, then the two routing shortcuts to its left
	mixerToggle.setBounds (headerBounds.removeFromRight (kHeaderButtonSize));
	headerBounds.removeFromRight (8);
	spreadOutputsButton.setBounds (headerBounds.removeFromRight (kHeaderButtonSize));
	headerBounds.removeFromRight (8);
	mergeToMasterButton.setBounds (headerBounds.removeFromRight (kHeaderButtonSize));
	headerBounds.removeFromRight (8);
	dcBlockButton.setBounds (headerBounds.removeFromRight (kHeaderButtonSize));

	// From the right: the card's scrollbar, then the master, then the scrolling rows. The bar is
	// outside everything it scrolls - one bar for the whole mixer, at the card's edge.
	verticalBar.setBounds (inner.removeFromRight (kScrollBarWidth)
								.withTrimmedTop (kGroupHeaderHeight + 2));

	// the master strip is pinned at the right of the scrollable rows - layoutMaster() places the
	// strip, because it follows the content height and the scroll position. The clip starts level
	// with the first row's strips (a row header sits above them in the scrolling column).
	inner.removeFromRight (4);
	masterColumn = inner.removeFromRight (kStripWidth);
	masterClip.setBounds (masterColumn.withTrimmedTop (kGroupHeaderHeight + 2));

	inner.removeFromRight (8);
	viewport.setBounds (inner);

	layoutStrips();
}

void MixerPanel::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
	if (updatingScrollBar)
	{
		return;
	}

	// the rows move; the master follows in visibleAreaChanged()
	viewport.setViewPosition (viewport.getViewPositionX(),
							  juce::jmax (0, juce::roundToInt (newRangeStart)));
}

void MixerPanel::syncScrollBar (int contentHeight)
{
	// the viewport's own scrollbars are not part of what the content can be seen through
	const int visible = juce::jmax (1, viewport.getMaximumVisibleHeight());
	verticalBar.setVisible (contentHeight > visible);

	// Writing the range back into the bar notifies this panel, which would push the viewport again
	// from inside its own scroll - the guard is what keeps the two from chasing each other.
	const juce::ScopedValueSetter<bool> guard (updatingScrollBar, true);

	verticalBar.setRangeLimits (0.0, juce::jmax (1, contentHeight));
	verticalBar.setCurrentRange (viewport.getViewPositionY(), juce::jmax (1, visible));
}

void MixerPanel::StripViewport::visibleAreaChanged (const juce::Rectangle<int>&)
{
	// the master is not inside the viewport, so scrolling the rows has to drag it along - and the
	// card's scrollbar is not either, so it has to be told where the rows went
	owner.layoutMaster();
	owner.syncScrollBar (owner.holder.getHeight());
}

void MixerPanel::MasterClip::mouseWheelMove (const juce::MouseEvent& event,
											 const juce::MouseWheelDetails& wheel)
{
	viewport.mouseWheelMove (event.getEventRelativeTo (&viewport), wheel);
}

void MixerPanel::layoutStrips (void)
{
	const auto& tracks = engine.getTracks();
	holder.groupHeaders.clear();

	// the tracks of one device form a group (a row of strips)
	std::vector<std::pair<size_t, size_t>> groups;
	for (size_t index = 0; index < tracks.size() && index < (size_t) strips.size(); )
	{
		const int deviceIndex = tracks[index].deviceIndex;
		size_t end = index;
		while (end < tracks.size() && tracks[end].deviceIndex == deviceIndex)
		{
			++end;
		}
		groups.emplace_back (index, end);
		index = end;
	}

	const int visibleWidth = juce::jmax (0, viewport.getWidth() - viewport.getScrollBarThickness());
	const int visibleHeight = juce::jmax (0, viewport.getHeight() - viewport.getScrollBarThickness());

	// The strips grow with the card, so a large (or maximised) window gets long faders; a small
	// card keeps the minimum and scrolls instead.
	const int fixedPerGroup = kGroupHeaderHeight + 2 + kGroupGap;
	const int stripHeight = groups.empty()
							  ? ChannelStrip::preferredHeight
							  : juce::jmax (ChannelStrip::preferredHeight,
											(visibleHeight - (int) groups.size() * fixedPerGroup) / (int) groups.size());

	int y = 0;
	int contentWidth = 0;
	for (const auto& group : groups)
	{
		const int stripCount = (int) (group.second - group.first);
		const int groupWidth = stripCount * (kStripWidth + kStripGap) - kStripGap;

		holder.groupHeaders.push_back ({ tracks[group.first].groupName,
										 juce::Rectangle<int> (0, y, groupWidth, kGroupHeaderHeight) });
		y += kGroupHeaderHeight + 2;

		int x = 0;
		for (size_t i = group.first; i < group.second; ++i)
		{
			strips[(int) i]->setBounds (x, y, kStripWidth, stripHeight);
			x += kStripWidth + kStripGap;
		}
		contentWidth = juce::jmax (contentWidth, x - kStripGap);
		y += stripHeight + kGroupGap;
	}

	holder.setSize (juce::jmax (contentWidth, visibleWidth), juce::jmax (y, visibleHeight));

	// y counts the gap that follows the last row, which is not content
	contentHeight = juce::jmax (0, y - kGroupGap);
	layoutMaster();
	syncScrollBar (holder.getHeight());
}

void MixerPanel::layoutMaster (void)
{
	// The master is measured against the rows, not against the card: its bottom edge has to land on
	// the last row's bottom edge, or it hangs below the console like a piece that was left over.
	// What it must never do is shrink below a strip's height - then it keeps that and scrolls with
	// the rows, which is what the clip is for.
	const int contentTop = kGroupHeaderHeight + 2;	// level with the first row's strips
	const int available = masterClip.getHeight();
	const int height = juce::jmax (ChannelStrip::preferredHeight, contentHeight - contentTop);

	// Scroll only when it does not fit: a master that ends inside the clip must sit still while the
	// rows scroll under it, or its bottom edge would drift away from theirs.
	const int scroll = (height > available) ? viewport.getViewPositionY() : 0;

	masterStrip.setBounds (0, -scroll, masterClip.getWidth(), height);
}

void MixerPanel::paintCardContent (juce::Graphics& g)
{
	// The header buttons glow in the accent colour while they are on. The card paints them (and
	// therefore behind the buttons): the glow reaches further than the shadow room a button keeps
	// inside its own bounds (ButtonPaint.h).
	//
	// headerToggles is the row. This used to be a second list written out here, and the DC
	// blocker was added to one and not the other - lit, with nothing around it.
	for (auto* button : headerToggles)
	{
		if (button->getToggleState())
		{
			drawButtonGlow (g, button->getBounds());
		}
	}

	// No legend on the right any more. It said "MUTE / SOLO / GAIN / PAN", which described the
	// strip controls beside it - and by the time it read that, the row above held four toggles,
	// one of which is a DC blocker and none of which is gain or pan. Every button carries its own
	// tooltip, so the line that had to be kept in step with the row is simply gone.
	paintCardHeaderRow (g, headerBounds, i18n::tr ("mixer.title"), {});
}

void MixerPanel::StripHolder::paint (juce::Graphics& g)
{
	g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
	g.setColour (theme::secondaryText());
	for (const auto& header : groupHeaders)
	{
		g.drawText (header.first, header.second, juce::Justification::centredLeft, true);
	}
}

} // namespace toybox::ui
