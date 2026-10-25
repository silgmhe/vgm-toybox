// POSIX Mutexes
// -------------

#include <stdlib.h>
#include <stddef.h>

#include <pthread.h>
#include <errno.h>

#include "../stdtype.h"
#include "OSMutex.h"

//typedef struct _os_mutex OS_MUTEX;
struct _os_mutex
{
	pthread_mutex_t hMutex;
};

uint8_t OSMutex_Init(OS_MUTEX** retMutex, uint8_t initLocked)
{
	OS_MUTEX* mtx;
	int retVal;
	
	mtx = (OS_MUTEX*)calloc(1, sizeof(OS_MUTEX));
	if (mtx == NULL)
		return 0xFF;
	
	retVal = pthread_mutex_init(&mtx->hMutex, NULL);
	if (retVal)
	{
		free(mtx);
		return 0x80;
	}
	if (initLocked)
		OSMutex_Lock(mtx);
	
	*retMutex = mtx;
	return 0x00;
}

void OSMutex_Deinit(OS_MUTEX* mtx)
{
	pthread_mutex_destroy(&mtx->hMutex);
	free(mtx);
	
	return;
}

uint8_t OSMutex_Lock(OS_MUTEX* mtx)
{
	int retVal;
	
	retVal = pthread_mutex_lock(&mtx->hMutex);
	if (! retVal)
		return 0x00;
	else
		return 0xFF;
}

uint8_t OSMutex_TryLock(OS_MUTEX* mtx)
{
	int retVal;
	
	retVal = pthread_mutex_trylock(&mtx->hMutex);
	if (! retVal)
		return 0x00;
	else if (retVal == EBUSY)
		return 0x01;
	else
		return 0xFF;
}

uint8_t OSMutex_Unlock(OS_MUTEX* mtx)
{
	int retVal;
	
	retVal = pthread_mutex_unlock(&mtx->hMutex);
	return retVal ? 0xFF : 0x00;
}
