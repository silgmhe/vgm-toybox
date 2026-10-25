#ifndef __PLAYERBASE_HPP__
#define __PLAYERBASE_HPP__

#include "../stdtype.h"
#include "../emu/EmuStructs.h"	// for DEV_DECL, DEV_GEN_CFG
#include "../emu/Resampler.h"	// for WAVE_32BS
#include "../utils/DataLoader.h"
#include <vector>


// GetState() bit masks
#define PLAYSTATE_PLAY	0x01	// is playing
#define PLAYSTATE_END	0x02	// has reached the end of the file
#define PLAYSTATE_PAUSE	0x04	// is paused (render wave, but don't advance in the song)
#define PLAYSTATE_SEEK	0x08	// is seeking

// GetCurPos()/Seek() units
#define PLAYPOS_FILEOFS	0x00	// file offset (in bytes)
#define PLAYPOS_TICK	0x01	// tick position (scale: internal tick rate)
#define PLAYPOS_SAMPLE	0x02	// sample number (scale: rendering sample rate)
#define PLAYPOS_COMMAND	0x03	// internal command ID

// callback functions and event constants
class PlayerBase;
typedef uint8_t (*PLAYER_EVENT_CB)(PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam);
#define PLREVT_NONE		0x00
#define PLREVT_START	0x01	// playback started
#define PLREVT_STOP		0x02	// playback stopped
#define PLREVT_LOOP		0x03	// starting next loop [evtParam: uint32_t* loopNumber, ret == 0x01 -> stop processing]
#define PLREVT_END		0x04	// reached the end of the song

typedef DATA_LOADER* (*PLAYER_FILEREQ_CB)(void* userParam, PlayerBase* player, const char* fileName);

typedef void (*PLAYER_LOG_CB)(void* userParam, PlayerBase* player, uint8_t level, uint8_t srcType, const char* srcTag, const char* message);
// log levels
#define PLRLOG_OFF		DEVLOG_OFF
#define PLRLOG_ERROR	DEVLOG_ERROR
#define PLRLOG_WARN		DEVLOG_WARN
#define PLRLOG_INFO		DEVLOG_INFO
#define PLRLOG_DEBUG	DEVLOG_DEBUG
#define PLRLOG_TRACE	DEVLOG_TRACE
// log source types
#define PLRLOGSRC_PLR	0x00	// player
#define PLRLOGSRC_EMU	0x01	// sound emulation


struct PLR_SONG_INFO
{
	uint32_t format;		// four-character-code for file format
	uint16_t fileVerMaj;	// file version (major, encoded as BCD)
	uint16_t fileVerMin;	// file version (minor, encoded as BCD)
	uint32_t tickRateMul;	// internal ticks per second: numerator
	uint32_t tickRateDiv;	// internal ticks per second: denumerator
	// 1 second = 1 tick * tickMult / tickDiv
	uint32_t songLen;		// song length in ticks
	uint32_t loopTick;	// tick position where the loop begins (-1 = no loop)
	int32_t volGain;		// song-specific volume gain, 16.16 fixed point factor (0x10000 = 100%)
	uint32_t deviceCnt;	// number of used sound devices (Note: excludes linked devices)
};

struct PLR_DEV_INFO
{
	uint32_t id;			// device ID
	uint32_t parentIdx;	// index of parent device, when this is a linked device, (uint32_t)-1 when there is no parent
						// Note: Linked devices are mainly an implementation detail.
						//       They can are used to determine properties like channel counts, but should NOT be shown to the user.
	DEV_ID type;		// device type
	uint16_t instance;	// instance ID of this device type (0xFFFF -> N/A for this format)
						// linked device: index of linked device in relation to parent
	uint16_t volume;		// output volume (0x100 = 100%)
	uint32_t core;		// FCC of device emulation core
	uint32_t smplRate;	// current sample rate (0 if not running)
	const DEV_DECL* devDecl;	// device declaration
	const DEV_GEN_CFG* devCfg;	// device configuration parameters
};

struct PLR_MUTE_OPTS
{
	uint8_t disable;		// suspend emulation (bitmask: 0x01 = main device, 0x02 = linked, 0xFF = all)
	uint32_t chnMute[2];	// channel muting mask ([1] is used for linked devices)
};
struct PLR_PAN_OPTS
{
	int16_t chnPan[2][32];	// channel panning [TODO: rethink how this should be really configured]
};

#define PLR_DEV_ID(chip, instance)	(0x80000000U | (instance << 16) | (chip << 0))

struct PLR_DEV_OPTS
{
	uint32_t emuCore[2];	// enforce a certain sound core (0 = use default, [1] is used for linked devices)
	uint8_t srMode;		// sample rate mode (see DEVRI_SRMODE)
	uint8_t resmplMode;	// resampling mode (0 - high quality, 1 - low quality, 2 - LQ down, HQ up)
	uint32_t smplRate;	// emulaiton sample rate
	uint32_t coreOpts;
	PLR_MUTE_OPTS muteOpts;
	PLR_PAN_OPTS panOpts;
};

struct PLR_GEN_OPTS
{
	uint32_t pbSpeed; // playback speed (16.16 fixed point scale, 0x10000 = 100%)
};


//	--- concept ---
//	- Player class does file rendering at fixed volume (but changeable speed)
//	- host program handles master volume + fading + stopping after X loops (notified via callback)

// TODO: rename to "PlayerEngine"
class PlayerBase
{
public:
	PlayerBase();
	virtual ~PlayerBase();
	
	virtual uint32_t GetPlayerType(void) const;
	virtual const char* GetPlayerName(void) const;
	static uint8_t PlayerCanLoadFile(DATA_LOADER *dataLoader);
	virtual uint8_t CanLoadFile(DATA_LOADER *dataLoader) const;
	virtual uint8_t LoadFile(DATA_LOADER *dataLoader) = 0;
	virtual uint8_t UnloadFile(void) = 0;
	
	virtual const char* const* GetTags(void) = 0;
	virtual uint8_t GetSongInfo(PLR_SONG_INFO& songInf) = 0;
	virtual uint8_t GetSongDeviceInfo(std::vector<PLR_DEV_INFO>& devInfList) const = 0;
	static uint8_t InitDeviceOptions(PLR_DEV_OPTS& devOpts);
	virtual uint8_t SetDeviceOptions(uint32_t id, const PLR_DEV_OPTS& devOpts) = 0;
	virtual uint8_t GetDeviceOptions(uint32_t id, PLR_DEV_OPTS& devOpts) const = 0;
	virtual uint8_t SetDeviceMuting(uint32_t id, const PLR_MUTE_OPTS& muteOpts) = 0;
	virtual uint8_t GetDeviceMuting(uint32_t id, PLR_MUTE_OPTS& muteOpts) const = 0;
	// player-specific options
	//virtual uint8_t SetPlayerOptions(const PLR_GEN_OPTS& playOpts) = 0;
	//virtual uint8_t GetPlayerOptions(PLR_GEN_OPTS& playOpts) const = 0;
	
	virtual uint32_t GetSampleRate(void) const;
	virtual uint8_t SetSampleRate(uint32_t sampleRate);
	virtual double GetPlaybackSpeed(void) const;
	virtual uint8_t SetPlaybackSpeed(double speed);
	virtual void SetUserDevices(const DEV_DECL** userDevList, uint8_t devStartOpts);
	virtual void SetEventCallback(PLAYER_EVENT_CB cbFunc, void* cbParam);
	virtual void SetFileReqCallback(PLAYER_FILEREQ_CB cbFunc, void* cbParam);
	virtual void SetLogCallback(PLAYER_LOG_CB cbFunc, void* cbParam);
	virtual uint32_t Tick2Sample(uint32_t ticks) const = 0;
	virtual uint32_t Sample2Tick(uint32_t samples) const = 0;
	virtual double Tick2Second(uint32_t ticks) const = 0;
	virtual double Sample2Second(uint32_t samples) const;
	
	virtual uint8_t GetState(void) const = 0;			// get playback state (playing / paused / ...)
	virtual uint32_t GetCurPos(uint8_t unit) const = 0;	// get current playback position
	virtual uint32_t GetCurLoop(void) const = 0;		// get current loop index (0 = 1st loop, 1 = 2nd loop, ...)
	virtual uint32_t GetTotalTicks(void) const = 0;	// get time for playing once in ticks
	virtual uint32_t GetLoopTicks(void) const = 0;	// get time for one loop in ticks
	virtual uint32_t GetTotalPlayTicks(uint32_t numLoops) const;	// get time for playing + looping (without fading)
	
	virtual uint8_t Start(void) = 0;
	virtual uint8_t Stop(void) = 0;
	virtual uint8_t Reset(void) = 0;
	virtual uint8_t Seek(uint8_t unit, uint32_t pos) = 0; // seek to playback position
	virtual uint32_t Render(uint32_t smplCnt, WAVE_32BS* data) = 0;
	
protected:
	uint32_t _outSmplRate;
	const DEV_DECL** _userDevList;
	uint8_t _devStartOpts;
	PLAYER_EVENT_CB _eventCbFunc;
	void* _eventCbParam;
	PLAYER_FILEREQ_CB _fileReqCbFunc;
	void* _fileReqCbParam;
	PLAYER_LOG_CB _logCbFunc;
	void* _logCbParam;
};

#endif	// __PLAYERBASE_HPP__
