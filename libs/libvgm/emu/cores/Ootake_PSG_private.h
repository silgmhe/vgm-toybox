#ifndef __OOTAKE_PSG_PRIVATE_H__
#define __OOTAKE_PSG_PRIVATE_H__

/*-----------------------------------------------------------------------------
	[PSG.h]
		ＰＳＧを記述するのに必要な定義および関数のプロトタイプ宣言を行ないます．

	Copyright (C) 2004 Ki

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**---------------------------------------------------------------------------*/
#include "../../stdtype.h"
#include "../snddef.h"

static uint8_t device_start_c6280_ootake(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);

/*-----------------------------------------------------------------------------
** 関数のプロトタイプ宣言を行ないます．
**---------------------------------------------------------------------------*/
static void*
OPSG_Init(
	uint32_t		clock,
	uint32_t		sampleRate);

static void
OPSG_Deinit(void* chip);

static void
OPSG_Mix(
	void*		chip,
	uint32_t		nSample,			// 書き出すサンプル数
	DEV_SMPL**	pDst);				// 出力先バッファ

static uint8_t
OPSG_Read(void* chip, uint8_t regNum);

static void
OPSG_Write(
	void*		chip,
	uint8_t		regNum,
	uint8_t		data);

//Kitao追加
static void
OPSG_ResetVolumeReg(void* chip);

//Kitao追加
static void
OPSG_SetMutePsgChannel(
	void*	chip,
	uint8_t	num,
	uint8_t	bMute);

static void OPSG_SetMuteMask(void* chip, uint32_t MuteMask);

//Kitao追加
static uint8_t
OPSG_GetMutePsgChannel(
	void*	chip,
	uint8_t	num);

//Kitao追加。v2.60
static void
OPSG_SetHoneyInTheSky(
	void*	chip,
	uint8_t	bHoneyInTheSky);

#endif	// __OOTAKE_PSG_PRIVATE_H__
