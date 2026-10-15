#include "MixerPanel.h"

#include "SoftShadow.h"
#include "Theme.h"

namespace vgmplay::ui
{
namespace
{
	constexpr int kHeaderHeight = 22;
	constexpr int kGroupHeaderHeight = 16;		// painted above each row of strips
	constexpr int kStripWidth = 88;				// meter + graduations + fader, see ChannelStrip
	constexpr int kStripGap = 6;
	constexpr int kGroupGap = 10;
	constexpr float kMeterDecay = 0.82f;		// per 20 Hz tick (~0.3 s falloff)

	/** Peak decay for the meters: rise instantly, fall slowly and settle exactly at zero, so
		the strips stop repainting (they skip a frame when the level does not move). */
	float decayPeak (float peak, float previous)
	{
		const float decayed = juce::jmax (peak, previous * kMeterDecay);
		return (decayed < 0.0005f) ? 0.0f : decayed;	// -66 dB is silence on a meter
	}

	bool deviceIsMuted (const std::vector<vgmplay::ChipEntry>& chips, vgmplay::playback::DeviceId deviceId)
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

MixerPanel::MixerPanel (vgmplay::MixerEngine& engineToUse)
	: engine (engineToUse)
{
	mixerToggle.onClick = [this]
	{
		engine.setMixerEnabled (! mixerToggle.getToggleState());
		refresh();
	};
	mixerToggle.setTooltip ("Mixer on: one render track per chip channel - off: the plain mix without per-channel tracks (saves CPU)");
	addAndMakeVisible (mixerToggle);

	masterStrip.setup ("MASTER", true);
	// the master fader is the mixer's own master gain - a stage of its own, in front of the
	// transport card's output volume (neither control drives the other)
	masterStrip.onGainChanged = [this] (float gain) { engine.setMixerMasterGain (gain); };
	addAndMakeVisible (masterStrip);

	addAndMakeVisible (viewport);
	viewport.setViewedComponent (&holder, false);
	viewport.setScrollBarsShown (true, true);

	refresh();	// hides the card while nothing is loaded
}

void MixerPanel::rebuildStrips (void)
{
	strips.clear();
	holder.groupHeaders.clear();

	const auto& tracks = engine.getTracks();
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		auto* strip = new ChannelStrip();
		strip->setup (tracks[i].name, false);

		const size_t index = i;
		strip->onGainChanged = [this, index] (float gain) { engine.setTrackGain (index, gain); };
		strip->onPanChanged = [this, index] (float pan) { engine.setTrackPan (index, pan); };
		strip->onMuteChanged = [this, index] (bool muted) { engine.setTrackMute (index, muted); };
		strip->onSoloChanged = [this, index] (bool soloed) { engine.setTrackSolo (index, soloed); };

		strips.add (strip);
		holder.addAndMakeVisible (strip);
	}

	meterLeft.assign (tracks.size(), 0.0f);
	meterRight.assign (tracks.size(), 0.0f);

	setVisible (! tracks.empty());
	layoutStrips();
	repaint();
}

void MixerPanel::refresh (void)
{
	const auto& tracks = engine.getTracks();
	const juce::String file = engine.getFile().getFullPathName();

	if (file != loadedFile || (int) tracks.size() != loadedTrackCount)
	{
		loadedFile = file;
		loadedTrackCount = (int) tracks.size();
		rebuildStrips();
	}

	if (tracks.empty())
	{
		return;
	}

	// the toggle follows the engine; a flip repaints the ring around the button as well
	if (mixerToggle.getToggleState() != engine.isMixerEnabled())
	{
		mixerToggle.setToggleState (engine.isMixerEnabled(), juce::dontSendNotification);
		repaint (mixerToggle.getBounds().expanded (theme::accentGlowBlur + theme::accentGlowOffsetY + 2));
	}

	const auto& chips = engine.getChips();
	for (size_t i = 0; i < tracks.size() && i < (size_t) strips.size(); ++i)
	{
		meterLeft[i] = decayPeak (engine.getTrackPeak (i, 0), meterLeft[i]);
		meterRight[i] = decayPeak (engine.getTrackPeak (i, 1), meterRight[i]);

		strips[(int) i]->setValues (engine.getTrackGain (i), engine.getTrackPan (i),
									engine.isTrackMuted (i), engine.isTrackSoloed (i),
									meterLeft[i], meterRight[i],
									deviceIsMuted (chips, tracks[i].deviceId) ? 0.45f : 1.0f);
	}

	masterPeakL = decayPeak (engine.getOutputPeak (0), masterPeakL);
	masterPeakR = decayPeak (engine.getOutputPeak (1), masterPeakR);
	// the master gain is applied in both modes, so its strip is never dimmed
	masterStrip.setValues (engine.getMixerMasterGain(), 0.0f, false, false,
						   masterPeakL, masterPeakR, 1.0f);
}

void MixerPanel::resized (void)
{
	if (getLocalBounds().isEmpty())
	{
		return;		// the editor had no room for the card - keep the children as they are
	}

	auto inner = getLocalBounds().reduced (theme::cardPadding, 8);

	headerBounds = inner.removeFromTop (kHeaderHeight);
	mixerToggle.setBounds (headerBounds.removeFromRight (kHeaderHeight));

	inner.removeFromTop (4);

	// the master strip is pinned at the right of the scrollable rows - layoutStrips() places
	// it, because it follows the height of the strip rows
	masterColumn = inner.removeFromRight (kStripWidth);

	inner.removeFromRight (8);
	viewport.setBounds (inner);

	layoutStrips();
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

	// the master strip rides along with the strips (aligned with the first row, clamped to the
	// card so a small mixer card never lets it overflow)
	const int masterTop = masterColumn.getY() + kGroupHeaderHeight + 2;
	const int masterHeight = juce::jlimit (0, juce::jmax (0, masterColumn.getBottom() - masterTop), stripHeight);
	masterStrip.setBounds (masterColumn.getX(), masterTop, masterColumn.getWidth(), masterHeight);
}

void MixerPanel::paintCardContent (juce::Graphics& g)
{
	// The mixer toggle glows in the accent colour while the mixer is on. The card paints it
	// (and therefore behind the button): a component cannot draw outside its own bounds.
	if (mixerToggle.getToggleState())
	{
		drawSoftShadow (g, mixerToggle.getBounds().toFloat(), theme::buttonRadius,
						theme::accentGlow(), theme::accentGlowBlur, { 0, theme::accentGlowOffsetY });
	}

	if (headerBounds.isEmpty())
	{
		return;
	}

	g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
	g.setColour (theme::secondaryText());
	g.drawText ("MIXER", headerBounds, juce::Justification::centredLeft);
	g.drawText ("MUTE / SOLO / GAIN / PAN", headerBounds, juce::Justification::centredRight);
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

} // namespace vgmplay::ui
