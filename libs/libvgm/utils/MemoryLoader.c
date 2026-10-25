#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../common_def.h"
#include "DataLoader.h"
#include "MemoryLoader.h"

enum
{
	// mode: compression
	MLMODE_CMP_RAW = 0x00,
	MLMODE_CMP_GZ = 0x10
};

typedef struct _memory_loader MEMORY_LOADER;

typedef uint32_t (*MLOAD_READ)(MEMORY_LOADER *loader, uint8_t *buffer, uint32_t numBytes);

struct _memory_loader
{
	uint8_t modeCompr;
	uint32_t srcSize;	// size of source data
	const uint8_t *srcData;
	uint32_t decSize;	// decompressed size
	uint32_t pos;
	z_stream zStream;

	MLOAD_READ ReadData;
};


static uint8_t MemoryLoader_dopen(void *context);
static uint32_t MemoryLoader_dread(void *context, uint8_t *buffer, uint32_t numBytes);
static uint32_t MemoryLoader_ReadDataRaw(MEMORY_LOADER *loader, uint8_t *buffer, uint32_t numBytes);
static uint32_t MemoryLoader_ReadDataGZ(MEMORY_LOADER *loader, uint8_t *buffer, uint32_t numBytes);
static uint32_t MemoryLoader_dlength(void *context);
static int32_t MemoryLoader_dtell(void *context);
static uint8_t MemoryLoader_dseek(void *context, uint32_t offset, uint8_t whence);
static uint8_t MemoryLoader_dclose(void *context);
static uint8_t MemoryLoader_deof(void *context);
//DATA_LOADER *MemoryLoader_Init(const uint8_t *buffer, uint32_t length);


INLINE uint32_t ReadLE32(const uint8_t *data)
{
	return	(data[0x03] << 24) | (data[0x02] << 16) |
			(data[0x01] <<  8) | (data[0x00] <<  0);
}

static uint8_t MemoryLoader_dopen(void *context)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;

	loader->pos = 0;

	// minimum gzip size of 18 bytes (10 bytes header + 4 bytes CRC32 + 4 bytes size)
	if(loader->srcSize >= 18 && loader->srcData[0] == 31 && loader->srcData[1] == 139)	// check for .gz file header
	{
		loader->modeCompr = MLMODE_CMP_GZ;
		loader->zStream.zalloc = Z_NULL;
		loader->zStream.zfree = Z_NULL;
		loader->zStream.opaque = Z_NULL;
		loader->zStream.avail_in = loader->srcSize;
		loader->zStream.next_in = (z_const Bytef *)loader->srcData;
		loader->decSize = ReadLE32(&loader->srcData[loader->srcSize - 4]);
		if(inflateInit2(&loader->zStream, 0x20 | 15) != Z_OK)
			return 0x01;
		loader->ReadData = &MemoryLoader_ReadDataGZ;
		return 0x00;
	}

	loader->modeCompr = MLMODE_CMP_RAW;
	loader->decSize = loader->srcSize;
	loader->ReadData = &MemoryLoader_ReadDataRaw;
	return 0x00;
}

static uint32_t MemoryLoader_dread(void *context, uint8_t *buffer, uint32_t numBytes)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;

	if(loader->pos >= loader->decSize) return 0;
	if(loader->pos + numBytes > loader->decSize)
		numBytes = loader->decSize - loader->pos;

	return loader->ReadData(loader, buffer, numBytes);
}

static uint32_t MemoryLoader_ReadDataRaw(MEMORY_LOADER *loader, uint8_t *buffer, uint32_t numBytes)
{
	memcpy(buffer, &loader->srcData[loader->pos], numBytes);
	loader->pos += numBytes;
	return numBytes;
}

static uint32_t MemoryLoader_ReadDataGZ(MEMORY_LOADER *loader, uint8_t *buffer, uint32_t numBytes)
{
	int ret;
	uint32_t bytesWritten;

	loader->zStream.avail_out = numBytes;
	loader->zStream.next_out = (Bytef *)buffer;
	ret = inflate(&loader->zStream, Z_SYNC_FLUSH);
	// usually returns Z_OK or Z_STREAM_END (when encountering EOF)
	// but even in case of errors, we'll just return the amount of bytes read.
	// TODO: maybe return 0 when getting errors in order to prevent playing garbage data.

	bytesWritten = loader->zStream.total_out - loader->pos;
	loader->pos = loader->zStream.total_out;
	if (ret == Z_STREAM_END)
		loader->decSize = loader->pos;
	return bytesWritten;
}

static uint32_t MemoryLoader_dlength(void *context)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
	return loader->decSize;
}

static int32_t MemoryLoader_dtell(void *context)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
	return loader->pos;
}

static uint8_t MemoryLoader_dseek(void *context, uint32_t offset, uint8_t whence)
{
	(void)context;
	(void)offset;
	(void)whence;
	return 0; /* TODO */
}

static uint8_t MemoryLoader_dclose(void *context)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
	if(loader->modeCompr == MLMODE_CMP_GZ)
		inflateEnd(&loader->zStream);
	return 0x00;
}

static uint8_t MemoryLoader_deof(void *context)
{
	MEMORY_LOADER *loader = (MEMORY_LOADER *)context;
	return loader->pos >= loader->decSize;
}

DATA_LOADER *MemoryLoader_Init(const uint8_t *buffer, uint32_t length)
{
	DATA_LOADER *dLoader;
	MEMORY_LOADER *mLoader;

	dLoader = (DATA_LOADER *)calloc(1, sizeof(DATA_LOADER));
	if(dLoader == NULL) return NULL;

	mLoader = (MEMORY_LOADER *)calloc(1, sizeof(MEMORY_LOADER));
	if(mLoader == NULL)
	{
		free(dLoader);
		return NULL;
	}

	mLoader->srcData = buffer;
	mLoader->srcSize = length;
	mLoader->decSize = 0;

	DataLoader_Setup(dLoader,&memoryLoader,mLoader);

	return dLoader;
}

const DATA_LOADER_CALLBACKS memoryLoader = {
	0x4D454D20,		// "MEM "
	"Memory Loader",
	MemoryLoader_dopen,
	MemoryLoader_dread,
	MemoryLoader_dseek,
	MemoryLoader_dclose,
	MemoryLoader_dtell,
	MemoryLoader_dlength,
	MemoryLoader_deof,
	NULL,
};
