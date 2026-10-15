#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "playback/MixerEngine.h"

/**
	Audio processor of the VGMPlay plugin.

	The plugin renders VGM/VGZ files (which are loaded through the GUI) with libvgm and has its
	own transport: it does not need MIDI or the host transport to produce sound.
*/
class VGMPlayAudioProcessor : public juce::AudioProcessor
{
public:
	VGMPlayAudioProcessor();
	~VGMPlayAudioProcessor() override = default;

	// --- AudioProcessor ---
	void prepareToPlay (double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
	bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
	void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "VGMPlay"; }
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

	vgmplay::MixerEngine& getEngine (void) { return engine; }

private:
	vgmplay::MixerEngine engine;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VGMPlayAudioProcessor)
};
