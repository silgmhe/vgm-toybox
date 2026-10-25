#ifndef __GYMPLAYER_HPP__
#define __GYMPLAYER_HPP__

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


#define FCC_GYM 	0x47594D00

// GYMX header (optional, added by YMAMP WinAMP plugin)
//	Ofs	Len	Description
//	000	04	"GYMX"
//	004	20	song title
//	024	20	game name
//	044	20	publisher
//	064	20	emulator ("Dumped with")
//	084	20	file creator ("Dumped by")
//	0A4	100	comments
//	1A4	04	loop start frame (0 = no loop)
//	1A8	04	uncompressed data size (0 = data is uncompressed, 1 = data is zlib compressed)

struct GYM_HEADER
{
	uint8_t hasHeader;
	uint32_t uncomprSize;
	uint32_t loopFrame;
	uint32_t dataOfs;
	uint32_t realFileSize;	// internal file size after possible decompression
};

struct GYM_PLAY_OPTIONS
{
	PLR_GEN_OPTS genOpts;
};


class GYMPlayer : public PlayerBase
{
private:
	struct DevCfg
	{
		DEV_ID type;
		uint16_t volume;
		std::vector<uint8_t> data;
	};
	struct DEVLOG_CB_DATA
	{
		GYMPlayer* player;
		size_t chipDevID;
	};
	struct GYM_CHIPDEV
	{
		VGM_BASEDEV base;
		size_t optID;
		DEVFUNC_WRITE_A8D8 write;
		DEVLOG_CB_DATA logCbData;
	};
	
public:
	GYMPlayer();
	~GYMPlayer();
	
	uint32_t GetPlayerType(void) const;
	const char* GetPlayerName(void) const;
	static uint8_t PlayerCanLoadFile(DATA_LOADER *dataLoader);
	uint8_t CanLoadFile(DATA_LOADER *dataLoader) const;
	uint8_t LoadFile(DATA_LOADER *dataLoader);
	uint8_t UnloadFile(void);
	const GYM_HEADER* GetFileHeader(void) const;
	
	const char* const* GetTags(void);
	uint8_t GetSongInfo(PLR_SONG_INFO& songInf);
	uint8_t GetSongDeviceInfo(std::vector<PLR_DEV_INFO>& devInfList) const;
	uint8_t SetDeviceOptions(uint32_t id, const PLR_DEV_OPTS& devOpts);
	uint8_t GetDeviceOptions(uint32_t id, PLR_DEV_OPTS& devOpts) const;
	uint8_t SetDeviceMuting(uint32_t id, const PLR_MUTE_OPTS& muteOpts);
	uint8_t GetDeviceMuting(uint32_t id, PLR_MUTE_OPTS& muteOpts) const;
	uint8_t SetPlayerOptions(const GYM_PLAY_OPTIONS& playOpts);
	uint8_t GetPlayerOptions(GYM_PLAY_OPTIONS& playOpts) const;
	
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
	size_t DeviceID2OptionID(uint32_t id) const;
	void RefreshMuting(GYM_CHIPDEV& chipDev, const PLR_MUTE_OPTS& muteOpts);
	void RefreshPanning(GYM_CHIPDEV& chipDev, const PLR_PAN_OPTS& panOpts);
	
	static bool CheckRawGYMFile(uint32_t dataLen, const uint8_t* data);
	uint8_t DecompressZlibData(void);
	void CalcSongLength(void);
	uint8_t LoadTags(void);
	void LoadTag(const char* tagName, const void* data, size_t maxlen);
	std::string GetUTF8String(const char* startPtr, const char* endPtr);

	void RefreshTSRates(void);
	
	static void PlayerLogCB(void* userParam, void* source, uint8_t level, const char* message);
	static void SndEmuLogCB(void* userParam, void* source, uint8_t level, const char* message);
	
	void GenerateDeviceConfig(void);
	uint8_t SeekToTick(uint32_t tick);
	uint8_t SeekToFilePos(uint32_t pos);
	void ParseFile(uint32_t ticks);
	void DoCommand(void);
	void DoFileEnd(void);
	
	CPCONV* _cpc1252;	// CP1252 -> UTF-8 codepage conversion
	DEV_LOGGER _logger;
	DATA_LOADER* _dLoad;
	uint32_t _fileLen;
	const uint8_t* _fileData;	// data pointer for quick access, equals _dLoad->GetFileData().data()
	std::vector<uint8_t> _decFData;
	
	GYM_HEADER _fileHdr;
	std::vector<DevCfg> _devCfgs;
	uint32_t _tickFreq;
	uint32_t _totalTicks;
	uint32_t _loopOfs;
	std::map<std::string, std::string> _tagData;
	std::vector<const char*> _tagList;
	
	std::vector<uint8_t> _pcmBuffer;
	uint32_t _pcmBaseTick;
	uint32_t _pcmInPos;	// input position (GYM -> buffer)
	uint32_t _pcmOutPos;	// output position (buffer -> YM2612)
	uint8_t _ymFreqRegs[0x20];	// cache of 0x0A0..0x0AF and 0x1A0..0x1AF frequency registers
	uint8_t _ymLatch[2];	// current latch value ([0] = normal channels, [1] = CH3 multi-freq mode registers]
	
	//uint32_t _outSmplRate;
	
	// tick/sample conversion rates
	uint64_t _tsMult;
	uint64_t _tsDiv;
	uint64_t _ttMult;
	uint64_t _lastTsMult;
	uint64_t _lastTsDiv;
	
	GYM_PLAY_OPTIONS _playOpts;
	PLR_DEV_OPTS _devOpts[2];	// 0 = YM2612, 1 = SEGA PSG
	std::vector<GYM_CHIPDEV> _devices;
	std::vector<std::string> _devNames;
	size_t _optDevMap[2];	// maps _devOpts vector index to _devices vector
	
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

#endif	// __GYMPLAYER_HPP__
