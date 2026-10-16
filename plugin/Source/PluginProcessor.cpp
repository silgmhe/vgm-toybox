#include "PluginProcessor.h"

#include "playback/BackendRegistry.h"

#include "PluginEditor.h"

using namespace toybox;

ToyboxAudioProcessor::ToyboxAudioProcessor()
	: AudioProcessor (buildBusesProperties())
{
	// The composition root: registering the backends here means every UI text, tooltip and file
	// filter that asks the registry (built while this object exists) sees the real format list.
	toybox::playback::registerBuiltInBackends();
}

void ToyboxAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	engine.prepare (sampleRate, samplesPerBlock);
}

void ToyboxAudioProcessor::releaseResources (void)
{
}

bool ToyboxAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
	// Every bus is either stereo or switched off (ADR-7 - no mono paths anywhere), and the main
	// bus may be off as long as at least one other bus is on: a host that routes all tracks to
	// extra outputs is a normal way to use a multi-output instrument.
	bool anyEnabled = false;
	for (int bus = 0; bus < layouts.outputBuses.size(); ++bus)
	{
		const auto& set = layouts.outputBuses.getReference (bus);
		if (set.isDisabled())
		{
			continue;
		}

		if (set != juce::AudioChannelSet::stereo())
		{
			return false;
		}

		anyEnabled = true;
	}

	return anyEnabled;
}

void ToyboxAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ignoreUnused (midiMessages);
	juce::ScopedNoDenormals noDenormals;

	const int numSamples = buffer.getNumSamples();

	// Every enabled bus (a disabled one has no channels, which reads as "not connected"): the
	// engine clears each one before it renders, so nothing has to be silenced here - and a bus
	// the host has not enabled is simply not in the buffer.
	playback::OutputBusBuffers buses[playback::kOutputBusCount];
	for (int bus = 0; bus < playback::kOutputBusCount; ++bus)
	{
		// getBusBuffer() hands out a view *by value* (it just points into `buffer`), so it is
		// taken by value here - binding a non-const reference to it is a construction MSVC
		// happens to accept but the standard does not.
		auto busBuffer = getBusBuffer (buffer, false, bus);
		if (busBuffer.getNumChannels() >= 2)
		{
			buses[bus].left = busBuffer.getWritePointer (0);
			buses[bus].right = busBuffer.getWritePointer (1);
		}
	}

	engine.render (buses, playback::kOutputBusCount, numSamples);
}

juce::AudioProcessorEditor* ToyboxAudioProcessor::createEditor (void)
{
	return new ToyboxAudioProcessorEditor (*this);
}

void ToyboxAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	PlaybackEngine& eng = engine;

	juce::XmlElement state ("VGMToyboxState");
	state.setAttribute ("file", eng.getFile().getFullPathName());
	state.setAttribute ("loopCount", eng.getLoopCount());
	state.setAttribute ("fadeSeconds", eng.getFadeSeconds());
	state.setAttribute ("volume", (double) eng.getMasterVolume());

	copyXmlToBinary (state, destData);
}

void ToyboxAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	// The standalone application always starts with a clean state: no file from the previous
	// session and no auto-play. Only plugin instances inside a host restore their session.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		return;
	}

	std::unique_ptr<juce::XmlElement> state (getXmlFromBinary (data, sizeInBytes));
	// the old tag is still accepted: renaming the product must not throw away the state a host
	// saved for a project (the plugin codes are unchanged, so hosts do load this build into them)
	if (state == nullptr || ! (state->hasTagName ("VGMToyboxState")
							   || state->hasTagName ("VGMPlayState")))
	{
		return;
	}

	PlaybackEngine& eng = engine;
	eng.setLoopCount (state->getIntAttribute ("loopCount", 1));
	eng.setFadeSeconds (state->getDoubleAttribute ("fadeSeconds", 3.0));
	eng.setMasterVolume ((float) state->getDoubleAttribute ("volume", 1.0));

	const juce::File file (state->getStringAttribute ("file"));
	if (file.existsAsFile())
	{
		juce::String errorMessage;
		eng.loadFile (file, errorMessage);
	}
}

//==============================================================================
// entry point used by the JUCE plugin wrappers (VST3, standalone, ...)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter (void)
{
	return new ToyboxAudioProcessor();
}
