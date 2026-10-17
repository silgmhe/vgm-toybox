#pragma once

#include <vector>

#include <juce_core/juce_core.h>

#include "playback/OutputRouting.h"

namespace toybox::playback
{

/** One track's values as they appear in the project state. `index` is the position in the track
	list the song produced, which is the only identity that survives a save/restore (the chord
	names and players are rebuilt from the song). */
struct SessionTrackState
{
	size_t index = 0;
	float gain = 1.0f;
	float pan = 0.0f;
	bool mute = false;
	bool solo = false;
	OutputTarget output = kMasterOutput;
};

/**
	Everything the host project keeps about a VGMToybox instance.

	The song travels **as data**, not as a path: a project has to open on a machine that never saw
	the file, and a host project cannot share a chunk between instances - so every instance carries
	its own copy. `songName` is only the file name (the extension picks the backend), never an
	absolute path, which would leak the directory layout and no longer load anything.

	The shape is plain values on purpose: it is filled under the engine lock and encoded outside it,
	which is what keeps the slow part (base64 + gzip) off the audio thread and off the lock.
*/
struct SessionState
{
	/** Bumped when the meaning of a field changes. Older or unknown versions are rejected - there
		is no migration path, and none is wanted: see docs/review/2026-10-17. */
	static constexpr int currentVersion = 2;

	int loopCount = 1;
	double fadeSeconds = 3.0;
	float volume = 1.0f;			// the output volume (the engine's last gain stage)

	// DAW sync (one-way, see DawSync.h). The cue is a position on the host's timeline, so it is
	// still meaningful when the project is reopened; a negative value means "no cue set".
	bool syncEnabled = false;
	double syncCueSeconds = -1.0;
	double syncCueOffsetMs = 0.0;
	double syncKnobRangeMs = 10.0;

	bool mixerEnabled = true;
	float mixerMasterGain = 1.0f;
	bool masterMuted = false;
	OutputTarget masterOutput = 0;
	DefaultRouting defaultRouting = DefaultRouting::master;

	juce::String songName;			// file name only - the extension is what matters
	juce::MemoryBlock songImage;	// the bytes as they were loaded (a .vgz stays packed)
	juce::int64 songSize = 0;		// the original size, checked against the decoded image
	/** False when the song was left out because it is over kMaxEmbeddedSongBytes. */
	bool songEmbedded = true;

	std::vector<SessionTrackState> tracks;
};

/** Songs larger than this are not embedded in the state (16 MB; the report walks through the
	memory cost of the 4x copies a save makes). Such a project restores everything but the song. */
inline constexpr int kMaxEmbeddedSongBytes = 16 * 1024 * 1024;

/** Writes the state into an XML element (the schema is documented in the report, §6 H-1). */
void writeSessionState (const SessionState& state, juce::XmlElement& xml);

/** Reads what writeSessionState() wrote. Missing attributes and unknown children keep their
	defaults, so a newer file still loads in an older build - only a different `version` is
	refused. Returns false and fills errorMessage when the XML is not a usable state. */
bool readSessionState (const juce::XmlElement& xml, SessionState& state, juce::String& errorMessage);

/** XML -> single line -> gzip. The result is what goes into the host's state chunk. */
juce::MemoryBlock encodeSessionState (const SessionState& state);

/** The reverse. Anything that is not a gzip stream carrying a version-2 XML is refused: that
	covers the old (version 1) blob format, which is deliberately not migrated. */
bool decodeSessionState (const void* data, int sizeInBytes, SessionState& state,
						 juce::String& errorMessage);

} // namespace toybox::playback
