#include "playback/SessionState.h"

namespace toybox::playback
{

namespace
{

/** The XML text a 16 MB song makes: 4/3 for base64 plus the markup, with room to spare. The cap
	matters because the blob comes from a project file: without it, a crafted chunk could ask for
	gigabytes before anything is parsed. */
constexpr int kMaxStateXmlBytes = 24 * 1024 * 1024;

const char* compatibleVersionMessage = "The project state was saved by an incompatible version.";
const char* corruptStateMessage = "The project state is corrupt.";

juce::String routingToName (DefaultRouting mode)
{
	return (mode == DefaultRouting::spreadAcrossBuses) ? "spreadAcrossBuses" : "master";
}

DefaultRouting routingFromName (const juce::String& name, DefaultRouting fallback)
{
	if (name == "spreadAcrossBuses")
	{
		return DefaultRouting::spreadAcrossBuses;
	}

	return (name == "master") ? DefaultRouting::master : fallback;
}

} // namespace

void writeSessionState (const SessionState& state, juce::XmlElement& xml)
{
	xml.setAttribute ("version", SessionState::currentVersion);
	// plain int throughout: XmlElement's overloads are (int) and (double), so an int64 would be
	// ambiguous between them (both are standard conversions of the same rank)
	xml.setAttribute ("loopCount", state.loopCount);
	xml.setAttribute ("fadeSeconds", state.fadeSeconds);
	xml.setAttribute ("volume", (double) state.volume);
	// optional on read, so an older build ignores them instead of refusing the file
	xml.setAttribute ("syncEnabled", state.syncEnabled ? 1 : 0);
	xml.setAttribute ("syncCue", state.syncCueSeconds);
	xml.setAttribute ("syncCueOffsetMs", state.syncCueOffsetMs);
	xml.setAttribute ("syncKnobRangeMs", state.syncKnobRangeMs);

	// Nothing loaded: no <song> at all, so reading an empty state stays trivially correct.
	const size_t imageSize = state.songImage.getSize();
	if (imageSize > 0)
	{
		juce::XmlElement* const song = xml.createNewChildElement ("song");
		song->setAttribute ("name", state.songName);
		song->setAttribute ("size", (int) imageSize);	// 16 MB at most gets this far

		// Over the limit the song is left out on purpose; the project still carries the rest, and
		// the reading side says why the song is missing.
		const bool embedded = imageSize <= (size_t) kMaxEmbeddedSongBytes;
		song->setAttribute ("embedded", embedded ? 1 : 0);
		if (embedded)
		{
			// base64 is [A-Za-z0-9+/=] only, so it needs no XML escaping - neither as text nor as
			// an attribute (which is why the image rides as text, where a 1 MB file costs nothing
			// extra).
			song->addTextElement (juce::Base64::toBase64 (state.songImage.getData(), imageSize));
		}
	}

	juce::XmlElement* const mixer = xml.createNewChildElement ("mixer");
	mixer->setAttribute ("enabled", state.mixerEnabled ? 1 : 0);
	mixer->setAttribute ("masterGain", (double) state.mixerMasterGain);
	mixer->setAttribute ("masterMuted", state.masterMuted ? 1 : 0);
	mixer->setAttribute ("masterOutput", state.masterOutput);
	mixer->setAttribute ("masterMidiChannel", state.masterMidiChannel);
	mixer->setAttribute ("defaultRouting", routingToName (state.defaultRouting));

	for (const auto& track : state.tracks)
	{
		juce::XmlElement* const element = mixer->createNewChildElement ("track");
		element->setAttribute ("index", (int) track.index);
		element->setAttribute ("gain", (double) track.gain);
		element->setAttribute ("pan", (double) track.pan);
		element->setAttribute ("mute", track.mute ? 1 : 0);
		element->setAttribute ("solo", track.solo ? 1 : 0);
		element->setAttribute ("output", track.output);
		element->setAttribute ("midiChannel", track.midiChannel);
	}
}

bool readSessionState (const juce::XmlElement& xml, SessionState& state, juce::String& errorMessage)
{
	if (! xml.hasTagName ("VGMToyboxState"))
	{
		errorMessage = corruptStateMessage;
		return false;
	}

	if (xml.getIntAttribute ("version", 0) != SessionState::currentVersion)
	{
		errorMessage = compatibleVersionMessage;
		return false;
	}

	// Defaults first: every attribute below is optional, which is the whole extension rule - a
	// newer file loads here minus what this build does not know.
	state = SessionState();
	state.loopCount = xml.getIntAttribute ("loopCount", state.loopCount);
	state.fadeSeconds = xml.getDoubleAttribute ("fadeSeconds", state.fadeSeconds);
	state.volume = (float) xml.getDoubleAttribute ("volume", (double) state.volume);
	state.syncEnabled = xml.getBoolAttribute ("syncEnabled", state.syncEnabled);
	state.syncCueSeconds = xml.getDoubleAttribute ("syncCue", state.syncCueSeconds);
	state.syncCueOffsetMs = xml.getDoubleAttribute ("syncCueOffsetMs", state.syncCueOffsetMs);
	state.syncKnobRangeMs = xml.getDoubleAttribute ("syncKnobRangeMs", state.syncKnobRangeMs);

	if (const juce::XmlElement* const song = xml.getChildByName ("song"))
	{
		state.songName = song->getStringAttribute ("name");
		state.songSize = song->getStringAttribute ("size").getLargeIntValue();
		state.songEmbedded = song->getBoolAttribute ("embedded", true);

		const juce::String encoded = song->getAllSubText().trim();
		if (state.songEmbedded && encoded.isNotEmpty())
		{
			// the name is not decoration: its extension is what picks the backend, so an image
			// without one cannot be played and there is nothing sensible to do with it
			if (state.songName.isEmpty())
			{
				errorMessage = corruptStateMessage;
				return false;
			}

			juce::MemoryOutputStream decoded;
			if (! juce::Base64::convertFromBase64 (decoded, encoded))
			{
				errorMessage = corruptStateMessage;
				return false;
			}

			state.songImage = decoded.getMemoryBlock();

			// A truncated image would only fail later, inside the backend and with a worse
			// message - and a wrong song is not something to guess about.
			if (state.songSize > 0 && (juce::int64) state.songImage.getSize() != state.songSize)
			{
				errorMessage = corruptStateMessage;
				return false;
			}
		}
	}

	if (const juce::XmlElement* const mixer = xml.getChildByName ("mixer"))
	{
		state.mixerEnabled = mixer->getBoolAttribute ("enabled", state.mixerEnabled);
		state.mixerMasterGain = (float) mixer->getDoubleAttribute ("masterGain",
																  (double) state.mixerMasterGain);
		state.masterMuted = mixer->getBoolAttribute ("masterMuted", state.masterMuted);
		state.masterOutput = mixer->getIntAttribute ("masterOutput", state.masterOutput);
		state.masterMidiChannel = juce::jlimit (0, 16,
												mixer->getIntAttribute ("masterMidiChannel", state.masterMidiChannel));
		state.defaultRouting = routingFromName (mixer->getStringAttribute ("defaultRouting"),
												state.defaultRouting);

		for (int i = 0; i < mixer->getNumChildElements(); ++i)
		{
			const juce::XmlElement* const element = mixer->getChildElement (i);
			if (element == nullptr || ! element->hasTagName ("track"))
			{
				continue;
			}

			SessionTrackState track;
			track.index = (size_t) juce::jmax (0, element->getIntAttribute ("index", 0));
			track.gain = (float) element->getDoubleAttribute ("gain", 1.0);
			track.pan = (float) element->getDoubleAttribute ("pan", 0.0);
			track.mute = element->getBoolAttribute ("mute", false);
			track.solo = element->getBoolAttribute ("solo", false);
			track.output = element->getIntAttribute ("output", kMasterOutput);
			track.midiChannel = juce::jlimit (0, 16,
											  element->getIntAttribute ("midiChannel", 0));
			state.tracks.push_back (track);
		}
	}

	return true;
}

juce::MemoryBlock encodeSessionState (const SessionState& state)
{
	juce::XmlElement xml ("VGMToyboxState");
	writeSessionState (state, xml);

	juce::MemoryOutputStream compressed;
	{
		// 31 = 15 | 16: the gzip container, not the zlib stream JUCE asks zlib for by default
		// (windowBits 15). The difference is two header bytes, and those are exactly what the
		// reading side checks, so this is not cosmetic. 15 is written out because juce_core keeps
		// MAX_WBITS to itself.
		juce::GZIPCompressorOutputStream gzip (compressed, 9, 15 | 16);
		xml.writeTo (gzip, juce::XmlElement::TextFormat().singleLine());
		gzip.flush();
	}

	return compressed.getMemoryBlock();
}

bool decodeSessionState (const void* data, int sizeInBytes, SessionState& state,
						 juce::String& errorMessage)
{
	if (data == nullptr || sizeInBytes < 2)
	{
		errorMessage = corruptStateMessage;
		return false;
	}

	// Every state this build writes starts with the gzip magic. The old format (copyXmlToBinary's
	// magic number + length + XML) does not, which is how it is turned away - deliberately, see
	// the report: it is not migrated, and there is no path fallback to fall back to.
	const auto* const bytes = static_cast<const unsigned char*> (data);
	if (bytes[0] != 0x1f || bytes[1] != 0x8b)
	{
		errorMessage = compatibleVersionMessage;
		return false;
	}

	juce::MemoryInputStream source (data, (size_t) sizeInBytes, false);
	juce::GZIPDecompressorInputStream gzip (&source, false,
											juce::GZIPDecompressorInputStream::gzipFormat);

	// Bounded on purpose: the decompressed size is not known up front and must not be trusted.
	juce::MemoryOutputStream raw;
	char buffer[64 * 1024];
	for (;;)
	{
		const int numRead = gzip.read (buffer, (int) sizeof (buffer));
		if (numRead <= 0)
		{
			break;
		}

		raw.write (buffer, (size_t) numRead);
		if (raw.getDataSize() > (size_t) kMaxStateXmlBytes)
		{
			errorMessage = corruptStateMessage;
			return false;
		}
	}

	const int rawSize = (int) raw.getDataSize();
	const auto xml = (rawSize > 0)
						 ? juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (raw.getData()),
																   rawSize))
						 : nullptr;
	if (xml == nullptr)
	{
		errorMessage = corruptStateMessage;
		return false;
	}

	return readSessionState (*xml, state, errorMessage);
}

} // namespace toybox::playback
