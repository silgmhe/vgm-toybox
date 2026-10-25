#ifndef __YMF262_H__
#define __YMF262_H__

#include "../../stdtype.h"

typedef void (*OPL3_TIMERHANDLER)(void *param,uint8_t timer,uint32_t period);
typedef void (*OPL3_IRQHANDLER)(void *param,uint8_t irq);
typedef void (*OPL3_UPDATEHANDLER)(void *param/*,int min_interval_us*/);


void *ymf262_init(uint32_t clock, uint32_t rate);
void ymf262_shutdown(void *chip);
void ymf262_reset_chip(void *chip);
void ymf262_write(void *chip, uint8_t a, uint8_t v);
uint8_t ymf262_read(void *chip, uint8_t a);
uint8_t ymf262_timer_over(void *chip, uint8_t c);
void ymf262_update_one(void *_chip, uint32_t length, DEV_SMPL **buffers);

void ymf262_set_timer_handler(void *chip, OPL3_TIMERHANDLER TimerHandler, void *param);
void ymf262_set_irq_handler(void *chip, OPL3_IRQHANDLER IRQHandler, void *param);
void ymf262_set_update_handler(void *chip, OPL3_UPDATEHANDLER UpdateHandler, void *param);

void ymf262_set_mute_mask(void *chip, uint32_t MuteMask);
void ymf262_set_volume(void *chip, int32_t volume);
void ymf262_set_vol_lr(void *chip, int32_t volLeft, int32_t volRight);
void ymf262_set_log_cb(void* chip, DEVCB_LOG func, void* param);

#endif	// __YMF262_H__
