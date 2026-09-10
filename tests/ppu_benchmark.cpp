// Synthetic renderer regression/benchmark. No commercial ROM is required.
#include <thread>
#include <mutex>
#include <condition_variable>
#include <emulator/emulator.hpp>
#define private public
#include <sfc/sfc.hpp>
#undef private
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace SuperFamicom;
int main(int argc, char** argv) {
  unsigned frames = argc > 1 ? atoi(argv[1]) : 120;
  bool stress = argc > 2;
  for(unsigned mode = 0; mode < 8; mode++) {
    uint64_t reference = 0;
    for(unsigned threaded = 0; threaded < 2; threaded++) {
      ppu.set_render_thread_mode(threaded ? 1 : 2);
      ppu.power();
      ppu.set_frameskip(0);
      uint32_t rng = 12345;
      for(unsigned i = 0; i < sizeof(ppu.vram); i++) {
        rng = rng * 1664525u + 1013904223u;
        ppu.vram_write(i, rng >> 24);
      }
      for(unsigned i = 0; i < 512; i++) ppu.cgram_write(i, (i * 13) & (i & 1 ? 0x7f : 0xff));
      for(unsigned i = 0; i < 544; i++) ppu.oam_write(i, i * 11);
      ppu.mmio_write(0x2105, mode);
      ppu.mmio_write(0x2107, 0x60);
      ppu.mmio_write(0x2108, 0x64);
      ppu.mmio_write(0x2109, 0x68);
      ppu.mmio_write(0x210a, 0x6c);
      ppu.mmio_write(0x212c, 0x1f);
      ppu.mmio_write(0x212d, 0x1f);
      ppu.mmio_write(0x211b, 0); ppu.mmio_write(0x211b, 1);
      ppu.mmio_write(0x211e, 0); ppu.mmio_write(0x211e, 1);
      uint64_t hash = 1469598103934665603ull;
      auto start = std::chrono::steady_clock::now();
      for(unsigned f = 0; f < frames; f++) {
        ppu.drain_render();
        ppu.mmio_write(0x2100, 0x80);
        // Sparse tile changes must not invalidate every other decoded tile.
        for(unsigned i = 0; i < 16; i++) ppu.vram_write((f * 64 + i) & 65535, f + i);
        if(stress) {
          ppu.mmio_write(0x2106, (f & 15) << 4 | 15);  // mosaic
          ppu.mmio_write(0x2133, (f & 1) ? 0x4d : 0x04); // interlace/hires/overscan/EXTBG
          ppu.display.interlace = (f & 1) != 0;
          ppu.PPUcounter::status.field = (f & 1) != 0;
        }
        ppu.mmio_write(0x2131, f % 3 ? 0 : 0x7f);
        ppu.mmio_write(0x2100, 15);
        ppu.sprite.frame();
        for(unsigned y = 1; y < (stress ? 240u : 225u); y++) {
          if(stress) {
            // Exercise in-flight snapshots: window, palette, scroll, forced-blank
            // VRAM writes, and more generations than the three snapshot slots.
            ppu.mmio_write(0x2123, (y & 1) ? 0xaa : 0x55);
            ppu.mmio_write(0x2125, (y & 1) ? 0xaa : 0x55);
            ppu.mmio_write(0x2126, y / 2);
            ppu.mmio_write(0x2127, 255 - y / 2);
            ppu.mmio_write(0x212e, 0x1f);
            ppu.mmio_write(0x2130, 0x12);
            ppu.mmio_write(0x210d, y); ppu.mmio_write(0x210d, 0);
            ppu.cgram_write((y * 2) & 511, f + y);
            if(y % 8 == 0) {
              ppu.mmio_write(0x2100, 0x80);
              ppu.vram_write((f * 64 + y * 16) & 65535, f ^ y);
              ppu.mmio_write(0x2100, 15);
            }
          }
          ppu.PPUcounter::status.vcounter = y;
          ppu.scanline();
          ppu.render_scanline();
        }
        ppu.drain_render();
        hash = (hash ^ ppu.framebuffer_hash()) * 1099511628211ull;
      }
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
      printf("mode=%u threaded=%u ms=%.4f hash=%016llx\n", mode, threaded, ms, (unsigned long long)hash);
      if(!threaded) reference = hash;
      else if(hash != reference) { fprintf(stderr, "Renderer mismatch in mode %u\n", mode); return 1; }
    }
  }
  ppu.set_render_thread_mode(2);
}
