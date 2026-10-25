#ifndef __ADLIBEMU_H__
#define __ADLIBEMU_H__

#include "../../stdtype.h"
#include "../snddef.h"

#if defined(OPLTYPE_IS_OPL2)
#define ADLIBEMU(name)			adlib_OPL2_##name
#elif defined(OPLTYPE_IS_OPL3)
#define ADLIBEMU(name)			adlib_OPL3_##name
#endif

typedef void (*ADL_UPDATEHANDLER)(void *param);

void* ADLIBEMU(init)(uint32_t clock, uint32_t samplerate);
void ADLIBEMU(stop)(void *chip);
void ADLIBEMU(reset)(void *chip);

void ADLIBEMU(writeIO)(void *chip, uint8_t addr, uint8_t val);
void ADLIBEMU(getsample)(void *chip, uint32_t numsamples, DEV_SMPL ** sndptr);

uint8_t ADLIBEMU(reg_read)(void *chip, uint8_t port);

void ADLIBEMU(set_update_handler)(void *chip, ADL_UPDATEHANDLER UpdateHandler, void* param);
void ADLIBEMU(set_mute_mask)(void *chip, uint32_t MuteMask);

void ADLIBEMU(set_volume)(void *chip, int32_t volume);
void ADLIBEMU(set_volume_lr)(void *chip, int32_t volL, int32_t volR);

#endif	// __ADLIBEMU_H__
