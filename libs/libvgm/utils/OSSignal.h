#ifndef __OSSIGNAL_H__
#define __OSSIGNAL_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "../stdtype.h"

typedef struct _os_signal OS_SIGNAL;

uint8_t OSSignal_Init(OS_SIGNAL** retSignal, uint8_t initState);
void OSSignal_Deinit(OS_SIGNAL* sig);
uint8_t OSSignal_Signal(OS_SIGNAL* sig);
uint8_t OSSignal_Reset(OS_SIGNAL* sig);
uint8_t OSSignal_Wait(OS_SIGNAL* sig);

#ifdef __cplusplus
}
#endif

#endif	// __OSSIGNAL_H__
