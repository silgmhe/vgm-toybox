#ifdef WIN32
#include <Windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "common_def.h"
#include "vgm/dblk_compr.h"


INLINE uint32_t GetSysTimeMS(void);
static uint8_t DecompressDataBlk_Old(uint32_t OutDataLen, uint8_t* OutData, uint32_t InDataLen, const uint8_t* InData, const PCM_COMPR_TBL* comprTbl);
static void CompressDataBlk_Old(uint32_t outLen, uint8_t* outData, uint32_t inLen, const uint8_t* inData, PCM_CMP_INF* ComprTbl);
uint16_t DataBlkCompr_GetIntSize(void);

// ---- Benchmarks ----
// 256 MB of input data
//	8-bit version: decompressed from 3 -> 8 bits (fits into uint8_t)
//	16-bit version: decompressed from 5 -> 12 bits (fits into uint16_t)
// All algorithms were executed once for warm up, then 4x for benchmarking.
//
//									FUINT8/FUINT16 bits
//	Algorithm			old 	32/32	16/16	32/16	16/32	8/16
//	--------			----	----	----	----	----	----
// MS VC 2010, 32-bit executable, compiled with /O2
//		Decompression
//	BitPack/copy 8		10148	 5121	 5105	 5129	 5105	 4704!
//	DPCM 8				 8034	 5792	 5967	 5788	 5967	 5511!
//	BitPack/LUT 8		10795	 5078	 6470	 5074	 6463	 4863!
//	BitPack/copy 16		 4750	 3385!	 5850	 5631	 3432	 5515
//	DPCM 16				 4361	 3674!	 4146	 3779	 4103	 4856
//	BitPack/LUT 16		 4832	 3386	 3732	 3296!	 3502	 3401
//		Compression
//	BitPack/copy 8		20428	 5924	 5429!	 5940	 5433!	 5441!
//	BitPack/LUT 8		35834	15577	18022	13767!	17601	18826
//	BitPack/copy 16		12652	 3440!	 3604	 3787	 3639	 3748
//	BitPack/LUT 16		23658	 3744!	 4006	 4048	 4017	 4107

// GCC 4.8.3, 32-bit executable, compiled with -O2
//		Decompression
//	BitPack/copy 8		 8779	 6314	 6154!	 6388	 6150!	 6310
//	DPCM 8				10035	 8533	 8249!	 8502	 8221!	 8428
//	BitPack/LUT 8		 9239	 6888	 6735!	 6888	 6747!	 6833
//	BitPack/copy 16		 6288	 3510!	 4056	 4317	 4041	 4049
//	DPCM 16				 6560	 5328	 4446!	 5371	 4426!	 5238
//	BitPack/LUT 16		 6560	 4602	 4193!	 4637	 4243	 4349
//		Compression
//	BitPack/copy 8		18089	 7188!	 7769	 7180!	 8284	 7968
//	BitPack/LUT 8		28151	18026	17905!	18654	16848	17987
//	BitPack/copy 16		11049	 4326	 4501	 4025!	 4965	 4477
//	BitPack/LUT 16		18428	 4434!	 4934	 4918	 4984	 4778

typedef struct
{
	uint8_t comprType;
	uint8_t subType;
	uint8_t bits;
	uint8_t canCompr;
} BENCH_LIST;

#define BENCH_SIZE		256	// compressed data size in MB
#define BENCH_WARM_REP	1	// number of times for warm up
#define BENCH_REPEAT	4	// number of times the benchmark is repeated
static uint32_t dblk_benchTime;

#define BENCHLIST_COUNT	6
static BENCH_LIST benchList[BENCHLIST_COUNT] =
{
	{0x00, 0x00,  8, 1},	// type 00 - bit packing, sub-type: 00 - copy
	{0x01, 0x00,  8, 0},	// type 01 - DPCM, sub-type: ignored
	{0x00, 0x02,  8, 1},	// type 00 - bit packing, sub-type: 02 - LUT
	{0x00, 0x00, 16, 1},	// type 00 - bit packing, sub-type: 00 - copy
	{0x01, 0x00, 16, 0},	// type 01 - DPCM, sub-type: ignored
	{0x00, 0x02, 16, 1},	// type 00 - bit packing, sub-type: 02 - LUT
};
static const char* TEXT_COMPR[] = {"Bit-Packing", "DPCM"};
static const char* TEXT_CMP_BPK[] = {"copy", "shift", "LUT"};

static uint8_t verbosity = 0;
// write 00s (best-case scenario for BitPack/LUT)
//static uint8_t bytePattern = 0x00;
// write alternating bits (average-case scenario for BitPack/LUT)
static uint8_t bytePattern = 0xA5;

static void GenerateComprStr(char* buffer, uint8_t comprType, uint8_t subType, uint8_t bits)
{
	const char** SUB_STRS = NULL;
	
	if (comprType == 0x00)
		SUB_STRS = TEXT_CMP_BPK;
	
	if (SUB_STRS != NULL)
		sprintf(buffer, "%s (%s/%u)", TEXT_COMPR[comprType], SUB_STRS[subType], bits);
	else
		sprintf(buffer, "%s (%u)", TEXT_COMPR[comprType], bits);
	
	return;
}

int main(int argc, char* argv[])
{
	uint8_t DPCMTbl[0x20] =
	{
		0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
		0x80,-0x01,-0x02,-0x04,-0x08,-0x10,-0x20,-0x40,
		0x00, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04,
		0x80,-0x10,-0x20,-0x40,-0x80,-0x01,-0x02,-0x04
	};
	PCM_COMPR_TBL PCMTbl8 = {0x01, 0, 8, 3, 0x20, {DPCMTbl}};
	PCM_COMPR_TBL PCMTbl16 = {0x01, 0, 12, 5, 0x20, {DPCMTbl}};
	PCM_CDB_INF cdbInf8 = {0, 0, {0x00, 0x00, 8, 3, 0x00, &PCMTbl8}};		// 3 -> 8 bits
	PCM_CDB_INF cdbInf16 = {0, 0, {0x00, 0x00, 12, 5, 0x00, &PCMTbl16}};	// 5 -> 12 bits
	PCM_CMP_INF* cmpInf8 = &cdbInf8.cmprInfo;
	PCM_CMP_INF* cmpInf16 = &cdbInf16.cmprInfo;
	
	uint16_t bitsFU8;
	uint16_t bitsFU16;
	
	// DecompressDataBlk Benchmark
	uint32_t dataLen;
	uint8_t* data;
	uint32_t dataLenRaw;
	uint8_t* dataRaw;
	uint32_t decLen;
	uint8_t* decData;
	uint32_t repCntr;
	uint32_t curBench;
	uint32_t curBT;
	// Bit 0 (01): 0 - old, 1 - new
	// Bit 1 (02): 0 - decompress, 1 - compress
	uint32_t benchTime[4 * BENCHLIST_COUNT];	// Bit Packing (copy)
	BENCH_LIST* tempBL;
	PCM_CDB_INF* tempCDB;
	PCM_CMP_INF* tempCInf;
	uint32_t startTime;
	char comprStr[0x20];
	
	/*{
		uint8_t cmpTbl8[0x10] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF};
		uint8_t decTbl8[0x100];
		uint16_t cmpTbl16[0x10] = {0x000, 0x040, 0x080, 0x0C0, 0x100, 0x140, 0x180, 0x1C0, 0x200, 0x300, 0x380, 0x3C0, 0x3E0, 0x3F0, 0x3F8, 0x400};
		uint16_t decTbl16[0x400];
		GenerateReverseLUT_8(0x100, decTbl8, 0x10, cmpTbl8);
		GenerateReverseLUT_16(0x400, decTbl16, 0x10, cmpTbl16);
		getchar();
		return 0;
	}*/
	repCntr = DataBlkCompr_GetIntSize();
	bitsFU8 = ((repCntr >> 0) & 0xFF) * 8;
	bitsFU16 = ((repCntr >> 8) & 0xFF) * 8;
	
	dataLenRaw = BENCH_SIZE * 1048576;
	cdbInf8.decmpLen = BPACK_SIZE_DEC(dataLenRaw, cmpInf8->bitsCmp, cmpInf8->bitsDec);
	cdbInf16.decmpLen = BPACK_SIZE_DEC(dataLenRaw, cmpInf16->bitsCmp, cmpInf16->bitsDec);
	decLen = (cdbInf8.decmpLen < cdbInf16.decmpLen) ? cdbInf16.decmpLen : cdbInf8.decmpLen;
	
	dataLen = 0x0A + dataLenRaw;	// including VGM data block header
	data = (uint8_t*)malloc(dataLen);
	dataRaw = &data[0x0A];
	memset(data, 0x00, 0x0A);
	memset(dataRaw, bytePattern, dataLenRaw);
	decData = (uint8_t*)malloc(decLen);
	
	if (verbosity >= 1)
	{
		printf("FUINT8 = %u bits, FUINT16 = %u bits\n", bitsFU8, bitsFU16);
		printf("Input Buffer size: %.2f MB, Output Buffer size: %.2f MB\n",
				dataLenRaw / 1048576.0f, decLen / 1048576.0f);
	}
	
	for (repCntr = 0; repCntr < 4 * BENCHLIST_COUNT; repCntr ++)
		benchTime[repCntr] = 0;
	for (repCntr = 0; repCntr < BENCH_WARM_REP + BENCH_REPEAT; repCntr ++)
	{
		if (verbosity >= 1)
			printf("---- Pass %u ----\n", 1 + repCntr);
		for (curBench = 0; curBench < BENCHLIST_COUNT; curBench ++)
		{
			tempBL = &benchList[curBench];
			curBT = curBench * 4;
			if (verbosity >= 2)
			{
				GenerateComprStr(comprStr, tempBL->comprType, tempBL->subType, tempBL->bits);
				printf("%s\n", comprStr);
			}
			
			tempCDB = (tempBL->bits <= 8) ? &cdbInf8 : &cdbInf16;
			tempCInf = &tempCDB->cmprInfo;
			tempCInf->comprType = tempBL->comprType;
			tempCInf->subType = tempBL->subType;
			
			// ---- decompression benchmark ----
			WriteComprDataBlkHdr(dataLen, data, tempCDB);
			DecompressDataBlk_Old(decLen, decData, dataLen, data, tempCInf->comprTbl);
			if (repCntr >= BENCH_WARM_REP)
				benchTime[curBT + 0x00] += dblk_benchTime;
			if (verbosity >= 2)
				printf("Decompression Time [old]: %u\n", dblk_benchTime);
			
			startTime = GetSysTimeMS();
			DecompressDataBlk(decLen, decData, dataLenRaw, dataRaw, tempCInf);
			dblk_benchTime = GetSysTimeMS() - startTime;
			if (verbosity >= 2)
				printf("Decompression Time [new]: %u\n", dblk_benchTime);
			if (repCntr >= BENCH_WARM_REP)
				benchTime[curBT + 0x01] += dblk_benchTime;
			
			if (tempBL->canCompr)
			{
				// ---- compression benchmark ----
				CompressDataBlk_Old(dataLen, data, tempCDB->decmpLen, decData, tempCInf);
				if (verbosity >= 2)
					printf("Compression Time [old]: %u\n", dblk_benchTime);
				if (repCntr >= BENCH_WARM_REP)
					benchTime[curBT + 0x02] += dblk_benchTime;
				
				startTime = GetSysTimeMS();
				CompressDataBlk(dataLen, data, tempCDB->decmpLen, decData, tempCInf);
				dblk_benchTime = GetSysTimeMS() - startTime;
				if (verbosity >= 2)
					printf("Compression Time [new]: %u\n", dblk_benchTime);
				if (repCntr >= BENCH_WARM_REP)
					benchTime[curBT + 0x03] += dblk_benchTime;
			}
		}
		fflush(stdout);
	}
	free(data);
	free(decData);
	if (verbosity >= 1)
		printf("\n");
	
	for (repCntr = 0; repCntr < 4 * BENCHLIST_COUNT; repCntr ++)
		benchTime[repCntr] = (benchTime[repCntr] + BENCH_REPEAT / 2) / BENCH_REPEAT;
	
	printf("Average Times (decompression):\n");
	for (curBench = 0; curBench < BENCHLIST_COUNT; curBench ++)
	{
		tempBL = &benchList[curBench];
		curBT = curBench * 4;
		
		GenerateComprStr(comprStr, tempBL->comprType, tempBL->subType, tempBL->bits);
		printf("%u/%u\t%s: old %u, new %u\n", bitsFU8, bitsFU16, comprStr, benchTime[curBT + 0x00], benchTime[curBT + 0x01]);
	}
	printf("\nAverage Times (compression):\n");
	for (curBench = 0; curBench < BENCHLIST_COUNT; curBench ++)
	{
		tempBL = &benchList[curBench];
		if (! tempBL->canCompr)
			continue;
		curBT = curBench * 4;
		
		GenerateComprStr(comprStr, tempBL->comprType, tempBL->subType, tempBL->bits);
		printf("%u/%u\t%s: old %u, new %u\n", bitsFU8, bitsFU16, comprStr, benchTime[curBT + 0x02], benchTime[curBT + 0x03]);
	}
	printf("\n");
	
	if (verbosity >= 1)
		printf("Done.\n");
#if defined(_MSC_VER) && defined(_DEBUG)
	getchar();
#endif
	return 0;
}

INLINE uint32_t GetSysTimeMS(void)
{
#ifdef WIN32
	return GetTickCount();
#else
	return 0;
#endif
}

void compression_test(void)
{
	PCM_CDB_INF cdbInf8 = {0, 0, {0x00, 0x01, 8, 4, 0x00, NULL}};		// 8 -> 4 bits
	PCM_CDB_INF cdbInf16 = {0, 0, {0x00, 0x00, 12, 8, 0x180, NULL}};	// 12 -> 8 bits
	PCM_CMP_INF* cmpInf8 = &cdbInf8.cmprInfo;
	PCM_CMP_INF* cmpInf16 = &cdbInf16.cmprInfo;
	
	uint32_t uncLen;
	uint8_t* uncData;
	uint32_t cmpLen;
	uint8_t* cmpData;
	FILE* hFile;
	
	hFile = fopen("demofiles/32XSample_Celtic.raw", "rb");
	fseek(hFile, 0, SEEK_END);
	uncLen = ftell(hFile);
	fseek(hFile, 0, SEEK_SET);
	uncData = (uint8_t*)malloc(uncLen);
	fread(uncData, 1, uncLen, hFile);
	fclose(hFile);	hFile = NULL;
	
	cmpLen = BPACK_SIZE_CMP(uncLen, cmpInf16->bitsCmp, cmpInf16->bitsDec) * 2;
	cmpData = (uint8_t*)malloc(cmpLen);
	
	CompressDataBlk_Old(cmpLen, cmpData, uncLen, uncData, cmpInf16);
	hFile = fopen("demofiles/32XSample_Celtic_old.cmp", "wb");
	fwrite(cmpData, 1, cmpLen, hFile);
	fclose(hFile);	hFile = NULL;
	free(cmpData);
	
	cmpData = (uint8_t*)malloc(cmpLen);
	CompressDataBlk(cmpLen, cmpData, uncLen, uncData, cmpInf16);
	hFile = fopen("demofiles/32XSample_Celtic_new.cmp", "wb");
	fwrite(cmpData, 1, cmpLen, hFile);
	fclose(hFile);	hFile = NULL;
	free(cmpData);
	
	return;
}


#define FUINT8	unsigned int
#define FUINT16	unsigned int

INLINE uint16_t ReadLE16(const uint8_t* Data)
{
	// read 16-Bit Word (Little Endian/Intel Byte Order)
	return (Data[0x01] << 8) | (Data[0x00] << 0);
}

INLINE uint32_t ReadLE32(const uint8_t* Data)
{
	// read 32-Bit Word (Little Endian/Intel Byte Order)
	return	(Data[0x03] << 24) | (Data[0x02] << 16) |
			(Data[0x01] <<  8) | (Data[0x00] <<  0);
}

static uint8_t DecompressDataBlk_Old(uint32_t outLen, uint8_t* outData, uint32_t inLen, const uint8_t* inData, const PCM_COMPR_TBL* comprTbl)
{
	uint8_t comprType;
	uint8_t bitsDec;
	FUINT8 bitsCmp;
	uint8_t cmpSubType;
	uint16_t addVal;
	const uint8_t* inPos;
	const uint8_t* inDataEnd;
	uint8_t* outPos;
	const uint8_t* outDataEnd;
	FUINT16 inVal;
	FUINT16 outVal;
	FUINT8 valSize;
	FUINT8 inShift;
	FUINT8 outShift;
	uint8_t* ent1B;
	uint16_t* ent2B;
	uint32_t Time;
	
	// ReadBits Variables
	FUINT8 bitsToRead;
	FUINT8 bitReadVal;
	FUINT8 inValB;
	FUINT8 bitMask;
	FUINT8 outBit;
	
	// Variables for DPCM
	uint16_t OutMask;
	
	comprType = inData[0x00];
	//*outLen = ReadLE32(&inData[0x01]);
	//*retOutData = (uint8_t*)realloc(*retOutData, *outLen);
	//outData = *retOutData;
	
	Time = GetSysTimeMS();
	switch(comprType)
	{
	case 0x00:	// Bit Packing compression
		bitsDec = inData[0x05];
		bitsCmp = inData[0x06];
		cmpSubType = inData[0x07];
		addVal = ReadLE16(&inData[0x08]);
		ent1B = NULL;
		ent2B = NULL;
		
		if (cmpSubType == 0x02)
		{
			PCM_COMPR_TBL PCMTbl = *comprTbl;
			ent1B = PCMTbl.values.d8;	// Big Endian note: Those are stored in LE and converted when reading.
			ent2B = PCMTbl.values.d16;
			if (! PCMTbl.valueCount)
			{
				printf("Error loading table-compressed data block! No table loaded!\n");
				return 0x10;
			}
			else if (bitsDec != PCMTbl.bitsDec || bitsCmp != PCMTbl.bitsCmp)
			{
				printf("Warning! Data block and loaded value table incompatible!\n");
				return 0x11;
			}
		}
		
		valSize = (bitsDec + 7) / 8;
		inPos = inData + 0x0A;
		inDataEnd = inData + inLen;
		inShift = 0;
		outShift = bitsDec - bitsCmp;
		outDataEnd = outData + outLen;
		outVal = 0x0000;
		
		for (outPos = outData; outPos < outDataEnd && inPos < inDataEnd; outPos += valSize)
		{
			//inVal = ReadBits(inData, inPos, &inShift, bitsCmp);
			// inlined - is 30% faster
			outBit = 0x00;
			inVal = 0x0000;
			bitsToRead = bitsCmp;
			while(bitsToRead)
			{
				bitReadVal = (bitsToRead >= 8) ? 8 : bitsToRead;
				bitsToRead -= bitReadVal;
				bitMask = (1 << bitReadVal) - 1;
				
				inShift += bitReadVal;
				inValB = (*inPos << inShift >> 8) & bitMask;
				if (inShift >= 8)
				{
					inShift -= 8;
					inPos ++;
					if (inShift)
						inValB |= (*inPos << inShift >> 8) & bitMask;
				}
				
				inVal |= inValB << outBit;
				outBit += bitReadVal;
			}
			
			switch(cmpSubType)
			{
			case 0x00:	// Copy
				outVal = inVal + addVal;
				break;
			case 0x01:	// Shift Left
				outVal = (inVal << outShift) + addVal;
				break;
			case 0x02:	// Table
				switch(valSize)
				{
				case 0x01:
					outVal = ent1B[inVal];
					break;
				case 0x02:
#ifdef VGM_LITTLE_ENDIAN
					outVal = ent2B[inVal];
#else
					outVal = ReadLE16((uint8_t*)&ent2B[inVal]);
#endif
					break;
				}
				break;
			}
			
#ifdef VGM_LITTLE_ENDIAN
			//memcpy(outPos, &outVal, valSize);
			if (valSize == 0x01)
				*((uint8_t*)outPos) = (uint8_t)outVal;
			else //if (valSize == 0x02)
				*((uint16_t*)outPos) = (uint16_t)outVal;
#else
			if (valSize == 0x01)
			{
				*outPos = (uint8_t)outVal;
			}
			else //if (valSize == 0x02)
			{
				// save explicitly in Little Endian
				outPos[0x00] = (uint8_t)((outVal & 0x00FF) >> 0);
				outPos[0x01] = (uint8_t)((outVal & 0xFF00) >> 8);
			}
#endif
		}
		break;
	case 0x01:	// Delta-PCM
		bitsDec = inData[0x05];
		bitsCmp = inData[0x06];
		outVal = ReadLE16(&inData[0x08]);
		
		{
			PCM_COMPR_TBL PCMTbl = *comprTbl;
			ent1B = PCMTbl.values.d8;
			ent2B = PCMTbl.values.d16;
			if (! PCMTbl.valueCount)
			{
				printf("Error loading table-compressed data block! No table loaded!\n");
				return 0x10;
			}
			else if (bitsDec != PCMTbl.bitsDec || bitsCmp != PCMTbl.bitsCmp)
			{
				printf("Warning! Data block and loaded value table incompatible!\n");
				return 0x11;
			}
		}
		
		valSize = (bitsDec + 7) / 8;
		OutMask = (1 << bitsDec) - 1;
		inPos = inData + 0x0A;
		inDataEnd = inData + inLen;
		inShift = 0;
		outShift = bitsDec - bitsCmp;
		outDataEnd = outData + outLen;
		addVal = 0x0000;
		
		for (outPos = outData; outPos < outDataEnd && inPos < inDataEnd; outPos += valSize)
		{
			//inVal = ReadBits(inData, inPos, &inShift, bitsCmp);
			// inlined - is 30% faster
			outBit = 0x00;
			inVal = 0x0000;
			bitsToRead = bitsCmp;
			while(bitsToRead)
			{
				bitReadVal = (bitsToRead >= 8) ? 8 : bitsToRead;
				bitsToRead -= bitReadVal;
				bitMask = (1 << bitReadVal) - 1;
				
				inShift += bitReadVal;
				inValB = (*inPos << inShift >> 8) & bitMask;
				if (inShift >= 8)
				{
					inShift -= 8;
					inPos ++;
					if (inShift)
						inValB |= (*inPos << inShift >> 8) & bitMask;
				}
				
				inVal |= inValB << outBit;
				outBit += bitReadVal;
			}
			
			switch(valSize)
			{
			case 0x01:
				addVal = ent1B[inVal];
				outVal += addVal;
				outVal &= OutMask;
				*((uint8_t*)outPos) = (uint8_t)outVal;
				break;
			case 0x02:
#ifdef VGM_LITTLE_ENDIAN
				addVal = ent2B[inVal];
				outVal += addVal;
				outVal &= OutMask;
				*((uint16_t*)outPos) = (uint16_t)outVal;
#else
				addVal = ReadLE16((uint8_t*)&ent2B[inVal]);
				outVal += addVal;
				outVal &= OutMask;
				outPos[0x00] = (uint8_t)((outVal & 0x00FF) >> 0);
				outPos[0x01] = (uint8_t)((outVal & 0xFF00) >> 8);
#endif
				break;
			}
		}
		break;
	default:
		printf("Error: Unknown data block compression!\n");
		return 0x80;
	}
	dblk_benchTime = GetSysTimeMS() - Time;
	
	return 0x00;
}

// Notes: writes outData[outLen] = 0x00 when finishing with 0 bits left
INLINE void WriteBits(uint8_t* Data, uint32_t* Pos, uint8_t* BitPos, uint16_t Value, uint8_t BitsToWrite)
{
	uint8_t BitWriteVal;
	uint32_t OutPos;
	uint8_t InVal;
	uint8_t BitMask;
	uint8_t OutShift;
	uint8_t InBit;
	
	OutPos = *Pos;
	OutShift = *BitPos;
	InBit = 0x00;
	
	Data[OutPos] &= ~(0xFF >> OutShift);
	while(BitsToWrite)
	{
		BitWriteVal = (BitsToWrite >= 8) ? 8 : BitsToWrite;
		BitsToWrite -= BitWriteVal;
		BitMask = (1 << BitWriteVal) - 1;
		
		InVal = (Value >> InBit) & BitMask;
		OutShift += BitWriteVal;
		Data[OutPos] |= InVal << 8 >> OutShift;
		if (OutShift >= 8)
		{
			OutShift -= 8;
			OutPos ++;
			Data[OutPos] = InVal << 8 >> OutShift;
		}
		
		InBit += BitWriteVal;
	}
	
	*Pos = OutPos;
	*BitPos = OutShift;
	return;
}

static void CompressDataBlk_Old(uint32_t outLen, uint8_t* outData, uint32_t inLen, const uint8_t* inData, PCM_CMP_INF* ComprTbl)
{
	uint8_t valSize;
	
	uint32_t CurPos;
	uint8_t* DstBuf;
	uint32_t DstPos;
	uint16_t SrcVal;
	uint16_t DstVal = 0x0000;
	uint32_t CurEnt = 0;
	uint16_t BitMask;
	uint16_t AddVal;
	uint8_t BitShift;
	const uint8_t* Ent1B;
	const uint16_t* Ent2B;
	uint8_t DstShift;
	uint32_t Time;
	
	valSize = (ComprTbl->bitsDec + 7) / 8;
	
	DstBuf = outData;
	
	BitMask = (1 << ComprTbl->bitsCmp) - 1;
	AddVal = ComprTbl->baseVal;
	BitShift = ComprTbl->bitsDec - ComprTbl->bitsCmp;
	if (ComprTbl->comprTbl == NULL)
	{
		Ent1B = NULL;
		Ent2B = NULL;
	}
	else
	{
		Ent1B = ComprTbl->comprTbl->values.d8;
		Ent2B = ComprTbl->comprTbl->values.d16;
	}
	
	Time = GetSysTimeMS();
	SrcVal = 0x0000;	// must be initialized (else 8-bit values don't fill it completely)
	DstPos = 0x00;
	DstShift = 0;
	for (CurPos = 0x00; CurPos < inLen; CurPos += valSize)
	{
		memcpy(&SrcVal, &inData[CurPos], valSize);
		switch(ComprTbl->subType)
		{
		case 0x00:	// Copy
			DstVal = SrcVal - AddVal;
			break;
		case 0x01:	// Shift Left
			DstVal = (SrcVal - AddVal) >> BitShift;
			break;
		case 0x02:	// Table
			switch(valSize)
			{
			case 0x01:
				for (CurEnt = 0x00; CurEnt < ComprTbl->comprTbl->valueCount; CurEnt ++)
				{
					if (SrcVal == Ent1B[CurEnt])
						break;
				}
				break;
			case 0x02:
				for (CurEnt = 0x00; CurEnt < ComprTbl->comprTbl->valueCount; CurEnt ++)
				{
					if (SrcVal == Ent2B[CurEnt])
						break;
				}
				break;
			}
			DstVal = (uint16_t)CurEnt;
			break;
		}
		
		WriteBits(DstBuf, &DstPos, &DstShift, DstVal, ComprTbl->bitsCmp);
	}
	if (DstShift)
		DstPos ++;
	dblk_benchTime = GetSysTimeMS() - Time;
	
	return;
}
