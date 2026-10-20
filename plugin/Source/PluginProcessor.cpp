#include "PluginProcessor.h"

#include "playback/BackendRegistry.h"
#include "playback/HostClockSource.h"
#include "settings/SettingsStore.h"

#include "PluginEditor.h"
#include "playback/StartupSong.h"

using namespace toybox;

ToyboxAudioProcessor::ToyboxAudioProcessor()
	: AudioProcessor (buildBusesProperties())
{
	// The composition root: registering the backends here means every UI text, tooltip and file
	// filter that asks the registry (built while this object exists) sees the real format list.
	toybox::playback::registerBuiltInBackends();

	// The pitch-bend preferences live in the shared settings file, not in the project: they
	// describe the instrument the MIDI goes to, and the editor is not the only thing that needs
	// them - a host that opens a project and plays without ever showing the window does too.
	auto& settings = settings::SettingsStore::getInstance();
	settings.addChangeListener (this);
	applyMidiSettings();
}

ToyboxAudioProcessor::~ToyboxAudioProcessor()
{
	settings::SettingsStore::getInstance().removeChangeListener (this);
}

void ToyboxAudioProcessor::applyMidiSettings (void)
{
	const auto& settings = settings::SettingsStore::getInstance();
	engine.setMidiPitchBendRange (settings.getMidiPitchBendRange());
	engine.setMidiPitchBendEnabled (settings.getMidiPitchBendEnabled());
}

void ToyboxAudioProcessor::changeListenerCallback (juce::ChangeBroadcaster*)
{
	// Theme and language changes come through here as well; applying two atomics is cheap enough
	// that telling the values apart would only add a failure mode.
	applyMidiSettings();
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

void ToyboxAudioProcessor::openSongFromAnotherInstance (const juce::File& file)
{
	// The application class has already asked the registry whether this file can be played (the
	// same predicate the startup line uses), so this only has to keep it until the window looks.
	if (wrapperType != wrapperType_Standalone || ! file.existsAsFile())
	{
		return;
	}

	instanceSong = file;
}

juce::File ToyboxAudioProcessor::takeSongFromAnotherInstance (void)
{
	const auto file = instanceSong;
	// Not "= {}": File's copy and move assignment make that ambiguous to clang. An empty File is
	// what is meant.
	instanceSong = juce::File();
	return file;
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

namespace
{
	/** Turns the engine's sample-offset events into the buffer the VST3/AU wrapper reads back. */
	class HostMidiSink final : public toybox::playback::MidiBlockSink
	{
	public:
		explicit HostMidiSink (juce::MidiBuffer& target) : buffer (target) {}

		void send (const toybox::playback::MidiBlockEvent& event) override
		{
			buffer.addEvent (juce::MidiMessage (event.status, event.data1, event.data2),
							 event.sampleOffset);
		}

	private:
		juce::MidiBuffer& buffer;
	};
}

void ToyboxAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	// Both MIDI buses are declared (NEEDS_MIDI_INPUT/OUTPUT): the input one is what Ableton Live
	// requires of an instrument, the output one carries the generated MIDI. The buffer is
	// *cleared* first: leaving the host's input events in it would echo them straight back.
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

	// DAW sync: publish what the host said. The engine follows it inside render(): the switch is
	// an atomic, so a block that does not follow costs nothing, and only a correction takes the
	// engine's lock (a try-lock - a block that loses it is retried, nothing blocks here).
	engine.getDawSync().publishHostClock (playback::readHostClock (getPlayHead(), getSampleRate()));

	// A stop does not need a branch here: the engine knows which notes it started and ends them
	// itself, on the channels that played them. Its note-offs are the messages a receiver must
	// act on; a broadcast all-notes-off only asks, and some receivers ignore the request.
	HostMidiSink midiSink (midiMessages);
	engine.render (buses, playback::kOutputBusCount, numSamples, &midiSink);
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

juce::String ToyboxAudioProcessor::restoreState (const void* data, int sizeInBytes)
{
	juce::String errorMessage;
	if (engine.readState (data, sizeInBytes, errorMessage))
	{
		clearStateMessage();
		return {};
	}

	// Everything that is not a state this build wrote - the old path-only format included - is
	// refused rather than guessed at, and the status line says so (review §6 H-1).
	if (errorMessage.isNotEmpty())
	{
		const juce::ScopedLock sl (stateMessageLock);
		stateMessage = errorMessage;
	}

	return errorMessage;
}

void ToyboxAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	// The standalone application always starts with a clean state: no file from the previous
	// session and no auto-play. Only plugin instances inside a host restore their session.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		return;
	}

	// VST3 does not promise which thread calls this (review M-2), and the whole restore - the
	// engine, the chip list, the current file - is message-thread data by contract. Hop to the
	// message thread and wait for it: the host is opening a project and cannot use the plugin
	// until this returns, so waiting is what makes the instance consistent when the call ends.
	// (The wait is on the host's thread, not on the audio thread: render() only sees the lock.)
	if (juce::MessageManager::getInstance()->isThisTheMessageThread())
	{
		restoreState (data, sizeInBytes);
		return;
	}

	// The blob belongs to the caller and may be gone after we return, so copy it before crossing.
	const juce::MemoryBlock blob (data, (size_t) sizeInBytes);
	juce::WaitableEvent done;

	juce::MessageManager::callAsync ([this, &blob, &done]
	{
		restoreState (blob.getData(), (int) blob.getSize());
		done.signal();
	});

	// A timeout rather than an infinite wait: if the host's message loop is already shutting down
	// the state cannot be restored, and hanging the host is worse than an empty plugin.
	done.wait (30000);
}

//==============================================================================
// entry point used by the JUCE plugin wrappers (VST3, standalone, ...)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter (void)
{
	return new ToyboxAudioProcessor();
}
