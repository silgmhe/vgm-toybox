#ifndef __NUKEDOPL3_H__
#define __NUKEDOPL3_H__

#include "../../stdtype.h"
#include "../snddef.h"

void nukedopl3_write(void *chip, uint8_t a, uint8_t v);
uint8_t nukedopl3_read(void *chip, uint8_t a);
void* nukedopl3_init(uint32_t clock, uint32_t rate);
void nukedopl3_shutdown(void *chip);
void nukedopl3_reset_chip(void *chip);
void nukedopl3_update(void *chip, uint32_t samples, DEV_SMPL **out);
void nukedopl3_set_mute_mask(void *chip, uint32_t MuteMask);
void nukedopl3_set_volume(void *chip, int32_t volume);
void nukedopl3_set_vol_lr(void *chip, int32_t volLeft, int32_t volRight);

#endif	// __NUKEDOPL3_H__
