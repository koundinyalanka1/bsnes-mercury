#ifdef CPU_CPP

uint8 CPU::pio() {
  return status.pio;
}

bool CPU::joylatch() {
  return status.joypad_strobe_latch;
}

bool CPU::interrupt_pending() {
  return false;
}

uint8 CPU::port_read(uint8 port) {
  return port_data[port & 3];
}

void CPU::port_write(uint8 port, uint8 data) {
  port_data[port & 3] = data;
}

alwaysinline void CPU::op_io() {
  add_clocks(6);
}

alwaysinline uint8 CPU::op_read(unsigned addr) {
  if(!cheat.enable()) {
    if((addr & 0xfe0000) == 0x7e0000) {
      regs.mdr = wram[addr & 0x1ffff];
      add_clocks(8);
      return regs.mdr;
    }
    if((addr & 0x40e000) == 0) {
      regs.mdr = wram[addr & 0x1fff];
      add_clocks(8);
      return regs.mdr;
    }
    if(uint8* page = bus.fast_read[addr >> Bus::fast_page_size_bits]) {
      regs.mdr = page[addr & Bus::fast_page_size_mask];
      add_clocks(speed(addr));
      return regs.mdr;
    }
  }
  regs.mdr = bus.read(addr);
  add_clocks(speed(addr));
  return regs.mdr;
}

alwaysinline void CPU::op_write(unsigned addr, uint8 data) {
  regs.mdr = data;
  if((addr & 0xfe0000) == 0x7e0000) {
    add_clocks(8);
    wram[addr & 0x1ffff] = data;
    return;
  }
  if((addr & 0x40e000) == 0) {
    add_clocks(8);
    wram[addr & 0x1fff] = data;
    return;
  }
  add_clocks(speed(addr));
  if(uint8* page = bus.fast_write[addr >> Bus::fast_page_size_bits]) {
    page[addr & Bus::fast_page_size_mask] = data;
    return;
  }
  bus.write(addr, data);
}

unsigned CPU::speed(unsigned addr) const {
  if(addr & 0x408000) {
    if(addr & 0x800000) return status.rom_speed;
    return 8;
  }
  if((addr + 0x6000) & 0x4000) return 8;
  if((addr - 0x4000) & 0x7e00) return 6;
  return 12;
}

#endif
