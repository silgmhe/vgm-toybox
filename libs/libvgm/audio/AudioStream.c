// Audio Stream - API functions
#define _CRTDBG_MAP_ALLOC
//#include <stdio.h>
#include <stdlib.h>
//#include <string.h>	// for memset

#include "../stdtype.h"
#include "../_stdbool.h"

#include "AudioStream.h"
#include "../utils/OSMutex.h"


#ifdef AUDDRV_WAVEWRITE
extern AUDIO_DRV audDrv_WaveWrt;
#endif

#ifdef AUDDRV_WINMM
extern AUDIO_DRV audDrv_WinMM;
#endif
#ifdef AUDDRV_DSOUND
extern AUDIO_DRV audDrv_DSound;
#endif
#ifdef AUDDRV_XAUD2
extern AUDIO_DRV audDrv_XAudio2;
#endif
#ifdef AUDDRV_WASAPI
extern AUDIO_DRV audDrv_WASAPI;
#endif

#ifdef AUDDRV_OSS
extern AUDIO_DRV audDrv_OSS;
#endif
#ifdef AUDDRV_SADA
extern AUDIO_DRV audDrv_SADA;
#endif
#ifdef AUDDRV_ALSA
extern AUDIO_DRV audDrv_ALSA;
#endif
#ifdef AUDDRV_LIBAO
extern AUDIO_DRV audDrv_LibAO;
#endif
#ifdef AUDDRV_PULSE
extern AUDIO_DRV audDrv_Pulse;
#endif

#ifdef AUDDRV_CA
extern AUDIO_DRV audDrv_CA;
#endif

AUDIO_DRV* audDrivers[] =
{
#ifdef AUDDRV_WAVEWRITE
	&audDrv_WaveWrt,
#endif
#ifdef AUDDRV_WINMM
	&audDrv_WinMM,
#endif
#ifdef AUDDRV_DSOUND
	&audDrv_DSound,
#endif
#ifdef AUDDRV_XAUD2
	&audDrv_XAudio2,
#endif
#ifdef AUDDRV_WASAPI
	&audDrv_WASAPI,
#endif
#ifdef AUDDRV_OSS
	&audDrv_OSS,
#endif
#ifdef AUDDRV_SADA
	&audDrv_SADA,
#endif
#ifdef AUDDRV_ALSA
	&audDrv_ALSA,
#endif
#ifdef AUDDRV_LIBAO
	&audDrv_LibAO,
#endif
#ifdef AUDDRV_PULSE
	&audDrv_Pulse,
#endif
#ifdef AUDDRV_CA
	&audDrv_CA,
#endif
	NULL
};


typedef struct _audio_driver_load
{
	uint8_t flags;
	AUDIO_DRV* drvStruct;
} ADRV_LOAD;

typedef struct _audio_driver_instance ADRV_INSTANCE;
typedef struct _audio_driver_list ADRV_LIST;
struct _audio_driver_list
{
	const ADRV_INSTANCE* drvInst;
	ADRV_LIST* next;
};
struct _audio_driver_instance
{
	uint32_t ID;	// -1 = unused
	AUDIO_DRV* drvStruct;
	AUDIO_OPTS drvOpts;
	void* drvData;
	void* userParam;
	AUDFUNC_FILLBUF mainCallback;
	ADRV_LIST* forwardDrvs;
	OS_MUTEX* hMutex;	// for locking access to "forwardDrvs"
};

#define ADFLG_ENABLE	0x01
#define ADFLG_IS_INIT	0x02
#define ADFLG_AVAILABLE	0x80

#define ADID_UNUSED		(uint32_t)-1


static uint8_t ADrvLst_Add(ADRV_LIST** headPtr, const ADRV_INSTANCE* drvData);
static uint8_t ADrvLst_Remove(ADRV_LIST** headPtr, const ADRV_INSTANCE* drvData);
static ADRV_LIST* ADrvLst_FindItem(ADRV_LIST* head, const ADRV_INSTANCE* drvData, ADRV_LIST** retLastItm);
static uint8_t ADrvLst_Clear(ADRV_LIST** headPtr);

uint8_t Audio_Init(void);
//uint8_t Audio_Deinit(void);
//uint32_t Audio_GetDriverCount(void);
//uint8_t Audio_GetDriverInfo(uint32_t drvID, AUDDRV_INFO** retDrvInfo);
static ADRV_INSTANCE* GetFreeRunSlot(void);
//uint8_t AudioDrv_Init(uint32_t drvID, void** retDrvStruct);
//uint8_t AudioDrv_Deinit(void** drvStruct);
//const AUDIO_DEV_LIST* AudioDrv_GetDeviceList(void* drvStruct);
//AUDIO_OPTS* AudioDrv_GetOptions(void* drvStruct);
//uint8_t AudioDrv_Start(void* drvStruct, uint32_t devID);
//uint8_t AudioDrv_Stop(void* drvStruct);
//uint8_t AudioDrv_Pause(void* drvStruct);
//uint8_t AudioDrv_Resume(void* drvStruct);
static uint32_t DoDataForwarding(void* drvStruct, void* userParam, uint32_t bufSize, void* data);
//uint8_t AudioDrv_SetCallback(void* drvStruct, AUDFUNC_FILLBUF FillBufCallback, void* userParam);
//uint8_t AudioDrv_DataForward_Add(void* drvStruct, const void* destDrvStruct);
//uint8_t AudioDrv_DataForward_Remove(void* drvStruct, const void* destDrvStruct);
//uint8_t AudioDrv_DataForward_RemoveAll(void* drvStruct);
//uint32_t AudioDrv_GetBufferSize(void* drvStruct);
//uint8_t AudioDrv_IsBusy(void* drvStruct);
//uint8_t AudioDrv_WriteData(void* drvStruct, uint32_t dataSize, void* data);
//uint32_t AudioDrv_GetLatency(void* drvStruct);


static uint32_t audDrvCount = 0;
static ADRV_LOAD* audDrvLoaded = NULL;

static uint32_t runDevCount = 0;
static ADRV_INSTANCE* runDevices = NULL;

static bool isInit = false;


#include "AudioStream_LstFuncs.h"


uint8_t Audio_Init(void)
{
	uint32_t curDrv;
	uint32_t curDrvLd;
	AUDIO_DRV* tempDrv;
	ADRV_LOAD* tempDLoad;
	
	if (isInit)
		return AERR_WASDONE;
	
	audDrvCount = 0;
	while(audDrivers[audDrvCount] != NULL)
		audDrvCount ++;
	audDrvLoaded = (ADRV_LOAD*)malloc(audDrvCount * sizeof(ADRV_LOAD));
	
	curDrvLd = 0;
	for (curDrv = 0; curDrv < audDrvCount; curDrv ++)
	{
		tempDrv = audDrivers[curDrv];
		tempDLoad = &audDrvLoaded[curDrvLd];
		tempDLoad->drvStruct = tempDrv;
		if (tempDrv->IsAvailable())
			tempDLoad->flags = ADFLG_ENABLE | ADFLG_AVAILABLE;
		else
			tempDLoad->flags = 0x00;
		curDrvLd ++;
	}
	
	audDrvCount = curDrvLd;
	if (! audDrvCount)
	{
		free(audDrvLoaded);	audDrvLoaded = NULL;
		return AERR_NODRVS;
	}
	
	//runDevCount = 0;
	//runDevices = NULL;
	runDevCount = 0x10;
	runDevices = (ADRV_INSTANCE*)realloc(runDevices, runDevCount * sizeof(ADRV_INSTANCE));
	for (curDrv = 0; curDrv < runDevCount; curDrv ++)
		runDevices[curDrv].ID = -1;
	
	isInit = true;
	return AERR_OK;
}

uint8_t Audio_Deinit(void)
{
	uint32_t curDev;
	ADRV_INSTANCE* tempAIns;
	
	if (! isInit)
		return AERR_WASDONE;
	
	for (curDev = 0; curDev < runDevCount; curDev ++)
	{
		tempAIns = &runDevices[curDev];
		if (tempAIns->ID != ADID_UNUSED)
		{
			tempAIns->drvStruct->Stop(tempAIns->drvData);
			tempAIns->drvStruct->Destroy(tempAIns->drvData);
			ADrvLst_Clear(&tempAIns->forwardDrvs);
		}
	}
	for (curDev = 0; curDev < audDrvCount; curDev ++)
	{
		if (audDrvLoaded[curDev].flags & ADFLG_IS_INIT)
			audDrvLoaded[curDev].drvStruct->Deinit();
	}
	
	free(runDevices);	runDevices = NULL;
	runDevCount = 0;
	free(audDrvLoaded);	audDrvLoaded = NULL;
	audDrvCount = 0;
	
	isInit = false;
	return AERR_OK;
}

uint32_t Audio_GetDriverCount(void)
{
	return audDrvCount;
}

uint8_t Audio_GetDriverInfo(uint32_t drvID, AUDDRV_INFO** retDrvInfo)
{
	if (drvID >= audDrvCount)
		return AERR_INVALID_DRV;
	
	if (retDrvInfo != NULL)
		*retDrvInfo = &audDrvLoaded[drvID].drvStruct->drvInfo;
	return AERR_OK;
}

static ADRV_INSTANCE* GetFreeRunSlot(void)
{
	uint32_t curDev;
	
	for (curDev = 0; curDev < runDevCount; curDev ++)
	{
		if (runDevices[curDev].ID == ADID_UNUSED)
		{
			runDevices[curDev].ID = curDev;
			return &runDevices[curDev];
		}
	}
	return NULL;
	
	// Can't do realloc due to external use of references!!
	/*curDev = runDevCount;
	runDevCount ++;
	runDevices = (ADRV_INSTANCE*)realloc(runDevices, runDevCount * sizeof(ADRV_INSTANCE));
	runDevices[curDev].ID = curDev;
	return &runDevices[curDev];*/
}

uint8_t AudioDrv_Init(uint32_t drvID, void** retDrvStruct)
{
	ADRV_LOAD* tempDLoad;
	AUDIO_DRV* tempDrv;
	ADRV_INSTANCE* audInst;
	void* drvData;
	uint8_t retVal;
	
	if (drvID >= audDrvCount)
		return AERR_INVALID_DRV;
	tempDLoad = &audDrvLoaded[drvID];
	if (! (tempDLoad->flags & ADFLG_ENABLE))
		return AERR_DRV_DISABLED;
	
	tempDrv = tempDLoad->drvStruct;
	if (! (tempDLoad->flags & ADFLG_IS_INIT))
	{
		retVal = tempDrv->Init();
		if (retVal)
			return retVal;
		tempDLoad->flags |= ADFLG_IS_INIT;
	}
	
	retVal = tempDrv->Create(&drvData);
	if (retVal)
		return retVal;
	audInst = GetFreeRunSlot();
	audInst->drvStruct = tempDrv;
	audInst->drvOpts = *tempDrv->GetDefOpts();
	audInst->drvData = drvData;
	audInst->userParam = NULL;
	audInst->mainCallback = NULL;
	audInst->forwardDrvs = NULL;
	audInst->hMutex = NULL;
	OSMutex_Init(&audInst->hMutex, 0);
	*retDrvStruct = (void*)audInst;
	
	return AERR_OK;
}

uint8_t AudioDrv_Deinit(void** drvStruct)
{
	ADRV_INSTANCE* audInst;
	AUDIO_DRV* aDrv;
	uint8_t retVal;
	
	if (drvStruct == NULL)
		return AERR_OK;
	
	audInst = (ADRV_INSTANCE*)(*drvStruct);
	aDrv = audInst->drvStruct;
	if (audInst->ID == ADID_UNUSED)
		return AERR_INVALID_DRV;
	
	retVal = aDrv->Stop(audInst->drvData);	// just in case
	// continue regardless of errors
	retVal = aDrv->Destroy(audInst->drvData);
	if (retVal)
		return retVal;
	
	*drvStruct = NULL;
	audInst->ID = ADID_UNUSED;
	audInst->drvStruct = NULL;
	audInst->drvData = NULL;
	audInst->userParam = NULL;
	audInst->mainCallback = NULL;
	ADrvLst_Clear(&audInst->forwardDrvs);
	OSMutex_Deinit(audInst->hMutex);
	audInst->hMutex = NULL;
	return AERR_OK;
}

const AUDIO_DEV_LIST* AudioDrv_GetDeviceList(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->GetDevList();
}

AUDIO_OPTS* AudioDrv_GetOptions(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	
	return &audInst->drvOpts;
}

void* AudioDrv_GetDrvData(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	
	if (audInst == NULL || audInst->ID == ADID_UNUSED)
		return NULL;
	
	return audInst->drvData;
}

uint8_t AudioDrv_Start(void* drvStruct, uint32_t devID)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	uint8_t retVal;
	
	retVal = aDrv->Start(audInst->drvData, devID, &audInst->drvOpts, audInst);
	if (retVal)
		return retVal;
	
	return AERR_OK;
}

uint8_t AudioDrv_Stop(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	uint8_t retVal;
	
	retVal = aDrv->Stop(audInst->drvData);
	if (retVal)
		return retVal;
	
	return AERR_OK;
}

uint8_t AudioDrv_Pause(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->Pause(audInst->drvData);
}

uint8_t AudioDrv_Resume(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->Resume(audInst->drvData);
}

static uint32_t DoDataForwarding(void* drvStruct, void* userParam, uint32_t bufSize, void* data)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	ADRV_LIST* fwdList;
	const ADRV_INSTANCE* fwdInst;
	uint32_t dataSize = 0;
	
	OSMutex_Lock(audInst->hMutex);
	// Using audInst->userParam instead of the userParam parameter makes
	// later changes of the userParam via SetCallback work properly.
	if (audInst->mainCallback != NULL)
		dataSize = audInst->mainCallback(drvStruct, audInst->userParam, bufSize, data);	// fill buffer
	fwdList = audInst->forwardDrvs;
	while(fwdList != NULL)
	{
		fwdInst = fwdList->drvInst;
		if (fwdInst->ID != ADID_UNUSED && fwdInst->drvStruct != NULL)
			fwdInst->drvStruct->WriteData(fwdInst->drvData, dataSize, data);
		fwdList = fwdList->next;
	}
	OSMutex_Unlock(audInst->hMutex);
	return dataSize;
}

uint8_t AudioDrv_SetCallback(void* drvStruct, AUDFUNC_FILLBUF FillBufCallback, void* userParam)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	uint8_t retVal;
	
	OSMutex_Lock(audInst->hMutex);
	audInst->userParam = userParam;
	audInst->mainCallback = FillBufCallback;
	OSMutex_Unlock(audInst->hMutex);	// workaround for deadlock
	if (audInst->forwardDrvs != NULL && audInst->mainCallback != NULL)
		retVal = aDrv->SetCallback(audInst->drvData, &DoDataForwarding, audInst->userParam);
	else
		retVal = aDrv->SetCallback(audInst->drvData, audInst->mainCallback, audInst->userParam);
	return retVal;
}

uint8_t AudioDrv_DataForward_Add(void* drvStruct, const void* destDrvStruct)
{
	ADRV_INSTANCE* audInstSrc = (ADRV_INSTANCE*)drvStruct;
	ADRV_INSTANCE* audInstDst = (ADRV_INSTANCE*)destDrvStruct;
	uint8_t retVal;
	
	if (audInstDst == NULL)
		return 0xFF;
	OSMutex_Lock(audInstSrc->hMutex);
	retVal = ADrvLst_Add(&audInstSrc->forwardDrvs, audInstDst);
	// If callbacks are enabled, make it use the Forwarding-Callback routine.
	if (audInstSrc->drvStruct != NULL && audInstSrc->mainCallback != NULL)
		audInstSrc->drvStruct->SetCallback(audInstSrc->drvData, &DoDataForwarding, audInstSrc->userParam);
	OSMutex_Unlock(audInstSrc->hMutex);
	return AERR_OK;
}

uint8_t AudioDrv_DataForward_Remove(void* drvStruct, const void* destDrvStruct)
{
	ADRV_INSTANCE* audInstSrc = (ADRV_INSTANCE*)drvStruct;
	ADRV_INSTANCE* audInstDst = (ADRV_INSTANCE*)destDrvStruct;
	uint8_t retVal;
	
	if (audInstDst == NULL)
		return 0xFF;
	OSMutex_Lock(audInstSrc->hMutex);
	retVal = ADrvLst_Remove(&audInstSrc->forwardDrvs, audInstDst);
	if (retVal)
	{
		OSMutex_Unlock(audInstSrc->hMutex);
		return retVal;
	}
	
	// make it call the original callback function
	if (audInstSrc->forwardDrvs == NULL && audInstSrc->drvStruct != NULL)
		audInstSrc->drvStruct->SetCallback(audInstSrc->drvData, audInstSrc->mainCallback, audInstSrc->userParam);
	OSMutex_Unlock(audInstSrc->hMutex);
	return AERR_OK;
}

uint8_t AudioDrv_DataForward_RemoveAll(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	
	uint8_t retVal;
	
	OSMutex_Lock(audInst->hMutex);
	retVal = ADrvLst_Clear(&audInst->forwardDrvs);
	if (retVal)
	{
		OSMutex_Unlock(audInst->hMutex);
		return retVal;
	}
	
	// make it call the original callback function
	if (audInst->drvStruct != NULL)
		audInst->drvStruct->SetCallback(audInst->drvData, audInst->mainCallback, audInst->userParam);
	OSMutex_Unlock(audInst->hMutex);
	return AERR_OK;
}

uint32_t AudioDrv_GetBufferSize(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->GetBufferSize(audInst->drvData);
}

uint8_t AudioDrv_IsBusy(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->IsBusy(audInst->drvData);
}

uint8_t AudioDrv_WriteData(void* drvStruct, uint32_t dataSize, void* data)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	ADRV_LIST* fwdList;
	const ADRV_INSTANCE* fwdInst;
	uint8_t retVal;
	
	retVal = aDrv->WriteData(audInst->drvData, dataSize, data);
	
	OSMutex_Lock(audInst->hMutex);
	fwdList = audInst->forwardDrvs;
	while(fwdList != NULL)
	{
		fwdInst = fwdList->drvInst;
		if (fwdInst->ID != ADID_UNUSED && fwdInst->drvStruct != NULL)
			fwdInst->drvStruct->WriteData(audInst->drvData, dataSize, data);
		fwdList = fwdList->next;
	}
	OSMutex_Unlock(audInst->hMutex);
	return retVal;
}

uint32_t AudioDrv_GetLatency(void* drvStruct)
{
	ADRV_INSTANCE* audInst = (ADRV_INSTANCE*)drvStruct;
	AUDIO_DRV* aDrv = audInst->drvStruct;
	
	return aDrv->GetLatency(audInst->drvData);
}
