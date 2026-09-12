#include <sfc/sfc.hpp>

#define DSP_CPP
namespace SuperFamicom {

DSP dsp;

#include "serialization.cpp"
#include "SPC_DSP.cpp"

void DSP::step(unsigned clocks) {
  clock += clocks;
}

void DSP::synchronize_smp() {
  if(SMP::Threaded == true) {
    if(clock >= 0 && scheduler.sync != Scheduler::SynchronizeMode::All) co_switch(smp.thread);
  } else {
    while(clock >= 0) smp.enter();
  }
}

void DSP::enter() {
  int clocks = 1;
  if(clock <= -(24 * 32)) clocks = 32;
  // This branch bounds clock to [-767, -24]; divide in native-width
  // arithmetic rather than paying for 64-bit division on 32-bit hosts.
  else if(clock <= -24) clocks = -(int)clock / 24;
  if(clocks < 1) clocks = 1;
  spc_dsp.run(clocks);
  step(24 * clocks);

  signed count = spc_dsp.sample_count();
  if(count > 0) {
    for(unsigned n = 0; n < count; n += 2) audio.sample(samplebuffer[n + 0], samplebuffer[n + 1]);
    spc_dsp.set_output(samplebuffer, 8192);
  }
}

bool DSP::mute() {
  return spc_dsp.mute();
}

uint8 DSP::read(uint8 addr) {
  return spc_dsp.read(addr);
}

void DSP::write(uint8 addr, uint8 data) {
  spc_dsp.write(addr, data);
}

void DSP::power() {
  spc_dsp.init(smp.apuram);
  spc_dsp.set_fast(fast_requested);
  spc_dsp.reset();
  spc_dsp.set_output(samplebuffer, 8192);
}

void DSP::reset() {
  spc_dsp.soft_reset();
  spc_dsp.set_output(samplebuffer, 8192);
}

void DSP::channel_enable(unsigned channel, bool enable) {
  channel_enabled[channel & 7] = enable;
  unsigned mask = 0;
  for(unsigned i = 0; i < 8; i++) {
    if(channel_enabled[i] == false) mask |= 1 << i;
  }
  spc_dsp.mute_voices(mask);
}

DSP::DSP() {
  for(unsigned i = 0; i < 8; i++) channel_enabled[i] = true;
  fast_requested = false;
  spc_dsp.set_fast(false);
}

}
