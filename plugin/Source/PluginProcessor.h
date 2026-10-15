#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "playback/PlaybackEngine.h"

/**
	Audio processor of the VGM Toybox plugin.

	The plugin renders VGM/VGZ files (which are loaded through the GUI) with libvgm and has its
	own transport: it does not need MIDI or the host transport to produce sound.
*/
class ToyboxAudioProcessor : public juce::AudioProcessor
{
public:
	ToyboxAudioProcessor();
	~ToyboxAudioProcessor() override = default;

	// --- AudioProcessor ---
	void prepareToPlay (double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
	bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
	void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "VGM Toybox"; }
	bool acceptsMidi() const override { return false; }
	bool producesMidi() const override { return false; }
	bool isMidiEffect() const override { return false; }
	double getTailLengthSeconds() const override { return 0.0; }

	int getNumPrograms() override { return 1; }
	int getCurrentProgram() override { return 0; }
	void setCurrentProgram (int) override {}
	const juce::String getProgramName (int) override { return {}; }
	void changeProgramName (int, const juce::String&) override {}

	void getStateInformation (juce::MemoryBlock& destData) override;
	void setStateInformation (const void* data, int sizeInBytes) override;

	toybox::PlaybackEngine& getEngine (void) { return engine; }

private:
	/** "Out 1/2" (the main bus, activated) plus the additional buses, which start **inactive**:
		a host only allocates the ones the user enables (docs/multitrack-output-design.md, ADR-2),
		so out of the box the plugin behaves exactly as it did before. Defined here because
		`BusesProperties` is a protected nested type of `AudioProcessor`. */
	static BusesProperties buildBusesProperties (void)
	{
		BusesProperties properties;
		properties = properties.withOutput (toybox::playback::outputBusName (0),
											juce::AudioChannelSet::stereo(), true);

		for (int bus = 1; bus < toybox::playback::kOutputBusCount; ++bus)
		{
			properties = properties.withOutput (toybox::playback::outputBusName (bus),
												juce::AudioChannelSet::stereo(), false);
		}

		return properties;
	}

	toybox::PlaybackEngine engine;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxAudioProcessor)
};
