#include "NesBackend.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "backends/nes/NsfApuChip.h"
#include "backends/nes/NsfN163Chip.h"
#include "backends/nes/NsfVrc6Chip.h"
#include "backends/nes/NsfVrc7Chip.h"
#include "formats/ftm/FtmSongIr.h"
#include "formats/ftm/FtmSongLoader.h"

namespace toybox::backends::nes
{

namespace
{

/** The rate the module compiler works in; every write carries a sample time at this rate. */
constexpr double kStreamRate = 44100.0;

/** One mixer track: which chip and which of its voices. The 2A03 is split further, because two of
	its five voices live in one core and three in another. */
struct TrackMap
{
	int chip = 0;			// index into SongIr::chips
	int bit = 0;			// voice index within the chip
};

/** The chips an open module needs, created from what the compiler reported. */
class ChipSet
{
public:
	void prepare (const midi::SongIr& song, double sampleRate, int maxBlockSize)
	{
		(void) maxBlockSize;

		for (const auto& slot : song.chips)
		{
			switch (slot.kind)
			{
			case midi::ChipKind::nesApu:
				apu.reset (new NsfApuChip (sampleRate));

				for (const auto& rom : song.ownedRoms)
				{
					if (rom.memoryType == 0x08 && ! rom.bytes.empty())
					{
						apu->setDpcmRom (rom.bytes.data(), rom.bytes.size());
					}
				}
				break;

			case midi::ChipKind::nesVrc6:
				vrc6.reset (new NsfVrc6Chip (sampleRate));
				break;

			case midi::ChipKind::nesVrc7:
				vrc7.reset (new NsfVrc7Chip (sampleRate));
				break;

			case midi::ChipKind::nesN163:
				n163.reset (new NsfN163Chip (sampleRate));
				break;

			default:
				break;
			}
		}
	}

	void write (const toybox::chips::RegisterWrite& entry)
	{
		const int address = ((int) entry.port << 8) | entry.reg;
		const int value = entry.value & 0xFF;

		if (entry.deviceId == 0)
		{
			if (apu != nullptr)
			{
				apu->write (0x4000 + entry.reg, value);
			}

			return;
		}

		if (vrc6 != nullptr)
		{
			vrc6->write (address, value);
		}
		else if (vrc7 != nullptr)
		{
			vrc7->write (address, value);
		}
		else if (n163 != nullptr)
		{
			n163->write (address, value);
		}
	}

	/** Mutes every voice of `chip` except `mask`, and silences the other chips entirely. */
	void isolate (int chipIndex, int mask)
	{
		const int allOn = 0x1F;

		apu_mask = (chipIndex == 0) ? mask : allOn;
		expansion_mask = (chipIndex == 1) ? mask : allOn;

		applyMasks();
	}

	void clearIsolation (void)
	{
		apu_mask = 0x1F;
		expansion_mask = 0x1F;
		applyMasks();
	}

	void setChipAudible (int chipIndex, bool audible)
	{
		if (chipIndex == 0)
		{
			apu_audible = audible;
		}
		else
		{
			expansion_audible = audible;
		}
	}

	void render (float* left, float* right, int samples)
	{
		if (apu != nullptr && apu_audible)
		{
			apu->render (left, right, samples);
		}

		if (expansion_audible)
		{
			if (vrc6 != nullptr)
			{
				vrc6->render (left, right, samples);
			}
			else if (vrc7 != nullptr)
			{
				vrc7->render (left, right, samples);
			}
			else if (n163 != nullptr)
			{
				n163->render (left, right, samples);
			}
		}
	}

private:
	void applyMasks (void)
	{
		if (apu != nullptr)
		{
			// Two squares in one core, three more voices in the other: the same mask is cut in two.
			apu->setMask (apu_mask & 0x03, (apu_mask >> 2) & 0x07);
		}

		if (vrc6 != nullptr)
		{
			vrc6->setMask (expansion_mask);
		}
		else if (vrc7 != nullptr)
		{
			vrc7->setMask (expansion_mask);
		}
		else if (n163 != nullptr)
		{
			n163->setMask (expansion_mask);
		}
	}

	std::unique_ptr<NsfApuChip> apu;
	std::unique_ptr<NsfVrc6Chip> vrc6;
	std::unique_ptr<NsfVrc7Chip> vrc7;
	std::unique_ptr<NsfN163Chip> n163;

	int apu_mask = 0x1F;
	int expansion_mask = 0x1F;
	bool apu_audible = true;
	bool expansion_audible = true;
};

/** One renderable module: the chip set plus a cursor into the write stream. */
class NesTrackPlayer final : public playback::ITrackPlayer
{
public:
	NesTrackPlayer (std::shared_ptr<const midi::SongIr> ir, std::vector<TrackMap> map,
					double sampleRate, int maxBlockSize)
		: song (std::move (ir)), trackMap (std::move (map))
	{
		(void) maxBlockSize;

		chips.prepare (*song, sampleRate, maxBlockSize);
		endSample = (std::int64_t) song->totalTicks;
	}

	void start (void) override { running = true; }

	void pause (void) override { running = false; }

	bool seekSeconds (double) override { return false; }

	void setTimeline (int, double) override {}

	void setTrackAudible (playback::TrackId track, bool audible) override
	{
		// The id encodes the chip and the voice, so isolation is a mask rather than a search.
		if (! audible)
		{
			isolated = false;
			chips.clearIsolation();
			return;
		}

		isolated = true;
		myChip = (int) (track >> 8);
		myBit = (int) (track & 0xFF);

		chips.isolate (myChip, 1 << myBit);
	}

	void setChipAudible (playback::DeviceId device, bool audible) override
	{
		chips.setChipAudible ((int) device, audible);
	}

	int render (float* left, float* right, int numSamples) override
	{
		if (! running || finished.load())
		{
			return 0;
		}

		std::fill (left, left + numSamples, 0.0f);
		std::fill (right, right + numSamples, 0.0f);

		int done = 0;

		while (done < numSamples)
		{
			const std::int64_t position = cursor + done;

			if (position >= endSample)
			{
				break;
			}

			// Apply every write that has come due, then render up to the next one - so a write lands
			// on the sample it was timestamped for rather than at a block boundary.
			while (next < song->writes.size()
				   && (std::int64_t) song->writes[next].sampleTime <= position)
			{
				chips.write (song->writes[next]);
				++next;
			}

			int limit = numSamples - done;

			if (next < song->writes.size())
			{
				const std::int64_t until = (std::int64_t) song->writes[next].sampleTime - position;
				limit = (int) std::min<std::int64_t> (limit, std::max<std::int64_t> (until, 1));
			}

			limit = (int) std::min<std::int64_t> (limit, endSample - position);

			chips.render (left + done, right + done, limit);
			done += limit;
		}

		cursor += numSamples;

		if (cursor >= endSample)
		{
			finished.store (true);
		}

		return done;
	}

	double positionSeconds (void) override { return (double) cursor / kStreamRate; }

	bool isFinished (void) const override { return finished.load(); }

private:
	std::shared_ptr<const midi::SongIr> song;
	std::vector<TrackMap> trackMap;
	ChipSet chips;
	std::size_t next = 0;
	std::int64_t cursor = 0;
	std::int64_t endSample = 0;
	bool running = false;
	bool isolated = false;
	int myChip = 0;
	int myBit = 0;
	std::atomic<bool> finished { false };
};

/** One open module: what it is, what it uses, and the factory for its players. */
class NesSongInstance final : public playback::ISongInstance
{
public:
	explicit NesSongInstance (const playback::SongMemory& memory) : image (memory) {}

	bool parse (juce::String& errorMessage)
	{
		std::string error;

		if (! formats::ftm::parseModule (image.data, image.size, module, error))
		{
			errorMessage = juce::String (error);
			return false;
		}

		auto loaded = std::make_shared<midi::SongIr>();

		if (! formats::ftm::loadSongIr (image.data, image.size, *loaded, error))
		{
			errorMessage = juce::String (error);
			return false;
		}

		song = std::move (loaded);

		info.formatName = "FamiTracker";
		info.singlePassSeconds = (double) song->totalTicks / kStreamRate;
		info.durationSeconds = info.singlePassSeconds;
		info.loopPointSeconds = -1.0;
		info.loopLengthSeconds = 0.0;
		info.loopsForever = false;

		if (! module.title.empty())
		{
			info.tags["title"] = juce::String (module.title);
		}

		if (! module.author.empty())
		{
			info.tags["artist"] = juce::String (module.author);
		}

		for (const auto& slot : song->chips)
		{
			playback::ChipInfo chip;
			chip.name = chipName (slot.kind);
			chip.core = "NSFPlay";
			chip.deviceId = (playback::DeviceId) info.chips.size();
			info.chips.push_back (chip);
		}

		buildTracks();
		return true;
	}

	bool prepare (double hostSampleRate, int maxBlockSize, juce::String&) override
	{
		sampleRate = hostSampleRate > 0.0 ? hostSampleRate : kStreamRate;
		blockSize = maxBlockSize > 0 ? maxBlockSize : 512;
		return true;
	}

	const playback::SongInfo& getInfo (void) const override { return info; }

	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks; }

	playback::BackendCapabilities getCapabilities (void) const override
	{
		playback::BackendCapabilities caps;
		caps.canSeek = false;
		caps.canLoop = false;
		caps.canFade = false;
		caps.hasLoopPoint = false;
		caps.canMuteChips = true;
		caps.isolation = playback::IsolationMode::multiInstance;
		caps.rendersAtHostRate = true;
		caps.nativeSampleRate = kStreamRate;
		return caps;
	}

	void setTimeline (int, double) override {}

	std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String&) override
	{
		return std::make_unique<NesTrackPlayer> (song, trackMap, sampleRate, blockSize);
	}

private:
	static juce::String chipName (midi::ChipKind kind)
	{
		switch (kind)
		{
		case midi::ChipKind::nesApu:	return "2A03";
		case midi::ChipKind::nesVrc6:	return "VRC6";
		case midi::ChipKind::nesVrc7:	return "VRC7";
		case midi::ChipKind::nesN163:	return "N163";
		default:						return "Expansion";
		}
	}

	static juce::String channelName (int type)
	{
		switch (type)
		{
		case 0:		return "Pulse 1";
		case 1:		return "Pulse 2";
		case 2:		return "Triangle";
		case 3:		return "Noise";
		case 4:		return "DPCM";
		case 5:		return "VRC6 Pulse 1";
		case 6:		return "VRC6 Pulse 2";
		case 7:		return "VRC6 Saw";
		case 11: case 12: case 13: case 14:
		case 15: case 16: case 17: case 18:
			return "N163 " + juce::String (type - 10);
		case 20: case 21: case 22: case 23: case 24: case 25:
			return "VRC7 " + juce::String (type - 19);
		default:
			return "Ch " + juce::String (type);
		}
	}

	/** A channel's place in the mixer: which chip, and which of its voices. */
	static bool mapChannel (int type, TrackMap& out)
	{
		if (type >= 0 && type <= 4)
		{
			// 0,1 are the squares; 2,3,4 are the three voices of the other 2A03 core.
			out.chip = 0;
			out.bit = (type <= 1) ? type : (2 + type);
			return true;
		}

		if (type >= 5 && type <= 7)
		{
			out.chip = 1;
			out.bit = type - 5;
			return true;
		}

		if (type >= 11 && type <= 18)
		{
			out.chip = 1;
			out.bit = type - 11;
			return true;
		}

		if (type >= 20 && type <= 25)
		{
			out.chip = 1;
			out.bit = type - 20;
			return true;
		}

		return false;
	}

	void buildTracks (void)
	{
		for (std::size_t channel = 0; channel < module.channelTypes.size(); ++channel)
		{
			TrackMap map;

			if (! mapChannel (module.channelTypes[channel], map))
			{
				continue;
			}

			playback::TrackInfo track;
			track.id = ((playback::TrackId) map.chip << 8) | (playback::TrackId) map.bit;
			track.deviceId = (playback::DeviceId) map.chip;
			track.deviceIndex = map.chip;
			track.channelIndex = (int) channel;
			track.isolatable = true;
			track.name = channelName (module.channelTypes[channel]);
			track.groupName = (map.chip == 0) ? "2A03" : chipName (song->chips[(std::size_t) 1].kind);
			track.midiCapability = (module.channelTypes[channel] == 3)
									   ? playback::MidiCapability::oneShot
									   : playback::MidiCapability::melodic;

			tracks.push_back (track);
			trackMap.push_back (map);
		}
	}

	playback::SongMemory image;
	formats::ftm::Module module;
	std::shared_ptr<const midi::SongIr> song;
	std::vector<playback::TrackInfo> tracks;
	std::vector<TrackMap> trackMap;
	playback::SongInfo info;
	double sampleRate = kStreamRate;
	int blockSize = 512;
};

} // namespace

bool isFtmImage (const void* data, std::size_t size)
{
	static const char kPlain[] = "FamiTracker Module";
	static const char kFork[] = "Dn-FamiTracker Module";

	const auto* bytes = static_cast<const char*> (data);

	if (bytes == nullptr)
	{
		return false;
	}

	return (size >= sizeof (kPlain) - 1 && std::memcmp (bytes, kPlain, sizeof (kPlain) - 1) == 0)
		   || (size >= sizeof (kFork) - 1 && std::memcmp (bytes, kFork, sizeof (kFork) - 1) == 0);
}

juce::String NesBackend::getName (void) const
{
	return "FamiTracker";
}

juce::StringArray NesBackend::getFileExtensions (void) const
{
	return { ".ftm" };
}

bool NesBackend::canOpen (const playback::SongMemory& memory) const
{
	return isFtmImage (memory.data, memory.size);
}

std::unique_ptr<playback::ISongInstance> NesBackend::open (const playback::SongMemory& memory,
														   juce::String& errorMessage)
{
	auto instance = std::make_unique<NesSongInstance> (memory);

	if (! instance->parse (errorMessage))
	{
		return nullptr;
	}

	return instance;
}

} // namespace toybox::backends::nes
