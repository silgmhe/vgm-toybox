#ifndef __NES_APU_H__
#define __NES_APU_H__

typedef struct _nesapu_state nesapu_state;

uint8_t nes_apu_read(void* chip, uint8_t address);
void nes_apu_write(void* chip, uint8_t offset, uint8_t data);

void nes_apu_update(void* chip, uint32_t samples, DEV_SMPL **outputs);
void* device_start_nesapu(uint32_t clock, uint32_t rate);
void device_stop_nesapu(void* chip);
void device_reset_nesapu(void* chip);

void nesapu_set_rom(void* chip, const uint8_t* ROMData);

void nesapu_set_mute_mask(void* chip, uint32_t MuteMask);
uint32_t nesapu_get_mute_mask(void* chip);
void nesapu_set_options(void *chip, uint32_t Flags);
void nesapu_set_panning(void* chip, int16_t square1, int16_t square2, int16_t triangle, int16_t noise, int16_t dpcm);

#endif	// __NES_APU_H__
