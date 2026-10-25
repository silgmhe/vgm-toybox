#ifndef _NES_CPU_H_
#define _NES_CPU_H_
#include "../device.h"

#define ILLEGAL_OPCODES 1
#define DISABLE_DECIMAL 1
#define USE_DIRECT_ZEROPAGE 0
#define USE_CALLBACK	1
#define USE_INLINEMMC 0
#define USE_USERPOINTER	1
#define External __inline
#include "km6502/km6502m.h"

#include "../Misc/log_cpu.h"

namespace xgm
{

class NES_MEM; // forward declaration
class NSF2_IRQ; // forward declaration

class NES_CPU : public IDevice
{
protected:
  int init_addr;
  int play_addr;
  int song;
  int region;
  K6502_Context context;
  bool breaked;
  int64_t fclocks_per_frame; // fCPU clocks per frame timer with fixed point precision
  int64_t fclocks_left_in_frame;
  uint32_t breakpoint;
  uint32_t irqs;
  unsigned int stolen_cycles;
  bool enable_irq;
  bool enable_nmi;
  bool extra_init;
  bool nmi_play;
  bool play_ready;
  IDevice *bus;
  NES_MEM* nes_mem;
  uint8_t nsf2_bits;
  NSF2_IRQ* nsf2_irq;
  CPULogger *log_cpu;

  void run_from (uint32_t address);

public:
  double nes_basecycles;
  NES_CPU (double clock = DEFAULT_CLOCK);
  ~NES_CPU ();
  void Reset ();
  void Start (
    int init_addr_,
    int play_addr_,
    double play_rate,
    int song_,
    int region_,
    uint8_t nsf2_bits_,
    bool enable_irq_,
    NSF2_IRQ* nsf2_irq_);
  int Exec (int clock); // returns number of clocks executed
  void SetMemory (IDevice *);
  void SetNESMemory (NES_MEM *);
  bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
  bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
  void SetLogger (CPULogger *logger);
  unsigned int GetPC() const;
  void StealCycles(unsigned int cycles);
  void EnableNMI(bool enable);

  // IRQ devices
  enum {
    IRQD_FRAME = 0,
    IRQD_DMC = 1,
    IRQD_NSF2 = 2,
	IRQD_COUNT
  };
  void UpdateIRQ(int device, bool on);
};

} // namespace xgm
#endif
