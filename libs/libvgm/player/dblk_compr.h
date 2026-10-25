#ifndef __DBLK_COMPR_H__
#define __DBLK_COMPR_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "../stdtype.h"
#include "../common_def.h"

typedef struct _pcm_compression_table
{
	uint8_t comprType;
	uint8_t cmpSubType;
	uint8_t bitsDec;
	uint8_t bitsCmp;
	uint16_t valueCount;
	union
	{
		uint8_t* d8;
		uint16_t* d16;	// note: stored in Native Endian
	} values;
} PCM_COMPR_TBL;

typedef struct _compression_parameters
{
	// Compression Types:
	//	00 - bit packing
	//	01 - Delta-PCM
	uint8_t comprType;
	uint8_t subType;	// compression sub-type
	uint8_t bitsDec;	// bits per value (decompressed)
	uint8_t bitsCmp;	// bits per value (compressed)
	uint16_t baseVal;
	const PCM_COMPR_TBL* comprTbl;
} PCM_CMP_INF;

typedef struct _pcm_compr_datablk_info
{
	// general data
	uint32_t hdrSize;		// number of bytes taken by the compression header in the VGM *1
	uint32_t decmpLen;	// size of decompressed data *2
	// *1 written by Read/WriteComprDataBlkHdr
	// *2 set by ReadComprDataBlkHdr, read by WriteComprDataBlkHdr
	
	// actual parameters required for (De-)CompressDataBlk
	PCM_CMP_INF cmprInfo;
} PCM_CDB_INF;

// small functions to help calculating data sizes
// Bit Packing/DPCM:
INLINE uint32_t BPACK_SIZE_CMP(uint32_t sizeDec, uint32_t bitsCmp, uint32_t bitsDec)
{
	uint32_t byteBits = (bitsDec + 7) & ~7;
	return (uint32_t)(((uint64_t)sizeDec * bitsCmp + 7) / byteBits);
}

INLINE uint32_t BPACK_SIZE_DEC(uint32_t sizeCmp, uint32_t bitsCmp, uint32_t bitsDec)
{
	uint32_t byteBits = (bitsDec + 7) & ~7;
	return (uint32_t)((uint64_t)sizeCmp * byteBits / bitsCmp);
}

uint8_t ReadComprDataBlkHdr(uint32_t inLen, const uint8_t* inData, PCM_CDB_INF* retCdbInf);
uint8_t WriteComprDataBlkHdr(uint32_t outLen, uint8_t* outData, PCM_CDB_INF* cdbInf);
uint8_t DecompressDataBlk(uint32_t outLen, uint8_t* outData, uint32_t inLen, const uint8_t* inData, const PCM_CMP_INF* cmprInfo);
uint8_t DecompressDataBlk_VGM(uint32_t* outLen, uint8_t** retOutData, uint32_t inLen, const uint8_t* inData, const PCM_COMPR_TBL* comprTbl);
uint8_t CompressDataBlk(uint32_t outLen, uint8_t* outData, uint32_t inLen, const uint8_t* inData, const PCM_CMP_INF* cmprInfo);
void ReadPCMComprTable(uint32_t dataSize, const uint8_t* data, PCM_COMPR_TBL* comprTbl);
uint32_t WriteCompressionTable(uint32_t dataSize, uint8_t* data, PCM_COMPR_TBL* comprTbl);
void GenerateReverseLUT_8(uint16_t dstLen, uint8_t* dstLUT, uint16_t srcLen, const uint8_t* srcLUT);
void GenerateReverseLUT_16(uint32_t dstLen, uint16_t* dstLUT, uint32_t srcLen, const uint16_t* srcLUT);

#ifdef __cplusplus
}
#endif

#endif	// __DBLK_COMPR_H__
