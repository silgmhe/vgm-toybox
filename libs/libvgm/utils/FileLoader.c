#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../common_def.h"
#include "FileLoader.h"

#if HAVE_FILELOADER_W
#include <wchar.h>
#endif

#ifdef _MSC_VER
#define strdup	_strdup
#define wcsdup	_wcsdup
#endif

enum
{
	// mode: compression
	FLMODE_CMP_RAW = 0x00,
	FLMODE_CMP_GZ = 0x10
};

typedef struct _file_loader FILE_LOADER;

typedef uint8_t (*FLOAD_GENERIC)(FILE_LOADER *loader);
typedef uint32_t (*FLOAD_READ)(FILE_LOADER *loader, uint8_t *buffer, uint32_t numBytes);
typedef uint8_t (*FLOAD_SEEK)(FILE_LOADER *loader, uint32_t offset, uint8_t whence);
typedef int32_t (*FLOAD_TELL)(FILE_LOADER *loader);
typedef uint32_t (*FLOAD_LENGTH)(FILE_LOADER *loader);

typedef union _loader_handles
{
	FILE *hFileRaw;
	gzFile hFileGZ;
} LOADER_HANDLES;

struct _file_loader
{
	uint8_t modeCompr;
	uint32_t bytesTotal;
	LOADER_HANDLES hLoad;
	char *fileName;
#if HAVE_FILELOADER_W
	wchar_t* fileNameW;	// Note: used when fileName == NULL
#endif

	FLOAD_READ Read;
	FLOAD_SEEK Seek;
	FLOAD_GENERIC Close;
	FLOAD_TELL Tell;
	FLOAD_GENERIC Eof;
};


static uint8_t FileLoader_dopen(void *context);
static uint32_t FileLoader_dread(void *context, uint8_t *buffer, uint32_t numBytes);
static uint8_t FileLoader_dseek(void *context, uint32_t offset, uint8_t whence);
static uint8_t FileLoader_dclose(void *context);
static int32_t FileLoader_dtell(void *context);
static uint32_t FileLoader_dlength(void *context);
static uint8_t FileLoader_deof(void *context);

static uint32_t FileLoader_ReadRaw(FILE_LOADER *loader, uint8_t *buffer, uint32_t numBytes);
static uint8_t FileLoader_SeekRaw(FILE_LOADER *loader, uint32_t offset, uint8_t whence);
static uint8_t FileLoader_CloseRaw(FILE_LOADER *loader);
static int32_t FileLoader_TellRaw(FILE_LOADER *loader);
static uint8_t FileLoader_EofRaw(FILE_LOADER *loader);

static uint32_t FileLoader_ReadGZ(FILE_LOADER *loader, uint8_t *buffer, uint32_t numBytes);
static uint8_t FileLoader_SeekGZ(FILE_LOADER *loader, uint32_t offset, uint8_t whence);
static uint8_t FileLoader_CloseGZ(FILE_LOADER *loader);
static int32_t FileLoader_TellGZ(FILE_LOADER *loader);
static uint8_t FileLoader_EofGZ(FILE_LOADER *loader);

//DATA_LOADER *FileLoader_Init(const char *fileName);
//DATA_LOADER *FileLoader_InitW(const wchar_t *fileName);
static void FileLoader_dfree(void *context);


INLINE uint32_t ReadLE32(const uint8_t *data)
{
	return	(data[0x03] << 24) | (data[0x02] << 16) |
			(data[0x01] <<  8) | (data[0x00] <<  0);
}

static uint8_t FileLoader_dopen(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	uint8_t fileHdr[4];
	size_t readBytes;

#if HAVE_FILELOADER_W
	if (loader->fileName == NULL)
		loader->hLoad.hFileRaw = _wfopen(loader->fileNameW, L"rb");
	else
#endif
		loader->hLoad.hFileRaw = fopen(loader->fileName, "rb");
	if (loader->hLoad.hFileRaw == NULL) return 0x01;

	readBytes = fread(fileHdr, 0x01, 4, loader->hLoad.hFileRaw);
	if (readBytes < 4)
		memset(&fileHdr[readBytes], 0x00, 4 - readBytes);

	if (fileHdr[0] == 31 && fileHdr[1] == 139)	// check for .gz file header
	{
		uint8_t sizeBuffer[4];
		fseek(loader->hLoad.hFileRaw, -4, SEEK_END);
		readBytes = fread(sizeBuffer, 0x04, 1, loader->hLoad.hFileRaw);
		loader->bytesTotal = (readBytes > 0) ? ReadLE32(sizeBuffer) : 0;
		if (loader->bytesTotal < (uint32_t)ftell(loader->hLoad.hFileRaw) / 2)	// catch "decompressed size too small"
			loader->bytesTotal = 0;
		fclose(loader->hLoad.hFileRaw);
		loader->hLoad.hFileRaw = NULL;

#if HAVE_FILELOADER_W
		if (loader->fileName == NULL)
			loader->hLoad.hFileGZ = gzopen_w(loader->fileNameW, "rb");
		else
#endif
			loader->hLoad.hFileGZ = gzopen(loader->fileName, "rb");
		if (loader->hLoad.hFileGZ == NULL)
			return 0x01;	// TODO: separate error code for "invalid .gz file"
		loader->modeCompr = FLMODE_CMP_GZ;
		loader->Read = &FileLoader_ReadGZ;
		loader->Seek = &FileLoader_SeekGZ;
		loader->Close = &FileLoader_CloseGZ;
		loader->Tell = &FileLoader_TellGZ;
		loader->Eof = &FileLoader_EofGZ;
		return 0x00;
	}

	fseek(loader->hLoad.hFileRaw, 0, SEEK_END);
	loader->bytesTotal = ftell(loader->hLoad.hFileRaw);
	rewind(loader->hLoad.hFileRaw);

	loader->modeCompr = FLMODE_CMP_RAW;
	loader->Read = &FileLoader_ReadRaw;
	loader->Seek = &FileLoader_SeekRaw;
	loader->Close = &FileLoader_CloseRaw;
	loader->Tell = &FileLoader_TellRaw;
	loader->Eof = &FileLoader_EofRaw;

	return 0x00;
}

static uint32_t FileLoader_dread(void *context, uint8_t *buffer, uint32_t numBytes)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->Read(loader, buffer, numBytes);
}

static uint8_t FileLoader_dseek(void *context, uint32_t offset, uint8_t whence)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->Seek(loader, offset, whence);
}

static uint8_t FileLoader_dclose(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->Close(loader);
}

static int32_t FileLoader_dtell(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->Tell(loader);
}

static uint32_t FileLoader_dlength(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->bytesTotal;
}

static uint8_t FileLoader_deof(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
	return loader->Eof(loader);
}


static uint32_t FileLoader_ReadRaw(FILE_LOADER *loader, uint8_t *buffer, uint32_t numBytes)
{
	return fread(buffer, 0x01, numBytes, loader->hLoad.hFileRaw);
}

static uint8_t FileLoader_SeekRaw(FILE_LOADER *loader, uint32_t offset, uint8_t whence)
{
	return (uint8_t)fseek(loader->hLoad.hFileRaw, offset, whence);
}

static uint8_t FileLoader_CloseRaw(FILE_LOADER *loader)
{
	fclose(loader->hLoad.hFileRaw);
	loader->hLoad.hFileRaw = NULL;
	return 0x00;
}

static int32_t FileLoader_TellRaw(FILE_LOADER *loader)
{
	return ftell(loader->hLoad.hFileRaw);
}

static uint8_t FileLoader_EofRaw(FILE_LOADER *loader)
{
	return feof(loader->hLoad.hFileRaw);
}


static uint32_t FileLoader_ReadGZ(FILE_LOADER *loader, uint8_t *buffer, uint32_t numBytes)
{
	return gzread(loader->hLoad.hFileGZ, buffer, numBytes);
}

static uint8_t FileLoader_SeekGZ(FILE_LOADER *loader, uint32_t offset, uint8_t whence)
{
	if(whence == SEEK_END) return 0;
	return (gzseek(loader->hLoad.hFileGZ, offset, whence) == -1) ? 0x01 : 0x00;
}

static uint8_t FileLoader_CloseGZ(FILE_LOADER *loader)
{
	gzclose(loader->hLoad.hFileGZ);
	loader->hLoad.hFileGZ = NULL;
	return 0x00;
}

static int32_t FileLoader_TellGZ(FILE_LOADER *loader)
{
	return gztell(loader->hLoad.hFileGZ);
}

static uint8_t FileLoader_EofGZ(FILE_LOADER *loader)
{
	return gzeof(loader->hLoad.hFileGZ);
}


DATA_LOADER *FileLoader_Init(const char *fileName)
{
	DATA_LOADER *dLoader;
	FILE_LOADER *fLoader;

	dLoader = (DATA_LOADER *)calloc(1, sizeof(DATA_LOADER));
	if(dLoader == NULL) return NULL;

	fLoader = (FILE_LOADER *)calloc(1, sizeof(FILE_LOADER));
	if(fLoader == NULL)
	{
		free(dLoader);
		return NULL;
	}

	fLoader->fileName = strdup(fileName);

	DataLoader_Setup(dLoader,&fileLoader,fLoader);

	return dLoader;
}

#if HAVE_FILELOADER_W
DATA_LOADER *FileLoader_InitW(const wchar_t *fileName)
{
	DATA_LOADER *dLoader;
	FILE_LOADER *fLoader;

	dLoader = (DATA_LOADER *)calloc(1, sizeof(DATA_LOADER));
	if(dLoader == NULL) return NULL;

	fLoader = (FILE_LOADER *)calloc(1, sizeof(FILE_LOADER));
	if(fLoader == NULL)
	{
		free(dLoader);
		return NULL;
	}

	fLoader->fileName = NULL;	// explicitly mark as "unused"
	fLoader->fileNameW = wcsdup(fileName);

	DataLoader_Setup(dLoader,&fileLoader,fLoader);

	return dLoader;
}
#endif

static void FileLoader_dfree(void *context)
{
	FILE_LOADER *loader = (FILE_LOADER *)context;
#if HAVE_FILELOADER_W
	if (loader->fileName == NULL)
		free(loader->fileNameW);
#endif
	free(loader->fileName);
	free(loader);
}

const DATA_LOADER_CALLBACKS fileLoader = {
	0x46494C45,		// "FILE"
	"File Loader",
	FileLoader_dopen,
	FileLoader_dread,
	FileLoader_dseek,
	FileLoader_dclose,
	FileLoader_dtell,
	FileLoader_dlength,
	FileLoader_deof,
	FileLoader_dfree,
};
