#include "LibvgmSongInstance.h"

#include "X1010Samples.h"
#include "C140Samples.h"
#include "C352Samples.h"
#include "Es5503Samples.h"
#include "Ga20Samples.h"
#include "K005289Samples.h"
#include "MikeySamples.h"
#include "Msm5232Samples.h"
#include "SegaPcmSamples.h"
#include "OpnAdpcmSamples.h"
#include "OpnDeltaTSamples.h"
#include "Y8950Samples.h"
#include "Ym2612DacSamples.h"
#include "Ymz280bSamples.h"
#include "K007232Samples.h"
#include "GameBoySamples.h"
#include "WonderswanSamples.h"
#include "SccSamples.h"
#include "HuC6280Samples.h"
#include "VsuSamples.h"
#include "MultiPcmSamples.h"
#include "Okim6295Samples.h"
#include "QSoundSamples.h"
#include "K053260Samples.h"
#include "K054539Samples.h"
#include "NesDpcmBank.h"

#include "LibvgmMuteIsolation.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>

#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include "LibvgmVgz.h"
#include "MidiSequenceBuilder.h"
#include "formats/vgm/VgmSongLoader.h"
#include "midi/ChipPlans.h"
#include "settings/SettingsStore.h"

namespace toybox::backends::libvgm
{

namespace
{
	/** The emulation core is reported as a four character code - show it as text. */
	juce::String fccToString (uint32_t code)
	{
		const auto fcc = (const char*) &code;
		juce::String result;
		for (int i = 0; i < 4; i ++)
		{
			if (fcc[i] == 0)
			{
				break;
			}
			result += juce::String::charToString ((juce::juce_wchar) (unsigned char) fcc[i]);
		}
		return result.trim();
	}

	/** Linked devices are an implementation detail of the cores (they are configured through
		their parent's options), so they never become tracks of their own. */
	bool isTrackDevice (const PLR_DEV_INFO& info)
	{
		return info.parentIdx == (uint32_t) -1;
	}

	/** What the MIDI decoder will be able to do for one channel of a device. This is deliberately
	/** Which MIDI chip a libvgm device id names.

		This is the seam SongIr.h describes: the MIDI side knows `ChipKind` and nothing about file
		formats, and every backend translates its own numbering once - here, for VGM. Two ids can
		name one kind (the 219 ASIC is a cut-down C140 and reports through the same decoder), and a
		device with no pitch at all maps to `unknown`, whose plan is empty.

		This switch is a translation, not a table: the left-hand values belong to libvgm and exist
		nowhere else in this project, so there is nothing for the two sides to drift apart into.
	*/
	midi::ChipKind chipKindForDevice (uint32_t deviceId)
	{
		using midi::ChipKind;

		switch (deviceId)
		{
		case DEVID_AY8910:      return ChipKind::ay8910;
		case DEVID_SN76496:     return ChipKind::sn76489;
		case DEVID_SAA1099:     return ChipKind::saa1099;
		case DEVID_POKEY:       return ChipKind::pokey;
		case DEVID_YM2413:      return ChipKind::ym2413;
		case DEVID_YM3812:      return ChipKind::ym3812;
		case DEVID_YM3526:      return ChipKind::ym3526;
		case DEVID_Y8950:       return ChipKind::y8950;
		case DEVID_YMF262:      return ChipKind::ymf262;
		case DEVID_YMF278B:     return ChipKind::ymf278b;
		case DEVID_YM2612:      return ChipKind::ym2612;
		case DEVID_YM2203:      return ChipKind::ym2203;
		case DEVID_YM2608:      return ChipKind::ym2608;
		case DEVID_YM2610:      return ChipKind::ym2610;
		case DEVID_YM2151:      return ChipKind::ym2151;
		case DEVID_YMF271:      return ChipKind::ymf271;
		case DEVID_YMZ280B:     return ChipKind::ymz280b;
		case DEVID_C140:        return ChipKind::c140;
		case DEVID_C219:        return ChipKind::c140;      // the 219 ASIC is a cut-down C140
		case DEVID_C352:        return ChipKind::c352;
		case DEVID_ES5503:      return ChipKind::es5503;
		case DEVID_GA20:        return ChipKind::ga20;
		case DEVID_K053260:     return ChipKind::k053260;
		case DEVID_K054539:     return ChipKind::k054539;
		case DEVID_YMW258:      return ChipKind::multiPcm;
		case DEVID_MSM6295:     return ChipKind::okim6295;
		case DEVID_QSOUND:      return ChipKind::qsound;
		case DEVID_RF5C68:      return ChipKind::rf5c68;
		case DEVID_SEGAPCM:     return ChipKind::segapcm;
		case DEVID_GB_DMG:      return ChipKind::gameBoy;
		case DEVID_C6280:       return ChipKind::huc6280;
		case DEVID_K005289:     return ChipKind::k005289;
		case DEVID_K007232:     return ChipKind::k007232;
		case DEVID_K051649:     return ChipKind::k051649;
		case DEVID_MIKEY:       return ChipKind::mikey;
		case DEVID_MSM5232:     return ChipKind::msm5232;
		case DEVID_NES_APU:     return ChipKind::nesApu;
		case DEVID_VBOY_VSU:    return ChipKind::vsu;
		case DEVID_WSWAN:       return ChipKind::wonderswan;
		case DEVID_X1_010:      return ChipKind::x1_010;

		// 32X PWM, MSM6258 and uPD7759 have no pitch to report at all. The reasoning is kept in
		// docs/issue; here it is enough that `unknown` carries an empty plan.
		default:                return ChipKind::unknown;
		}
	}

	/** What the MIDI decoder will be able to do for one channel of a device.

		The answer comes from midi/ChipPlans, which is keyed on the chip's kind rather than on
		libvgm's device id. That is what this function needed. Capability used to be a 255-line
		switch over `DEVID_*`, and adding Ay8910Decoder left its entry saying "nothing in this
		backend decodes the SSG" - the table and the decoders were keyed on different vocabularies,
		so nothing connected them. Now a chip whose decoder exists is one edit away from a plan,
		and a test holds every plan to the promise its decoder makes.
	*/
	playback::MidiCapability midiCapabilityFor (const PLR_DEV_INFO& device, int channelIndex)
	{
		if (channelIndex < 0)
		{
			return playback::MidiCapability::unavailable;
		}

		// The one device whose plan depends on a configuration bit rather than on its kind: the
		// non-B YM2610 leaves channels 0 and 3 unwired. Everything else - including the SSG an OPN
		// chip links behind itself, which libvgm numbers after the parent's channels - is a run of
		// channels, and lives in the table.
		if (device.type == DEVID_YM2610 && channelIndex < 6)
		{
			const bool isB = device.devCfg != nullptr && (device.devCfg->flags & 0x01) != 0;
			return (isB || (channelIndex != 0 && channelIndex != 3))
					   ? playback::MidiCapability::melodic
					   : playback::MidiCapability::unavailable;
		}

		return midi::channelPlanFor (chipKindForDevice (device.type)).at (channelIndex);
	}
}

LibvgmSongInstance::LibvgmSongInstance (const playback::SongMemory& image)
	: memory (image), playerMemory (image)
{
}

bool LibvgmSongInstance::unpackImage (juce::String& errorMessage)
{
	if (! isCompressedImage (memory.data, memory.size))
	{
		return true;	// plain image - the players read the core's bytes directly
	}

	if (! flattenCompressedImage (memory.data, memory.size, ownedImage) || ownedImage.getSize() == 0)
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	playerMemory.data = ownedImage.getData();
	playerMemory.size = ownedImage.getSize();
	return true;
}

bool LibvgmSongInstance::prepare (double hostSampleRate, int newMaxBlockSize, juce::String& errorMessage)
{
	sampleRate = (hostSampleRate > 0.0) ? hostSampleRate : 44100.0;
	maxBlockSize = juce::jmax (1, newMaxBlockSize);

	probe = std::make_unique<LibvgmTrackPlayer>();
	probe->prepare (sampleRate, maxBlockSize);
	if (! probe->load (playerMemory, errorMessage))
	{
		probe.reset();
		return false;
	}

	applyTimelineTo (*probe);

	// The probe is started like every rendering player: libvgm only reports the complete device
	// list (and the final loop time) once the player has walked the file once, and the engine
	// reads both from here. The probe is never rendered, so it costs no CPU.
	probe->start();

	enumerate();
	midiSequence = buildMidiSequence (playerMemory.data, playerMemory.size, deviceInfo, tracks,
									  toybox::settings::SettingsStore::getInstance().getTransposeBySampleRoot());
	if (midiSequence == nullptr)
	{
		// The builder only understands VGM today; do not offer a MIDI channel that can never
		// produce an event for S98/DRO/GYM.
		for (auto& track : tracks)
		{
			track.midiCapability = playback::MidiCapability::unavailable;
		}
	}
	// The samples this song starts, taken from the same bytes the players load. A VGM says what it
	// plays in its command stream, so this is one walk with no emulation - unlike the NSF path,
	// which has to run the song to find out.
	//
	// Only when the file actually has a 2A03: the walk reads the whole command stream, and for a
	// file built on other chips that read can only come back empty. `enumerate()` above has
	// already filled deviceInfo.
	const bool hasNes = std::any_of (deviceInfo.begin (), deviceInfo.end (),
									 [] (const PLR_DEV_INFO& device) { return device.type == DEVID_NES_APU; });



	// The same walk the MIDI path makes, kept because the display needs it while the song plays.
	// Failure leaves it null and the panel draws nothing, which is the same answer that path gives
	// for a file it cannot read.
	//
	// Read before the sample collection below, and that order is load-bearing: the C140 collector
	// walks the parsed writes, so asking it before this has run finds nothing and reports a song
	// with no samples - which is exactly what a file whose samples are all C140 looked like.
	{
		midi::SongIr parsed;
		std::string error;

		formats::vgm::VgmSongLoader loader;
		if (loader.load (playerMemory.data, playerMemory.size, parsed, error))
		{
			songIr = std::make_shared<const midi::SongIr> (std::move (parsed));
		}

	}

	// One list, because the menu exports one folder: which chip a sample came from is in its name.
	// Each collector only looks at its own chip's writes, so a file with both gets both.
	std::vector<playback::DecodedSample> collected;

	// NES DPCM reads the song bytes directly, not the register stream, so it has its own signature
	// and is not in the table below.
	if (hasNes)
	{
		collected = collectNesDpcmSamples (playerMemory.data, playerMemory.size);
	}

	// YM2612's DAC block builds the table every track player and the export borrow, so it stays
	// explicit: collecting and producing shared state are the same step here.
	const bool hasYm2612 = songIr != nullptr
		&& std::any_of (songIr->chips.begin (), songIr->chips.end (),
						[] (const midi::ChipSlot& slot) { return slot.kind == midi::ChipKind::ym2612; });

	if (hasYm2612 && songIr != nullptr)
	{
		// The DAC is fed one byte at a time, so there is no ROM to point at. What the file carries is
		// the PCM block the driver streams from - or, when it has none, the pauses in the stream.
		//
		// **One table for the players and the export.** The scan over every stream byte (and the
		// merge with the seeks) runs here, once; the eleven track players and the export borrow it.
		dacTable = dac::buildTable (*songIr, 4u << 20);

		for (auto& sample : collectYm2612DacSamples (*songIr, dacTable.get ()))
		{
			collected.push_back (std::move (sample));
		}
	}
	else
	{
		dacTable.reset ();		// a song without a YM2612 has no table to borrow
	}

	// **One table for the rest.** Each row pairs a chip kind with the collector that reads it; the
	// "is this chip in the song" test and the move-into-`collected` loop appear once instead of
	// nineteen times. The clock or rate a collector needs is resolved inside its row, because that is
	// a fact about the chip - not about the dispatch.
	//
	// `irHas` asks the parsed song rather than `deviceInfo`, and the difference matters: a Namco
	// System 2 rip does not answer DEVID_C140 in libvgm's own list (the board is a C219), so a
	// question asked of `deviceInfo` would say "no" for a file whose rows are drawing waves now.
	auto irHas = [] (const midi::SongIr& ir, midi::ChipKind kind)
	{
		return std::any_of (ir.chips.begin (), ir.chips.end (),
							[kind] (const midi::ChipSlot& slot) { return slot.kind == kind; });
	};
	auto clockOf = [this] (uint16_t type) -> uint32_t
	{
		for (const auto& device : deviceInfo)
			if (device.type == type && device.devCfg != nullptr && device.devCfg->clock > 0)
				return device.devCfg->clock;
		return 0;
	};
	auto rateOf = [this] (uint16_t type) -> double
	{
		for (const auto& device : deviceInfo)
			if (device.type == type && device.smplRate > 0)
				return (double) device.smplRate;
		return 0.0;
	};

	struct Row
	{
		midi::ChipKind kind;
		std::function<std::vector<playback::DecodedSample> (const midi::SongIr&)> collect;
	};
	const Row rows[]
	{
		{ midi::ChipKind::c140,		[] (const midi::SongIr& ir) { return collectC140Samples (ir); } },
		{ midi::ChipKind::c352,		[] (const midi::SongIr& ir) { return collectC352Samples (ir); } },
		{ midi::ChipKind::qsound,	[] (const midi::SongIr& ir) { return collectQSoundSamples (ir); } },
		{ midi::ChipKind::k053260,	[] (const midi::SongIr& ir) { return collectK053260Samples (ir); } },
		{ midi::ChipKind::k054539,	[] (const midi::SongIr& ir) { return collectK054539Samples (ir); } },
		// The ROM arrives as a `0x80` block; the clock turns each voice's pitch into a byte rate.
		{ midi::ChipKind::segapcm,	[&] (const midi::SongIr& ir) { return collectSegaPcmSamples (ir, clockOf (DEVID_SEGAPCM)); } },
		// The OPL2's sample player: ranges named by registers, bytes from a `0x88` block.
		{ midi::ChipKind::y8950,	[] (const midi::SongIr& ir) { return collectY8950Samples (ir); } },
		// Two players in one chip: PCM channels name a range in the `0x91` image, wave channels walk
		// uploaded tables. Both become files - PCM as recordings, tables as cycles (`isCycle`).
		{ midi::ChipKind::x1_010,	[&] (const midi::SongIr& ir)
			{
				auto out = collectX1010Samples (ir);
				auto wave = collectX1010WaveSamples (ir, clockOf (DEVID_X1_010));
				out.insert (out.end (), std::make_move_iterator (wave.begin ()),
							std::make_move_iterator (wave.end ()));
				return out;
			} },
		// The sample ROM arrives as a `0x86` block; the clock turns a voice's fnum into a rate.
		{ midi::ChipKind::ymz280b,	[&] (const midi::SongIr& ir) { return collectYmz280bSamples (ir, clockOf (DEVID_YMZ280B)); } },
		// No memory and no clock: the shape comes from the pitch index, the rate from the key.
		{ midi::ChipKind::msm5232,	[] (const midi::SongIr& ir) { return collectMsm5232Samples (ir, 0); } },
		{ midi::ChipKind::mikey,	[&] (const midi::SongIr& ir) { return collectMikeySamples (ir, clockOf (DEVID_MIKEY)); } },
		// The PROM arrives as a `0xC3` RAM block; the stream only names which of eight waves plays.
		{ midi::ChipKind::k005289,	[&] (const midi::SongIr& ir) { return collectK005289Samples (ir, clockOf (DEVID_K005289)); } },
		{ midi::ChipKind::vsu,		[&] (const midi::SongIr& ir) { return collectVsuSamples (ir, clockOf (DEVID_VBOY_VSU)); } },
		{ midi::ChipKind::huc6280,	[&] (const midi::SongIr& ir) { return collectHuC6280Samples (ir, clockOf (DEVID_C6280)); } },
		// SCC: the clock plus the K052539 layout flag (VGM clock field bit 31 -> `devCfg->flags`).
		{ midi::ChipKind::k051649,	[&] (const midi::SongIr& ir)
			{
				const uint32_t clock = clockOf (DEVID_K051649);
				bool plusMode = false;
				for (const auto& device : deviceInfo)
					if (device.type == DEVID_K051649 && device.devCfg != nullptr)
						plusMode = (device.devCfg->flags & 1u) != 0;
				return collectSccSamples (ir, clock, plusMode);
			} },
		{ midi::ChipKind::wonderswan,	[&] (const midi::SongIr& ir) { return collectWonderswanSamples (ir, clockOf (DEVID_WSWAN)); } },
		{ midi::ChipKind::gameBoy,	[&] (const midi::SongIr& ir) { return collectGameBoySamples (ir, clockOf (DEVID_GB_DMG)); } },
		{ midi::ChipKind::es5503,	[&] (const midi::SongIr& ir) { return collectEs5503Samples (ir, clockOf (DEVID_ES5503)); } },
		// The rate the device reports is the one the render used (OKIM6295 taught this rule); a clock
		// computed here would guess the board's pin-7 strap and play 1.25x fast when wrong.
		{ midi::ChipKind::k007232,	[&] (const midi::SongIr& ir) { return collectK007232Samples (ir, rateOf (DEVID_K007232)); } },
		{ midi::ChipKind::multiPcm,	[&] (const midi::SongIr& ir) { return collectMultiPcmSamples (ir, rateOf (DEVID_YMW258), clockOf (DEVID_YMW258)); } },
		{ midi::ChipKind::ga20,		[&] (const midi::SongIr& ir) { return collectGa20Samples (ir, rateOf (DEVID_GA20), clockOf (DEVID_GA20)); } },
		{ midi::ChipKind::okim6295,	[&] (const midi::SongIr& ir) { return collectOkim6295Samples (ir, rateOf (DEVID_MSM6295), clockOf (DEVID_MSM6295)); } },
	};

	for (const auto& row : rows)
	{
		if (songIr != nullptr && irHas (*songIr, row.kind))
		{
			for (auto& sample : row.collect (*songIr))
			{
				collected.push_back (std::move (sample));
			}
		}
	}

	// **The OPN family is two chips and two collectors.** ADPCM-A and Delta-T share one `0x82`
	// image but read it differently, and the song may carry a YM2610 or a YM2608 - either triggers
	// both. The clock both need is the device's own; either board's value is correct.
	const bool hasOpnAdpcm = songIr != nullptr
		&& std::any_of (songIr->chips.begin (), songIr->chips.end (),
						[] (const midi::ChipSlot& slot)
						{ return slot.kind == midi::ChipKind::ym2610 || slot.kind == midi::ChipKind::ym2608; });

	if (hasOpnAdpcm && songIr != nullptr)
	{
		const uint32_t clock = (std::max) (clockOf (DEVID_YM2610), clockOf (DEVID_YM2608));
		for (auto& sample : collectOpnAdpcmSamples (*songIr, clock)) collected.push_back (std::move (sample));
		// Delta-T takes a block of its own (`0x83` OPNB / `0x81` OPNA) and a per-board page size.
		for (auto& sample : collectOpnDeltaTSamples (*songIr, clock)) collected.push_back (std::move (sample));
	}

	// **RF5C164 contributes no samples, on purpose** - four export models each failed the ear, see
	// design 9.5.2 and docs/research/2026-11-14-rf5c164-post-mortem.md. The collector stays in the
	// tree as the record; this dispatch does not call it.

	if (! collected.empty ())
	{
		dpcmSamples = std::make_shared<const std::vector<playback::DecodedSample>> (
			std::move (collected));
	}

	probe->setSamples (dpcmSamples);
	probe->setReplaySource (songIr, deviceInfo, dacTable);
	probe->setTrackList (&tracks);	// setTrackAudible() resolves the track through this list
	refreshTimelineFacts();
	return true;
}

void LibvgmSongInstance::applyTimelineTo (LibvgmTrackPlayer& player) const
{
	player.setTimeline (loopCount, fadeSeconds);
}

void LibvgmSongInstance::setTimeline (int loops, double fade)
{
	loopCount = juce::jmax (0, loops);
	fadeSeconds = juce::jmax (0.0, fade);

	if (probe != nullptr)
	{
		applyTimelineTo (*probe);
	}

	// the cached facts depend on the loop setting, so they are re-read here - the core asks
	// for getInfo() right after this call
	refreshTimelineFacts();
}

void LibvgmSongInstance::refreshTimelineFacts (void)
{
	if (probe == nullptr || probe->player().GetPlayer() == NULL)
	{
		info.durationSeconds = 0.0;
		info.singlePassSeconds = 0.0;
		info.loopLengthSeconds = 0.0;
		info.loopPointSeconds = -1.0;
		info.loopsForever = false;
		return;
	}

	PlayerA& player = probe->player();

	// libvgm reports a negative length while the song loops forever, which would make the seek
	// bar unwritable (the position would be clamped to 0 on every refresh) - measure one
	// playthrough instead and let the position jump back at every repetition
	const double total = player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
	const double singlePass = juce::jmax (0.0, player.GetTotalTime (PLAYTIME_TIME_PBK));
	info.loopsForever = (total < 0.0);
	info.singlePassSeconds = singlePass;
	info.durationSeconds = info.loopsForever ? singlePass : total;

	// GetLoopTime() is the *length* of the loop, so the loop point is the length of a single
	// playthrough minus that length - the position the seek bar jumps back to. It has to be
	// derived from the *single* pass, not from `duration`: with two loops `duration` already
	// contains one repeat, and the marker would sit a whole loop too late.
	const double loopLength = juce::jmax (0.0, player.GetLoopTime());
	info.loopLengthSeconds = loopLength;
	info.loopPointSeconds = (loopLength > 0.0) ? juce::jmax (0.0, singlePass - loopLength) : -1.0;
}

/** What the probe knows about a song, in this project's vocabulary.

	libvgm answers in its own: a device list of DEVID/core/config triples, channel counts obtained by
	calling into the core declaration, names from SndEmu_GetDevName. This is where that becomes
	ChipInfo/TrackInfo, which the engine, the mixer and the MIDI side all speak. */
struct SongDescription
{
	playback::SongInfo info;
	std::vector<playback::TrackInfo> tracks;
	std::vector<PLR_DEV_INFO> devices;
};

SongDescription describeSong (LibvgmTrackPlayer* probe)
{
	SongDescription result;

	if (probe == nullptr)
	{
		return result;
	}

	if (probe == nullptr)
	{
		return result;
	}

	PlayerBase* pBase = probe->player().GetPlayer();
	if (pBase == NULL)
	{
		return result;
	}

	result.info.formatName = juce::String (pBase->GetPlayerName());

	// --- tags: the flat tag/value list the player offers ------------------------------
	const char* const* tagList = pBase->GetTags();
	if (tagList != NULL)
	{
		for (const char* const* tag = tagList; *tag != NULL; tag += 2)
		{
			if (tag[1] == NULL || tag[1][0] == '\0')
			{
				continue;
			}
			result.info.tags[juce::String (tag[0])] = juce::String::fromUTF8 (tag[1]);
		}

		// prefer the untranslated tags over the Japanese ones
		for (auto it = result.info.tags.begin(); it != result.info.tags.end();)
		{
			if (it->first.endsWith ("-JPN") && result.info.tags.count (it->first.dropLastCharacters (4)) > 0)
			{
				it = result.info.tags.erase (it);
			}
			else
			{
				++it;
			}
		}
	}

	// --- devices and their channels ---------------------------------------------------
	std::vector<PLR_DEV_INFO> devices;
	pBase->GetSongDeviceInfo (devices);
	result.devices = devices;

	std::map<uint32_t, int> instancesByType;
	for (const auto& device : devices)
	{
		if (isTrackDevice (device))
		{
			++instancesByType[(uint32_t) device.type];
		}
	}

	// the chips card: one entry per device (the second half of a T6W28 would only repeat its
	// group, so it stays hidden here - the mixer *must* keep it, its channels carry audio)
	for (const auto& device : devices)
	{
		if (! isTrackDevice (device))
		{
			continue;
		}
		if (device.type == DEVID_SN76496 && (device.devCfg->flags & 0x01) && (device.instance & 0x01))
		{
			continue;
		}

		playback::ChipInfo chip;
		const char* chipName = SndEmu_GetDevName (device.type, 0x01, device.devCfg);
		chip.name = juce::String (chipName != NULL ? chipName : "?");
		chip.core = fccToString (device.core);
		chip.deviceId = device.id;
		result.info.chips.push_back (chip);
	}

	// the mixer tracks: one per channel, or one for the whole device when the core the song
	// picked cannot mute single channels (without it the channels cannot be isolated, and
	// duplicating the device's audio across tracks would be wrong)
	int deviceIndex = 0;

	/** Whether a device's core can mute single channels - and therefore whether its voices can be
		tracks at all. */
	const auto isIsolatable = [] (const PLR_DEV_INFO& info)
	{
		if (info.devDecl == nullptr)
		{
			return false;
		}

		for (const DEV_DEF* const* core = info.devDecl->cores; *core != nullptr; ++core)
		{
			if ((*core)->coreID == info.core)
			{
				return (*core)->SetMuteMask != nullptr;
			}
		}

		return false;
	};

	for (size_t headIndex = 0; headIndex < devices.size(); ++headIndex)
	{
		const auto& device = devices[headIndex];

		if (! isTrackDevice (device))
		{
			continue;
		}

		const DEV_DECL* decl = device.devDecl;
		const int channelCount = channelCountOf (device);
		const char** channelNames = (decl != nullptr && decl->channelNames != nullptr)
									  ? decl->channelNames (device.devCfg) : nullptr;

		juce::String groupName = [&device]
		{
			const char* deviceName = SndEmu_GetDevName (device.type, 0x01, device.devCfg);
			return (deviceName != nullptr) ? juce::String (deviceName) : juce::String ("Device");
		}();
		if (instancesByType[(uint32_t) device.type] > 1)
		{
			const int instanceNumber = (device.instance == 0xFFFF) ? 1 : (int) device.instance + 1;
			groupName << " #" << instanceNumber;
		}

		const bool isolatable = isIsolatable (device);

		// `sourceId` and `deviceChannel` are what identify the voice: `device.id` is the head's, which a
		// linked voice set shares, and `channelIndex` is the song-wide number. Both are known here and
		// nowhere downstream, so they are written down rather than reconstructed later.
		const auto addTrack = [&result, &device, deviceIndex, &groupName] (int channelIndex,
																			const juce::String& name,
																			bool trackIsolatable,
																			playback::DeviceId sourceId,
																			int deviceChannel,
																			uint16_t sourceType)
		{
			playback::TrackInfo track;
			track.id = (playback::TrackId) result.tracks.size();	// the handle the core passes back
			track.deviceId = device.id;
			track.deviceIndex = deviceIndex;
			track.channelIndex = channelIndex;
			track.isolatable = trackIsolatable;
			track.sourceDeviceId = sourceId;
			track.deviceChannel = deviceChannel;
			track.sourceDeviceType = sourceType;
			track.name = name;
			track.groupName = groupName;
			track.midiCapability = midiCapabilityFor (device, channelIndex);
			result.tracks.push_back (std::move (track));
		};

		if (! isolatable)
		{
			addTrack (-1, "All", false, device.id, -1, device.type);
		}
		else
		{
			for (int channel = 0; channel < channelCount; ++channel)
			{
				const juce::String name = (channelNames != nullptr && channelNames[channel] != nullptr)
											? juce::String (channelNames[channel])
											: ("Ch " + juce::String (channel + 1));
				addTrack (channel, name, true, device.id, channel, device.type);
			}
		}

		// A linked device that is a voice set of its own - the SSG behind an OPN chip - gets its
		// channels as tracks, numbered after the parent's so the parent's indices do not move: a
		// session saved before this change keeps its per-track gains and mutes.
		//
		// Leaving the SSG out of the layout and trusting the mask to keep it quiet is what this
		// replaces, and the mask does not (docs/memo/2026-10-25-linked-devices-and-channel-numbering.md).
		int linkedBase = channelCount;

		for (const auto& child : devices)
		{
			if (child.parentIdx != (uint32_t) headIndex
				|| linkedDeviceKindFor (child.type) != LinkedDeviceKind::extraVoices)
			{
				continue;
			}

			const int childChannels = channelCountOf (child);
			const bool childIsolatable = isIsolatable (child);

			for (int channel = 0; channel < childChannels; ++channel)
			{
				addTrack (linkedBase + channel, "SSG " + juce::String (channel + 1), childIsolatable,
						 child.id, channel, child.type);
			}

			linkedBase += childChannels;
		}

		++deviceIndex;
	}
	return result;
}
void LibvgmSongInstance::enumerate (void)
{
	const SongDescription described = describeSong (probe.get());

	info = described.info;
	tracks = described.tracks;
	deviceInfo = described.devices;
}

playback::BackendCapabilities LibvgmSongInstance::getCapabilities (void) const
{
	playback::BackendCapabilities caps;
	caps.canSeek = true;
	caps.canLoop = true;
	caps.canFade = true;
	caps.hasLoopPoint = (info.loopPointSeconds >= 0.0);
	caps.canMuteChips = ! info.chips.empty();
	caps.canEmitMidi = true;		// the register stream is parsed into a sequence at load
	caps.isolation = playback::IsolationMode::multiInstance;
	caps.rendersAtHostRate = true;		// the players are built at the host rate (and rebuilt when it changes)
	caps.nativeSampleRate = 0.0;
	return caps;
}

std::unique_ptr<playback::ITrackPlayer> LibvgmSongInstance::createPlayer (juce::String& errorMessage)
{
	auto player = std::make_unique<LibvgmTrackPlayer>();
	player->prepare (sampleRate, maxBlockSize);

	if (! player->load (playerMemory, errorMessage))
	{
		return nullptr;
	}

	player->setTimeline (loopCount, fadeSeconds);
	player->setSamples (dpcmSamples);
	player->setReplaySource (songIr, deviceInfo, dacTable);
	player->setTrackList (&tracks);
	return player;
}

} // namespace toybox::backends::libvgm
