#ifndef __OSMUTEX_H__
#define __OSMUTEX_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "../stdtype.h"

typedef struct _os_mutex OS_MUTEX;

uint8_t OSMutex_Init(OS_MUTEX** retMutex, uint8_t initLocked);
void OSMutex_Deinit(OS_MUTEX* mtx);
uint8_t OSMutex_Lock(OS_MUTEX* mtx);
uint8_t OSMutex_TryLock(OS_MUTEX* mtx);
uint8_t OSMutex_Unlock(OS_MUTEX* mtx);

#ifdef __cplusplus
}
#endif

#endif	// __OSMUTEX_H__
