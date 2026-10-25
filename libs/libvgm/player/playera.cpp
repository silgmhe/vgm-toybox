#include <string.h>
#include <vector>

#include "../stdtype.h"
#include "../common_def.h"
#include "../utils/DataLoader.h"
#include "playerbase.hpp"
#include "../emu/Resampler.h"

#include "playera.hpp"

static void SampleConv_toU8(void* buffer, int32_t value)
{
	value >>= 16;	// 24 bit -> 8 bit
	if (value < -0x80)
		value = -0x80;
	else if (value > +0x7F)
		value = +0x7F;
	*(uint8_t*)buffer = (uint8_t)(0x80 + value);
	return;
}

static void SampleConv_toS16(void* buffer, int32_t value)
{
	int16_t v;
	value >>= 8;	// 24 bit -> 16 bit
	if (value < -0x8000)
		value = -0x8000;
	else if (value > +0x7FFF)
		value = +0x7FFF;
	v = (int16_t)value;
	memcpy(buffer,&v,sizeof(v));
	return;
}

static void SampleConv_toS24(void* buffer, int32_t value)
{
	uint8_t tmp[3];
	if (value < -0x800000)
		value = -0x800000;
	else if (value > +0x7FFFFF)
		value = +0x7FFFFF;
#if defined(VGM_LITTLE_ENDIAN)
	tmp[0] = ( value       ) & 0xFF;
	tmp[1] = ( value >> 8  ) & 0xFF;
	tmp[2] = ( value >> 16 ) & 0xFF;
#elif defined(VGM_BIG_ENDIAN)
	tmp[0] = ( value >> 16 ) & 0xFF;
	tmp[1] = ( value >> 8  ) & 0xFF;
	tmp[2] = ( value       ) & 0xFF;
#else
#error unknown endianness
#endif
	memcpy(buffer,tmp,sizeof(tmp));
	return;
}

static void SampleConv_toS32(void* buffer, int32_t value)
{
	// internal scale is 24-bit, so limit to that
	if (value < -0x800000)
		value = -0x800000;
	else if (value > +0x7FFFFF)
		value = +0x7FFFFF;
	value *= (1 << 8);	// 24 bit -> 32 bit
	memcpy(buffer,&value,sizeof(value));
	return;
}

static void SampleConv_toF32(void* buffer, int32_t value)
{
	// limiting not required here
	float v = value / (float)0x800000;
	memcpy(buffer,&v,sizeof(v));
	return;
}

static PlayerA::PLR_SMPL_PACK GetSampleConvFunc(uint8_t bits)
{
	if (bits == 8)
		return SampleConv_toU8;
	else if (bits == 16)
		return SampleConv_toS16;
	else if (bits == 24)
		return SampleConv_toS24;
	else if (bits == 32)
		return SampleConv_toS32;
	else
		return NULL;
}

PlayerA::PlayerA()
{
	_config.masterVol = 0x10000;	// fixed point 16.16
	_config.ignoreVolGain = false;
	_config.chnInvert = 0x00;
	_config.loopCount = 2;
	_config.fadeSmpls = 0;
	_config.endSilenceSmpls = 0;
	_config.pbSpeed = 1.0;
	
	_outSmplChns = 2;
	_outSmplBits = 16;
	_outSmplPack = GetSampleConvFunc(_outSmplBits);
	_smplRate = 44100;
	_outSmplSize1 = _outSmplBits / 8;
	_outSmplSizeA = _outSmplSize1 * _outSmplChns;
	
	_plrCbFunc = NULL;
	_plrCbParam = NULL;
	_myPlayState = 0x00;
	_player = NULL;
	_dLoad = NULL;
	_songVolume = CalcSongVolume();
	_fadeSmplStart = (uint32_t)-1;
	_endSilenceStart = (uint32_t)-1;
	
	return;
}

PlayerA::~PlayerA()
{
	Stop();
	UnloadFile();
	UnregisterAllPlayers();
	return;
}

void PlayerA::RegisterPlayerEngine(PlayerBase* player)
{
	player->SetEventCallback(PlayerA::PlayCallbackS, this);
	//player->SetFileReqCallback(_frCbFunc, _frCbParam);
	player->SetSampleRate(_smplRate);
	player->SetPlaybackSpeed(_config.pbSpeed);
	_avbPlrs.push_back(player);
	return;
}

void PlayerA::UnregisterAllPlayers(void)
{
	for (size_t curPlr = 0; curPlr < _avbPlrs.size(); curPlr ++)
		delete _avbPlrs[curPlr];
	_avbPlrs.clear();
	return;
}

const std::vector<PlayerBase*>& PlayerA::GetRegisteredPlayers(void) const
{
	return _avbPlrs;
}

uint8_t PlayerA::SetOutputSettings(uint32_t smplRate, uint8_t channels, uint8_t smplBits, uint32_t smplBufferLen)
{
	if (channels != 2)
		return 0xF0;	// TODO: support channels = 1
	PLR_SMPL_PACK smplPackFunc = GetSampleConvFunc(smplBits);
	if (smplPackFunc == NULL)
		return 0xF1;	// unsupported sample format
	
	_outSmplChns = channels;
	_outSmplBits = smplBits;
	_outSmplPack = smplPackFunc;
	SetSampleRate(smplRate);
	_outSmplSize1 = _outSmplBits / 8;
	_outSmplSizeA = _outSmplSize1 * _outSmplChns;
	_smplBuf.resize(smplBufferLen);
	return 0x00;
}

uint32_t PlayerA::GetSampleRate(void) const
{
	return _smplRate;
}

void PlayerA::SetSampleRate(uint32_t sampleRate)
{
	_smplRate = sampleRate;
	for (size_t curPlr = 0; curPlr < _avbPlrs.size(); curPlr++)
	{
		if (_avbPlrs[curPlr] == _player && (_player->GetState() & PLAYSTATE_PLAY))
			continue;
		_avbPlrs[curPlr]->SetSampleRate(_smplRate);
	}
	return;
}

double PlayerA::GetPlaybackSpeed(void) const
{
	return _config.pbSpeed;
}

void PlayerA::SetPlaybackSpeed(double speed)
{
	_config.pbSpeed = speed;
	for (size_t curPlr = 0; curPlr < _avbPlrs.size(); curPlr++)
		_avbPlrs[curPlr]->SetPlaybackSpeed(_config.pbSpeed);
	return;
}

uint32_t PlayerA::GetLoopCount(void) const
{
	return _config.loopCount;
}

void PlayerA::SetLoopCount(uint32_t loops)
{
	_config.loopCount = loops;
	return;
}

uint32_t PlayerA::GetFadeSamples(void) const
{
	return _config.fadeSmpls;
}

int32_t PlayerA::GetMasterVolume(void) const
{
	return _config.masterVol;
}

void PlayerA::SetMasterVolume(int32_t volume)
{
	_config.masterVol = volume;
	_songVolume = CalcSongVolume();
	return;
}

int32_t PlayerA::GetSongVolume(void) const
{
	return _songVolume;
}

void PlayerA::SetFadeSamples(uint32_t smplCnt)
{
	_config.fadeSmpls = smplCnt;
	return;
}

uint32_t PlayerA::GetEndSilenceSamples(void) const
{
	return _config.endSilenceSmpls;
}

void PlayerA::SetEndSilenceSamples(uint32_t smplCnt)
{
	_config.endSilenceSmpls = smplCnt;
	return;
}

const PlayerA::Config& PlayerA::GetConfiguration(void) const
{
	return _config;
}

void PlayerA::SetConfiguration(const PlayerA::Config& config)
{
	double oldPbSpeed = _config.pbSpeed;
	
	_config = config;
	SetMasterVolume(_config.masterVol);
	if (oldPbSpeed != _config.pbSpeed)
		SetPlaybackSpeed(_config.pbSpeed);	// refresh in all players
	return;
}

void PlayerA::SetEventCallback(PLAYER_EVENT_CB cbFunc, void* cbParam)
{
	_plrCbFunc = cbFunc;
	_plrCbParam = cbParam;
	return;
}

void PlayerA::SetFileReqCallback(PLAYER_FILEREQ_CB cbFunc, void* cbParam)
{
	for (size_t curPlr = 0; curPlr < _avbPlrs.size(); curPlr ++)
		_avbPlrs[curPlr]->SetFileReqCallback(cbFunc, cbParam);
	return;
}

void PlayerA::SetLogCallback(PLAYER_LOG_CB cbFunc, void* cbParam)
{
	for (size_t curPlr = 0; curPlr < _avbPlrs.size(); curPlr ++)
		_avbPlrs[curPlr]->SetLogCallback(cbFunc, cbParam);
	return;
}

uint8_t PlayerA::GetState(void) const
{
	if (_player == NULL)
		return 0x00;
	uint8_t finalState = _myPlayState;
	if (_fadeSmplStart != (uint32_t)-1)
		finalState |= PLAYSTATE_FADE;
	return finalState;
}

uint32_t PlayerA::GetCurPos(uint8_t unit) const
{
	if (_player == NULL)
		return (uint32_t)-1;
	return _player->GetCurPos(unit);
}

double PlayerA::GetCurTime(uint8_t flags) const
{
	if (_player == NULL)
		return -1.0;
	
	// using samples here, as it may be more accurate than the (possibly low-resolution) ticks
	double secs = _player->Sample2Second(_player->GetCurPos(PLAYPOS_SAMPLE));
	uint32_t curLoop = _player->GetCurLoop();
	if (! (flags & PLAYTIME_LOOP_INCL) && curLoop > 0)
		secs -= _player->Tick2Second(_player->GetLoopTicks() * curLoop);
	
	if (! (flags & PLAYTIME_TIME_PBK))
		secs *= _player->GetPlaybackSpeed();
	return secs;
}

double PlayerA::GetTotalTime(uint8_t flags) const
{
	if (_player == NULL)
		return -1.0;
	
	double secs;
	if (flags & PLAYTIME_LOOP_INCL)
		secs = _player->Tick2Second(_player->GetTotalPlayTicks(_config.loopCount));
	else
		secs = _player->Tick2Second(_player->GetTotalPlayTicks(1));
	if (secs < 0.0)	// indicates infinite runtime
		return secs;
	
	// Fade and silence time are unaffected by playback speed and thus must be applied before speed scaling.
	if ((flags & PLAYTIME_WITH_FADE) && _player->GetLoopTicks() > 0)
		secs += _player->Sample2Second(GetFadeSamples());
	if (flags & PLAYTIME_WITH_SLNC)
		secs += _player->Sample2Second(GetEndSilenceSamples());
	
	if (! (flags & PLAYTIME_TIME_PBK))
		secs *= _player->GetPlaybackSpeed();
	return secs;
}

uint32_t PlayerA::GetCurLoop(void) const
{
	if (_player == NULL)
		return (uint32_t)-1;
	return _player->GetCurLoop();
}

double PlayerA::GetLoopTime(void) const
{
	if (_player == NULL)
		return -1.0;
	return _player->Tick2Second(_player->GetLoopTicks());
}

PlayerBase* PlayerA::GetPlayer(void)
{
	return _player;
}

const PlayerBase* PlayerA::GetPlayer(void) const
{
	return _player;
}

void PlayerA::FindPlayerEngine(void)
{
	size_t curPlr;
	
	_player = NULL;
	for (curPlr = 0; curPlr < _avbPlrs.size(); curPlr ++)
	{
		uint8_t retVal = _avbPlrs[curPlr]->CanLoadFile(_dLoad);
		if (! retVal)
		{
			_player = _avbPlrs[curPlr];
			return;
		}
	}
	
	return;
}

uint8_t PlayerA::LoadFile(DATA_LOADER* dLoad)
{
	_dLoad = dLoad;
	FindPlayerEngine();
	if (_player == NULL)
		return 0xFF;
	
	// set the configuration so that the configuration is loaded properly
	_player->SetSampleRate(_smplRate);
	_player->SetPlaybackSpeed(_config.pbSpeed);
	
	uint8_t retVal = _player->LoadFile(dLoad);
	if (retVal >= 0x80)
		return retVal;
	return retVal;
}

uint8_t PlayerA::UnloadFile(void)
{
	if (_player == NULL)
		return 0xFF;
	
	_player->Stop();
	uint8_t retVal = _player->UnloadFile();
	_player = NULL;
	_dLoad = NULL;
	return retVal;
}

uint32_t PlayerA::GetFileSize(void)
{
	if (_dLoad == NULL)
		return 0x00;
	
	uint32_t result = DataLoader_GetTotalSize(_dLoad);
	if (result != (uint32_t)-1)
		return result;
	else
		return DataLoader_GetSize(_dLoad);
}

uint8_t PlayerA::Start(void)
{
	if (_player == NULL)
		return 0xFF;
	_player->SetSampleRate(_smplRate);
	_player->SetPlaybackSpeed(_config.pbSpeed);
	_songVolume = CalcSongVolume();
	_fadeSmplStart = (uint32_t)-1;
	_endSilenceStart = (uint32_t)-1;
	
	uint8_t retVal = _player->Start();
	_myPlayState = _player->GetState() & (PLAYSTATE_PLAY | PLAYSTATE_END);
	return retVal;
}

uint8_t PlayerA::Stop(void)
{
	if (_player == NULL)
		return 0xFF;
	uint8_t retVal = _player->Stop();
	_myPlayState = _player->GetState() & (PLAYSTATE_PLAY | PLAYSTATE_END);
	_myPlayState |= PLAYSTATE_FIN;
	return retVal;
}

uint8_t PlayerA::Reset(void)
{
	if (_player == NULL)
		return 0xFF;
	_fadeSmplStart = (uint32_t)-1;
	_endSilenceStart = (uint32_t)-1;
	uint8_t retVal = _player->Reset();
	_myPlayState = _player->GetState() & (PLAYSTATE_PLAY | PLAYSTATE_END);
	return retVal;
}

uint8_t PlayerA::FadeOut(void)
{
	if (_player == NULL)
		return 0xFF;
	if (_fadeSmplStart == (uint32_t)-1)
		_fadeSmplStart = _player->GetCurPos(PLAYPOS_SAMPLE);
	return 0x00;
}

uint8_t PlayerA::Seek(uint8_t unit, uint32_t pos)
{
	if (_player == NULL)
		return 0xFF;
	uint8_t retVal = _player->Seek(unit, pos);
	_myPlayState = _player->GetState() & (PLAYSTATE_PLAY | PLAYSTATE_END);
	
	uint32_t pbSmpl = _player->GetCurPos(PLAYPOS_SAMPLE);
	if (pbSmpl < _fadeSmplStart)
		_fadeSmplStart = (uint32_t)-1;
	if (pbSmpl < _endSilenceStart)
		_endSilenceStart = (uint32_t)-1;
	return retVal;
}

#if 1
#define VOLCALC64
#define VOL_BITS	16	// use .X fixed point for working volume
#else
#define VOL_BITS	8	// use .X fixed point for working volume
#endif
#define VOL_SHIFT	(16 - VOL_BITS)	// shift for master volume -> working volume

// Pre- and post-shifts are used to make the calculations as accurate as possible
// without causing the sample data (likely 24 bits) to overflow while applying the volume gain.
// Smaller values for VOL_PRESH are more accurate, but have a higher risk of overflows during calculations.
// (24 + VOL_POSTSH) must NOT be larger than 31
#define VOL_PRESH	4	// sample data pre-shift
#define VOL_POSTSH	(VOL_BITS - VOL_PRESH)	// post-shift after volume multiplication

// 16.16 fixed point multiplication
#define MUL16X16_FIXED(a, b)	(int32_t)(((int64_t)a * b) >> 16)

int32_t PlayerA::CalcSongVolume(void)
{
	int32_t volume = _config.masterVol;
	
	if (! _config.ignoreVolGain && _player != NULL)
	{
		PLR_SONG_INFO songInfo;
		uint8_t retVal = _player->GetSongInfo(songInfo);
		if (! retVal)
			volume = MUL16X16_FIXED(volume, songInfo.volGain);
	}
	
	return volume;
}

int32_t PlayerA::CalcCurrentVolume(uint32_t playbackSmpl)
{
	int32_t curVol;	// 16.16 fixed point
	
	// 1. master volume
	curVol = _songVolume;
	
	// 2. apply fade-out factor
	if (playbackSmpl >= _fadeSmplStart)
	{
		uint32_t fadeSmpls;
		uint64_t fadeVol;	// 64 bit for less type casts when doing multiplications with .16 fixed point
		
		fadeSmpls = playbackSmpl - _fadeSmplStart;
		if (fadeSmpls >= _config.fadeSmpls)
			return 0x0000;	// going beyond fade time -> volume 0
		
		fadeVol = (uint64_t)fadeSmpls * 0x10000 / _config.fadeSmpls;
		fadeVol = 0x10000 - fadeVol;	// fade from full volume to silence
		fadeVol = fadeVol * fadeVol;	// logarithmic fading sounds nicer
		curVol = (int32_t)(((int64_t)fadeVol * curVol) >> 32);
	}
	
	return curVol;
}

uint32_t PlayerA::Render(uint32_t bufSize, void* data)
{
	uint8_t* bData = (uint8_t*)data;
	uint32_t basePbSmpl;
	uint32_t smplCount;
	uint32_t smplRendered;
	uint32_t curSmpl;
	WAVE_32BS fnlSmpl;	// final sample value
	int32_t curVolume;
	
	smplCount = bufSize / _outSmplSizeA;
	if (_player == NULL)
	{
		memset(data, 0x00, smplCount * _outSmplSizeA);
		return smplCount * _outSmplSizeA;
	}
	if (! (_player->GetState() & PLAYSTATE_PLAY))
	{
		//fprintf(stderr, "Player Warning: calling Render while not playing! playState = 0x%02X\n", _player->GetState());
		memset(data, 0x00, smplCount * _outSmplSizeA);
		return smplCount * _outSmplSizeA;
	}
	
	if (! smplCount)
	{
		_player->Render(0, NULL);	// dummy-rendering
		return 0;
	}
	
	if (smplCount > (uint32_t)_smplBuf.size())
		smplCount = (uint32_t)_smplBuf.size();
	memset(&_smplBuf[0], 0, smplCount * sizeof(WAVE_32BS));
	basePbSmpl = _player->GetCurPos(PLAYPOS_SAMPLE);
	smplRendered = _player->Render(smplCount, &_smplBuf[0]);
	smplCount = smplRendered;
	
	curVolume = CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
	for (curSmpl = 0; curSmpl < smplCount; curSmpl ++, basePbSmpl ++)
	{
		if (basePbSmpl >= _fadeSmplStart)
		{
			uint32_t fadeSmpls = basePbSmpl - _fadeSmplStart;
			if (fadeSmpls >= _config.fadeSmpls && ! (_myPlayState & PLAYSTATE_END))
			{
				if (_endSilenceStart == (uint32_t)-1)
					_endSilenceStart = basePbSmpl;
				_myPlayState |= PLAYSTATE_END;
			}
			
			curVolume = CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
		}
		if (basePbSmpl >= _endSilenceStart)
		{
			uint32_t silenceSmpls = basePbSmpl - _endSilenceStart;
			if (silenceSmpls >= _config.endSilenceSmpls && ! (_myPlayState & PLAYSTATE_FIN))
			{
				_myPlayState |= PLAYSTATE_FIN;
				if (_plrCbFunc != NULL)
					_plrCbFunc(_player, _plrCbParam, PLREVT_END, NULL);
				// NOTE: We are effectively discarding rendered samples here!
				// We can get away with that for now, as the application is supposed to
				// stop playback at this point, but we shouldn't really do this.
				break;
			}
		}
		
		// Input is about 24 bits (some cores might output a bit more)
		fnlSmpl = _smplBuf[curSmpl];
		
#ifdef VOLCALC64
		fnlSmpl.L = (int32_t)( ((int64_t)fnlSmpl.L * curVolume) >> VOL_BITS );
		fnlSmpl.R = (int32_t)( ((int64_t)fnlSmpl.R * curVolume) >> VOL_BITS );
#else
		fnlSmpl.L = ((fnlSmpl.L >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
		fnlSmpl.R = ((fnlSmpl.R >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
#endif
		
		if (_config.chnInvert & 0x01)
			fnlSmpl.L = -fnlSmpl.L;
		if (_config.chnInvert & 0x02)
			fnlSmpl.R = -fnlSmpl.R;
		
		_outSmplPack(&bData[(curSmpl * 2 + 0) * _outSmplSize1], fnlSmpl.L);
		_outSmplPack(&bData[(curSmpl * 2 + 1) * _outSmplSize1], fnlSmpl.R);
	}
	
	return curSmpl * _outSmplSizeA;
}

/*static*/ uint8_t PlayerA::PlayCallbackS(PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam)
{
	PlayerA* plr = (PlayerA*)userParam;
	return plr->PlayCallback(player, evtType, evtParam);
}

uint8_t PlayerA::PlayCallback(PlayerBase* player, uint8_t evtType, void* evtParam)
{
	uint8_t retVal = 0x00;
	
	if (evtType != PLREVT_END)	// We will generate our own PLREVT_END event depending on fading/endSilence.
	{
		if (_plrCbFunc != NULL)
			retVal = _plrCbFunc(player, _plrCbParam, evtType, evtParam);
		if (retVal)
			return retVal;
	}
	
	switch(evtType)
	{
	case PLREVT_START:
	case PLREVT_STOP:
		break;
	case PLREVT_LOOP:
		{
			uint32_t* curLoop = (uint32_t*)evtParam;
			if (_config.loopCount > 0 && *curLoop >= _config.loopCount)
			{
				//if (_config.fadeSmpls == 0)
				//	return 0x01;	// send "stop" signal to player engine
				FadeOut();
			}
		}
		break;
	case PLREVT_END:
		_myPlayState |= PLAYSTATE_END;
		_endSilenceStart = player->GetCurPos(PLAYPOS_SAMPLE);
		break;
	}
	return 0x00;
}
