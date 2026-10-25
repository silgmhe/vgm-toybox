#ifndef __PLAYERA_HPP__
#define __PLAYERA_HPP__

#include <vector>
#include "../stdtype.h"
#include "../utils/DataLoader.h"
#include "../emu/Resampler.h"	// for WAVE_32BS
#include "playerbase.hpp"

#define PLAYSTATE_FADE	0x10	// is fading
#define PLAYSTATE_FIN	0x20	// finished playing (file end + fading + trailing silence)

#define PLAYTIME_LOOP_EXCL	0x00	// excluding loops, jumps back in time when the file loops
#define PLAYTIME_LOOP_INCL	0x01	// including loops, no jumping back
#define PLAYTIME_TIME_FILE	0x00	// file time, progresses slower/faster when playback speed is adjusted
#define PLAYTIME_TIME_PBK	0x02	// playback time, file duration will be longer/shorter when playback speed is adjusted
#define PLAYTIME_WITH_FADE	0x10	// include fade out time (looping songs only)
#define PLAYTIME_WITH_SLNC	0x20	// include silence after songs

// TODO: find a proper name for this class
class PlayerA
{
public:
	struct Config
	{
		int32_t masterVol;	// master volume (16.16 fixed point, negative value = phase inversion)
		bool ignoreVolGain;	// ignore track-specific volume gain
		uint8_t chnInvert;	// channel phase inversion (bit 0 - left, bit 1 - right)
		uint32_t loopCount;
		uint32_t fadeSmpls;
		uint32_t endSilenceSmpls;
		double pbSpeed;
	};
	typedef void (*PLR_SMPL_PACK)(void* buffer, int32_t value);

	PlayerA();
	~PlayerA();
	void RegisterPlayerEngine(PlayerBase* player);
	void UnregisterAllPlayers(void);
	const std::vector<PlayerBase*>& GetRegisteredPlayers(void) const;
	
	uint8_t SetOutputSettings(uint32_t smplRate, uint8_t channels, uint8_t smplBits, uint32_t smplBufferLen);
	uint32_t GetSampleRate(void) const;
	void SetSampleRate(uint32_t sampleRate);
	double GetPlaybackSpeed(void) const;
	void SetPlaybackSpeed(double speed);
	uint32_t GetLoopCount(void) const;
	void SetLoopCount(uint32_t loops);
	int32_t GetMasterVolume(void) const;
	void SetMasterVolume(int32_t volume);
	int32_t GetSongVolume(void) const;
	uint32_t GetFadeSamples(void) const;
	void SetFadeSamples(uint32_t smplCnt);
	uint32_t GetEndSilenceSamples(void) const;
	void SetEndSilenceSamples(uint32_t smplCnt);
	const Config& GetConfiguration(void) const;
	void SetConfiguration(const Config& config);

	void SetEventCallback(PLAYER_EVENT_CB cbFunc, void* cbParam);
	void SetFileReqCallback(PLAYER_FILEREQ_CB cbFunc, void* cbParam);
	void SetLogCallback(PLAYER_LOG_CB cbFunc, void* cbParam);
	uint8_t GetState(void) const;
	uint32_t GetCurPos(uint8_t unit) const;
	double GetCurTime(uint8_t flags) const;	// TODO: add GetCurSample()
	double GetTotalTime(uint8_t flags) const;	// TODO: add GetTotalSamples()
	uint32_t GetCurLoop(void) const;
	double GetLoopTime(void) const;	// TODO: add GetLoopSamples()
	PlayerBase* GetPlayer(void);
	const PlayerBase* GetPlayer(void) const;
	
	uint8_t LoadFile(DATA_LOADER* dLoad);
	uint8_t UnloadFile(void);
	uint32_t GetFileSize(void);
	uint8_t Start(void);
	uint8_t Stop(void);
	uint8_t Reset(void);
	uint8_t FadeOut(void);
	uint8_t Seek(uint8_t unit, uint32_t pos);
	uint32_t Render(uint32_t bufSize, void* data);
private:
	void FindPlayerEngine(void);
	int32_t CalcSongVolume(void);
	int32_t CalcCurrentVolume(uint32_t playbackSmpl);
	static uint8_t PlayCallbackS(PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam);
	uint8_t PlayCallback(PlayerBase* player, uint8_t evtType, void* evtParam);
	
	std::vector<PlayerBase*> _avbPlrs;	// available players
	uint32_t _smplRate;
	Config _config;
	PLAYER_EVENT_CB _plrCbFunc;
	void* _plrCbParam;
	uint8_t _myPlayState;
	
	uint8_t _outSmplChns;
	uint8_t _outSmplBits;
	uint32_t _outSmplSize1;	// for 1 channel
	uint32_t _outSmplSizeA;	// for all channels
	PLR_SMPL_PACK _outSmplPack;
	std::vector<WAVE_32BS> _smplBuf;
	PlayerBase* _player;
	DATA_LOADER* _dLoad;
	int32_t _songVolume;
	uint32_t _fadeSmplStart;
	uint32_t _endSilenceStart;
};

#endif	// __PLAYERA_HPP__
