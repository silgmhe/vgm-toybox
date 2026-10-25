#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "playback/MidiSequence.h"
#include "playback/ChipState.h"

/**
	The playback backend port - the standard every format engine (libvgm today, an SNES/SPC
	engine tomorrow) is plugged into.

	Nothing in this header may include a format engine's headers: the core (PlaybackEngine and
	everything above it) only ever sees these types. The rule is enforced by a source guard in
	the build script ("no backend headers outside Source/backends/").

	Three ports, from the outside in:

	  plugins and players -> PlaybackEngine -> ISongBackend -> ISongInstance -> ITrackPlayer

	* `ISongBackend`  recognises a file image and opens it.
	* `ISongInstance` is one open song: its metadata, its tracks and its capabilities.
	* `ITrackPlayer`  is one renderable, *isolated* instance - a song player that can be asked
	  to output only a single track. The mixer creates one per track (see IsolationMode).

	Real-time contract (the core relies on it, the tests guard it):

	1. `ITrackPlayer::render()` runs on the audio thread: **no locks, no allocation, no logging,
	   no exceptions**. Switching track parameters (gain / pan / mute / solo) never goes through
	   a backend at all - the core mixes with plain atomics.
	2. Everything else is message-thread only and is called while the engine holds its lock, so
	   a backend may allocate and log there.
	3. `render()` writes at most `numSamples` frames into the two float buffers and returns how
	   many it wrote; the caller leaves the rest silent. Full scale is +/- 1.0.
	4. Backends never call back into the core from the audio thread - the "finished" state is a
	   plain atomic the backend writes and the core reads.
*/
namespace toybox::playback
{

/** Opaque handle of a sound device inside the loaded song. Was libvgm's `PLR_DEV_INFO::id`. */
using DeviceId = std::uint32_t;

/** Opaque handle of one track (a channel of a device, or a whole device). */
using TrackId = std::uint32_t;

/** The song image, owned by the core and shared by every player of the song. */
struct SongMemory
{
	const void* data = nullptr;
	std::size_t size = 0;
	juce::File file;				// for the extension check and for the UI

	bool isValid (void) const { return data != nullptr && size > 0; }
};

/** One sound device the song uses - the chips card shows these. */
struct ChipInfo
{
	juce::String name;				// "YM2612", "SN76489", ...
	juce::String core;				// the emulation core libvgm picked ("Nuked", "MAME", ...)
	DeviceId deviceId {};
};

/** What MIDI output can be derived from a mixer track. */
enum class MidiCapability
{
	unavailable,	// no MIDI decoder for this chip/channel yet
	melodic,		// the channel has a note and pitch (FM, PSG, square, ...)
	oneShot			// the channel can only trigger a fixed note (ADPCM, DAC, noise, ...)
};

/** One mixer track: a channel of a device, or the whole device when it cannot be split. */
struct TrackInfo
{
	TrackId id {};
	DeviceId deviceId {};
	int deviceIndex = 0;			// devices are numbered in song order; tracks of one device group
	int channelIndex = -1;			// -1 = the whole device (it cannot be isolated any further)
	bool isolatable = true;			// false: the backend cannot render this device channel by channel
	juce::String name;				// "Pulse 1" / "Ch 3" / "All"
	juce::String groupName;			// the group header above the strip row (device name, "#2" ...)
	MidiCapability midiCapability = MidiCapability::unavailable;
};

/** Metadata and timeline facts of an open song, in seconds. */
struct SongInfo
{
	juce::String formatName;					// the engine that opened the file ("VGMPlayer")
	/** Play length with the current loop setting; `loopsForever` songs report one pass. */
	double durationSeconds = 0.0;
	/** One playthrough, independent of the loop setting (excludes the fade tail). */
	double singlePassSeconds = 0.0;
	/** Length of the loop, or 0 when the song has no loop point. */
	double loopLengthSeconds = 0.0;
	/** Position the seek bar jumps back to, or < 0 when the song has no loop point. */
	double loopPointSeconds = -1.0;
	/** The song only ends when the user stops it (the backend reports an infinite runtime). */
	bool loopsForever = false;
	std::map<juce::String, juce::String> tags;
	std::vector<ChipInfo> chips;
};

/** How a backend achieves "one instance per track". */
enum class IsolationMode
{
	/** Every track gets its own player instance (the emulation is suspended elsewhere). */
	multiInstance,
	/** One instance outputs one track internally (per-channel gain inside the emulation). */
	inPlaceChannelMix,
	/** No isolation at all - the song is a single track. */
	none
};

/** What the backend can do - the core degrades explicitly instead of assuming. */
struct BackendCapabilities
{
	bool canSeek = false;				// seekSeconds() works
	bool canLoop = false;				// the loopCount of setTimeline() is honoured
	bool canFade = false;				// the fadeSeconds of setTimeline() is honoured
	bool hasLoopPoint = false;			// the song carries a loop point (the seek bar marker)
	bool canMuteChips = false;			// setChipAudible() works (the chips card)

	/** The backend produces a MIDI sequence at all. Off for a backend that emulates rather than
		reads registers - offering a MIDI channel there would promise notes nothing can send. */
	bool canEmitMidi = false;
	IsolationMode isolation = IsolationMode::none;
	bool rendersAtHostRate = false;		// the backend can render at any host sample rate
	double nativeSampleRate = 0.0;		// its own rate when it cannot (0 = does not care)
};

/** One renderable player of a song, isolated to a single track. Created already prepared and
	loaded (ISongInstance::createPlayer()), so the core never has to know how a backend gets
	its bytes - unpacking a container is entirely the backend's business. */
class ITrackPlayer
{
public:
	virtual ~ITrackPlayer (void) = default;

	virtual void start (void) = 0;
	/** Stops advancing the player. A backend that only advances inside render() (libvgm does)
		may implement this as a no-op - the core stops calling render() while it is paused. */
	virtual void pause (void) = 0;
	/** Returns false when the backend cannot seek (see BackendCapabilities::canSeek). */
	virtual bool seekSeconds (double seconds) = 0;
	/** Loop count (0 = forever) and fade time of the last repetition. Message thread. */
	virtual void setTimeline (int loopCount, double fadeSeconds) = 0;

	/** Makes this instance output only `track` (the isolation the mixer is built on). */
	virtual void setTrackAudible (TrackId track, bool audible) = 0;

	/** Scales this instance's output. Formats whose mix has headroom - the NES is the example, its
		five four-bit channels never reach full scale - ask for a boost here rather than at the
		engine's master, so a format default cannot change how another format sounds. Ignored by
		backends that do not need it. */
	virtual void setOutputGain (float linear) { juce::ignoreUnused (linear); }

	/** Fills a snapshot of the chips' live state for display. Backends that cannot describe their
		chips leave it untouched and return false - a panel that shows nothing is better than one
		that shows invented numbers. */
	virtual bool captureState (ChipStateSnapshot& snapshot) const
	{
		juce::ignoreUnused (snapshot);
		return false;
	}
	/** Silences one device (the chips card). No-op when the backend cannot mute chips. */
	virtual void setChipAudible (DeviceId device, bool audible) = 0;

	/** Audio thread: see the real-time contract above. Returns the frames written. */
	virtual int render (float* left, float* right, int numSamples) = 0;

	/** Fills one buffer per track for the same block, for backends that can produce every track in
		a single pass (IsolationMode::inPlaceChannelMix). uffers holds 	rackCount stereo pairs,
		each 
umSamples frames long; the backend writes all of them or none.

		Returns the frames written, or 0 when it cannot render several tracks at once - the core then
		falls back to one player per track, which is what every backend did before this existed.
		Default is 0, so adding it changed nothing for anyone. */
	virtual int renderTracks (float* const* buffers, int trackCount, int numSamples)
	{
		(void) buffers;
		(void) trackCount;
		(void) numSamples;
		return 0;
	}

	/** Playback position in seconds (message thread; the core caches it for the UI). */
	virtual double positionSeconds (void) = 0;
	/** Set by the backend when the song reached its end (plain atomic, read lock-free). */
	virtual bool isFinished (void) const = 0;
};

/** One open song: metadata, track list and the factory for its players. */
class ISongInstance
{
public:
	virtual ~ISongInstance (void) = default;

	/** Metadata and timeline facts - refreshed by setTimeline(), so read it after calling it. */
	virtual const SongInfo& getInfo (void) const = 0;
	/** Optional MIDI event stream for this song; null when the backend has no decoder. */
	virtual const MidiSequence* getMidiSequence (void) const { return nullptr; }
	/** One entry per mixer track; empty when the song has no devices at all. */
	virtual const std::vector<TrackInfo>& getTracks (void) const = 0;
	virtual BackendCapabilities getCapabilities (void) const = 0;

	/** Host sample rate and maximum block size - call once after open(), before createPlayer().
		Returns false (and fills errorMessage) when the song cannot be prepared. */
	virtual bool prepare (double hostSampleRate, int maxBlockSize, juce::String& errorMessage) = 0;

	/** Loop count (0 = forever) and fade seconds; the next getInfo() reflects them and players
		created later already carry them (live players are updated by ITrackPlayer::setTimeline). */
	virtual void setTimeline (int loopCount, double fadeSeconds) = 0;

	/** Creates one player, prepared for the host and with the song loaded. Returns nullptr and
		fills errorMessage when the song cannot be loaded. Called once per track in multiInstance
		mode, once otherwise. */
	virtual std::unique_ptr<ITrackPlayer> createPlayer (juce::String& errorMessage) = 0;
};

/** A format backend: recognises a song image and opens it. */
class ISongBackend
{
public:
	virtual ~ISongBackend (void) = default;

	virtual juce::String getName (void) const = 0;
	/** The file extensions this backend handles (".vgm", ".vgz", ...) - drives the file dialog. */
	virtual juce::StringArray getFileExtensions (void) const = 0;
	/** Cheap check (magic bytes, extension) whether open() has a chance to succeed. */
	virtual bool canOpen (const SongMemory& memory) const = 0;
	/** Opens the song. Returns nullptr and fills errorMessage when it cannot. */
	virtual std::unique_ptr<ISongInstance> open (const SongMemory& memory, juce::String& errorMessage) = 0;
};

} // namespace toybox::playback
