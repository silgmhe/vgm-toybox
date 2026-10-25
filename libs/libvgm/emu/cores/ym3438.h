#ifndef __YM3438_H__
#define __YM3438_H__

#include "../../stdtype.h"
#include "../snddef.h"

void nukedopn2_write(void *chip, uint8_t port, uint8_t data);
uint8_t nukedopn2_read(void *chip, uint8_t port);
void nukedopn2_update(void *chip, uint32_t numsamples, DEV_SMPL **sndptr);
void nukedopn2_set_options(void *chip, uint32_t flags);
void nukedopn2_set_mute_mask(void *chip, uint32_t mute);
void* nukedopn2_init(uint32_t clock, uint32_t rate);
void nukedopn2_shutdown(void *chip);
void nukedopn2_reset_chip(void *chip);

#endif	// __YM3438_H__
