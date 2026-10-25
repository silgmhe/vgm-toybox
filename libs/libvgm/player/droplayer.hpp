#ifndef __DROPLAYER_HPP__
#define __DROPLAYER_HPP__

#include "../stdtype.h"
#include "../emu/EmuStructs.h"
#include "../emu/Resampler.h"
#include "helper.h"
#include "playerbase.hpp"
#include "../utils/DataLoader.h"
#include "../emu/logging.h"
#include <vector>
#include <string>


#define FCC_DRO 	0x44524F00

// DRO v0 header (DOSBox 0.62, SVN r1864)
//	Ofs	Len	Description
//	00	08	"DBRAWOPL"
//	04	04	data length in milliseconds
//	08	04	data length in bytes
//	0C	01	hardware type (0 = OPL2, 1 = OPL3, 2 = DualOPL2)

// DRO v1 header (DOSBox 0.63, SVN r2065)
//	Ofs	Len	Description
//	00	08	"DBRAWOPL"
//	04	04	version minor
//	08	04	version major
//	0C	04	data length in milliseconds
//	10	04	data length in bytes
//	14	04	hardware type (0 = OPL2, 1 = OPL3, 2 = DualOPL2)

// DRO v2 header (DOSBox 0.73, SVN r3178)
//	Ofs	Len	Description
//	00	08	"DBRAWOPL"
//	04	04	version major
//	08	04	version minor
//	0C	04	data length in "pairs" (1 pair = 2 bytes, a pair consists of command + data)
//	10	04	data length in milliseconds
//	14	01	hardware type (0 = OPL2, 1 = DualOPL2, 2 = OPL3)
//	15	01	data format (0 = interleaved command/data)
//	16	01	compression (0 = no compression)
//	17	01	command code for short delay
//	18	01	command code for long delay
//	19	01	size of register codemap cl
//	1A	cl	register codemap

struct DRO_HEADER
{
	uint16_t verMajor;
	uint16_t verMinor;
	uint32_t dataSize;	// in bytes
	uint32_t lengthMS;
	uint8_t hwType;
	uint8_t format;
	uint8_t compression;
	uint8_t cmdDlyShort;
	uint8_t cmdDlyLong;
	uint8_t regCmdCnt;
	uint8_t regCmdMap[0x80];
	uint32_t dataOfs;
};

// DRO v2 often incorrectly specify DualOPL2 instead of OPL3
// These constants allow a configuration of how to handle DualOPL2 in DRO v2 files.
#define DRO_V2OPL3_DETECT	0x00	// scan the initialization block and use OPL3 if "OPL3 enable" if found [default]
#define DRO_V2OPL3_HEADER	0x01	// strictly follow the DRO header
#define DRO_V2OPL3_ENFORCE	0x02	// always enforce OPL3 mode when the DRO says DualOPL2
struct DRO_PLAY_OPTIONS
{
	PLR_GEN_OPTS genOpts;
	uint8_t v2opl3Mode;	// DRO v2 DualOPL2 -> OPL3 fixes
};


class DROPlayer : public PlayerBase
{
private:
	struct DEVLOG_CB_DATA
	{
		DROPlayer* player;
		size_t chipDevID;
	};
	struct DRO_CHIPDEV
	{
		VGM_BASEDEV base;
		size_t optID;
		DEVFUNC_WRITE_A8D8 write;
		DEVLOG_CB_DATA logCbData;
	};
	
public:
	DROPlayer();
	~DROPlayer();
	
	uint32_t GetPlayerType(void) const;
	const char* GetPlayerName(void) const;
	static uint8_t PlayerCanLoadFile(DATA_LOADER *dataLoader);
	uint8_t CanLoadFile(DATA_LOADER *dataLoader) const;
	uint8_t LoadFile(DATA_LOADER *dataLoader);
	uint8_t UnloadFile(void);
	const DRO_HEADER* GetFileHeader(void) const;
	
	const char* const* GetTags(void);
	uint8_t GetSongInfo(PLR_SONG_INFO& songInf);
	uint8_t GetSongDeviceInfo(std::vector<PLR_DEV_INFO>& devInfList) const;
	uint8_t SetDeviceOptions(uint32_t id, const PLR_DEV_OPTS& devOpts);
	uint8_t GetDeviceOptions(uint32_t id, PLR_DEV_OPTS& devOpts) const;
	uint8_t SetDeviceMuting(uint32_t id, const PLR_MUTE_OPTS& muteOpts);
	uint8_t GetDeviceMuting(uint32_t id, PLR_MUTE_OPTS& muteOpts) const;
	uint8_t SetPlayerOptions(const DRO_PLAY_OPTIONS& playOpts);
	uint8_t GetPlayerOptions(DRO_PLAY_OPTIONS& playOpts) const;
	
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
	void RefreshMuting(DRO_CHIPDEV& chipDev, const PLR_MUTE_OPTS& muteOpts);
	void RefreshPanning(DRO_CHIPDEV& chipDev, const PLR_PAN_OPTS& panOpts);
	
	void ScanInitBlock(void);

	void RefreshTSRates(void);
	
	static void PlayerLogCB(void* userParam, void* source, uint8_t level, const char* message);
	static void SndEmuLogCB(void* userParam, void* source, uint8_t level, const char* message);
	
	void GenerateDeviceConfig(void);
	uint8_t SeekToTick(uint32_t tick);
	uint8_t SeekToFilePos(uint32_t pos);
	void ParseFile(uint32_t ticks);
	void DoCommand_v1(void);
	void DoCommand_v2(void);
	void DoFileEnd(void);
	void WriteReg(uint8_t port, uint8_t reg, uint8_t data);
	
	DEV_LOGGER _logger;
	DATA_LOADER* _dLoad;
	const uint8_t* _fileData;	// data pointer for quick access, equals _dLoad->GetFileData().data()
	
	DRO_HEADER _fileHdr;
	std::vector<DEV_ID> _devTypes;
	std::vector<uint8_t> _devPanning;
	std::vector<DEV_GEN_CFG> _devCfgs;
	uint8_t _realHwType;
	uint8_t _portShift;	// 0 for OPL2 (1 port per chip), 1 for OPL3 (2 ports per chip)
	uint8_t _portMask;	// (1 << _portShift) - 1
	uint32_t _tickFreq;
	uint32_t _totalTicks;
	
	// information about the initialization block (ends with first delay)
	uint32_t _initBlkEndOfs;	// offset of end of init. block (for special fixes)
	std::vector<bool> _initRegSet;	// registers set during init. block
	uint8_t _initOPL3Enable;	// value of OPL3 enable register set during init. block
	
	//uint32_t _outSmplRate;
	
	// tick/sample conversion rates
	uint64_t _tsMult;
	uint64_t _tsDiv;
	uint64_t _ttMult;
	uint64_t _lastTsMult;
	uint64_t _lastTsDiv;
	
	DRO_PLAY_OPTIONS _playOpts;
	PLR_DEV_OPTS _devOpts[3];	// 0 = 1st OPL2, 1 = 2nd OPL2, 2 = 1st OPL3
	std::vector<DRO_CHIPDEV> _devices;
	std::vector<std::string> _devNames;
	size_t _optDevMap[3];	// maps _devOpts vector index to _devices vector
	
	uint32_t _filePos;
	uint32_t _fileTick;
	uint32_t _playTick;
	uint32_t _playSmpl;
	
	uint8_t _playState;
	uint8_t _psTrigger;	// used to temporarily trigger special commands
	uint8_t _selPort;		// currently selected OPL chip (for DRO v1)
	//PLAYER_EVENT_CB _eventCbFunc;
	//void* _eventCbParam;
};

#endif	// __DROPLAYER_HPP__
