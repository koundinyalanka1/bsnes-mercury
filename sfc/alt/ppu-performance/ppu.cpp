#include <sfc/sfc.hpp>

#define PPU_CPP
namespace SuperFamicom {

PPU ppu;

#include "mmio/mmio.cpp"
#include "window/window.cpp"
#include "cache/cache.cpp"
#include "background/background.cpp"
#include "sprite/sprite.cpp"
#include "screen/screen.cpp"
#include "serialization.cpp"
#include "render-thread.cpp"

void PPU::step(unsigned clocks) {
  clock += clocks;
}

void PPU::synchronize_cpu() {
  if(CPU::Threaded == true) {
    if(clock >= 0 && scheduler.sync != Scheduler::SynchronizeMode::All) co_switch(cpu.thread);
  } else {
    while(clock >= 0) cpu.enter();
  }
}

void PPU::Enter() { ppu.enter(); }

void PPU::enter() {
  while(true) {
    if(scheduler.sync == Scheduler::SynchronizeMode::All) {
      scheduler.exit(Scheduler::ExitReason::SynchronizeEvent);
    }

    scanline();
    if(vcounter() < display.height && vcounter()) {
      add_clocks(512);
      render_scanline();
      add_clocks(lineclocks() - 512);
    } else {
      add_clocks(lineclocks());
    }
  }
}

void PPU::add_clocks(unsigned clocks) {
  tick(clocks);
  step(clocks);
  synchronize_cpu();
}

void PPU::render_scanline_inline() {
  if(regs.display_disable) return screen.render_black();
  screen.scanline();
  bg1.render();
  bg2.render();
  bg3.render();
  bg4.render();
  sprite.plot();
  screen.render();
}

void PPU::render_scanline() {
  if(display.framecounter) return;  //skip this frame?
  bg1.scanline();
  bg2.scanline();
  bg3.scanline();
  bg4.scanline();
  sprite.evaluate();
  if(!render_thread_running) {
    render_scanline_inline();
    return;
  }
  //Fill the ring slot directly rather than a stack copy: this removes a 1944-byte memset
  //plus the two full-job memcpys that used to happen inside the mutex.
  wait_for_job_slot();
  capture_line_job(jobs[job_write]);
  publish_line_job();
}

void PPU::scanline() {
  display.width = !hires() ? 256 : 512;
  display.height = !overscan() ? 225 : 240;
  if(vcounter() == 0) frame();
  if(vcounter() == display.height && regs.display_disable == false) sprite.address_reset();
}

void PPU::frame() {
  sprite.frame();
  system.frame();
  display.interlace = regs.interlace;
  display.overscan = regs.overscan;
  display.framecounter = display.frameskip == 0 ? 0 : (display.framecounter + 1) % display.frameskip;
}

void PPU::enable() {
  function<uint8 (unsigned)> reader = {&PPU::mmio_read, (PPU*)&ppu};
  function<void (unsigned, uint8)> writer = {&PPU::mmio_write, (PPU*)&ppu};

  bus.map(reader, writer, 0x00, 0x3f, 0x2100, 0x213f);
  bus.map(reader, writer, 0x80, 0xbf, 0x2100, 0x213f);
}

void PPU::power() {
  drain_render();
  for(auto& n : vram) n = 0;
  for(auto& n : oam) n = 0;
  for(auto& n : cgram) n = 0;
  worker_cache.invalidate();
  worker_cache_gen = ~0u;
  mark_vram_dirty();
  reset();
}

void PPU::reset() {
  drain_render();
  create(Enter, system.cpu_frequency());
  PPUcounter::reset();
  memset(surface, 0, 512 * 512 * sizeof(uint32));
  mmio_reset();
  display.interlace = false;
  display.overscan = false;
}

void PPU::layer_enable(unsigned layer, unsigned priority, bool enable) {
  switch(layer * 4 + priority) {
  case  0: bg1.priority0_enable = enable; break;
  case  1: bg1.priority1_enable = enable; break;
  case  4: bg2.priority0_enable = enable; break;
  case  5: bg2.priority1_enable = enable; break;
  case  8: bg3.priority0_enable = enable; break;
  case  9: bg3.priority1_enable = enable; break;
  case 12: bg4.priority0_enable = enable; break;
  case 13: bg4.priority1_enable = enable; break;
  case 16: sprite.priority0_enable = enable; break;
  case 17: sprite.priority1_enable = enable; break;
  case 18: sprite.priority2_enable = enable; break;
  case 19: sprite.priority3_enable = enable; break;
  }
}

void PPU::set_frameskip(unsigned frameskip) {
  display.frameskip = frameskip;
  display.framecounter = 0;
}

uint32 PPU::framebuffer_hash() const {
  uint32 h = 2166136261u;
  unsigned height = display.height ? display.height : 224;
  unsigned width = display.width ? display.width : 256;
  for(unsigned y = 0; y < height; y++) {
    const uint32* row = output + y * 1024;
    for(unsigned x = 0; x < width; x++) {
      h ^= row[x] + y;
      h *= 16777619u;
    }
  }
  return h;
}

PPU::PPU() :
cache(*this),
worker_cache(*this),
bg1(*this, Background::ID::BG1),
bg2(*this, Background::ID::BG2),
bg3(*this, Background::ID::BG3),
bg4(*this, Background::ID::BG4),
sprite(*this),
screen(*this) {
  surface = new uint32[512 * 512];
  output = surface + 16 * 512;
  display.width = 256;
  display.height = 224;
  display.frameskip = 0;
  display.framecounter = 0;

  ppu_fast_paths = true;
  render_src = nullptr;
  render_thread_mode = RenderThreadAuto;
  render_thread_running = false;
  render_thread_stop = false;
  worker_cache_gen = 0;
  vram_slot_current = -1;
  mark_vram_dirty();
  vram_gen = 0;
  job_read = job_write = job_count = 0;
  render_thread_affinity_cpu = -1;
  for(unsigned i = 0; i < VramSlots; i++) {
    vram_slot[i] = new uint8[64 * 1024]();
    vram_slot_ref[i] = 0;
  }
  set_render_thread_mode(RenderThreadAuto);
  //Covers the case where the thread never started, so stop_render_thread() returned early
  //and left neither cache holding its tiledata.
  sync_cache_allocation();
}

PPU::~PPU() {
  stop_render_thread();
  delete[] surface;
  for(unsigned i = 0; i < VramSlots; i++) delete[] vram_slot[i];
}

}
