// Audio Stream - PulseAudio
//	- libpulse-simple

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pulse/simple.h>

#include <unistd.h>		// for usleep()
#define	Sleep(msec)	usleep(msec * 1000)

#include "../stdtype.h"

#include "AudioStream.h"
#include "../utils/OSThread.h"
#include "../utils/OSSignal.h"
#include "../utils/OSMutex.h"


typedef struct _pulse_driver
{
	void* audDrvPtr;
	volatile uint8_t devState;	// 0 - not running, 1 - running, 2 - terminating
	
	pa_sample_spec pulseFmt;
	uint32_t bufSmpls;
	uint32_t bufSize;
	uint32_t bufCount;
	uint8_t* bufSpace;
	
	OS_THREAD* hThread;
	OS_SIGNAL* hSignal;
	OS_MUTEX* hMutex;
	pa_simple* hPulse;
	volatile uint8_t pauseThread;
	uint8_t canPause;
	char* streamDesc;
	
	void* userParam;
	AUDFUNC_FILLBUF FillBuffer;
} DRV_PULSE;


uint8_t Pulse_IsAvailable(void);
uint8_t Pulse_Init(void);
uint8_t Pulse_Deinit(void);
const AUDIO_DEV_LIST* Pulse_GetDeviceList(void);
AUDIO_OPTS* Pulse_GetDefaultOpts(void);

uint8_t Pulse_Create(void** retDrvObj);
uint8_t Pulse_Destroy(void* drvObj);
uint8_t Pulse_Start(void* drvObj, uint32_t deviceID, AUDIO_OPTS* options, void* audDrvParam);
uint8_t Pulse_Stop(void* drvObj);
uint8_t Pulse_Pause(void* drvObj);
uint8_t Pulse_Resume(void* drvObj);

uint8_t Pulse_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam);
uint32_t Pulse_GetBufferSize(void* drvObj);
uint8_t Pulse_IsBusy(void* drvObj);
uint8_t Pulse_WriteData(void* drvObj, uint32_t dataSize, void* data);

uint8_t Pulse_SetStreamDesc(void* drvObj, const char* fileName);
const char* Pulse_GetStreamDesc(void* drvObj);
uint32_t Pulse_GetLatency(void* drvObj);
static void PulseThread(void* Arg);


AUDIO_DRV audDrv_Pulse =
{
	{ADRVTYPE_OUT, ADRVSIG_PULSE, "PulseAudio"},
	
	Pulse_IsAvailable,
	Pulse_Init, Pulse_Deinit,
	Pulse_GetDeviceList, Pulse_GetDefaultOpts,
	
	Pulse_Create, Pulse_Destroy,
	Pulse_Start, Pulse_Stop,
	Pulse_Pause, Pulse_Resume,
	
	Pulse_SetCallback, Pulse_GetBufferSize,
	Pulse_IsBusy, Pulse_WriteData,
	
	Pulse_GetLatency,
};


static char* PulseDevNames[1] = {"default"};
static AUDIO_OPTS defOptions;
static AUDIO_DEV_LIST deviceList;

static uint8_t isInit = 0;
static uint32_t activeDrivers;

uint8_t Pulse_IsAvailable(void)
{
	return 1;
}

uint8_t Pulse_Init(void)
{
	if (isInit)
		return AERR_WASDONE;
	
	deviceList.devCount = 1;
	deviceList.devNames = PulseDevNames;
	
	
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

uint8_t Pulse_Deinit(void)
{
	if (! isInit)
		return AERR_WASDONE;
	
	deviceList.devCount = 0;
	deviceList.devNames = NULL;
	
	isInit = 0;
	
	return AERR_OK;
}

const AUDIO_DEV_LIST* Pulse_GetDeviceList(void)
{
	return &deviceList;
}

AUDIO_OPTS* Pulse_GetDefaultOpts(void)
{
	return &defOptions;
}


uint8_t Pulse_Create(void** retDrvObj)
{
	DRV_PULSE* drv;
	uint8_t retVal8;
	
	drv = (DRV_PULSE*)malloc(sizeof(DRV_PULSE));
	drv->devState = 0;
	drv->hPulse = NULL;
	drv->hThread = NULL;
	drv->hSignal = NULL;
	drv->hMutex = NULL;
	drv->userParam = NULL;
	drv->FillBuffer = NULL;
	drv->streamDesc = strdup("libvgm");
	
	activeDrivers ++;
	retVal8  = OSSignal_Init(&drv->hSignal, 0);
	retVal8 |= OSMutex_Init(&drv->hMutex, 0);
	if (retVal8)
	{
		Pulse_Destroy(drv);
		*retDrvObj = NULL;
		return AERR_API_ERR;
	}
	*retDrvObj = drv;
	
	return AERR_OK;
}

uint8_t Pulse_Destroy(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	if (drv->devState != 0)
		Pulse_Stop(drvObj);
	if (drv->hThread != NULL)
	{
		OSThread_Cancel(drv->hThread);
		OSThread_Deinit(drv->hThread);
	}
	if (drv->hSignal != NULL)
		OSSignal_Deinit(drv->hSignal);
	if (drv->hMutex != NULL)
		OSMutex_Deinit(drv->hMutex);
	
	free(drv->streamDesc);
	free(drv);
	activeDrivers --;
	
	return AERR_OK;
}

uint8_t Pulse_SetStreamDesc(void* drvObj, const char* streamDesc)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	//The Simple API does not accept updates to the stream description
	if (drv->hPulse)
		return AERR_WASDONE;
	
	free(drv->streamDesc);
	drv->streamDesc = strdup(streamDesc);
	
	return AERR_OK;
}

const char* Pulse_GetStreamDesc(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	return drv->streamDesc;
}

uint8_t Pulse_Start(void* drvObj, uint32_t deviceID, AUDIO_OPTS* options, void* audDrvParam)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	uint64_t tempInt64;
	uint8_t retVal8;
	
	if (drv->devState != 0)
		return 0xD0;	// already running
	
	drv->audDrvPtr = audDrvParam;
	if (options == NULL)
		options = &defOptions;
	drv->pulseFmt.channels = options->numChannels;
	drv->pulseFmt.rate = options->sampleRate;
	
	tempInt64 = (uint64_t)options->sampleRate * options->usecPerBuf;
	drv->bufSmpls = (uint32_t)((tempInt64 + 500000) / 1000000);
	drv->bufSize = (options->numBitsPerSmpl * drv->pulseFmt.channels / 8) * drv->bufSmpls;
	drv->bufCount = options->numBuffers ? options->numBuffers : 10;
	
	if (options->numBitsPerSmpl == 8)
		drv->pulseFmt.format = PA_SAMPLE_U8;
	else if (options->numBitsPerSmpl == 16)
		drv->pulseFmt.format = PA_SAMPLE_S16NE;
	else if (options->numBitsPerSmpl == 24)
		drv->pulseFmt.format = PA_SAMPLE_S24NE;
	else if (options->numBitsPerSmpl == 32)
		drv->pulseFmt.format = PA_SAMPLE_S32NE;
	else
		return 0xCF;
	
	drv->canPause = 1;
	
	drv->hPulse = pa_simple_new(NULL, "libvgm", PA_STREAM_PLAYBACK, NULL, drv->streamDesc, &drv->pulseFmt, NULL, NULL, NULL);
	if(!drv->hPulse)
		return 0xC0;
	
	OSSignal_Reset(drv->hSignal);
	retVal8 = OSThread_Init(&drv->hThread, &PulseThread, drv);
	if (retVal8)
	{
		pa_simple_free(drv->hPulse);
		return 0xC8;	// CreateThread failed
	}
	
	drv->bufSpace = (uint8_t*)malloc(drv->bufSize);
	
	drv->devState = 1;
	drv->pauseThread = 0x00;
	OSSignal_Signal(drv->hSignal);
	
	return AERR_OK;
}

uint8_t Pulse_Stop(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	if (drv->devState != 1)
		return 0xD8;	// is already stopped (or stopping)
	
	drv->devState = 2;
	
	OSThread_Join(drv->hThread);
	OSThread_Deinit(drv->hThread);	drv->hThread = NULL;
	
	free(drv->bufSpace);
	drv->bufSpace = NULL;
	
	pa_simple_free(drv->hPulse);
	
	drv->devState = 0;
	
	return AERR_OK;
}

uint8_t Pulse_Pause(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	drv->pauseThread |= 0x01;
	return AERR_OK;
}

uint8_t Pulse_Resume(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	drv->pauseThread &= ~0x01;
	return AERR_OK;
}

uint8_t Pulse_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	drv->pauseThread |= 0x02;
	OSMutex_Lock(drv->hMutex);
	drv->userParam = userParam;
	drv->FillBuffer = FillBufCallback;
	drv->pauseThread &= ~0x02;
	OSMutex_Unlock(drv->hMutex);
	
	return AERR_OK;
}

uint32_t Pulse_GetBufferSize(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	return drv->bufSize;
}

uint8_t Pulse_IsBusy(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	if (drv->FillBuffer != NULL)
		return AERR_BAD_MODE;
	
	return AERR_OK;
}

uint8_t Pulse_WriteData(void* drvObj, uint32_t dataSize, void* data)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	int retVal;
	
	if (dataSize > drv->bufSize)
		return AERR_TOO_MUCH_DATA;
	
	retVal = pa_simple_write(drv->hPulse, data, (size_t) dataSize, NULL);
	if (retVal > 0)
		return 0xFF;
	
	return AERR_OK;
}


uint32_t Pulse_GetLatency(void* drvObj)
{
	DRV_PULSE* drv = (DRV_PULSE*)drvObj;
	
	return (uint32_t)(pa_simple_get_latency(drv->hPulse, NULL) / 1000);
}

static void PulseThread(void* Arg)
{
	DRV_PULSE* drv = (DRV_PULSE*)Arg;
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
			retVal = pa_simple_write(drv->hPulse, drv->bufSpace, (size_t) bufBytes, NULL);
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
