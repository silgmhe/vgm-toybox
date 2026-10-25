#ifndef __S98PLAYER_HPP__
#define __S98PLAYER_HPP__

#include "../stdtype.h"
#include "../emu/EmuStructs.h"
#include "../emu/Resampler.h"
#include "../utils/StrUtils.h"
#include "helper.h"
#include "playerbase.hpp"
#include "../utils/DataLoader.h"
#include "../emu/logging.h"
#include <vector>
#include <map>
#include <string>


#define FCC_S98 	0x53393800

struct S98_HEADER
{
	uint8_t fileVer;
	uint32_t tickMult;	// [v1] tick timing numerator
	uint32_t tickDiv;		// [v2] tick timing denumerator
	uint32_t compression;	// [v1: 0 - no compression, >0 - size of uncompressed data] [v2: ??] [v3: must be 0]
	uint32_t tagOfs;		// [v1/2: song title file offset] [v3: tag data file offset]
	uint32_t dataOfs;		// play data file offset
	uint32_t loopOfs;		// loop file offset
};
struct S98_DEVICE
{
	uint32_t devType;
	uint32_t clock;
	uint32_t pan;			// [v2: reserved] [v3: pan setting]
	uint32_t app_spec;	// [v2: application-specific] [v3: reserved]
};
struct S98_PLAY_OPTIONS
{
	PLR_GEN_OPTS genOpts;
};


class S98Player : public PlayerBase
{
private:
	struct DevCfgBuffer
	{
		std::vector<uint8_t> data;
	};
	struct DEVLOG_CB_DATA
	{
		S98Player* player;
		size_t chipDevID;
	};
	struct S98_CHIPDEV
	{
		VGM_BASEDEV base;
		size_t optID;
		std::vector<uint8_t> cfg;
		DEVFUNC_WRITE_A8D8 write;
		DEVLOG_CB_DATA logCbData;
	};
	struct DEVLINK_CB_DATA
	{
		S98Player* player;
		S98_CHIPDEV* chipDev;
	};
	
public:
	S98Player();
	~S98Player();
	
	uint32_t GetPlayerType(void) const;
	const char* GetPlayerName(void) const;
	static uint8_t PlayerCanLoadFile(DATA_LOADER *dataLoader);
	uint8_t CanLoadFile(DATA_LOADER *dataLoader) const;
	uint8_t LoadFile(DATA_LOADER *dataLoader);
	uint8_t UnloadFile(void);
	const S98_HEADER* GetFileHeader(void) const;
	
	const char* const* GetTags(void);
	uint8_t GetSongInfo(PLR_SONG_INFO& songInf);
	uint8_t GetSongDeviceInfo(std::vector<PLR_DEV_INFO>& devInfList) const;
	uint8_t SetDeviceOptions(uint32_t id, const PLR_DEV_OPTS& devOpts);
	uint8_t GetDeviceOptions(uint32_t id, PLR_DEV_OPTS& devOpts) const;
	uint8_t SetDeviceMuting(uint32_t id, const PLR_MUTE_OPTS& muteOpts);
	uint8_t GetDeviceMuting(uint32_t id, PLR_MUTE_OPTS& muteOpts) const;
	uint8_t SetPlayerOptions(const S98_PLAY_OPTIONS& playOpts);
	uint8_t GetPlayerOptions(S98_PLAY_OPTIONS& playOpts) const;
	
	//uint32_t GetSampleRate(void) const;
	uint8_t SetSampleRate(uint32_t sampleRate);
	double GetPlaybackSpeed(void) const;
	uint8_t SetPlaybackSpeed(double speed);
	//void SetEventCallback(PLAYER_EVENT_CB cbFunc, void* cbParam);
	uint32_t Tick2Sample(uint32_t ticks) const;
	uint32_t Sample2Tick(uint32_t samples) const;
	double Tick2Second(uint32_t ticks) const;
	//double Sample2Second(uint32_t samples) const;
	
	uint8_t GetState(void) const;
	uint32_t GetCurPos(uint8_t unit) const;
	uint32_t GetCurLoop(void) const;
	uint32_t GetTotalTicks(void) const;	// get time for playing once in ticks
	uint32_t GetLoopTicks(void) const;	// get time for one loop in ticks
	//uint32_t GetTotalPlayTicks(uint32_t numLoops) const;	// get time for playing + looping (without fading)
	
	uint8_t Start(void);
	uint8_t Stop(void);
	uint8_t Reset(void);
	uint8_t Seek(uint8_t unit, uint32_t pos);
	uint32_t Render(uint32_t smplCnt, WAVE_32BS* data);
	
private:
	size_t GetDeviceInstance(size_t id) const;
	size_t DeviceID2OptionID(uint32_t id) const;
	void RefreshMuting(S98_CHIPDEV& chipDev, const PLR_MUTE_OPTS& muteOpts);
	void RefreshPanning(S98_CHIPDEV& chipDev, const PLR_PAN_OPTS& panOpts);
	
	void CalcSongLength(void);
	uint8_t LoadTags(void);
	std::string GetUTF8String(const char* startPtr, const char* endPtr);
	uint8_t ParsePSFTags(const std::string& tagData);
	uint32_t ReadVarInt(uint32_t& filePos);
	
	void RefreshTSRates(void);
	
	static void PlayerLogCB(void* userParam, void* source, uint8_t level, const char* message);
	static void SndEmuLogCB(void* userParam, void* source, uint8_t level, const char* message);
	
	void GenerateDeviceConfig(void);
	static void DeviceLinkCallback(void* userParam, VGM_BASEDEV* cDev, DEVLINK_INFO* dLink);
	uint8_t SeekToTick(uint32_t tick);
	uint8_t SeekToFilePos(uint32_t pos);
	void ParseFile(uint32_t ticks);
	void HandleEOF(void);
	void DoCommand(void);
	void DoRegWrite(uint8_t deviceID, uint8_t port, uint8_t reg, uint8_t data);
	
	enum
	{
		_OPT_DEV_COUNT = 0x0A
	};
	
	CPCONV* _cpcSJIS;	// ShiftJIS -> UTF-8 codepage conversion
	DEV_LOGGER _logger;
	DATA_LOADER *_dLoad;
	const uint8_t* _fileData;	// data pointer for quick access, equals _dLoad->GetFileData().data()
	
	S98_HEADER _fileHdr;
	std::vector<S98_DEVICE> _devHdrs;
	std::vector<DevCfgBuffer> _devCfgs;
	uint32_t _totalTicks;
	uint32_t _loopTick;
	std::map<std::string, std::string> _tagData;
	std::vector<const char*> _tagList;
	
	//uint32_t _outSmplRate;
	
	// tick/sample conversion rates
	uint64_t _tsMult;
	uint64_t _tsDiv;
	uint64_t _ttMult;
	uint64_t _lastTsMult;
	uint64_t _lastTsDiv;
	
	static const DEV_ID _OPT_DEV_LIST[_OPT_DEV_COUNT];	// list of configurable libvgm devices
	
	S98_PLAY_OPTIONS _playOpts;
	PLR_DEV_OPTS _devOpts[_OPT_DEV_COUNT * 2];	// space for 2 instances per chip
	size_t _devOptMap[0x100][2];	// maps libvgm device ID to _devOpts vector
	std::vector<S98_CHIPDEV> _devices;
	std::vector<std::string> _devNames;
	size_t _optDevMap[_OPT_DEV_COUNT * 2];	// maps _devOpts vector index to _devices vector
	
	uint32_t _filePos;
	uint32_t _fileTick;
	uint32_t _playTick;
	uint32_t _playSmpl;
	uint32_t _curLoop;
	uint32_t _lastLoopTick;
	
	uint8_t _playState;
	uint8_t _psTrigger;	// used to temporarily trigger special commands
	//PLAYER_EVENT_CB _eventCbFunc;
	//void* _eventCbParam;
};

#endif	// __S98PLAYER_HPP__
