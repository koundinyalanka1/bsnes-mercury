// Regression coverage for direct pages versus the byte-wise mapping tables.
// Build against the same headers/core as tests/ppu_benchmark.cpp.
#include <sfc/sfc.hpp>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace SuperFamicom;

static void check_map(unsigned size, unsigned base, unsigned mask,
                      unsigned lo, unsigned hi, bool writable, bool expect_fast) {
  std::vector<uint8> memory(size);
  for(unsigned i = 0; i < size; i++) memory[i] = (i ^ (i >> 8) ^ (i >> 16)) & 255;
  function<uint8(unsigned)> read = [&](unsigned addr) { return memory.at(addr % size); };
  function<void(unsigned, uint8)> write = [&](unsigned addr, uint8 data) {
    if(writable) memory.at(addr % size) = data;
  };
  bus.map_reset();
  bus.map(read, write, 0x00, 0x3f, lo, hi, size, base, mask,
          writable ? Cartridge::Mapping::fastmode_readwrite : Cartridge::Mapping::fastmode_readonly,
          memory.data());
  for(unsigned bank = 0; bank < 64; bank++) {
    for(unsigned offset = lo; offset <= hi; offset++) {
      unsigned addr = (bank << 16) | offset;
      assert(bool(bus.fast_read[addr >> Bus::fast_page_size_bits]) == expect_fast);
      assert(bool(bus.fast_write[addr >> Bus::fast_page_size_bits]) == (expect_fast && writable));
      assert(bus.read(addr) == read(bus.target[addr]));
      if((offset & 255) == 0 || offset == hi) {
        uint8 old = read(bus.target[addr]);
        bus.write(addr, old ^ 0xa5);
        assert(read(bus.target[addr]) == (writable ? (old ^ 0xa5) : old));
      }
    }
  }
  // Cheats must still see the underlying byte on a direct ROM page.
  const unsigned cheat_addr = (0x20 << 16) | (lo + 32);
  const uint8 original = bus.read(cheat_addr);
  cheat.append(cheat_addr, original, original ^ 0xff);
  assert(bus.read(cheat_addr) == (original ^ 0xff));
  cheat.reset();
  assert(bus.read(cheat_addr) == original);
  // A partial MMIO overlay must revoke the whole direct page, while the
  // unaffected bytes in that page keep their original callback mapping.
  const unsigned overlay = lo + 16;
  unsigned writes = 0;
  function<uint8(unsigned)> io_read = [](unsigned) { return uint8(0x5a); };
  function<void(unsigned, uint8)> io_write = [&](unsigned, uint8) { writes++; };
  bus.map(io_read, io_write, 0, 0, overlay, overlay);
  assert(!bus.fast_read[overlay >> Bus::fast_page_size_bits]);
  assert(!bus.fast_write[overlay >> Bus::fast_page_size_bits]);
  assert(bus.read(overlay) == 0x5a);
  bus.write(overlay, 0x81);
  assert(writes == 1);
  assert(bus.read(overlay - 1) == read(bus.target[overlay - 1]));
  bus.map_reset(); // release callbacks before their captured storage disappears
}

int main() {
  ppu.set_render_thread_mode(2); // disabled
  check_map(2 << 20, 0, 0, 0x8000, 0xffff, false, true); // LoROM
  check_map(3 << 20, 0, 0, 0x0000, 0xffff, false, true); // non-power-of-two HiROM
  check_map(2 << 20, 0x8000, 0x8000, 0x8000, 0xffff, false, true);
  check_map(0x2000, 0, 0xe000, 0x6000, 0x7fff, true, true); // SRAM
  check_map(0x1000, 0, 0xe000, 0x6000, 0x7fff, true, false); // sub-page mirrors
  check_map(0x2000, 1, 0, 0x6000, 0x7fff, true, false); // unaligned backing
  check_map(0x10000, 0x2000, 0, 0x8000, 0xffff, false, false); // base splits bank span
  check_map(0x2000, 0, 1, 0x6000, 0x7fff, true, false); // noncontiguous page
  check_map(0x2000, 0, 0, 0x6001, 0x7ffe, true, false); // partial page
  puts("Direct ROM/RAM pages, mirrors, writes and MMIO overlays match slow mappings.");
}
