#include "SPC_DSP.h"

struct DSP : Thread {
  enum : bool { Threaded = false };
  alwaysinline void step(unsigned clocks);
  alwaysinline void synchronize_smp();

  bool mute();
  uint8 read(uint8 addr);
  void write(uint8 addr, uint8 data);

  void enter();
  void power();
  void reset();

  void channel_enable(unsigned channel, bool enable);
  //SPC_DSP::init() clears fast_mode, and power() runs it on every load and state
  //restore. Remember the request so the setting is not silently dropped there.
  void set_fast(bool enable) { fast_requested = enable; spc_dsp.set_fast(enable); }

  void serialize(serializer&);
  DSP();

private:
  SPC_DSP spc_dsp;
  int16 samplebuffer[8192];
  bool channel_enabled[8];
  bool fast_requested;
};

extern DSP dsp;
