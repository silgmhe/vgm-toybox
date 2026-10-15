#include "DropHintOverlay.h"

#include "playback/BackendRegistry.h"

#include "Theme.h"

namespace vgmplay::ui
{

DropHintOverlay::DropHintOverlay (void)
{
	// the overlay covers the whole editor - without this it would swallow every mouse click
	setInterceptsMouseClicks (false, false);

	label.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
	label.setColour (juce::Label::textColourId, theme::accent());
	label.setJustificationType (juce::Justification::centred);
	// the format list comes from the backend registry - a build with more backends advertises
	// them without touching this file
	label.setText ("Drop a " + playback::BackendRegistry::getInstance().getExtensionListText() + " file here",
				   juce::dontSendNotification);
	label.setInterceptsMouseClicks (false, false);
	addAndMakeVisible (label);

	setVisible (false);
}

void DropHintOverlay::paint (juce::Graphics& g)
{
	juce::Path outline, dashed;
	outline.addRoundedRectangle (getLocalBounds().toFloat().reduced (5.0f), 12.0f);
	const float dashes[] = { 8.0f, 6.0f };
	juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
	g.setColour (theme::accent().withAlpha (0.75f));
	g.fillPath (dashed);
}

void DropHintOverlay::resized (void)
{
	label.setBounds (getLocalBounds().reduced (theme::margin));
}

} // namespace vgmplay::ui
