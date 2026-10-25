#include <stdlib.h>
#include <string.h>

#include "../stdtype.h"
#include "DataLoader.h"

uint8_t DataLoader_Reset(DATA_LOADER *loader)
{
	if (loader->_status == DLSTAT_EMPTY) return 1;

	DataLoader_CancelLoading(loader);

	if(loader->_data) {
		free(loader->_data);
		loader->_data = NULL;
		loader->_bytesLoaded = 0;
	}

	loader->_status = DLSTAT_EMPTY;

	return 0;
}

uint8_t *DataLoader_GetData(DATA_LOADER *loader) {
	return loader->_data;
}

uint32_t DataLoader_GetTotalSize(const DATA_LOADER *loader) {
	return loader->_bytesTotal;
}

uint32_t DataLoader_GetSize(const DATA_LOADER *loader) {
	return loader->_bytesLoaded;
}

uint8_t DataLoader_GetStatus(const DATA_LOADER *loader) {
	return loader->_status;
}

uint8_t DataLoader_CancelLoading(DATA_LOADER *loader)
{
	if(loader->_status != DLSTAT_LOADING) return 0x01;

	if(loader->_callbacks->dclose(loader->_context)) return 0x01;
	loader->_status = DLSTAT_LOADED;

	return 0x00;
}

uint8_t DataLoader_Load(DATA_LOADER *loader)
{
	uint8_t ret;
	if (loader->_status == DLSTAT_LOADING)
		return 0x01;

	DataLoader_Reset(loader);

	ret = loader->_callbacks->dopen(loader->_context);
	if(ret) return ret;

	loader->_bytesLoaded = 0x00;
	loader->_status = DLSTAT_LOADING;
	loader->_bytesTotal = loader->_callbacks->dlength(loader->_context);

	if (loader->_readStopOfs > 0)
		DataLoader_Read(loader,loader->_readStopOfs);

	return 0x00;
}

void DataLoader_SetPreloadBytes(DATA_LOADER *loader, uint32_t byteCount)
{
	loader->_readStopOfs = byteCount;
	return;
}

void DataLoader_ReadUntil(DATA_LOADER *loader, uint32_t fileOffset)
{
	if (fileOffset > loader->_bytesLoaded)
		DataLoader_Read(loader, fileOffset - loader->_bytesLoaded);
	return;
}

void DataLoader_ReadAll(DATA_LOADER *loader)
{
	while(DataLoader_Read(loader,loader->_bytesTotal - loader->_bytesLoaded) >0)
		;
	return;
}

uint32_t DataLoader_Read(DATA_LOADER *loader, uint32_t numBytes)
{
	uint32_t endOfs;
	uint32_t readBytes;

	if (loader->_status != DLSTAT_LOADING)
		return 0;

	endOfs = loader->_bytesLoaded + numBytes;
	if (endOfs < loader->_bytesLoaded)
		endOfs = (uint32_t)-1;
	if (endOfs > loader->_bytesTotal)
		endOfs = loader->_bytesTotal;

	loader->_data = (uint8_t *)realloc(loader->_data,endOfs);
	if(loader->_data == NULL) {
		return 0;
	}

	numBytes = endOfs - loader->_bytesLoaded;
	readBytes = loader->_callbacks->dread(loader->_context,&loader->_data[loader->_bytesLoaded],numBytes);
	if(!readBytes) return 0;
	loader->_bytesLoaded += readBytes;

	/* Note: The check (loaded >= total) is necessary, as the EoF flag is set only when
	         reading *beyond* the end of the file. Stopping at the last byte doesn't set it. */
	if (loader->_bytesLoaded >= loader->_bytesTotal || loader->_callbacks->deof(loader->_context))
	{
		DataLoader_CancelLoading(loader);
		loader->_status = DLSTAT_LOADED;
	}

	return readBytes;
}

void DataLoader_Deinit(DATA_LOADER *dLoader)
{
	if(dLoader == NULL) return;
	DataLoader_Reset(dLoader);
	if(dLoader->_context != NULL)
	{
		if (dLoader->_callbacks->ddeinit)
			dLoader->_callbacks->ddeinit(dLoader->_context);
		else
			free(dLoader->_context);
	}
	free(dLoader);
}

void DataLoader_Setup(DATA_LOADER *loader, const DATA_LOADER_CALLBACKS *callbacks, void *context) {
	loader->_data = NULL;
	loader->_status = DLSTAT_EMPTY;
	loader->_readStopOfs = (uint32_t)-1;
	loader->_context = context;
	loader->_callbacks = callbacks;
}
