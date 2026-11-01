#include "ChipWriteRouter.h"

#include "emu/SoundDevs.h"

#include "chips/Ay8910Decoder.h"
#include "chips/C140Decoder.h"
#include "chips/C352Decoder.h"
#include "chips/Es5503Decoder.h"
#include "chips/Ga20Decoder.h"
#include "chips/HuC6280Decoder.h"
#include "chips/K005289Decoder.h"
#include "chips/K007232Decoder.h"
#include "chips/K051649Decoder.h"
#include "chips/K053260Decoder.h"
#include "chips/K054539Decoder.h"
#include "chips/MikeyDecoder.h"
#include "chips/Msm5232Decoder.h"
#include "chips/MultiPcmDecoder.h"
#include "chips/Okim6295Decoder.h"
#include "chips/OplDecoder.h"
#include "chips/OpmDecoder.h"
#include "chips/OpnDecoder.h"
#include "chips/PokeyDecoder.h"
#include "chips/QSoundDecoder.h"
#include "chips/Rf5c68Decoder.h"
#include "chips/Saa1099Decoder.h"
#include "chips/SegaPcmDecoder.h"
#include "chips/Sn76489Decoder.h"
#include "chips/VsuDecoder.h"
#include "chips/WswanDecoder.h"
#include "chips/X1_010Decoder.h"
#include "chips/Ymf271Decoder.h"
#include "chips/Ymz280bDecoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/NesApuDecoder.h"

namespace toybox::backends::libvgm
{
namespace
{
	uint64_t typeInstanceKey (DEV_ID type, int instance)
	{
		return ((uint64_t) type << 16) | (uint64_t) (instance & 0xFFFF);
	}
}

ChipWriteRouter::ChipWriteRouter (const std::vector<PLR_DEV_INFO>& deviceList)
	: devices (deviceList)
{
	// Copying the list is what makes the pointers below safe: a reference would leave them aimed at
	// storage someone else could reallocate, and the router outlives the call that built it.
	for (const auto& device : devices)
	{
		if (device.parentIdx != (uint32_t) -1)
		{
			continue;
		}

		deviceByType[typeInstanceKey (device.type, device.instance)] = &device;
		if (deviceByType.count (typeInstanceKey (device.type, 0)) == 0)
		{
			deviceByType[typeInstanceKey (device.type, 0)] = &device;
		}
	}
}

const PLR_DEV_INFO* ChipWriteRouter::deviceFor (DEV_ID type, int instance) const
{
	const auto exact = deviceByType.find (typeInstanceKey (type, instance));
	if (exact != deviceByType.end())
	{
		return exact->second;
	}

	const auto first = deviceByType.find (typeInstanceKey (type, 0));
	return first == deviceByType.end() ? nullptr : first->second;
}

const PLR_DEV_INFO* ChipWriteRouter::rf5cDeviceFor (bool is164, int instance) const
{
	for (const auto& device : devices)
	{
		if (device.parentIdx != (uint32_t) -1 || device.type != DEVID_RF5C68
			|| device.instance != instance || device.devCfg == nullptr)
		{
			continue;
		}

		if (((device.devCfg->flags & 0x01) != 0) == is164)
		{
			return &device;
		}
	}

	// A file that names the chip without the flag that tells the two apart: take the first of the
	// type, which is what every other device lookup does.
	return deviceFor (DEVID_RF5C68, instance);
}

void ChipWriteRouter::queueOpn (DEV_ID type, int instance, uint8_t reg, uint16_t value,
								uint32_t time, uint8_t port)
{
	const PLR_DEV_INFO* device = deviceFor (type, instance);

	constexpr uint8_t kLastSsgRegister = 0x0F;	// 0x00..0x0F are the AY's file

	if (port == 0 && reg <= kLastSsgRegister)
	{
		if (onSsgWrite != nullptr)
		{
			onSsgWrite (device, reg, (uint8_t) value, time);
		}

		return;
	}

	decoders.decoder<chips::OpnDecoder> ().queue (device, reg, value, time, port);
}

void ChipWriteRouter::route (const midi::SongIr& ir, const chips::RegisterWrite& w)
{
	const auto& slot = ir.chips[w.deviceId];
	const int instance = slot.instance;
	const auto port = (uint8_t) w.port;
	const auto reg = w.reg;
	const auto value = w.value;
	const auto time = w.sampleTime;

	// The three chips with a buffer of their own. They are not "special cases" in the sense of
	// needing different registers - they need a whole song before their state means anything, and
	// that reason belongs to whoever is extracting notes, not to the routing.
	const auto sendToSeparateBuffer = [&] (const std::function<void (const PLR_DEV_INFO*, uint8_t, uint8_t,
																   uint32_t)>& sink,
										   DEV_ID type)
	{
		if (sink == nullptr)
		{
			return;
		}

		if (const PLR_DEV_INFO* device = deviceFor (type, instance))
		{
			sink (device, reg, (uint8_t) value, time);
		}
	};

	switch (slot.kind)
	{
	case midi::ChipKind::sn76489:
		decoders.decoder<chips::Sn76489Decoder> ().queue (deviceFor (DEVID_SN76496, instance), reg, value, time);
		break;
	case midi::ChipKind::ym2413:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_YM2413, instance), reg, value, time);
		break;
	case midi::ChipKind::ym3812:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_YM3812, instance), reg, value, time);
		break;
	case midi::ChipKind::ym3526:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_YM3526, instance), reg, value, time);
		break;
	case midi::ChipKind::y8950:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_Y8950, instance), reg, value, time);
		break;
	case midi::ChipKind::ymf262:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_YMF262, instance), reg, value, time, port);
		break;
	case midi::ChipKind::ymf278b:
		decoders.decoder<chips::OplDecoder> ().queue (deviceFor (DEVID_YMF278B, instance), reg, value, time, port);
		break;
	case midi::ChipKind::ym2612:
		decoders.decoder<chips::OpnDecoder> ().queue (deviceFor (DEVID_YM2612, instance), reg, value, time, port);
		break;
	// The SSG behind an OPN chip answers registers 0x00..0x0F on port 0; the rest of that port
	// is the FM half. Two decoders, because they are two voice sets: three square channels
	// with their own clock (half the chip's on the YM2608 and YM2610, the full clock on the
	// YM2203) and their own numbering - which is why their notes belong on tracks after the
	// FM ones and not on top of them.
	case midi::ChipKind::ym2203:
		queueOpn (DEVID_YM2203, instance, reg, value, time, port);
		break;
	case midi::ChipKind::ym2608:
		queueOpn (DEVID_YM2608, instance, reg, value, time, port);
		break;
	case midi::ChipKind::ym2610:
		queueOpn (DEVID_YM2610, instance, reg, value, time, port);
		break;
	case midi::ChipKind::ym2151:
		decoders.decoder<chips::OpmDecoder> ().queue (deviceFor (DEVID_YM2151, instance), reg, value, time);
		break;
	case midi::ChipKind::ymf271:
		decoders.decoder<chips::Ymf271Decoder> ().queue (deviceFor (DEVID_YMF271, instance), reg, value, time, port);
		break;
	case midi::ChipKind::ymz280b:
		decoders.decoder<chips::Ymz280bDecoder> ().queue (deviceFor (DEVID_YMZ280B, instance), reg, value, time);
		break;
	case midi::ChipKind::ay8910:
		decoders.decoder<chips::Ay8910Decoder> ().queue (deviceFor (DEVID_AY8910, instance), reg, value, time);
		break;
	case midi::ChipKind::gameBoy:
		sendToSeparateBuffer (onGbWrite, DEVID_GB_DMG);
		break;
	case midi::ChipKind::nesApu:
		sendToSeparateBuffer (onNesWrite, DEVID_NES_APU);
		break;
	case midi::ChipKind::huc6280:
		decoders.decoder<chips::HuC6280Decoder> ().queue (deviceFor (DEVID_C6280, instance), reg, value, time);
		break;
	case midi::ChipKind::k051649:
		decoders.decoder<chips::K051649Decoder> ().queue (deviceFor (DEVID_K051649, instance), reg, value, time, port);
		break;
	case midi::ChipKind::k053260:
		decoders.decoder<chips::K053260Decoder> ().queue (deviceFor (DEVID_K053260, instance), reg, value, time);
		break;
	case midi::ChipKind::k054539:
		decoders.decoder<chips::K054539Decoder> ().queue (deviceFor (DEVID_K054539, instance), reg, value, time, port);
		break;
	case midi::ChipKind::c140:
	{
		// the 219 ASIC answers the same command; the runtime may only have started that one
		const PLR_DEV_INFO* device = deviceFor (DEVID_C140, instance);
		decoders.decoder<chips::C140Decoder> ().queue (device != nullptr ? device : deviceFor (DEVID_C219, 0),
					reg, value, time, port);
		break;
	}
	case midi::ChipKind::c352:
		decoders.decoder<chips::C352Decoder> ().queue (deviceFor (DEVID_C352, instance), reg, value, time, port);
		break;
	case midi::ChipKind::qsound:
		decoders.decoder<chips::QSoundDecoder> ().queue (deviceFor (DEVID_QSOUND, instance), reg, value, time);
		break;
	case midi::ChipKind::ga20:
		decoders.decoder<chips::Ga20Decoder> ().queue (deviceFor (DEVID_GA20, instance), reg, value, time);
		break;
	case midi::ChipKind::es5503:
		decoders.decoder<chips::Es5503Decoder> ().queue (deviceFor (DEVID_ES5503, instance), reg, value, time);
		break;
	case midi::ChipKind::rf5c68:
		decoders.decoder<chips::Rf5c68Decoder> ().queue (rf5cDeviceFor (false, instance), reg, value, time);
		break;
	case midi::ChipKind::rf5c164:
		decoders.decoder<chips::Rf5c68Decoder> ().queue (rf5cDeviceFor (true, instance), reg, value, time);
		break;
	case midi::ChipKind::segapcm:
		decoders.decoder<chips::SegaPcmDecoder> ().queue (deviceFor (DEVID_SEGAPCM, instance), reg, value, time, port);
		break;
	case midi::ChipKind::okim6295:
		decoders.decoder<chips::Okim6295Decoder> ().queue (deviceFor (DEVID_MSM6295, instance), reg, value, time);
		break;
	case midi::ChipKind::saa1099:
		decoders.decoder<chips::Saa1099Decoder> ().queue (deviceFor (DEVID_SAA1099, instance), reg, value, time);
		break;
	case midi::ChipKind::pokey:
		decoders.decoder<chips::PokeyDecoder> ().queue (deviceFor (DEVID_POKEY, instance), reg, value, time);
		break;
	case midi::ChipKind::x1_010:
		decoders.decoder<chips::X1_010Decoder> ().queue (deviceFor (DEVID_X1_010, instance), reg, value, time, port);
		break;
	case midi::ChipKind::wonderswan:
		decoders.decoder<chips::WswanDecoder> ().queue (deviceFor (DEVID_WSWAN, instance), reg, value, time);
		break;
	case midi::ChipKind::multiPcm:
		decoders.decoder<chips::MultiPcmDecoder> ().queue (deviceFor (DEVID_YMW258, instance), reg, value, time);
		break;
	case midi::ChipKind::mikey:
		decoders.decoder<chips::MikeyDecoder> ().queue (deviceFor (DEVID_MIKEY, instance), reg, value, time);
		break;
	case midi::ChipKind::k005289:
		decoders.decoder<chips::K005289Decoder> ().queue (deviceFor (DEVID_K005289, instance), reg, value, time);
		break;
	case midi::ChipKind::k007232:
		decoders.decoder<chips::K007232Decoder> ().queue (deviceFor (DEVID_K007232, instance), reg, value, time);
		break;
	case midi::ChipKind::msm5232:
		decoders.decoder<chips::Msm5232Decoder> ().queue (deviceFor (DEVID_MSM5232, instance), reg, value, time);
		break;
	case midi::ChipKind::vsu:
		decoders.decoder<chips::VsuDecoder> ().queue (deviceFor (DEVID_VBOY_VSU, instance), reg, value, time);
		break;
	default:
		break;
	}
}

} // namespace toybox::backends::libvgm
