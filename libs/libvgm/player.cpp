// DRO/S98/VGM player test program
// -------------------------------
// Warning: This just serves as a tool for testing features.
// Thus, the user interface can be a complicated mess and a pain to use.

#ifdef _WIN32
//#define _WIN32_WINNT	0x500	// for GetConsoleWindow()
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <string>

#ifdef _WIN32
extern "C" int __cdecl _getch(void);	// from conio.h
extern "C" int __cdecl _kbhit(void);
#else
#include <unistd.h>		// for STDIN_FILENO and usleep()
#include <termios.h>
#include <sys/time.h>	// for struct timeval in _kbhit()
#define	Sleep(msec)	usleep(msec * 1000)
#endif

#include "common_def.h"
#include "utils/DataLoader.h"
#include "player/playerbase.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/vgmplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/playera.hpp"
#include "audio/AudioStream.h"
#include "audio/AudioStream_SpcDrvFuns.h"
#include "emu/SoundDevs.h"	// for DEVID_*
#include "emu/EmuCores.h"
#include "utils/OSMutex.h"

//#define USE_MEMORY_LOADER 1	// define to use the in-memory loader

#if USE_MEMORY_LOADER
#include "utils/MemoryLoader.h"
#else
#include "utils/FileLoader.h"
#endif

int main(int argc, char* argv[]);
static void DoChipControlMode(PlayerBase* player);
static void StripNewline(char* str);
static std::string FCC2Str(uint32_t fcc);
#if USE_MEMORY_LOADER
static uint8_t *SlurpFile(const char *fileName, uint32_t *fileSize);
#endif
static const char* GetFileTitle(const char* filePath);
static uint32_t FillBuffer(void* drvStruct, void* userParam, uint32_t bufSize, void* Data);
static uint8_t FilePlayCallback(PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam);
static DATA_LOADER* RequestFileCallback(void* userParam, PlayerBase* player, const char* fileName);
static const char* LogLevel2Str(uint8_t level);
static void PlayerLogCallback(void* userParam, PlayerBase* player, uint8_t level, uint8_t srcType,
	const char* srcTag, const char* message);
static uint32_t GetNthAudioDriver(uint8_t adrvType, int32_t drvNumber);
static uint8_t InitAudioSystem(void);
static uint8_t DeinitAudioSystem(void);
static uint8_t StartAudioDevice(void);
static uint8_t StopAudioDevice(void);
static uint8_t StartDiskWriter(const char* fileName);
static uint8_t StopDiskWriter(void);
#ifndef _WIN32
static void changemode(uint8_t noEcho);
static int _kbhit(void);
#define	_getch	getchar
#endif


static void* audDrv;
static void* audDrvLog;
static std::vector<uint8_t> locAudBuf;	// local audio buffer (for WAV dumping)
static OS_MUTEX* renderMtx;	// render thread mutex

static uint32_t sampleRate = 44100;
static uint32_t maxLoops = 2;
static bool manualRenderLoop = false;
static volatile uint8_t playState;

static uint32_t idWavOut;
static uint32_t idWavOutDev;
static uint32_t idWavWrt;

static int32_t AudioOutDrv = -2;
static int32_t WaveWrtDrv = -1;

static uint32_t masterVol = 0x10000;	// fixed point 16.16

static uint8_t showTags = 1;
static bool showFileInfo = false;
static uint8_t logLevel = DEVLOG_INFO;
static uint8_t pbTimeMode = PLAYTIME_LOOP_INCL | PLAYTIME_TIME_FILE;

static PlayerA mainPlr;

int main(int argc, char* argv[])
{
	int argbase;
	uint8_t retVal;
	DATA_LOADER *dLoad;
	int curSong;
	bool needRefresh;
	
	if (argc < 2)
	{
		printf("Usage: %s inputfile\n", argv[0]);
		return 0;
	}
	argbase = 1;
#ifdef _WIN32
	SetConsoleOutputCP(65001);	// set UTF-8 codepage
#endif
	
	retVal = InitAudioSystem();
	if (retVal)
		return 1;
	retVal = StartAudioDevice();
	if (retVal)
	{
		DeinitAudioSystem();
		return 1;
	}
	playState = 0x00;
	
	// I'll keep the instances of the players for the program's life time.
	// This way player/chip options are kept between track changes.
	mainPlr.RegisterPlayerEngine(new VGMPlayer);
	mainPlr.RegisterPlayerEngine(new S98Player);
	mainPlr.RegisterPlayerEngine(new DROPlayer);
	mainPlr.RegisterPlayerEngine(new GYMPlayer);
	mainPlr.SetEventCallback(FilePlayCallback, NULL);
	mainPlr.SetFileReqCallback(RequestFileCallback, NULL);
	mainPlr.SetLogCallback(PlayerLogCallback, NULL);
	//mainPlr.SetOutputSettings() is done in StartAudioDevice()
	{
		PlayerA::Config pCfg = mainPlr.GetConfiguration();
		pCfg.masterVol = masterVol;
		pCfg.loopCount = maxLoops;
		pCfg.fadeSmpls = sampleRate * 4;	// fade over 4 seconds
		pCfg.endSilenceSmpls = sampleRate / 2;	// 0.5 seconds of silence at the end
		pCfg.pbSpeed = 1.0;
		mainPlr.SetConfiguration(pCfg);
	}

	for (curSong = argbase; curSong < argc; curSong ++)
	{
	
	printf("Loading %s ...  ", GetFileTitle(argv[curSong]));
	fflush(stdout);

#if USE_MEMORY_LOADER
	uint32_t fileSize;
	uint8_t *fileData = SlurpFile(argv[curSong],&fileSize);
	dLoad = MemoryLoader_Init(fileData, fileSize);
#else
	dLoad = FileLoader_Init(argv[curSong]);
#endif

	if(dLoad == NULL) continue;
	DataLoader_SetPreloadBytes(dLoad,0x100);
	retVal = DataLoader_Load(dLoad);
	if (retVal)
	{
		DataLoader_Deinit(dLoad);
		fprintf(stderr, "Error 0x%02X loading file!\n", retVal);
		continue;
	}
	retVal = mainPlr.LoadFile(dLoad);
	if (retVal)
	{
		DataLoader_Deinit(dLoad);
		fprintf(stderr, "Error 0x%02X loading file!\n", retVal);
		continue;
	}
	
	PlayerBase* player = mainPlr.GetPlayer();
	mainPlr.SetLoopCount(maxLoops);
	if (player->GetPlayerType() == FCC_S98)
	{
		S98Player* s98play = dynamic_cast<S98Player*>(player);
		const S98_HEADER* s98hdr = s98play->GetFileHeader();
		
		printf("S98 v%u, Total Length: %.2f s, Loop Length: %.2f s, Tick Rate: %u/%u", s98hdr->fileVer,
				player->Tick2Second(player->GetTotalTicks()), player->Tick2Second(player->GetLoopTicks()),
				s98hdr->tickMult, s98hdr->tickDiv);
	}
	else if (player->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
		const VGM_HEADER* vgmhdr = vgmplay->GetFileHeader();
		
		printf("VGM v%3X, Total Length: %.2f s, Loop Length: %.2f s", vgmhdr->fileVer,
				player->Tick2Second(player->GetTotalTicks()), player->Tick2Second(player->GetLoopTicks()));
		mainPlr.SetLoopCount(vgmplay->GetModifiedLoopCount(maxLoops));
	}
	else if (player->GetPlayerType() == FCC_DRO)
	{
		DROPlayer* droplay = dynamic_cast<DROPlayer*>(player);
		const DRO_HEADER* drohdr = droplay->GetFileHeader();
		const char* hwType;
		
		if (drohdr->hwType == 0)
			hwType = "OPL2";
		else if (drohdr->hwType == 1)
			hwType = "DualOPL2";
		else if (drohdr->hwType == 2)
			hwType = "OPL3";
		else
			hwType = "unknown";
		printf("DRO v%u, Total Length: %.2f s, HW Type: %s", drohdr->verMajor,
				player->Tick2Second(player->GetTotalTicks()), hwType);
	}
	
	if (showTags > 0)
	{
		const char* songTitle = NULL;
		const char* songAuthor = NULL;
		const char* songGame = NULL;
		const char* songSystem = NULL;
		const char* songDate = NULL;
		const char* songComment = NULL;
	
		const char* const* tagList = player->GetTags();
		for (const char* const* t = tagList; *t; t += 2)
		{
			if (!strcmp(t[0], "TITLE"))
				songTitle = t[1];
			else if (!strcmp(t[0], "ARTIST"))
				songAuthor = t[1];
			else if (!strcmp(t[0], "GAME"))
				songGame = t[1];
			else if (!strcmp(t[0], "SYSTEM"))
				songSystem = t[1];
			else if (!strcmp(t[0], "DATE"))
				songDate = t[1];
			else if (!strcmp(t[0], "COMMENT"))
				songComment = t[1];
		}
		
		if (songTitle != NULL && songTitle[0] != '\0')
			printf("\nSong Title: %s", songTitle);
		if (showTags >= 2)
		{
			if (songAuthor != NULL && songAuthor[0] != '\0')
				printf("\nSong Author: %s", songAuthor);
			if (songGame != NULL && songGame[0] != '\0')
				printf("\nSong Game: %s", songGame);
			if (songSystem != NULL && songSystem[0] != '\0')
				printf("\nSong System: %s", songSystem);
			if (songDate != NULL && songDate[0] != '\0')
				printf("\nSong Date: %s", songDate);
			if (songComment != NULL && songComment[0] != '\0')
				printf("\nSong Comment: %s", songComment);
		}
	}
	
	putchar('\n');
	
	{
		PLR_DEV_OPTS devOpts;
		uint32_t devOptID;
		
		devOptID = PLR_DEV_ID(DEVID_SN76496, 0);
		retVal = player->GetDeviceOptions(devOptID, devOpts);
		if (! (retVal & 0x80))
		{
			static const int16_t panPos[4] = {0x00, -0x80, +0x80, 0x00};
			if (! devOpts.emuCore[0])
				devOpts.emuCore[0] = FCC_MAXM;
			memcpy(devOpts.panOpts.chnPan, panPos, sizeof(panPos));
			player->SetDeviceOptions(devOptID, devOpts);
		}
		
		devOptID = PLR_DEV_ID(DEVID_YM2413, 0);
		retVal = player->GetDeviceOptions(devOptID, devOpts);
		if (! (retVal & 0x80))
		{
			static const int16_t panPos[14] = {
				-0x100, +0x100, -0x80, +0x80, -0x40, +0x40, -0xC0, +0xC0, 0x00,
				-0x60, +0x60, 0x00, -0xC0, +0xC0};
			memcpy(devOpts.panOpts.chnPan, panPos, sizeof(panPos));
			player->SetDeviceOptions(devOptID, devOpts);
		}
		
		devOptID = PLR_DEV_ID(DEVID_AY8910, 0);
		retVal = player->GetDeviceOptions(devOptID, devOpts);
		if (! (retVal & 0x80))
		{
			static const int16_t panPos[3] = {-0x80, +0x80, 0x00};
			memcpy(devOpts.panOpts.chnPan, panPos, sizeof(panPos));
			player->SetDeviceOptions(devOptID, devOpts);
		}

		devOptID = PLR_DEV_ID(DEVID_C6280, 0);
		retVal = player->GetDeviceOptions(devOptID, devOpts);
		if (! (retVal & 0x80))
		{
			if (! devOpts.emuCore[0])
				devOpts.emuCore[0] = FCC_MAME;
			player->SetDeviceOptions(devOptID, devOpts);
		}
	}
	
	mainPlr.Start();
	
	if (showFileInfo)
	{
		// only after calling PlayerA::Start() we can obtain info about the currently used sound cores
		PLR_SONG_INFO sInf;
		std::vector<PLR_DEV_INFO> diList;
		size_t curDev;
		
		player->GetSongInfo(sInf);
		player->GetSongDeviceInfo(diList);
		printf("SongInfo: %s v%X.%02X, Rate %u/%u, Len %u, Loop at %d, devices: %u\n",
			FCC2Str(sInf.format).c_str(), sInf.fileVerMaj, sInf.fileVerMin,
			sInf.tickRateMul, sInf.tickRateDiv, sInf.songLen, sInf.loopTick, sInf.deviceCnt);
		for (curDev = 0; curDev < diList.size(); curDev ++)
		{
			const PLR_DEV_INFO& pdi = diList[curDev];
			const char* devName = "";
			uint16_t chns = 0;
			if (pdi.devDecl != NULL)
			{
				devName = pdi.devDecl->name(pdi.devCfg);
				chns = pdi.devDecl->channelCount(pdi.devCfg);
			}
			if (pdi.parentIdx == (uint32_t)-1)
			{
				// main device
				printf(" Dev %d: Type 0x%02X %7s #%d, Core %-4s, Clk %u, Rate %u, Vol 0x%X, Chns %u\n",
					(int)pdi.id, pdi.type, devName, (int8_t)pdi.instance, FCC2Str(pdi.core).c_str(),
					pdi.devCfg->clock, pdi.smplRate, pdi.volume, chns);
			}
			else
			{
				// linked device
				uint32_t parentId = diList[pdi.parentIdx].id;
				printf("  Dev %d-%d: Type 0x%02X %7s, Core %-4s, Clk %u, Rate %u, Vol 0x%X, Chns %u\n",
					(int)parentId, (int)pdi.instance, pdi.type, devName, FCC2Str(pdi.core).c_str(),
					pdi.devCfg->clock, pdi.smplRate, pdi.volume, chns);
			}
		}
	}
	const std::vector<VGMPlayer::DACSTRM_DEV>* vgmPcmStrms = NULL;
	if (player->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
		vgmPcmStrms = &vgmplay->GetStreamDevInfo();
	}
	
	StartDiskWriter("waveOut.wav");
	
	if (audDrv != NULL)
		retVal = AudioDrv_SetCallback(audDrv, FillBuffer, &mainPlr);
	else
		retVal = 0xFF;
	manualRenderLoop = (retVal != 0x00);
#ifndef _WIN32
	changemode(1);
#endif
	playState &= ~PLAYSTATE_END;
	needRefresh = true;
	while(! (playState & PLAYSTATE_END))
	{
		if (! (playState & PLAYSTATE_PAUSE))
			needRefresh = true;	// always update when playing
		if (needRefresh)
		{
			const char* pState;
			
			if (playState & PLAYSTATE_PAUSE)
				pState = "Paused";
			else if (mainPlr.GetState() & PLAYSTATE_END)
				pState = "Finish ";
			else if (mainPlr.GetState() & PLAYSTATE_FADE)
				pState = "Fading ";
			else
				pState = "Playing";
			if (vgmPcmStrms == NULL || vgmPcmStrms->empty())
			{
				printf("%s %.2f / %.2f ...   \r", pState, mainPlr.GetCurTime(pbTimeMode), mainPlr.GetTotalTime(pbTimeMode));
			}
			else
			{
				const VGMPlayer::DACSTRM_DEV* strmDev = &(*vgmPcmStrms)[0];
				std::string pbMode = " ";
				if (strmDev->pbMode & 0x10)
					pbMode += 'R';	// reverse playback
				if (strmDev->pbMode & 0x80)
					pbMode += 'L';	// looping
				if (pbMode.length() == 1)
					pbMode = "";
				printf("%s %.2f / %.2f [%02X / %02X at %4.1f KHz%s] ...     \r",
					pState, mainPlr.GetCurTime(pbTimeMode), mainPlr.GetTotalTime(pbTimeMode),
					1 + strmDev->lastItem, strmDev->maxItems, strmDev->freq / 1000.0,
					pbMode.c_str());
			}
			fflush(stdout);
			needRefresh = false;
		}
		
		if (manualRenderLoop && ! (playState & PLAYSTATE_PAUSE))
		{
			uint32_t wrtBytes = FillBuffer(audDrvLog, &mainPlr, (uint32_t)locAudBuf.size(), &locAudBuf[0]);
			AudioDrv_WriteData(audDrvLog, wrtBytes, &locAudBuf[0]);
		}
		else
		{
			Sleep(50);
		}
		
		if (_kbhit())
		{
			int inkey = _getch();
			int letter = toupper(inkey);
			
			if (letter == ' ' || letter == 'P')
			{
				playState ^= PLAYSTATE_PAUSE;
				if (audDrv != NULL)
				{
					if (playState & PLAYSTATE_PAUSE)
						AudioDrv_Pause(audDrv);
					else
						AudioDrv_Resume(audDrv);
				}
			}
			else if (letter == 'R')	// restart
			{
				OSMutex_Lock(renderMtx);
				mainPlr.Reset();
				OSMutex_Unlock(renderMtx);
			}
			else if (letter >= '0' && letter <= '9')
			{
				uint32_t maxPos;
				uint8_t pbPos10;
				uint32_t destPos;
				
				OSMutex_Lock(renderMtx);
				maxPos = mainPlr.GetPlayer()->GetTotalPlayTicks(maxLoops);
				pbPos10 = letter - '0';
				destPos = maxPos * pbPos10 / 10;
				mainPlr.Seek(PLAYPOS_TICK, destPos);
				OSMutex_Unlock(renderMtx);
			}
			else if (letter == 'B')	// previous file
			{
				if (curSong > argbase)
				{
					playState |= PLAYSTATE_END;
					curSong -= 2;
				}
			}
			else if (letter == 'N')	// next file
			{
				if (curSong + 1 < argc)
					playState |= PLAYSTATE_END;
			}
			else if (inkey == 0x1B || letter == 'Q')	// quit
			{
				playState |= PLAYSTATE_END;
				curSong = argc - 1;
			}
			else if (letter == 'F')	// fade out
			{
				OSMutex_Lock(renderMtx);
				mainPlr.FadeOut();
				OSMutex_Unlock(renderMtx);
			}
			else if (letter == 'C')	// chip control
			{
#ifndef _WIN32
				changemode(0);	// make sure entered charactered are echoed
#endif
				DoChipControlMode(mainPlr.GetPlayer());
#ifndef _WIN32
				changemode(1);
#endif
			}
			needRefresh = true;
		}
	}
#ifndef _WIN32
	changemode(0);
#endif
	// remove callback to prevent further rendering
	// also waits for render thread to finish its work
	if (audDrv != NULL)
		AudioDrv_SetCallback(audDrv, NULL, NULL);
	
	StopDiskWriter();
	
	mainPlr.Stop();
	mainPlr.UnloadFile();
	DataLoader_Deinit(dLoad);	dLoad = NULL;
#if USE_MEMORY_LOADER
	free(fileData);
#endif
	
	}	// end for(curSong)
	
	mainPlr.UnregisterAllPlayers();
	
	StopAudioDevice();
	DeinitAudioSystem();
	printf("Done.\n");
	
#if defined(_DEBUG) && (_MSC_VER >= 1400)
	// doesn't work well with C++ containers
	//if (_CrtDumpMemoryLeaks())
	//	_getch();
#endif
	
	return 0;
}

/*
Command Mode overview

Sound Chip ID:
	D - display configuration
		T param - show tags (0/D/OFF - off, 1/E/ON - on)
		FI param - show file information (see above)
		LL param - set log level (0..5 = off/error/warn/info/debug/trace, see emu/EmuStructs.h)
		TD param - set time display mode (bit mask: 0/1 = exclude/include loops, 0/2 = file/playback time, 4 = with fade time)
		Q - quit
	P - player configuration
		SPD param - set playback speed (1.0 = 100%)
		[DRO]
			OPL3 param - DualOPL2 -> OPL3 patch, (0/1/2, see DRO_V2OPL3_*)
		[VGM]
			PHZ param - set playback rate in Hz (set to 50 to play NTSC VGMs in PAL speed)
			HSO param - hard-stop old VGMs (<1.50) when they finish
	[number] - sound chip # configuration (active sound chip)
			Note: The number is the ID of the active sound chip (as shown by device info).
			      All sound chips can be controlled with 0x800I00NN (NN = libvgm device ID, I = instance, i.e. 0 or 1)
		C param - set emulation core to param (four-character code, case sensitive, empty = use default)
		LC param - set emulation core of *linked device* (OPN SSG/OPL4 FM) to param (four-character code, case sensitive, empty = use default)
		O param - set sound core options (core-specific)
		SRM param - set sample rate mode (0/1/2, see DEVRI_SRMODE_*)
		SR param - set emulated sample rate (0 = use rate of output stream)
		RSM param - set resampling mode
		M param,param,... - set mute options
			This is a list of channels to be toggled. (0 = first channel)
			Additional valid letters:
				E - enable sound chip
				D - disable sound chip
				O - all channels on
				X - mute all channels
		P param,param,... - set channel panning
			This is a list of stereo positions, one for each channel.
			-1.0 (left) .. 0.0 (centre) .. +1.0 (right)
		Q - quit
*/
static void DoChipControlMode(PlayerBase* player)
{
	int letter;
	int mode;
	char line[0x80];
	char* endPtr;
	int chipID;
	uint8_t retVal;
	
	printf("Command Mode. ");
	mode = 0;	// start
	chipID = -1;
	while(mode >= 0)
	{
		if (mode == 0)
		{
			PLR_DEV_OPTS devOpts;
			
			// number (sound chip ID) / D (display) / P (player options)
			printf("Sound Chip ID: ");
			endPtr = fgets(line, 0x80, stdin);
			if (endPtr == NULL)
				return;
			StripNewline(line);
			if (line[0] == '\0')
				return;
			
			// Note: In MSVC 2010, strtol returns 0x7FFFFFFF for the string "0x8000000C".
			// strtoul returns the correct value and also properly returns -1 for "-1".
			chipID = (int)strtoul(line, &endPtr, 0);
			if (endPtr > line)
			{
				retVal = player->GetDeviceOptions((uint32_t)chipID, devOpts);
				if (retVal & 0x80)
				{
					printf("Invalid sound chip ID.\n");
					continue;
				}
				
				printf("Cfg: Core %s, Opts 0x%X, srMode 0x%02X, sRate %u, resampleMode 0x%02X\n",
					FCC2Str(devOpts.emuCore[0]).c_str(), devOpts.coreOpts, devOpts.srMode,
					devOpts.smplRate, devOpts.resmplMode);
				printf("Muting: Chip %s [0x%02X], Channel Mask: 0x%02X\n",
					(devOpts.muteOpts.disable & 0x01) ? "Off" : "On", devOpts.muteOpts.disable,
					devOpts.muteOpts.chnMute[0]);
				mode = 1;
			}
			else
			{
				letter = toupper((unsigned char)*line);
				if (letter == 'D')
				{
					mode = 10;
				}
				else if (letter == 'P')
				{
					switch(player->GetPlayerType())
					{
					case FCC_DRO:
					{
						DROPlayer* droplay = dynamic_cast<DROPlayer*>(player);
						DRO_PLAY_OPTIONS playOpts;
						droplay->GetPlayerOptions(playOpts);
						double spd = playOpts.genOpts.pbSpeed / (double)0x10000;
						printf("Opts: Speed %.3f, OPL3Mode %u\n", spd, playOpts.v2opl3Mode);
						mode = 2;
					}
						break;
					case FCC_VGM:
					{
						VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
						VGM_PLAY_OPTIONS playOpts;
						vgmplay->GetPlayerOptions(playOpts);
						double spd = playOpts.genOpts.pbSpeed / (double)0x10000;
						printf("Opts: Speed %.3f, PlaybkHz %u, HardStopOld %u\n",
							spd, playOpts.playbackHz, playOpts.hardStopOld);
						mode = 2;
					}
						break;
					case FCC_S98:
					{
						S98Player* s98play = dynamic_cast<S98Player*>(player);
						S98_PLAY_OPTIONS playOpts;
						s98play->GetPlayerOptions(playOpts);
						double spd = playOpts.genOpts.pbSpeed / (double)0x10000;
						printf("Opts: Speed %.3f\n", spd);
						mode = 2;
					}
						break;
					case FCC_GYM:
					{
						GYMPlayer* gymplay = dynamic_cast<GYMPlayer*>(player);
						GYM_PLAY_OPTIONS playOpts;
						gymplay->GetPlayerOptions(playOpts);
						double spd = playOpts.genOpts.pbSpeed / (double)0x10000;
						printf("Opts: Speed %.3f\n", spd);
						mode = 2;
					}
						break;
					}
				}
			}
		}
		else if (mode == 1)	// sound chip command mode
		{
			char* tokenStr;
			PLR_DEV_OPTS devOpts;
			
			retVal = player->GetDeviceOptions((uint32_t)chipID, devOpts);
			if (retVal & 0x80)
			{
				mode = 0;
				continue;
			}
			
			// Core / Linked Core / Opts / SRMode / SampleRate / ReSampleMode / Muting
			printf("Command [C/LC/O/SRM/SR/RSM/M data]: ");
			endPtr = fgets(line, 0x80, stdin);
			if (endPtr == NULL)
				return;
			StripNewline(line);
			
			tokenStr = strtok(line, " ");
			for (endPtr = line; *endPtr != '\0'; endPtr ++)
				*endPtr = (char)toupper((unsigned char)*endPtr);
			tokenStr = endPtr + 1;
			
			if (! strcmp(line, "C"))
			{
				std::string fccStr(tokenStr);
				fccStr.resize(4, 0x00);
				devOpts.emuCore[0] =
					(fccStr[0] << 24) | (fccStr[1] << 16) |
					(fccStr[2] <<  8) | (fccStr[3] <<  0);
				player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "LC"))
			{
				std::string fccStr(tokenStr);
				fccStr.resize(4, 0x00);
				devOpts.emuCore[1] =
					(fccStr[0] << 24) | (fccStr[1] << 16) |
					(fccStr[2] <<  8) | (fccStr[3] <<  0);
				player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "O"))
			{
				devOpts.coreOpts = (uint32_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "SRM"))
			{
				devOpts.srMode = (uint8_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "SR"))
			{
				devOpts.smplRate = (uint32_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "RSM"))
			{
				devOpts.resmplMode = (uint8_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					player->SetDeviceOptions((uint32_t)chipID, devOpts);
			}
			else if (! strcmp(line, "M"))
			{
				PLR_MUTE_OPTS& muteOpts = devOpts.muteOpts;
				
				letter = '\0';
				tokenStr = strtok(NULL, ",");
				while(tokenStr != NULL)
				{
					letter = toupper((unsigned char)*tokenStr);
					if (letter == 'E')
						muteOpts.disable = 0x00;
					else if (letter == 'D')
						muteOpts.disable = 0xFF;
					else if (letter == 'O')
						muteOpts.chnMute[0] = 0;
					else if (letter == 'X')
						muteOpts.chnMute[0] = ~0;
					else if (isalnum(letter))
					{
						long chnID = strtol(tokenStr, &endPtr, 0);
						if (endPtr > tokenStr)
							muteOpts.chnMute[0] ^= (1 << chnID);
					}
					
					tokenStr = strtok(NULL, ",");
				}
				
				player->SetDeviceMuting((uint32_t)chipID, muteOpts);
				printf("-> Chip %s [0x%02X], Channel Mask: 0x%02X\n",
					(muteOpts.disable & 0x01) ? "Off" : "On", muteOpts.disable, muteOpts.chnMute[0]);
			}
			else if (! strcmp(line, "P"))
			{
				PLR_PAN_OPTS& panOpts = devOpts.panOpts;
				uint32_t chnID;
				uint32_t curChn;
				
				letter = '\0';
				tokenStr = strtok(NULL, ",");
				chnID = 0;
				while(tokenStr != NULL && chnID < 32)
				{
					double panPos = strtod(tokenStr, &endPtr);
					if (endPtr > tokenStr)
						panOpts.chnPan[0][chnID] = (int16_t)(panPos * 0x100);
					
					tokenStr = strtok(NULL, ",");
					chnID ++;
				}
				
				player->SetDeviceOptions((uint32_t)chipID, devOpts);
				printf("-> Panning: ");
				for (curChn = 0; curChn < chnID; curChn ++)
					printf("%.2f,", panOpts.chnPan[0][chnID] / (float)0x100);
				printf("\b \n");
			}
			else if (! strcmp(line, "Q"))
				mode = -1;
			else
				mode = 0;
		}
		else if (mode == 2)	// player configuration mode
		{
			switch(player->GetPlayerType())
			{
			case FCC_DRO:
			{
				DROPlayer* droplay = dynamic_cast<DROPlayer*>(player);
				DRO_PLAY_OPTIONS playOpts;
				char* tokenStr;
				
				droplay->GetPlayerOptions(playOpts);
				
				printf("Command [SPD/OPL3 data]: ");
				endPtr = fgets(line, 0x80, stdin);
				if (endPtr == NULL)
					return;
				StripNewline(line);
				
				tokenStr = strtok(line, " ");
				for (endPtr = line; *endPtr != '\0'; endPtr ++)
					*endPtr = (char)toupper((unsigned char)*endPtr);
				tokenStr = endPtr + 1;
				
				if (! strcmp(line, "SPD"))
				{
					double spd = strtod(tokenStr, &endPtr);
					if (endPtr > tokenStr)
						droplay->SetPlaybackSpeed(spd);
				}
				else if (! strcmp(line, "OPL3"))
				{
					playOpts.v2opl3Mode = (uint8_t)strtoul(tokenStr, &endPtr, 0);
					if (endPtr > tokenStr)
						droplay->SetPlayerOptions(playOpts);
				}
				else if (! strcmp(line, "Q"))
					mode = -1;
				else
					mode = 0;
			}
				break;
			case FCC_S98:
			{
				S98Player* s98play = dynamic_cast<S98Player*>(player);
				S98_PLAY_OPTIONS playOpts;
				char* tokenStr;
				
				s98play->GetPlayerOptions(playOpts);
				
				printf("Command [SPD data]: ");
				endPtr = fgets(line, 0x80, stdin);
				if (endPtr == NULL)
					return;
				StripNewline(line);
				
				tokenStr = strtok(line, " ");
				for (endPtr = line; *endPtr != '\0'; endPtr ++)
					*endPtr = (char)toupper((unsigned char)*endPtr);
				tokenStr = endPtr + 1;
				
				if (! strcmp(line, "SPD"))
				{
					double spd = strtod(tokenStr, &endPtr);
					if (endPtr > tokenStr)
						s98play->SetPlaybackSpeed(spd);
				}
				else if (! strcmp(line, "Q"))
					mode = -1;
				else
					mode = 0;
			}
				break;
			case FCC_VGM:
			{
				VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
				VGM_PLAY_OPTIONS playOpts;
				char* tokenStr;
				
				vgmplay->GetPlayerOptions(playOpts);
				
				printf("Command [SPD/PHZ/HSO data]: ");
				endPtr = fgets(line, 0x80, stdin);
				if (endPtr == NULL)
					return;
				StripNewline(line);
				
				tokenStr = strtok(line, " ");
				for (endPtr = line; *endPtr != '\0'; endPtr ++)
					*endPtr = (char)toupper((unsigned char)*endPtr);
				tokenStr = endPtr + 1;
				
				if (! strcmp(line, "SPD"))
				{
					double spd = strtod(tokenStr, &endPtr);
					if (endPtr > tokenStr)
						vgmplay->SetPlaybackSpeed(spd);
				}
				else if (! strcmp(line, "PHZ"))
				{
					playOpts.playbackHz = (uint32_t)strtoul(tokenStr, &endPtr, 0);
					if (endPtr > tokenStr)
						vgmplay->SetPlayerOptions(playOpts);
				}
				else if (! strcmp(line, "HSO"))
				{
					playOpts.hardStopOld = (uint8_t)strtoul(tokenStr, &endPtr, 0);
					if (endPtr > tokenStr)
						vgmplay->SetPlayerOptions(playOpts);
				}
				else if (! strcmp(line, "Q"))
					mode = -1;
				else
					mode = 0;
			}
				break;
			case FCC_GYM:
			{
				GYMPlayer* gymplay = dynamic_cast<GYMPlayer*>(player);
				GYM_PLAY_OPTIONS playOpts;
				char* tokenStr;
				
				gymplay->GetPlayerOptions(playOpts);
				
				printf("Command [SPD data]: ");
				endPtr = fgets(line, 0x80, stdin);
				if (endPtr == NULL)
					return;
				StripNewline(line);
				
				tokenStr = strtok(line, " ");
				for (endPtr = line; *endPtr != '\0'; endPtr ++)
					*endPtr = (char)toupper((unsigned char)*endPtr);
				tokenStr = endPtr + 1;
				
				if (! strcmp(line, "SPD"))
				{
					double spd = strtod(tokenStr, &endPtr);
					if (endPtr > tokenStr)
						gymplay->SetPlaybackSpeed(spd);
				}
				else if (! strcmp(line, "Q"))
					mode = -1;
				else
					mode = 0;
			}
				break;
			default:
				printf("No player-specific configuration available.\n");
				mode = 0;
				break;
			}
		}
		else if (mode == 10)	// display configuration mode
		{
			char* tokenStr;
			
			// Tags / FileInfo
			printf("Command [T/FI/LL/TD data]: ");
			endPtr = fgets(line, 0x80, stdin);
			if (endPtr == NULL)
				return;
			StripNewline(line);
			
			tokenStr = strtok(line, " ");
			for (endPtr = line; *endPtr != '\0'; endPtr ++)
				*endPtr = (char)toupper((unsigned char)*endPtr);
			tokenStr = endPtr + 1;
			
			if (! strcmp(line, "T") || ! strcmp(line, "FI"))
			{
				int val = (int)strtol(tokenStr, &endPtr, 0);
				if (endPtr == tokenStr)
				{
					for (endPtr = tokenStr; *endPtr != '\0'; endPtr ++)
						*endPtr = (char)toupper((unsigned char)*endPtr);
					letter = *tokenStr;
					if (letter == 'E' || ! strcmp(tokenStr, "ON"))
						val = 1;
					else if (letter == 'D' || ! strcmp(tokenStr, "OFF"))
						val = 0;
					else
						val = -1;
				}
				if (val >= 0)
				{
					if (! strcmp(line, "T"))
						showTags = val;
					else if (! strcmp(line, "FI"))
						showFileInfo = !!val;
				}
			}
			else if (! strcmp(line, "LL"))
			{
				uint8_t newLevel = (uint8_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					logLevel = newLevel;
			}
			else if (! strcmp(line, "TD"))
			{
				uint8_t newTimeMode = (uint8_t)strtoul(tokenStr, &endPtr, 0);
				if (endPtr > tokenStr)
					pbTimeMode = newTimeMode;
			}
			else if (! strcmp(line, "Q"))
				mode = -1;
			else
				mode = 0;
		}
	}
	
	return;
}

static void StripNewline(char* str)
{
	char* strPtr;
	
	strPtr = str;
	while(*strPtr != '\0')
		strPtr ++;
	
	while(strPtr > str && iscntrl((unsigned char)strPtr[-1]))
		strPtr --;
	*strPtr = '\0';
	
	return;
}

static std::string FCC2Str(uint32_t fcc)
{
	char result[5];
	result[0] = (char)((fcc >> 24) & 0xFF);
	result[1] = (char)((fcc >> 16) & 0xFF);
	result[2] = (char)((fcc >>  8) & 0xFF);
	result[3] = (char)((fcc >>  0) & 0xFF);
	result[4] = '\0';
	return std::string(result);
}

#if USE_MEMORY_LOADER
static uint8_t *SlurpFile(const char *fileName, uint32_t *fileSize)
{
	*fileSize = 0;
	FILE *hFile = fopen(fileName,"rb");
	uint32_t hFileSize;
	uint8_t *fileData;
	if(hFile == NULL) return NULL;
	fseek(hFile,0,SEEK_END);
	hFileSize = ftell(hFile);
	rewind(hFile);
	fileData = (uint8_t *)malloc(hFileSize);
	if(fileData == NULL)
	{
		fclose(hFile);
		return NULL;
	}
	if(fread(fileData,1,hFileSize,hFile) != hFileSize)
	{
		free(fileData);
		fclose(hFile);
		return NULL;
	}
	fclose(hFile);
	*fileSize = hFileSize;
	return fileData;
}
#endif

static const char* GetFileTitle(const char* filePath)
{
	const char* dirSep1;
	const char* dirSep2;
	
	dirSep1 = strrchr(filePath, '/');
	dirSep2 = strrchr(filePath, '\\');
	if (dirSep2 > dirSep1)
		dirSep1 = dirSep2;
	
	return (dirSep1 == NULL) ? filePath : (dirSep1 + 1);
}

static uint32_t FillBuffer(void* drvStruct, void* userParam, uint32_t bufSize, void* data)
{
	PlayerA& myPlr = *(PlayerA*)userParam;
	if (! (myPlr.GetState() & PLAYSTATE_PLAY))
	{
		fprintf(stderr, "Player Warning: calling Render while not playing! playState = 0x%02X\n", myPlr.GetState());
		memset(data, 0x00, bufSize);
		return bufSize;
	}
	
	uint32_t renderedBytes;
	OSMutex_Lock(renderMtx);
	renderedBytes = myPlr.Render(bufSize, data);
	OSMutex_Unlock(renderMtx);
	
	return renderedBytes;
}

static uint8_t FilePlayCallback(PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam)
{
	switch(evtType)
	{
	case PLREVT_START:
		//printf("Playback started.\n");
		break;
	case PLREVT_STOP:
		//printf("Playback stopped.\n");
		break;
	case PLREVT_LOOP:
		{
			uint32_t* curLoop = (uint32_t*)evtParam;
			if (player->GetState() & PLAYSTATE_SEEK)
				break;
			printf("Loop %u.\n", 1 + *curLoop);
		}
		break;
	case PLREVT_END:
		if (playState & PLAYSTATE_END)
			break;
		playState |= PLAYSTATE_END;
		printf("Song End.\n");
		break;
	}
	return 0x00;
}

static DATA_LOADER* RequestFileCallback(void* userParam, PlayerBase* player, const char* fileName)
{
	DATA_LOADER* dLoad = FileLoader_Init(fileName);
	if (dLoad == NULL)
		return NULL;
	uint8_t retVal = DataLoader_Load(dLoad);
	if (! retVal)
		return dLoad;
	DataLoader_Deinit(dLoad);
	return NULL;
}

static const char* LogLevel2Str(uint8_t level)
{
	static const char* LVL_NAMES[6] = {" ??? ", "Error", "Warn ", "Info ", "Debug", "Trace"};
	if (level >= (sizeof(LVL_NAMES) / sizeof(LVL_NAMES[0])))
		level = 0;
	return LVL_NAMES[level];
}

static void PlayerLogCallback(void* userParam, PlayerBase* player, uint8_t level, uint8_t srcType,
	const char* srcTag, const char* message)
{
	if (level > logLevel)
		return;	// don't print messages with higher verbosity than current log level
	if (srcType == PLRLOGSRC_PLR)
		printf("[%s] %s: %s", LogLevel2Str(level), player->GetPlayerName(), message);
	else
		printf("[%s] %s %s: %s", LogLevel2Str(level), player->GetPlayerName(), srcTag, message);
	return;
}

static uint32_t GetNthAudioDriver(uint8_t adrvType, int32_t drvNumber)
{
	// special numbers for drvNumber:
	//	-1 - don't select any
	//	-2 - select last found driver
	if (drvNumber == -1)
		return (uint32_t)-1;
	
	uint32_t drvCount;
	uint32_t curDrv;
	int32_t typedDrv;
	uint32_t lastDrv;
	AUDDRV_INFO* drvInfo;
	
	// go through all audio drivers get the ID of the requested Output/Disk Writer driver
	drvCount = Audio_GetDriverCount();
	lastDrv = (uint32_t)-1;
	for (typedDrv = 0, curDrv = 0; curDrv < drvCount; curDrv ++)
	{
		Audio_GetDriverInfo(curDrv, &drvInfo);
		if (drvInfo->drvType == adrvType)
		{
			lastDrv = curDrv;
			if (typedDrv == drvNumber)
				return curDrv;
			typedDrv ++;
		}
	}
	
	if (drvNumber == -2)
		return lastDrv;
	return (uint32_t)-1;
}

// initialize audio system and search for requested audio drivers
static uint8_t InitAudioSystem(void)
{
	AUDDRV_INFO* drvInfo;
	uint8_t retVal;
	
	retVal = OSMutex_Init(&renderMtx, 0);
	
	printf("Opening Audio Device ...\n");
	retVal = Audio_Init();
	if (retVal == AERR_NODRVS)
		return retVal;
	
	idWavOut = GetNthAudioDriver(ADRVTYPE_OUT, AudioOutDrv);
	idWavOutDev = 0;	// default device
	if (AudioOutDrv != -1 && idWavOut == (uint32_t)-1)
	{
		fprintf(stderr, "Requested Audio Output driver not found!\n");
		Audio_Deinit();
		return AERR_NODRVS;
	}
	idWavWrt = GetNthAudioDriver(ADRVTYPE_DISK, WaveWrtDrv);
	
	audDrv = NULL;
	if (idWavOut != (uint32_t)-1)
	{
		Audio_GetDriverInfo(idWavOut, &drvInfo);
		printf("Using driver %s.\n", drvInfo->drvName);
		retVal = AudioDrv_Init(idWavOut, &audDrv);
		if (retVal)
		{
			fprintf(stderr, "WaveOut: Driver Init Error: %02X\n", retVal);
			Audio_Deinit();
			return retVal;
		}
#ifdef AUDDRV_DSOUND
		if (drvInfo->drvSig == ADRVSIG_DSOUND)
			DSound_SetHWnd(AudioDrv_GetDrvData(audDrv), GetDesktopWindow());
#endif
	}
	
	audDrvLog = NULL;
	if (idWavWrt != (uint32_t)-1)
	{
		retVal = AudioDrv_Init(idWavWrt, &audDrvLog);
		if (retVal)
			audDrvLog = NULL;
	}
	
	return 0x00;
}

static uint8_t DeinitAudioSystem(void)
{
	uint8_t retVal;
	
	retVal = 0x00;
	if (audDrv != NULL)
	{
		retVal = AudioDrv_Deinit(&audDrv);	audDrv = NULL;
	}
	if (audDrvLog != NULL)
	{
		AudioDrv_Deinit(&audDrvLog);	audDrvLog = NULL;
	}
	Audio_Deinit();
	
	OSMutex_Deinit(renderMtx);	renderMtx = NULL;
	
	return retVal;
}

static uint8_t StartAudioDevice(void)
{
	AUDIO_OPTS* opts;
	uint8_t retVal;
	uint32_t smplSize;
	uint32_t smplAlloc;
	uint32_t localAudBufSize;
	
	opts = NULL;
	smplAlloc = 0x00;
	
	if (audDrv != NULL)
		opts = AudioDrv_GetOptions(audDrv);
	else if (audDrvLog != NULL)
		opts = AudioDrv_GetOptions(audDrvLog);
	if (opts == NULL)
		return 0xFF;
	opts->sampleRate = sampleRate;
	opts->numChannels = 2;
	opts->numBitsPerSmpl = 16;
	smplSize = opts->numChannels * opts->numBitsPerSmpl / 8;
	
	if (audDrv != NULL)
	{
		printf("Opening Device %u ...\n", idWavOutDev);
		retVal = AudioDrv_Start(audDrv, idWavOutDev);
		if (retVal)
		{
			fprintf(stderr, "Device Init Error: %02X\n", retVal);
			return retVal;
		}
		
		smplAlloc = AudioDrv_GetBufferSize(audDrv) / smplSize;
		localAudBufSize = 0;
	}
	else
	{
		smplAlloc = opts->sampleRate / 4;
		localAudBufSize = smplAlloc * smplSize;
	}
	
	locAudBuf.resize(localAudBufSize);
	mainPlr.SetOutputSettings(opts->sampleRate, opts->numChannels, opts->numBitsPerSmpl, smplAlloc);
	
	return 0x00;
}

static uint8_t StopAudioDevice(void)
{
	uint8_t retVal;
	
	retVal = 0x00;
	if (audDrv != NULL)
		retVal = AudioDrv_Stop(audDrv);
	locAudBuf.clear();
	
	return retVal;
}

static uint8_t StartDiskWriter(const char* fileName)
{
	AUDIO_OPTS* opts;
	uint8_t retVal;
	
	if (audDrvLog == NULL)
		return 0x00;
	
	opts = AudioDrv_GetOptions(audDrvLog);
	if (audDrv != NULL)
		*opts = *AudioDrv_GetOptions(audDrv);
	
	WavWrt_SetFileName(AudioDrv_GetDrvData(audDrvLog), fileName);
	retVal = AudioDrv_Start(audDrvLog, 0);
	if (! retVal && audDrv != NULL)
		AudioDrv_DataForward_Add(audDrv, audDrvLog);
	return retVal;
}

static uint8_t StopDiskWriter(void)
{
	if (audDrvLog == NULL)
		return 0x00;
	
	if (audDrv != NULL)
		AudioDrv_DataForward_Remove(audDrv, audDrvLog);
	return AudioDrv_Stop(audDrvLog);
}


#ifndef _WIN32
static struct termios oldterm;
static uint8_t termmode = 0xFF;

static void changemode(uint8_t noEcho)
{
	if (termmode == 0xFF)
	{
		tcgetattr(STDIN_FILENO, &oldterm);
		termmode = 0;
	}
	if (termmode == noEcho)
		return;
	
	if (noEcho)
	{
		struct termios newterm;
		newterm = oldterm;
		newterm.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &newterm);
		termmode = 1;
	}
	else
	{
		tcsetattr(STDIN_FILENO, TCSANOW, &oldterm);
		termmode = 0;
	}
	
	return;
}

static int _kbhit(void)
{
	struct timeval tv;
	fd_set rdfs;
	
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	
	FD_ZERO(&rdfs);
	FD_SET(STDIN_FILENO, &rdfs);
	select(STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);
	
	return FD_ISSET(STDIN_FILENO, &rdfs);;
}
#endif
