#include "FakeBackend.h"

#include <cmath>

#include "playback/BackendRegistry.h"

namespace vgmplay::backends::fake
{

namespace
{
	constexpr double kSongSeconds = 1.0;		// the fake song is exactly one second long
	constexpr double kLoopPointSeconds = 0.5;
	constexpr float kAmplitude = 0.25f;			// full scale of one fake track

	/** Frequency of the n-th fake track - three clearly different tones. */
	double trackFrequency (int index) { return 220.0 * (double) (index + 1); }

	/**
		One fake track player: a sine tone that can be switched off, so the mixer's isolation,
		gain, pan, mute and solo can all be measured against a known signal.

		The player counts its own position, which makes seek/position checks independent of any
		emulation warm-up behaviour.
	*/
	class FakeTrackPlayer final : public playback::ITrackPlayer
	{
	public:
		FakeTrackPlayer (double hostSampleRate, int trackIndex)
			: sampleRate (hostSampleRate), index (trackIndex), myTone (trackIndex % 3)
		{
		}

		void start (void) override {}
		void pause (void) override {}

		bool seekSeconds (double seconds) override
		{
			position = juce::jmax (0.0, seconds);
			for (auto& tonePhase : phases)
			{
				tonePhase = 0.0;	// a deterministic restart, so level checks stay exact
			}
			return true;
		}

		void setTimeline (int /*loopCount*/, double /*fadeSeconds*/) override {}

		void setTrackAudible (playback::TrackId /*track*/, bool audible) override
		{
			isolated = true;
			audibleNow = audible;
		}

		void setChipAudible (playback::DeviceId /*device*/, bool /*audible*/) override {}

		int render (float* left, float* right, int numSamples) override
		{
			for (int i = 0; i < numSamples; ++i)
			{
				float value = 0.0f;

				// isolated: only this track's tone (and only while it is switched on);
				// un-isolated (plain player mode): the whole fake song, i.e. all three tones
				for (int tone = 0; tone < 3; ++tone)
				{
					if (isolated && (! audibleNow || tone != myTone))
					{
						continue;
					}

					value += kAmplitude * (float) std::sin (phases[(size_t) tone]);
				}

				for (int tone = 0; tone < 3; ++tone)
				{
					double& tonePhase = phases[(size_t) tone];
					tonePhase += juce::MathConstants<double>::twoPi * trackFrequency (tone) / sampleRate;
					if (tonePhase > juce::MathConstants<double>::twoPi)
					{
						tonePhase -= juce::MathConstants<double>::twoPi;
					}
				}

				left[i] = value;
				right[i] = value;
				position += 1.0 / sampleRate;
			}

			return numSamples;
		}

		double positionSeconds (void) override { return position; }
		bool isFinished (void) const override { return position >= kSongSeconds; }

	private:
		double sampleRate = 44100.0;
		int index = 0;
		int myTone = 0;
		double position = 0.0;
		double phases[3] = { 0.0, 0.0, 0.0 };
		bool isolated = false;
		bool audibleNow = true;
	};

	/** One fake song: two devices, three tracks, fixed metadata. */
	class FakeSongInstance final : public playback::ISongInstance
	{
	public:
		FakeSongInstance (void)
		{
			info.formatName = "Fake tones";
			info.durationSeconds = kSongSeconds;
			info.loopPointSeconds = kLoopPointSeconds;
			info.loopsForever = false;
			info.tags["Title"] = "Fake Song";
			info.tags["Artist"] = "VGMPlay tests";

			playback::ChipInfo chipA;
			chipA.name = "Fake Tone A";
			chipA.core = "sine";
			chipA.deviceId = 100;
			info.chips.push_back (chipA);

			playback::ChipInfo chipB;
			chipB.name = "Fake Tone B";
			chipB.core = "sine";
			chipB.deviceId = 200;
			info.chips.push_back (chipB);

			const auto addTrack = [this] (playback::DeviceId device, int deviceIndex,
										  int channelIndex, const juce::String& name,
										  const juce::String& group)
			{
				playback::TrackInfo track;
				track.id = (playback::TrackId) tracks.size();
				track.deviceId = device;
				track.deviceIndex = deviceIndex;
				track.channelIndex = channelIndex;
				track.isolatable = true;
				track.name = name;
				track.groupName = group;
				tracks.push_back (std::move (track));
			};

			addTrack (100, 0, 0, "Tone 1", "Fake Tone A");
			addTrack (100, 0, 1, "Tone 2", "Fake Tone A");
			addTrack (200, 1, 0, "Tone 3", "Fake Tone B");
		}

		bool prepare (double hostSampleRate, int newMaxBlockSize, juce::String& /*errorMessage*/) override
		{
			sampleRate = (hostSampleRate > 0.0) ? hostSampleRate : 44100.0;
			maxBlockSize = juce::jmax (1, newMaxBlockSize);
			return true;
		}

		const playback::SongInfo& getInfo (void) const override { return info; }
		const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks; }

		playback::BackendCapabilities getCapabilities (void) const override
		{
			playback::BackendCapabilities caps;
			caps.canSeek = true;
			caps.canLoop = false;			// deliberately: the core must cope with that too
			caps.canFade = false;
			caps.hasLoopPoint = true;
			caps.canMuteChips = false;
			caps.isolation = playback::IsolationMode::multiInstance;
			caps.rendersAtHostRate = true;
			return caps;
		}

		void setTimeline (int /*loopCount*/, double /*fadeSeconds*/) override {}

		std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String& /*errorMessage*/) override
		{
			return std::make_unique<FakeTrackPlayer> (sampleRate, playerIndex++);
		}

	private:
		double sampleRate = 44100.0;
		int maxBlockSize = 512;
		int playerIndex = 0;
		playback::SongInfo info;
		std::vector<playback::TrackInfo> tracks;
	};
}

bool FakeBackend::canOpen (const playback::SongMemory& memory) const
{
	return memory.file.getFileExtension().equalsIgnoreCase (".fake");
}

std::unique_ptr<playback::ISongInstance> FakeBackend::open (const playback::SongMemory& /*memory*/,
															juce::String& /*errorMessage*/)
{
	return std::make_unique<FakeSongInstance>();
}

} // namespace vgmplay::backends::fake

namespace vgmplay::playback
{
/**
	The composition root of the build that links this file: the core test target links it
	*instead of* Source/backends/BuiltInBackends.cpp, so "the backend this build ships" is the
	fake one there - and no libvgm code is linked at all.
*/
void registerBuiltInBackends (void)
{
	static bool registered = false;
	if (registered)
	{
		return;
	}
	registered = true;

	BackendRegistry::getInstance().add (std::make_unique<backends::fake::FakeBackend>());
}

} // namespace vgmplay::playback
