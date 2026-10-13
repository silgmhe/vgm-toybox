#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

class VGMPlayAudioProcessorEditor : public juce::AudioProcessorEditor,
									private juce::Timer,
									public juce::FileDragAndDropTarget
{
public:
	explicit VGMPlayAudioProcessorEditor (VGMPlayAudioProcessor&);
	~VGMPlayAudioProcessorEditor() override;

	void paint (juce::Graphics& g) override;
	void resized() override;
	bool keyPressed (const juce::KeyPress& key) override;

private:
	/** macOS-flavoured light theme: white cards on a light grey window, hairline borders,
		blue accent for anything that is "on". */
	class LightLookAndFeel : public juce::LookAndFeel_V4
	{
	public:
		LightLookAndFeel (void);

		void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
								   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
		void drawButtonText (juce::Graphics&, juce::TextButton&,
							 bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
		void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
							   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
		void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
						  bool ticked, bool isEnabled,
						  bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
		void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
							   float sliderPos, float minSliderPos, float maxSliderPos,
							   juce::Slider::SliderStyle, juce::Slider&) override;
		void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
						   int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
		juce::Font getComboBoxFont (juce::ComboBox&) override;
		void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
	};

	/** Small button that draws a play/pause/stop/loop icon. */
	class IconButton : public juce::Button
	{
	public:
		enum class Icon { play, pause, stop, loop };

		IconButton (const juce::String& buttonName, Icon iconToDraw)
			: juce::Button (buttonName), icon (iconToDraw)
		{
		}

		void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

	private:
		Icon icon;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
	};

	void timerCallback (void) override;
	bool isInterestedInFileDrag (const juce::StringArray& files) override;
	void fileDragEnter (const juce::StringArray& files, int x, int y) override;
	void fileDragExit (const juce::StringArray& files) override;
	void filesDropped (const juce::StringArray& files, int x, int y) override;

	void openFileChooser (void);
	void loadFile (const juce::File& file);
	void updateControls (void);
	void rebuildChipButtons (void);
	void seekRelative (double seconds);
	void setLoopForever (bool shouldLoopForever);

	VGMPlayAudioProcessor& processor;
	vgmplay::VgmEngine& engine;

	LightLookAndFeel lookAndFeel;

	juce::TextButton openButton { "Open VGM..." };
	IconButton playButton { "Play", IconButton::Icon::play };
	IconButton pauseButton { "Pause", IconButton::Icon::pause };
	IconButton stopButton { "Stop", IconButton::Icon::stop };
	IconButton loopButton { "Loop", IconButton::Icon::loop };
	juce::Slider positionSlider;
	juce::Slider volumeSlider;
	juce::ComboBox loopBox;
	juce::ComboBox fadeBox;
	juce::Label titleLabel;
	juce::Label detailsLabel;
	juce::Label infoLabel;
	juce::Label statusLabel;
	juce::Label dropHintLabel;
	juce::OwnedArray<juce::ToggleButton> chipButtons;
	juce::TooltipWindow tooltipWindow { this, 650 };

	// card areas, computed in resized() and painted in paint()
	juce::Rectangle<int> infoCardBounds;
	juce::Rectangle<int> chipsCardBounds;
	juce::Rectangle<int> chipsHeaderBounds;
	juce::Rectangle<int> transportCardBounds;
	juce::Rectangle<int> loopMarkerBounds;	// strip above the position slider
	double loopMarkerNormalised = -1.0;		// < 0 = the song has no loop point to show

	std::unique_ptr<juce::FileChooser> fileChooser;
	bool userIsDragging = false;
	int lastLoopCount = -1;			// for the loop button / combo box sync
	juce::String lastError;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VGMPlayAudioProcessorEditor)
};
