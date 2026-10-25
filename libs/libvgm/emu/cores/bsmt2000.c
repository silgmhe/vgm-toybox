// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Paul Leaman, Miguel Angel Horna, Mao, cam900
/***************************************************************************

  Data East BSMT2000
  ==================

  Core for BSMT2000 sound chip, adapted for libvgm, with fixes based on M1/MAME/PinMAME source.

  The BSMT2000 is a custom TMS320C15 DSP with internal ROM and external sample ROM.
  It supports multiple PCM voices and a single ADPCM channel, with stereo output.

  FIXES:
  - Register layout and handling match M1/MAME/PinMAME (9 registers per voice)
  - Position, rate, and loop logic use fixed-point math (16.16)
  - Linear interpolation for PCM voices
  - ADPCM/compressed channel handling is per M1/MAME/PinMAME
  - Voice init matches M1/MAME/PinMAME

  Modifications for PINMAME by Steve Ellenoff & Martin Adrian & Carsten Waechter

  References:
  https://github.com/vpinball/pinmame/blob/master/src/sound/bsmt2000.c
  https://github.com/vpinball/pinmame/blob/master/src/sound/bsmt2000.h
  M1's BSMT2000 source code. https://vgmrips.net/forum/viewtopic.php?t=110
  https://www.researchgate.net/publication/291338452_Hacking_a_Sega_Whitestar_Pinball
  
***************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#include "../../stdtype.h"
#include "../EmuStructs.h"
#include "../SoundDevs.h"
#include "../EmuCores.h"
#include "../snddef.h"
#include "../EmuHelper.h"
#include "../logging.h"
#include "bsmt2000.h"

/* ==== Constants ==== */

#define BSMT2000_CLOCK        24000000    /* default 24MHz clock */
#define BSMT2000_CHANNELS     12          /* up to 12 PCM voices, plus ADPCM */
#define BSMT2000_ADPCM_INDEX  12          /* 0..11 = PCM, 12 = ADPCM/compressed */
#define BSMT2000_REG_CURRPOS  0
#define BSMT2000_REG_RATE 1
#define BSMT2000_REG_LOOPEND     2
#define BSMT2000_REG_LOOPSTART  3
#define BSMT2000_REG_BANK 4
#define BSMT2000_REG_RIGHTVOL     5
#define BSMT2000_REG_LEFTVOL 6
#define BSMT2000_REG_TOTAL    7

#define BSMT2000_MAX_VOICES   (BSMT2000_CHANNELS + 1)  /* 12 PCM + 1 ADPCM/compressed */
#define BSMT2000_SAMPLE_CHUNK 10000

#define BSMT2000_ROM_BANKSIZE 0x10000     /* 64k per bank */

static const uint8_t regmap[8][7] = {
    { 0x00, 0x18, 0x24, 0x30, 0x3c, 0x48, 0xff }, // last one (stereo/leftvol) unused, set to max for mapping
    { 0x00, 0x16, 0x21, 0x2c, 0x37, 0x42, 0x4d },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // mode 2 only a testmode left channel
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // mode 3 only a testmode right channel
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // mode 4 only a testmode left channel
    { 0x00, 0x18, 0x24, 0x30, 0x3c, 0x54, 0x60 },
    { 0x00, 0x10, 0x18, 0x20, 0x28, 0x38, 0x40 },
    { 0x00, 0x12, 0x1b, 0x24, 0x2d, 0x3f, 0x48 } };

/* ==== Internal Voice State ==== */
typedef struct
{
    uint16_t reg[9]; // 9 registers per voice, match MAME/M1
    uint32_t position; // 16.16 fixed-point
    uint32_t loop_start_position;
    uint32_t loop_stop_position;
    uint32_t adjusted_rate;
} bsmt2000_voice;

/* ==== Main Chip State ==== */
typedef struct _bsmt2000_state bsmt2000_state;
struct _bsmt2000_state
{
    DEV_DATA _devData;
    DEV_LOGGER logger;

    // Sample ROM
    uint8_t *sample_rom;
    uint32_t sample_rom_length;
    uint32_t sample_rom_mask;
    uint32_t total_banks;

    // Voices and ADPCM/compressed
    bsmt2000_voice voice[BSMT2000_MAX_VOICES];
    uint8_t right_volume_set; // Right volume is set (from PinMAME)
    uint8_t voices;         // actual number of voices (usually 11 or 12)
    uint8_t stereo;         // stereo output enabled?
    uint8_t adpcm;          // ADPCM/compressed enabled?
    uint8_t mode;           // current mode (0,1,5,6,7)
    uint8_t last_register;  // last register written
    uint16_t latch;         // buffer to combine 8-bit writes to a 16-bit words
    uint32_t clock;         // Chip clock

    // ADPCM/compressed state
    int32_t adpcm_current;
    int32_t adpcm_delta_n;

    // Output sample rate
    double sample_rate;

    // Mute mask
    uint8_t Muted[BSMT2000_MAX_VOICES];

    // Misc
    // (can add more fields as needed)
	DEVCB_SRATE_CHG SmpRateFunc;
	void* SmpRateData;
};

/* ==== Prototypes ==== */
static void bsmt2000_update(void *param, uint32_t samples, DEV_SMPL **outputs);
static uint8_t device_start_bsmt2000(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
static void device_stop_bsmt2000(void *info);
static void device_reset_bsmt2000(void *info);

static void bsmt2000_w(void *info, uint8_t offset, uint8_t data);
static uint8_t bsmt2000_r(void *info, uint8_t offset);
static void bsmt2000_write_data(void *info, uint8_t address, uint16_t data);

static void bsmt2000_alloc_rom(void* info, uint32_t memsize);
static void bsmt2000_write_rom(void *info, uint32_t offset, uint32_t length, const uint8_t* data);
static void bsmt2000_set_mute_mask(void *info, uint32_t MuteMask);
static uint32_t bsmt2000_get_mute_mask(void *info);
static void bsmt2000_set_log_cb(void* info, DEVCB_LOG func, void* param);
static void bsmt2000_set_srchg_cb(void *info, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr);

/* ==== Device Function Table ==== */
static DEVDEF_RWFUNC devFunc[] =
{
    {RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, bsmt2000_w},
    {RWF_REGISTER | RWF_READ, DEVRW_A8D8, 0, bsmt2000_r},
    {RWF_REGISTER | RWF_QUICKWRITE, DEVRW_A8D16, 0, bsmt2000_write_data},
    {RWF_MEMORY | RWF_WRITE, DEVRW_BLOCK, 0, bsmt2000_write_rom},
    {RWF_MEMORY | RWF_WRITE, DEVRW_MEMSIZE, 0, bsmt2000_alloc_rom},
    {RWF_CHN_MUTE | RWF_WRITE, DEVRW_ALL, 0, bsmt2000_set_mute_mask},
    {0x00, 0x00, 0, NULL}
};

/* ==== Device Definition ==== */
DEV_DEF devDef =
{
    "BSMT2000", "MAME", FCC_MAME,

    device_start_bsmt2000,
    device_stop_bsmt2000,
    device_reset_bsmt2000,
    bsmt2000_update,

    NULL, // SetOptionBits
    bsmt2000_set_mute_mask,
    NULL, // SetPanning
    bsmt2000_set_srchg_cb,
    bsmt2000_set_log_cb, // SetLoggingCallback
    NULL, // LinkDevice

    devFunc,    // rwFuncs
};

static const char* DeviceName(const DEV_GEN_CFG* devCfg)
{
	return "BSMT2000";
}

static uint16_t DeviceChannels(const DEV_GEN_CFG* devCfg)
{
	return BSMT2000_MAX_VOICES;
}

static const char** DeviceChannelNames(const DEV_GEN_CFG* devCfg)
{
	static const char* names[BSMT2000_MAX_VOICES] =
	{
		"PCM 1", "PCM 2", "PCM 3", "PCM 4", "PCM 5", "PCM 6", "PCM 7", "PCM 8",
		"PCM 9", "PCM 10", "PCM 11", "PCM 12",
		"ADPCM",
	};
	return names;
}

static const DEVLINK_IDS* DeviceLinkIDs(const DEV_GEN_CFG* devCfg)
{
	return NULL;
}

const DEV_DECL sndDev_BSMT2000 =
{
	DEVID_BSMT2000,
	DeviceName,
	DeviceChannels,
	DeviceChannelNames,
	DeviceLinkIDs,
	{	// cores
		&devDef,
		NULL
	}
};

/* ==== Utility Macros ==== */
#define MIN(x,y) ((x)<(y)?(x):(y))

/* ==== Internal Functions ==== */

static void init_voice(bsmt2000_voice *voice)
{
    memset(voice, 0, sizeof(*voice));
    // M1/MAME uses 0 for volumes on reset/init.
    voice->reg[BSMT2000_REG_LEFTVOL] = 0;
    voice->reg[BSMT2000_REG_RIGHTVOL] = 0;
    voice->position = 0;
    voice->adjusted_rate = 0;
    voice->loop_start_position = 0;
    voice->loop_stop_position = 0;
}
static void init_all_voices(bsmt2000_state *chip)
{
    int i;
    for (i = 0; i < BSMT2000_MAX_VOICES; i++)
        init_voice(&chip->voice[i]);
}

/* ==== Device Start ==== */
static uint8_t device_start_bsmt2000(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf)
{
    bsmt2000_state *chip;
    chip = (bsmt2000_state *)calloc(1, sizeof(bsmt2000_state));
    if (!chip)
        return 0xFF;

    chip->sample_rom = NULL;
    chip->sample_rom_length = 0x00;
    chip->sample_rom_mask = 0x00;
    chip->total_banks = 0;

    chip->right_volume_set = 0;

    chip->voices = 12;       // default: 12 PCM (may be overridden later)
    chip->stereo = 1;        // default: stereo enabled
    chip->adpcm = 1;         // default: ADPCM enabled

    chip->mode = 1;
    chip->clock = cfg->clock;
    chip->sample_rate = chip->clock / 1000.0;
    chip->last_register = 1; // Mode 1

    chip->adpcm_current = 0;
    chip->adpcm_delta_n = 10;

    init_all_voices(chip);
    bsmt2000_set_mute_mask(chip, 0x0000);

    chip->_devData.chipInf = chip;
    INIT_DEVINF(retDevInf, &chip->_devData, (uint32_t)chip->sample_rate, &devDef);

    return 0x00;
}

/* ==== Device Stop ==== */
static void device_stop_bsmt2000(void *info)
{
    bsmt2000_state *chip = (bsmt2000_state *)info;
    free(chip->sample_rom);
    free(chip);
}

/* ==== Device Reset ==== */
static void device_reset_bsmt2000(void *info) {
    bsmt2000_state *chip = (bsmt2000_state *)info;

    init_all_voices(chip);
    chip->adpcm_current = 0;
    chip->adpcm_delta_n = 10;
    switch (chip->last_register)
    {
        default:
            break;
        /* mode 0: 24kHz, 12 channel PCM, 1 channel ADPCM, mono; from PinMAME */
        case 0:
            chip->sample_rate = chip->clock / 1000.;
            chip->stereo = 0;
            chip->voices = 12;
            chip->adpcm = 1;
            chip->mode = 0;
            break;
        /* mode 1: 24kHz, 11 channel PCM, 1 channel ADPCM, stereo */
        case 1:
            chip->sample_rate = chip->clock / 1000.;
            chip->stereo = 1;
            chip->voices = 11;
            chip->adpcm = 1;
            chip->mode = 1;
            break;
        /* mode 5: 24kHz, 12 channel PCM, stereo */
        case 5:
            chip->sample_rate = chip->clock / 1000.;
            chip->stereo = 1;
            chip->voices = 12;
            chip->adpcm = 0;
            chip->mode = 5;
            break;
        /* mode 6: 34kHz, 8 channel PCM, stereo */
        case 6:
            chip->sample_rate = chip->clock / 706.;
            chip->stereo = 1;
            chip->voices = 8;
            chip->adpcm = 0;
            chip->mode = 6;
            break;
        /* mode 7: 32kHz, 9 channel PCM, stereo */
        case 7:
            chip->sample_rate = chip->clock / 750.;
            chip->stereo = 1;
            chip->voices = 9;
            chip->adpcm = 0;
            chip->mode = 7;
            break;
    }
    if (chip->SmpRateFunc != NULL)
        chip->SmpRateFunc(chip->SmpRateData, (uint32_t)chip->sample_rate);
}
/* ==== Register/Command Interface ==== */

static void bsmt2000_w(void *info, uint8_t offset, uint8_t data)
{
    bsmt2000_state *chip = (bsmt2000_state *)info;
    switch (offset)
    {
    case 0:
        chip->latch = (chip->latch & 0x00ff) | (data << 8);
        break;
    case 1:
        chip->latch = (chip->latch & 0xff00) | data;
        break;
    case 2:
        bsmt2000_write_data(chip, data, chip->latch);
        break;
    case 0x10:
        chip->last_register = data;
        device_reset_bsmt2000(info);
        break;
    default:
        emu_logf(&chip->logger, DEVLOG_DEBUG, "unexpected bsmt2000 write to offset %d == %02X\n", offset, data);
        break;
    }
}

static uint8_t bsmt2000_r(void *info, uint8_t offset)
{
    /* Always ready (bit 7 = 1) */
    return 0x80;
}

static void bsmt2000_write_data(void *info, uint8_t address, uint16_t data) {
    bsmt2000_state *chip = (bsmt2000_state *)info;
    bsmt2000_voice *voice;

    chip->last_register = address;

    // Standard voices (interleaved register layout)
    if (address < 0x6d) {
        int voice_index;
        int regindex = BSMT2000_REG_TOTAL - 1;
        while (address < regmap[chip->mode][regindex])
            --regindex;

        voice_index = address - regmap[chip->mode][regindex];
        if (voice_index >= chip->voices)
            return;

        voice = &chip->voice[voice_index];
        voice->reg[regindex] = data;

        switch (regindex) {
            case BSMT2000_REG_CURRPOS: // REG_CURRPOS
                voice->position = data << 16;
                break;
            case BSMT2000_REG_RATE: // REG_RATE
                voice->adjusted_rate = data << 5;
                break;
            case BSMT2000_REG_LOOPSTART: // REG_LOOPSTART
                voice->loop_start_position = data << 16;
                break;
            case BSMT2000_REG_LOOPEND: // REG_LOOPEND
                voice->loop_stop_position = data << 16;
                break;
            case BSMT2000_REG_RIGHTVOL:
                chip->right_volume_set = 1;
                break;
        }
    }
    // Compressed/ADPCM channel (11-voice model only)
    else if (chip->adpcm != 0 && address >= 0x6d) {
        voice = &chip->voice[BSMT2000_ADPCM_INDEX];
        switch (address) {
        case 0x6d:
            voice->reg[BSMT2000_REG_LOOPEND] = data; // REG_LOOPEND
            voice->loop_stop_position = data << 16;
            break;
        case 0x6f:
            voice->reg[BSMT2000_REG_BANK] = data; // REG_BANK
            break;
        case 0x6e: // main right channel volume control, used when ADPCM is alreay playing
        case 0x74:
            voice->reg[BSMT2000_REG_RIGHTVOL] = data; // REG_RIGHTVOL
            chip->right_volume_set = 1;
            break;
        case 0x75:
            voice->reg[BSMT2000_REG_CURRPOS] = data; // REG_CURRPOS
            voice->position = data << 16;
            chip->adpcm_current = 0;
            chip->adpcm_delta_n = 10;
            break;
        case 0x70: // main left channel volume control, used when ADPCM is alreay playing
        case 0x78:
            voice->reg[BSMT2000_REG_LEFTVOL] = data; // REG_LEFTVOL
            break;
        }
    }
}

/* ==== Sample ROM Handling ==== */
static void bsmt2000_alloc_rom(void* info, uint32_t memsize) {
    bsmt2000_state* chip = (bsmt2000_state *)info;
    memsize = (memsize + BSMT2000_ROM_BANKSIZE - 1) & ~(BSMT2000_ROM_BANKSIZE - 1);
    chip->total_banks = memsize / BSMT2000_ROM_BANKSIZE;
    if (chip->sample_rom_length == memsize)
        return;
    chip->sample_rom = (uint8_t*)realloc(chip->sample_rom, memsize);
    chip->sample_rom_length = memsize;
    chip->sample_rom_mask = pow2_mask(memsize);
    chip->total_banks = memsize / BSMT2000_ROM_BANKSIZE;
    memset(chip->sample_rom, 0, memsize);
}

static void bsmt2000_write_rom(void *info, uint32_t offset, uint32_t length, const uint8_t* data) {
    bsmt2000_state* chip = (bsmt2000_state *)info;
    if (offset > chip->sample_rom_length) return;
    if (offset + length > chip->sample_rom_length)
        length = chip->sample_rom_length - offset;
    memcpy(&chip->sample_rom[offset], data, length);
}

/* ==== Mute Mask ==== */
static void bsmt2000_set_mute_mask(void *info, uint32_t MuteMask)
{
    bsmt2000_state* chip = (bsmt2000_state *)info;
    uint8_t CurChn;
    for (CurChn = 0; CurChn < BSMT2000_MAX_VOICES; CurChn++)
        chip->Muted[CurChn] = (MuteMask >> CurChn) & 0x01;
}

static uint32_t bsmt2000_get_mute_mask(void *info)
{
    bsmt2000_state* chip = (bsmt2000_state *)info;
    uint32_t muteMask = 0;
    uint8_t CurChn;
    for (CurChn = 0; CurChn < BSMT2000_MAX_VOICES; CurChn++)
        muteMask |= (chip->Muted[CurChn] << CurChn);
    return muteMask;
}

/* ==== Logging ==== */
static void bsmt2000_set_log_cb(void* info, DEVCB_LOG func, void* param)
{
    bsmt2000_state* chip = (bsmt2000_state *)info;
    dev_logger_set(&chip->logger, chip, func, param);
}

/* ==== Interpolation Macro ==== */
#define INTERPOLATE(s1,s2,frac)  (((s1) * (int32_t)(0x10000 - ((frac)&0xffff)) + (s2) * (int32_t)((frac)&0xffff)) >> 16)

/* ==== Sound Update ==== */
static void bsmt2000_update(void *param, uint32_t samples, DEV_SMPL **outputs)
{
    bsmt2000_state *chip = (bsmt2000_state *)param;
    int32_t left[BSMT2000_SAMPLE_CHUNK];
    int32_t right[BSMT2000_SAMPLE_CHUNK];
    bsmt2000_voice *voice;
    int samp, v, length = MIN(samples, BSMT2000_SAMPLE_CHUNK);	// TODO: allow updating in larger chunks

    if (!chip->sample_rom || !chip->sample_rom_length)
    {
        memset(outputs[0], 0, samples * sizeof(*outputs[0]));
        memset(outputs[1], 0, samples * sizeof(*outputs[1]));
        return;
    }
    memset(left, 0, length * sizeof(left[0]));
    memset(right, 0, length * sizeof(right[0]));

    // PCM voices
    for (v = 0; v < chip->voices; v++)
    {
        uint8_t *base;
        uint32_t rate, pos;
        int32_t lvol, rvol;

        if (chip->Muted[v])
            continue;
        voice = &chip->voice[v];
        if (voice->reg[BSMT2000_REG_BANK] >= chip->total_banks) // REG_BANK
            continue;
        base = chip->sample_rom + voice->reg[BSMT2000_REG_BANK] * BSMT2000_ROM_BANKSIZE;
        rate = voice->adjusted_rate;
        pos = voice->position;
        rvol = voice->reg[BSMT2000_REG_RIGHTVOL]; // REG_RIGHTVOL
        lvol = chip->stereo ? voice->reg[BSMT2000_REG_LEFTVOL] : rvol; // REG_LEFTVOL
        if (chip->stereo && !chip->right_volume_set)
            rvol = lvol;

        for (samp = 0; samp < length; samp ++) {
            int32_t idx = pos >> 16;
            int32_t sample;
            if (1)
            {
                // no interpolation (original chip behaviour)
                sample = (int8_t)base[idx] << 8;
            }
            else
            {
                // linear interpolation (softer samples, breaks SFX in "Tales from the Crypt")
                int32_t s1 = (int8_t)base[idx] << 8;
                int32_t s2 = (int8_t)base[idx+1] << 8;
                sample = INTERPOLATE(s1, s2, pos);
            }
            left[samp]  += (sample * lvol) >> 16;
            right[samp] += (sample * rvol) >> 16;
            pos += rate;
            if (pos >= voice->loop_stop_position)
                pos += voice->loop_start_position - voice->loop_stop_position;
        }
        voice->position = pos;
    }

    // ADPCM/compressed voice (11-voice model only)
    if (chip->adpcm != 0 && !chip->Muted[BSMT2000_ADPCM_INDEX])
    {
        voice = &chip->voice[BSMT2000_ADPCM_INDEX];
        if (voice->reg[BSMT2000_REG_BANK] < chip->total_banks)
        {
            uint8_t *base = chip->sample_rom + voice->reg[BSMT2000_REG_BANK] * BSMT2000_ROM_BANKSIZE;
            uint32_t rate = 0x02aa << 4;
            uint32_t pos = voice->position;
            int32_t rvol = voice->reg[BSMT2000_REG_RIGHTVOL];
            int32_t lvol = chip->stereo ? voice->reg[BSMT2000_REG_LEFTVOL] : rvol;
            if (chip->stereo && !chip->right_volume_set)
                rvol = lvol;

            for (samp = 0; samp < length && pos < voice->loop_stop_position; samp ++)
            {
                uint32_t oldpos = pos;
                left[samp]  += (chip->adpcm_current * (lvol * 2)) >> 16;
                right[samp] += (chip->adpcm_current * (rvol * 2)) >> 16;

                pos += rate;
                if ((oldpos ^ pos) & 0x8000)
                {
                    static const uint8_t delta_tab[16] = { 154, 154, 128, 102, 77, 58, 58, 58, 58, 58, 58, 58, 77, 102, 128, 154 };
                    int nibble = base[oldpos >> 16] >> ((~oldpos >> 13) & 4);
                    int32_t value = (nibble & 0xF) | ((nibble & 0x8) ? ~0xF : 0);
                    int temp;

                    temp = chip->adpcm_delta_n * value;
                    if (value > 0)
                        temp += chip->adpcm_delta_n >> 1;
                    else
                        temp -= chip->adpcm_delta_n >> 1;

                    chip->adpcm_current += temp;
                    if (chip->adpcm_current > 32767)
                        chip->adpcm_current = 32767;
                    else if (chip->adpcm_current < -32768)
                        chip->adpcm_current = -32768;

                    chip->adpcm_delta_n = (chip->adpcm_delta_n * delta_tab[value + 8]) >> 6;
                    if (chip->adpcm_delta_n > 2000)
                        chip->adpcm_delta_n = 2000;
                    else if (chip->adpcm_delta_n < 1)
                        chip->adpcm_delta_n = 1;
                }
            }
            voice->position = pos;
        }
    }

    // Output clamp and write
    for (samp = 0; samp < length; samp++) {
        int32_t l = left[samp];
        int32_t r = right[samp];
        l = (l > 32767) ? 32767 : (l < -32768) ? -32768 : l;
        r = (r > 32767) ? 32767 : (r < -32768) ? -32768 : r;
        outputs[0][samp] = (DEV_SMPL)l;
        outputs[1][samp] = (DEV_SMPL)r;
    }
}

static void bsmt2000_set_srchg_cb(void *info, DEVCB_SRATE_CHG CallbackFunc, void* DataPtr)
{
    bsmt2000_state *chip = (bsmt2000_state *)info;
	
	// set Sample Rate Change Callback routine
	chip->SmpRateFunc = CallbackFunc;
	chip->SmpRateData = DataPtr;
	
	return;
}
