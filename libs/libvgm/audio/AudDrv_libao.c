// Audio Stream - libao
//	- libao

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ao/ao.h>

#include <unistd.h>		// for usleep()
#define	Sleep(msec)	usleep(msec * 1000)

#include "../stdtype.h"

#include "AudioStream.h"
#include "../utils/OSThread.h"
#include "../utils/OSSignal.h"
#include "../utils/OSMutex.h"


typedef struct
{
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
} WAVEFORMAT;	// from MSDN Help

#define WAVE_FORMAT_PCM	0x0001


typedef struct _libao_driver
{
	void* audDrvPtr;
	volatile uint8_t devState;	// 0 - not running, 1 - running, 2 - terminating
	
	WAVEFORMAT waveFmt;
	uint32_t bufSmpls;
	uint32_t bufSize;
	uint32_t bufCount;
	uint8_t* bufSpace;
	
	OS_THREAD* hThread;
	OS_SIGNAL* hSignal;
	OS_MUTEX* hMutex;
	ao_device* hDevAO;
	volatile uint8_t pauseThread;
	
	void* userParam;
	AUDFUNC_FILLBUF FillBuffer;
	ao_sample_format aoParams;
} DRV_AO;


uint8_t LibAO_IsAvailable(void);
uint8_t LibAO_Init(void);
uint8_t LibAO_Deinit(void);
const AUDIO_DEV_LIST* LibAO_GetDeviceList(void);
AUDIO_OPTS* LibAO_GetDefaultOpts(void);

uint8_t LibAO_Create(void** retDrvObj);
uint8_t LibAO_Destroy(void* drvObj);
uint8_t LibAO_Start(void* drvObj, uint32_t deviceID, AUDIO_OPTS* options, void* audDrvParam);
uint8_t LibAO_Stop(void* drvObj);
uint8_t LibAO_Pause(void* drvObj);
uint8_t LibAO_Resume(void* drvObj);

uint8_t LibAO_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam);
uint32_t LibAO_GetBufferSize(void* drvObj);
uint8_t LibAO_IsBusy(void* drvObj);
uint8_t LibAO_WriteData(void* drvObj, uint32_t dataSize, void* data);

uint32_t LibAO_GetLatency(void* drvObj);
static void AoThread(void* Arg);


AUDIO_DRV audDrv_LibAO =
{
	{ADRVTYPE_OUT, ADRVSIG_LIBAO, "libao"},
	
	LibAO_IsAvailable,
	LibAO_Init, LibAO_Deinit,
	LibAO_GetDeviceList, LibAO_GetDefaultOpts,
	
	LibAO_Create, LibAO_Destroy,
	LibAO_Start, LibAO_Stop,
	LibAO_Pause, LibAO_Resume,
	
	LibAO_SetCallback, LibAO_GetBufferSize,
	LibAO_IsBusy, LibAO_WriteData,
	
	LibAO_GetLatency,
};


static char* aoDevNames[1] = {"default"};
static AUDIO_OPTS defOptions;
static AUDIO_DEV_LIST deviceList;

static uint8_t isInit = 0;
static uint32_t activeDrivers;

uint8_t LibAO_IsAvailable(void)
{
	return 1;
}

uint8_t LibAO_Init(void)
{
	// TODO: generate a list of all libao drivers
	//uint32_t numDevs;
	//uint32_t curDev;
	//uint32_t devLstID;
	
	if (isInit)
		return AERR_WASDONE;
	
	ao_initialize();
	deviceList.devCount = 1;
	deviceList.devNames = aoDevNames;
	
	
	memset(&defOptions, 0x00, sizeof(AUDIO_OPTS));
	defOptions.sampleRate = 44100;
	defOptions.numChannels = 2;
	defOptions.numBitsPerSmpl = 16;
	defOptions.usecPerBuf = 10000;	// 10 ms per buffer
	defOptions.numBuffers = 10;	// 100 ms latency
	
	
	activeDrivers = 0;
	isInit = 1;
	
	return AERR_OK;
}

uint8_t LibAO_Deinit(void)
{
	//uint32_t curDev;
	
	if (! isInit)
		return AERR_WASDONE;
	
	deviceList.devCount = 0;
	deviceList.devNames = NULL;
	
	ao_shutdown();
	
	isInit = 0;
	
	return AERR_OK;
}

const AUDIO_DEV_LIST* LibAO_GetDeviceList(void)
{
	return &deviceList;
}

AUDIO_OPTS* LibAO_GetDefaultOpts(void)
{
	return &defOptions;
}


uint8_t LibAO_Create(void** retDrvObj)
{
	DRV_AO* drv;
	uint8_t retVal8;
	
	drv = (DRV_AO*)malloc(sizeof(DRV_AO));
	drv->devState = 0;
	drv->hDevAO = NULL;
	drv->hThread = NULL;
	drv->hSignal = NULL;
	drv->hMutex = NULL;
	drv->userParam = NULL;
	drv->FillBuffer = NULL;
	
	activeDrivers ++;
	retVal8  = OSSignal_Init(&drv->hSignal, 0);
	retVal8 |= OSMutex_Init(&drv->hMutex, 0);
	if (retVal8)
	{
		LibAO_Destroy(drv);
		*retDrvObj = NULL;
		return AERR_API_ERR;
	}
	*retDrvObj = drv;
	
	return AERR_OK;
}

uint8_t LibAO_Destroy(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	if (drv->devState != 0)
		LibAO_Stop(drvObj);
	if (drv->hThread != NULL)
	{
		OSThread_Cancel(drv->hThread);
		OSThread_Deinit(drv->hThread);
	}
	if (drv->hSignal != NULL)
		OSSignal_Deinit(drv->hSignal);
	if (drv->hMutex != NULL)
		OSMutex_Deinit(drv->hMutex);
	
	free(drv);
	activeDrivers --;
	
	return AERR_OK;
}

uint8_t LibAO_Start(void* drvObj, uint32_t deviceID, AUDIO_OPTS* options, void* audDrvParam)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	uint64_t tempInt64;
	int retVal;
	uint8_t retVal8;
	
	if (drv->devState != 0)
		return 0xD0;	// already running
	
	drv->audDrvPtr = audDrvParam;
	if (options == NULL)
		options = &defOptions;
	drv->waveFmt.wFormatTag = WAVE_FORMAT_PCM;
	drv->waveFmt.nChannels = options->numChannels;
	drv->waveFmt.nSamplesPerSec = options->sampleRate;
	drv->waveFmt.wBitsPerSample = options->numBitsPerSmpl;
	drv->waveFmt.nBlockAlign = drv->waveFmt.wBitsPerSample * drv->waveFmt.nChannels / 8;
	drv->waveFmt.nAvgBytesPerSec = drv->waveFmt.nSamplesPerSec * drv->waveFmt.nBlockAlign;
	
	tempInt64 = (uint64_t)options->sampleRate * options->usecPerBuf;
	drv->bufSmpls = (uint32_t)((tempInt64 + 500000) / 1000000);
	drv->bufSize = drv->waveFmt.nBlockAlign * drv->bufSmpls;
	drv->bufCount = options->numBuffers ? options->numBuffers : 10;
	
	drv->aoParams.bits = drv->waveFmt.wBitsPerSample;
	drv->aoParams.rate = drv->waveFmt.nSamplesPerSec;
	drv->aoParams.channels = drv->waveFmt.nChannels;
	drv->aoParams.byte_format = AO_FMT_NATIVE;
	drv->aoParams.matrix = NULL;
	
	drv->hDevAO = ao_open_live(ao_default_driver_id(), &drv->aoParams, NULL);
	if (drv->hDevAO == NULL)
		return 0xC0;		// open() failed
	
	OSSignal_Reset(drv->hSignal);
	retVal8 = OSThread_Init(&drv->hThread, &AoThread, drv);
	if (retVal8)
	{
		retVal = ao_close(drv->hDevAO);
		drv->hDevAO = NULL;
		return 0xC8;	// CreateThread failed
	}
	
	drv->bufSpace = (uint8_t*)malloc(drv->bufSize);
	
	drv->devState = 1;
	drv->pauseThread = 0x00;
	OSSignal_Signal(drv->hSignal);
	
	return AERR_OK;
}

uint8_t LibAO_Stop(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	int retVal;
	
	if (drv->devState != 1)
		return 0xD8;	// is already stopped (or stopping)
	
	drv->devState = 2;
	
	OSThread_Join(drv->hThread);
	OSThread_Deinit(drv->hThread);	drv->hThread = NULL;
	
	free(drv->bufSpace);	drv->bufSpace = NULL;
	
	retVal = ao_close(drv->hDevAO);
	if (! retVal)
		return 0xC4;		// close failed -- but why ???
	drv->hDevAO = NULL;
	drv->devState = 0;
	
	return AERR_OK;
}

uint8_t LibAO_Pause(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	if (drv->hDevAO == NULL)
		return 0xFF;
	
	drv->pauseThread |= 0x01;
	return AERR_OK;
}

uint8_t LibAO_Resume(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	if (drv->hDevAO == NULL)
		return 0xFF;
	
	drv->pauseThread &= ~0x01;
	return AERR_OK;
}


uint8_t LibAO_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	drv->pauseThread |= 0x02;
	OSMutex_Lock(drv->hMutex);
	drv->userParam = userParam;
	drv->FillBuffer = FillBufCallback;
	drv->pauseThread &= ~0x02;
	OSMutex_Unlock(drv->hMutex);
	
	return AERR_OK;
}

uint32_t LibAO_GetBufferSize(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	return drv->bufSize;
}

uint8_t LibAO_IsBusy(void* drvObj)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	
	if (drv->FillBuffer != NULL)
		return AERR_BAD_MODE;
	
	//return AERR_BUSY;
	return AERR_OK;
}

uint8_t LibAO_WriteData(void* drvObj, uint32_t dataSize, void* data)
{
	DRV_AO* drv = (DRV_AO*)drvObj;
	int retVal;
	
	if (dataSize > drv->bufSize)
		return AERR_TOO_MUCH_DATA;
	
	retVal = ao_play(drv->hDevAO, data, dataSize);
	if (! retVal)
		return 0xFF;
	return AERR_OK;
}


uint32_t LibAO_GetLatency(void* drvObj)
{
	//DRV_AO* drv = (DRV_AO*)drvObj;
	
	return 0;	// There's no API call that lets you receive the current latency.
}

static void AoThread(void* Arg)
{
	DRV_AO* drv = (DRV_AO*)Arg;
	uint32_t didBuffers;	// number of processed buffers
	uint32_t bufBytes;
	int retVal;
	
	OSSignal_Wait(drv->hSignal);	// wait until the initialization is done
	
	while(drv->devState == 1)
	{
		didBuffers = 0;
		OSMutex_Lock(drv->hMutex);
		if (! drv->pauseThread && drv->FillBuffer != NULL)
		{
			bufBytes = drv->FillBuffer(drv->audDrvPtr, drv->userParam, drv->bufSize, drv->bufSpace);
			retVal = ao_play(drv->hDevAO, (char*)drv->bufSpace, bufBytes);
			didBuffers ++;
		}
		OSMutex_Unlock(drv->hMutex);
		if (! didBuffers)
			Sleep(1);
		
		while(drv->FillBuffer == NULL && drv->devState == 1)
			Sleep(1);
	}
	
	return;
}
