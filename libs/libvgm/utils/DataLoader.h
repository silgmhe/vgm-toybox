#ifndef __DATALOADER_H__
#define __DATALOADER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../stdtype.h"

typedef uint8_t (*DLOADCB_GENERIC)(void *context);
typedef void (*DLOADCB_GEN_CALL)(void *context);
typedef uint32_t (*DLOADCB_READ)(void *context, uint8_t *buffer, uint32_t numBytes);
typedef uint8_t (*DLOADCB_SEEK)(void *context, uint32_t offset, uint8_t whence);
typedef int32_t (*DLOADCB_TELL)(void *context);
typedef uint32_t (*DLOADCB_LENGTH)(void *context);

typedef struct _data_loader_callbacks
{
	uint32_t type;            /* 4-character-code for the file loader */
	const char *name;       /* human-readable name of the file loader */
	DLOADCB_GENERIC dopen;  /* open a file, URL, piece of memory, return 0 on success */
	DLOADCB_READ dread;     /* read bytes into buffer */
	DLOADCB_SEEK dseek;     /* seek to byte offset */
	DLOADCB_GENERIC dclose; /* closes out file, return 0 on success */
	DLOADCB_TELL dtell;     /* returns the current position of the data */
	DLOADCB_LENGTH dlength; /* returns the length of the data, in bytes */
	DLOADCB_GENERIC deof;   /* determines if we've seen eof or not (return 1 for eof) */
	DLOADCB_GEN_CALL ddeinit;   /* deinitialize loader and free context, may be NULL */
} DATA_LOADER_CALLBACKS;

enum
{
	DLSTAT_EMPTY = 0,
	DLSTAT_LOADING = 1,
	DLSTAT_LOADED = 2
};

typedef struct _data_loader
{
	uint8_t _status;
	uint32_t _bytesTotal;
	uint32_t _bytesLoaded;
	uint32_t _readStopOfs;
	uint8_t *_data;
	const DATA_LOADER_CALLBACKS *_callbacks;
	void *_context;
} DATA_LOADER;

/* calls the dopen and dlength functions
 * by default, loads whole file into memory, use
 * DataLoader_SetPreloadBytes to change this */
uint8_t DataLoader_Load(DATA_LOADER *loader);

/* Resets the DataLoader (calls DataReader_CancelLoading, unloads data, etc */
uint8_t DataLoader_Reset(DATA_LOADER *loader);

/* Returns a pointer to the DataLoader's memory buffer
 * call after any invocation of "Read", "ReadUntil", etc,
 * since the memory buffer pointer can change */
uint8_t *DataLoader_GetData(DATA_LOADER *loader);

/* returns _bytesTotal */
uint32_t DataLoader_GetTotalSize(const DATA_LOADER *loader);

/* returns _bytesLoaded */
uint32_t DataLoader_GetSize(const DATA_LOADER *loader);

/* returns _status */
uint8_t DataLoader_GetStatus(const DATA_LOADER *loader);

/* calls dread, then deof
 * if deof > 0, also calls CancelLoading (dclose) */
uint32_t DataLoader_Read(DATA_LOADER *loader, uint32_t numBytes);

/* calls dclose */
uint8_t DataLoader_CancelLoading(DATA_LOADER *loader);

/* sets number of bytes to preload */
void DataLoader_SetPreloadBytes(DATA_LOADER *loader, uint32_t byteCount);

/* reads until offset */
void DataLoader_ReadUntil(DATA_LOADER *loader, uint32_t fileOffset);

/* read all data */
void DataLoader_ReadAll(DATA_LOADER *loader);

/* convenience function for MemoryLoader,FileLoader, etc */
void DataLoader_Setup(DATA_LOADER *loader, const DATA_LOADER_CALLBACKS *callbacks, void *context);

/* tear-down function */
void DataLoader_Deinit(DATA_LOADER *loader);


#ifdef __cplusplus
}
#endif

#endif /* __DATALOADER_H__ */
