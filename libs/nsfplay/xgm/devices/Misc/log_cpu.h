#ifndef _LOG_CPU_H_
#define _LOG_CPU_H_

#include "../device.h"
#include <cstdio>

namespace xgm
{

class NES_CPU; // forward
class NSF; // forward

class CPULogger : public IDevice
{
public:
    CPULogger();
    virtual ~CPULogger();

    // IDevice virtuals
    virtual void Reset ();
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual void SetOption (int id, int val);

    // CPULogger specific
    int GetLogLevel () const;
    void SetSoundchip (uint8_t soundchip_);
    void SetFilename (const char * filename_);
    void Begin (const char* title);
    void Init (uint8_t reg_a, uint8_t reg_x);
    void Play ();
    void SetCPU (NES_CPU* c);
    void SetNSF (NSF* n);

protected:
    int log_level;
    uint8_t soundchip;
    FILE* file;
    char* filename;
    uint32_t frame_count;
	NES_CPU* cpu;
    NSF* nsf;
    uint8_t bank[8];
};

}

#endif