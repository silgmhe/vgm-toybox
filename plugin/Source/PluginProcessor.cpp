#include "PluginProcessor.h"

#include "PluginEditor.h"

using namespace vgmplay;

VGMPlayAudioProcessor::VGMPlayAudioProcessor()
	: AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void VGMPlayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	engine.prepare (sampleRate, samplesPerBlock);
}

void VGMPlayAudioProcessor::releaseResources (void)
{
}

bool VGMPlayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
	return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void VGMPlayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ignoreUnused (midiMessages);
	juce::ScopedNoDenormals noDenormals;

	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();

	// libvgm renders stereo only - silence any additional channels of the output bus
	for (int ch = 2; ch < numChannels; ++ch)
	{
		buffer.clear (ch, 0, numSamples);
	}

	if (numChannels >= 2 && numSamples > 0)
	{
		float* outputs[2] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
		engine.render (outputs, numSamples);
	}
	else
	{
		buffer.clear();
	}
}

juce::AudioProcessorEditor* VGMPlayAudioProcessor::createEditor (void)
{
	return new VGMPlayAudioProcessorEditor (*this);
}

void VGMPlayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	VgmEngine& eng = engine;

	juce::XmlElement state ("VGMPlayState");
	state.setAttribute ("file", eng.getFile().getFullPathName());
	state.setAttribute ("loopCount", eng.getLoopCount());
	state.setAttribute ("fadeSeconds", eng.getFadeSeconds());
	state.setAttribute ("volume", (double) eng.getMasterVolume());

	copyXmlToBinary (state, destData);
}

void VGMPlayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	// The standalone application always starts with a clean state: no file from the previous
	// session and no auto-play. Only plugin instances inside a host restore their session.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		return;
	}

	std::unique_ptr<juce::XmlElement> state (getXmlFromBinary (data, sizeInBytes));
	if (state == nullptr || ! state->hasTagName ("VGMPlayState"))
	{
		return;
	}

	VgmEngine& eng = engine;
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
	return new VGMPlayAudioProcessor();
}
