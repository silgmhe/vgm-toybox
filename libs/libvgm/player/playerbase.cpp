#include "playerbase.hpp"

#include <stdlib.h>
#include <string.h>	// for memset()

PlayerBase::PlayerBase() :
	_outSmplRate(0),
	_userDevList(NULL),
	_devStartOpts(0x00),
	_eventCbFunc(NULL),
	_eventCbParam(NULL),
	_fileReqCbFunc(NULL),
	_fileReqCbParam(NULL),
	_logCbFunc(NULL),
	_logCbParam(NULL)
{
}

PlayerBase::~PlayerBase()
{
}

uint32_t PlayerBase::GetPlayerType(void) const
{
	return 0x00000000;
}

const char* PlayerBase::GetPlayerName(void) const
{
	return "";
}

/*static*/ uint8_t PlayerBase::PlayerCanLoadFile(DATA_LOADER *dataLoader)
{
	return 0xFF;
}

uint8_t PlayerBase::CanLoadFile(DATA_LOADER *dataLoader) const
{
	return this->PlayerCanLoadFile(dataLoader);
}

/*static*/ uint8_t PlayerBase::InitDeviceOptions(PLR_DEV_OPTS& devOpts)
{
	devOpts.emuCore[0] = 0x00;
	devOpts.emuCore[1] = 0x00;
	devOpts.srMode = DEVRI_SRMODE_NATIVE;
	devOpts.resmplMode = 0x00;
	devOpts.smplRate = 0;
	devOpts.coreOpts = 0x00;
	devOpts.muteOpts.disable = 0x00;
	devOpts.muteOpts.chnMute[0] = 0x00;
	devOpts.muteOpts.chnMute[1] = 0x00;
	memset(devOpts.panOpts.chnPan, 0x00, sizeof(devOpts.panOpts.chnPan));
	return 0x00;
}

uint32_t PlayerBase::GetSampleRate(void) const
{
	return _outSmplRate;
}

uint8_t PlayerBase::SetSampleRate(uint32_t sampleRate)
{
	_outSmplRate = sampleRate;
	return 0x00;
}

double PlayerBase::GetPlaybackSpeed(void) const
{
	return -1;	// not implemented
}

uint8_t PlayerBase::SetPlaybackSpeed(double speed)
{
	return 0xFF;	// not implemented
}

void PlayerBase::SetUserDevices(const DEV_DECL** userDevList, uint8_t devStartOpts)
{
	_userDevList = userDevList;
	_devStartOpts = devStartOpts;
	
	return;
}

void PlayerBase::SetEventCallback(PLAYER_EVENT_CB cbFunc, void* cbParam)
{
	_eventCbFunc = cbFunc;
	_eventCbParam = cbParam;
	
	return;
}

void PlayerBase::SetFileReqCallback(PLAYER_FILEREQ_CB cbFunc, void* cbParam)
{
	_fileReqCbFunc = cbFunc;
	_fileReqCbParam = cbParam;
	
	return;
}

void PlayerBase::SetLogCallback(PLAYER_LOG_CB cbFunc, void* cbParam)
{
	_logCbFunc = cbFunc;
	_logCbParam = cbParam;
	
	return;
}

double PlayerBase::Sample2Second(uint32_t samples) const
{
	if (samples == (uint32_t)-1)
		return -1.0;
	return samples / (double)_outSmplRate;
}

uint32_t PlayerBase::GetTotalPlayTicks(uint32_t numLoops) const
{
	if (numLoops == 0 && GetLoopTicks() > 0)
		return (uint32_t)-1;
	return GetTotalTicks() + GetLoopTicks() * (numLoops - 1);
}
