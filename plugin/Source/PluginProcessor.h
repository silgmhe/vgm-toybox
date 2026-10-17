#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "playback/PlaybackEngine.h"

/**
	Audio processor of the VGMToybox plugin.

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

	const juce::String getName() const override { return "VGMToybox"; }
	/** True although the processor never reads a note: an instrument that offers no MIDI input is
		refused by some hosts (Ableton Live among them), so the bus is declared and the events are
		dropped (processBlock()). Keep this in step with NEEDS_MIDI_INPUT in CMakeLists.txt - that
		flag is what actually creates the VST3 bus. */
	bool acceptsMidi() const override { return true; }
	/** The mirror of acceptsMidi(): the MIDI-out bus is declared for the feature that will come,
		although nothing is emitted today (processBlock() clears the buffer, which is also what
		keeps the announced output silent). Keep in step with NEEDS_MIDI_OUTPUT. */
	bool producesMidi() const override { return true; }
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

	/** The DAW-sync state: enable, cue and the offset knob (playback/DawSync.h).

		One-way on purpose - the host clock is the master and the song position is a pure function
		of it, so the plugin never drives the host. The editor writes the cue/offset, processBlock()
		publishes the host clock once per block and the engine follows it. It lives in the engine
		so that render() can follow it and so that it is part of the project state. */
	toybox::playback::DawSync& getDawSync (void) { return engine.getDawSync(); }

	/** What restoring a project had to say - an incompatible state, a song left out of it - for
		the editor's status line. Sticky on purpose: it is the only hint the user gets when a host
		opens a project that did not come back, and the editor may not exist yet when the host
		calls setStateInformation(). Thread-safe: the host may not use the message thread. */
	juce::String getStateMessage (void) const;
	void clearStateMessage (void);

	/** Standalone only: the song the application was launched with - Explorer's "Open with", a
		command line, a desktop shortcut - so that double-clicking a file starts playing it.

		A plugin never gets one. It has no command line of its own (the host has one, and a file in
		it belongs to the host's startup), which is what the wrapper type tells apart. The first
		argument that is an existing file of a supported format wins; options and anything else in
		the line are ignored. */
	juce::File getStartupFile (void) const;

private:
	/** "Out 1/2" (the main bus, activated) plus the additional buses, which start **inactive**:
		a host only allocates the ones the user enables (docs/desgin/multitrack-output-design.md, ADR-2),
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

	/** The body of setStateInformation(): decodes and installs one state, message thread, and
		reports what the status line should say (empty on success). Split out so the public call
		can hop threads first - a host may call it from anywhere (review M-2). */
	juce::String restoreState (const void* data, int sizeInBytes);

	toybox::PlaybackEngine engine;

	juce::CriticalSection stateMessageLock;
	juce::String stateMessage;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxAudioProcessor)
};
