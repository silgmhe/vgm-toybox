#include "PluginProcessor.h"

#include "playback/BackendRegistry.h"

#include "PluginEditor.h"
#include "playback/StartupSong.h"

using namespace toybox;

ToyboxAudioProcessor::ToyboxAudioProcessor()
	: AudioProcessor (buildBusesProperties())
{
	// The composition root: registering the backends here means every UI text, tooltip and file
	// filter that asks the registry (built while this object exists) sees the real format list.
	toybox::playback::registerBuiltInBackends();
}

juce::File ToyboxAudioProcessor::getStartupFile (void) const
{
	if (wrapperType != wrapperType_Standalone)
	{
		return {};
	}

	// Which argument is a song is a rule of its own, with the arguments already split and "can this
	// be played" supplied by the registry - see playback/StartupSong.h, where vgm_core_test can
	// reach it (it could not from here, which is how a path with a space in it stayed broken).
	const auto arguments = juce::JUCEApplicationBase::getCommandLineParameterArray();
	const auto& registry = playback::BackendRegistry::getInstance();

	const auto song = playback::chooseStartupSong (arguments, [&registry] (const juce::File& file)
																{
																	return registry.isSupportedFile (file);
																});
	if (song.existsAsFile())
	{
		return song;
	}

	// Nothing usable: leave a breadcrumb, because "I double-clicked a song and nothing happened" is
	// otherwise impossible to diagnose from the outside. A launch with no arguments stays silent -
	// there was nothing to fail at.
	if (! arguments.isEmpty())
	{
		juce::File::getSpecialLocation (juce::File::tempDirectory)
			.getChildFile ("vgmtoybox-startup.log")
			.replaceWithText ("Started with: " + arguments.joinIntoString (" ") + juce::newLine
							  + "No argument was a supported song file." + juce::newLine);
	}

	return {};
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
	// Both MIDI buses are declared (NEEDS_MIDI_INPUT/OUTPUT): the input one is what Ableton Live
	// requires of an instrument, the output one is reserved for a future MIDI-out feature.
	// Nothing is turned into sound and nothing is emitted - so the buffer is *cleared*, not just
	// ignored: the wrapper reads it back as the output events now that a MIDI out bus exists, and
	// leaving the input events in it would echo them straight back to the host.
	midiMessages.clear();
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

juce::String ToyboxAudioProcessor::getStateMessage (void) const
{
	const juce::ScopedLock sl (stateMessageLock);
	return stateMessage;
}

void ToyboxAudioProcessor::clearStateMessage (void)
{
	const juce::ScopedLock sl (stateMessageLock);
	stateMessage.clear();
}

void ToyboxAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	// The standalone has no project around it: it starts from the command line and never reads
	// this back (setStateInformation() returns below), so writing a blob would only be work for
	// the properties file to carry. Both sides early-return for the same reason.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		destData.reset();
		return;
	}

	// The whole session - song bytes and mixer - is the engine's state now (playback/SessionState.h
	// says why the song travels as data). It is deliberately not cached: a typical VGM is tens of
	// kilobytes, and a stale blob is a bug that only shows up after an autosave.
	engine.writeState (destData);
}

void ToyboxAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	// The standalone application always starts with a clean state: no file from the previous
	// session and no auto-play. Only plugin instances inside a host restore their session.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		return;
	}

	juce::String errorMessage;
	if (engine.readState (data, sizeInBytes, errorMessage))
	{
		clearStateMessage();
		return;
	}

	// Everything that is not a state this build wrote - the old path-only format included - is
	// refused rather than guessed at, and the status line says so (review §6 H-1).
	if (errorMessage.isNotEmpty())
	{
		const juce::ScopedLock sl (stateMessageLock);
		stateMessage = errorMessage;
	}
}

//==============================================================================
// entry point used by the JUCE plugin wrappers (VST3, standalone, ...)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter (void)
{
	return new ToyboxAudioProcessor();
}
