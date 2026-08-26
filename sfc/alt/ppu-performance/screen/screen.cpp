#ifdef PPU_CPP

unsigned PPU::Screen::get_palette(unsigned color) {
  const uint8* cgram = self.cgram_data();
  #if defined(ENDIAN_LSB)
  return ((const uint16*)cgram)[color];
  #else
  color <<= 1;
  return (cgram[color + 0] << 0) + (cgram[color + 1] << 8);
  #endif
}

unsigned PPU::Screen::get_direct_color(unsigned p, unsigned t) {
  return ((t & 7) << 2) | ((p & 1) << 1) |
         (((t >> 3) & 7) << 7) | (((p >> 1) & 1) << 6) |
         ((t >> 6) << 13) | ((p >> 2) << 12);
}

uint16 PPU::Screen::addsub(unsigned x, unsigned y, bool halve) {
  const bool color_mode = self.render_src ? self.render_src->screen_regs.color_mode : regs.color_mode;
  if(!color_mode) {
    if(!halve) {
      unsigned sum = x + y;
      unsigned carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
      return (sum - carry) | (carry - (carry >> 5));
    } else {
      return (x + y - ((x ^ y) & 0x0421)) >> 1;
    }
  } else {
    unsigned diff = x - y + 0x8420;
    unsigned borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
    if(!halve) {
      return (diff - borrow) & (borrow - (borrow >> 5));
    } else {
      return (((diff - borrow) & (borrow - (borrow >> 5))) & 0x7bde) >> 1;
    }
  }
}

void PPU::Screen::scanline() {
  const auto& pregs = self.render_regs();
  const auto& r = self.render_src ? self.render_src->screen_regs : regs;
  unsigned main_color = get_palette(0);
  unsigned sub_color = (pregs.pseudo_hires == false && pregs.bgmode != 5 && pregs.bgmode != 6)
                     ? r.color : main_color;

  for(unsigned x = 0; x < 256; x++) {
    output.main[x].color = main_color;
    output.main[x].priority = 0;
    output.main[x].source = 6;

    output.sub[x].color = sub_color;
    output.sub[x].priority = 0;
    output.sub[x].source = 6;
  }

  window.render(0);
  window.render(1);
}

void PPU::Screen::render_black() {
  uint32* data = self.output + self.render_vcounter() * 1024;
  if(self.render_interlace() && self.render_field()) data += 512;
  unsigned width = self.render_src ? self.render_src->display_width : self.display.width;
  memset(data, 0, width << 2);
}

uint16 PPU::Screen::get_pixel_main(unsigned x) {
  const auto& r = self.render_src ? self.render_src->screen_regs : regs;
  auto main = output.main[x];
  auto sub = output.sub[x];

  if(!r.addsub_mode) {
    sub.source = 6;
    sub.color = r.color;
  }

  if(!window.main[x]) {
    if(!window.sub[x]) {
      return 0x0000;
    }
    main.color = 0x0000;
  }

  if(main.source != 5 && r.color_enable[main.source] && window.sub[x]) {
    bool halve = false;
    if(r.color_halve && window.main[x]) {
      if(!r.addsub_mode || sub.source != 6) halve = true;
    }
    return addsub(main.color, sub.color, halve);
  }

  return main.color;
}

uint16 PPU::Screen::get_pixel_sub(unsigned x) {
  const auto& r = self.render_src ? self.render_src->screen_regs : regs;
  auto main = output.sub[x];
  auto sub = output.main[x];

  if(!r.addsub_mode) {
    sub.source = 6;
    sub.color = r.color;
  }

  if(!window.main[x]) {
    if(!window.sub[x]) {
      return 0x0000;
    }
    main.color = 0x0000;
  }

  if(main.source != 5 && r.color_enable[main.source] && window.sub[x]) {
    bool halve = false;
    if(r.color_halve && window.main[x]) {
      if(!r.addsub_mode || sub.source != 6) halve = true;
    }
    return addsub(main.color, sub.color, halve);
  }

  return main.color;
}

void PPU::Screen::render() {
  const auto& pregs = self.render_regs();
  const auto& r = self.render_src ? self.render_src->screen_regs : regs;
  uint32* data = self.output + self.render_vcounter() * 1024;
  if(self.render_interlace() && self.render_field()) data += 512;

  bool any_math = false;
  for(unsigned n = 0; n < 7; n++) any_math |= r.color_enable[n];

  if(!pregs.pseudo_hires && pregs.bgmode != 5 && pregs.bgmode != 6) {
    if(!any_math) {
      for(unsigned i = 0; i < 256; i++) {
        unsigned color = window.main[i] ? output.main[i].color : 0;
        data[i] = pregs.display_brightness << 15 | color;
      }
    } else {
      for(unsigned i = 0; i < 256; i++) {
        data[i] = pregs.display_brightness << 15 | get_pixel_main(i);
      }
    }
  } else {
    for(unsigned i = 0; i < 256; i++) {
      *data++ = pregs.display_brightness << 15 | get_pixel_sub(i);
      *data++ = pregs.display_brightness << 15 | get_pixel_main(i);
    }
  }
}

PPU::Screen::Screen(PPU& self) : self(self) {
}

PPU::Screen::~Screen() {
}

void PPU::Screen::Output::plot_main(unsigned x, unsigned color, unsigned priority, unsigned source) {
  if(priority > main[x].priority) {
    main[x].color = color;
    main[x].priority = priority;
    main[x].source = source;
  }
}

void PPU::Screen::Output::plot_sub(unsigned x, unsigned color, unsigned priority, unsigned source) {
  if(priority > sub[x].priority) {
    sub[x].color = color;
    sub[x].priority = priority;
    sub[x].source = source;
  }
}

#endif
