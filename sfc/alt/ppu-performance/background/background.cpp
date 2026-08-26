#ifdef PPU_CPP

#include "mode7.cpp"

unsigned PPU::Background::get_tile(unsigned hoffset, unsigned voffset) {
  const BgSnap* snap = self.render_src ? &self.render_src->bg[id] : nullptr;
  const unsigned mask_x_ = snap ? snap->mask_x : mask_x;
  const unsigned mask_y_ = snap ? snap->mask_y : mask_y;
  const unsigned tile_width_ = snap ? snap->tile_width : tile_width;
  const unsigned tile_height_ = snap ? snap->tile_height : tile_height;
  const unsigned scx_ = snap ? snap->scx : scx;
  const unsigned scy_ = snap ? snap->scy : scy;
  const unsigned screen_addr = snap ? snap->regs.screen_addr : regs.screen_addr;
  const uint8* vram = self.vram_data();

  unsigned tile_x = (hoffset & mask_x_) >> tile_width_;
  unsigned tile_y = (voffset & mask_y_) >> tile_height_;

  unsigned tile_pos = ((tile_y & 0x1f) << 5) + (tile_x & 0x1f);
  if(tile_y & 0x20) tile_pos += scy_;
  if(tile_x & 0x20) tile_pos += scx_;

  const uint16 tiledata_addr = screen_addr + (tile_pos << 1);
  return (vram[tiledata_addr + 0] << 0) + (vram[tiledata_addr + 1] << 8);
}

void PPU::Background::offset_per_tile(unsigned x, unsigned y, unsigned& hoffset, unsigned& voffset) {
  const auto& pregs = self.render_regs();
  const auto& bg3r = self.render_src ? self.render_src->bg[ID::BG3].regs : self.bg3.regs;
  unsigned opt_x = (x + (hscroll & 7)), hval, vval;
  if(opt_x >= 8) {
    hval = self.bg3.get_tile((opt_x - 8) + (bg3r.hoffset & ~7), bg3r.voffset + 0);
    if(pregs.bgmode != 4)
    vval = self.bg3.get_tile((opt_x - 8) + (bg3r.hoffset & ~7), bg3r.voffset + 8);

    if(pregs.bgmode == 4) {
      if(hval & opt_valid_bit) {
        if(!(hval & 0x8000)) {
          hoffset = opt_x + (hval & ~7);
        } else {
          voffset = y + hval;
        }
      }
    } else {
      if(hval & opt_valid_bit) {
        hoffset = opt_x + (hval & ~7);
      }
      if(vval & opt_valid_bit) {
        voffset = y + vval;
      }
    }
  }
}

void PPU::Background::scanline() {
  if(self.vcounter() == 1) {
    mosaic_vcounter = regs.mosaic + 1;
    mosaic_voffset = 1;
  } else if(--mosaic_vcounter == 0) {
    mosaic_vcounter = regs.mosaic + 1;
    mosaic_voffset += regs.mosaic + 1;
  }
  if(self.regs.display_disable) return;

  hires = (self.regs.bgmode == 5 || self.regs.bgmode == 6);
  width = !hires ? 256 : 512;

  tile_height = regs.tile_size ? 4 : 3;
  tile_width = hires ? 4 : tile_height;

  mask_x = (tile_height == 4 ? width << 1 : width);
  mask_y = mask_x;
  if(regs.screen_size & 1) mask_x <<= 1;
  if(regs.screen_size & 2) mask_y <<= 1;
  mask_x--;
  mask_y--;

  scx = (regs.screen_size & 1 ? 32 << 5 : 0);
  scy = (regs.screen_size & 2 ? 32 << 5 : 0);
  if(regs.screen_size == 3) scy <<= 1;
}

void PPU::Background::render() {
  const BgSnap* snap = self.render_src ? &self.render_src->bg[id] : nullptr;
  const auto& r = snap ? snap->regs : regs;
  const bool pri0_en = snap ? snap->priority0_enable : priority0_enable;
  const bool pri1_en = snap ? snap->priority1_enable : priority1_enable;

  if(r.mode == Mode::Inactive) return;
  if(r.main_enable == false && r.sub_enable == false) return;

  bool plot_sub = r.sub_enable;
  if(plot_sub && self.can_skip_sub_screen()) plot_sub = false;

  if(r.main_enable) window.render(0);
  if(plot_sub) window.render(1);
  if(r.mode == Mode::Mode7) return render_mode7();

  unsigned priority0 = (pri0_en ? r.priority0 : 0);
  unsigned priority1 = (pri1_en ? r.priority1 : 0);
  if(priority0 + priority1 == 0) return;

  unsigned mosaic_hcounter = 1;
  unsigned mosaic_palette = 0;
  unsigned mosaic_priority = 0;
  unsigned mosaic_color = 0;

  const auto& pregs = self.render_regs();
  const bool hires_ = snap ? snap->hires : hires;
  const signed width_ = snap ? snap->width : width;
  const unsigned tile_width_ = snap ? snap->tile_width : tile_width;
  const unsigned tile_height_ = snap ? snap->tile_height : tile_height;
  const unsigned mask_x_ = snap ? snap->mask_x : mask_x;
  const unsigned mask_y_ = snap ? snap->mask_y : mask_y;
  const unsigned mosaic_voffset_ = snap ? snap->mosaic_voffset : mosaic_voffset;
  const bool direct_color = self.render_src ? self.render_src->screen_regs.direct_color : self.screen.regs.direct_color;

  const unsigned bgpal_index = (pregs.bgmode == 0 ? id << 5 : 0);
  const unsigned pal_size = 2 << r.mode;
  const unsigned tile_mask = 0x0fff >> r.mode;
  const unsigned tiledata_index = r.tiledata_addr >> (4 + r.mode);

  hscroll = r.hoffset;
  vscroll = r.voffset;

  unsigned y = (r.mosaic == 0 ? self.render_vcounter() : mosaic_voffset_);
  if(hires_) {
    hscroll <<= 1;
    if(pregs.interlace) y = (y << 1) + self.render_field();
  }

  unsigned tile_pri, tile_num;
  unsigned pal_index, pal_num;
  unsigned hoffset, voffset, col;
  bool mirror_x, mirror_y;

  const bool is_opt_mode = (pregs.bgmode == 2 || pregs.bgmode == 4 || pregs.bgmode == 6);
  const bool is_direct_color_mode = (direct_color == true && id == ID::BG1 && (pregs.bgmode == 3 || pregs.bgmode == 4));

  signed x = 0 - (hscroll & 7);
  while(x < width_) {
    hoffset = x + hscroll;
    voffset = y + vscroll;
    if(is_opt_mode) offset_per_tile(x, y, hoffset, voffset);
    hoffset &= mask_x_;
    voffset &= mask_y_;

    tile_num = get_tile(hoffset, voffset);
    mirror_y = tile_num & 0x8000;
    mirror_x = tile_num & 0x4000;
    tile_pri = tile_num & 0x2000 ? priority1 : priority0;
    pal_num = (tile_num >> 10) & 7;
    pal_index = (bgpal_index + (pal_num << pal_size)) & 0xff;

    if(tile_width_  == 4 && (bool)(hoffset & 8) != mirror_x) tile_num +=  1;
    if(tile_height_ == 4 && (bool)(voffset & 8) != mirror_y) tile_num += 16;
    tile_num = ((tile_num & 0x03ff) + tiledata_index) & tile_mask;

    if(mirror_y) voffset ^= 7;
    unsigned mirror_xmask = !mirror_x ? 0 : 7;

    uint8* tiledata = self.render_cache().tile(r.mode, tile_num);
    tiledata += ((voffset & 7) * 8);

    if(r.mosaic == 0 && hires_ == false) {
      for(unsigned n = 0; n < 8; n++, x++) {
        if(x & width_) continue;
        unsigned pal = tiledata[n ^ mirror_xmask];
        if(pal == 0) continue;
        unsigned color = is_direct_color_mode
          ? self.screen.get_direct_color(pal_num, pal)
          : self.screen.get_palette(pal_index + pal);
        if(r.main_enable && !window.main[x]) self.screen.output.plot_main(x, color, tile_pri, id);
        if(plot_sub && !window.sub[x]) self.screen.output.plot_sub(x, color, tile_pri, id);
      }
      continue;
    }

    for(unsigned n = 0; n < 8; n++, x++) {
      if(x & width_) continue;
      if(--mosaic_hcounter == 0) {
        mosaic_hcounter = r.mosaic + 1;
        mosaic_palette = tiledata[n ^ mirror_xmask];
        mosaic_priority = tile_pri;
        if(is_direct_color_mode) {
          mosaic_color = self.screen.get_direct_color(pal_num, mosaic_palette);
        } else {
          mosaic_color = self.screen.get_palette(pal_index + mosaic_palette);
        }
      }
      if(mosaic_palette == 0) continue;

      if(hires_ == false) {
        if(r.main_enable && !window.main[x]) self.screen.output.plot_main(x, mosaic_color, mosaic_priority, id);
        if(plot_sub && !window.sub[x]) self.screen.output.plot_sub(x, mosaic_color, mosaic_priority, id);
      } else {
        signed half_x = x >> 1;
        if(x & 1) {
          if(r.main_enable && !window.main[half_x]) self.screen.output.plot_main(half_x, mosaic_color, mosaic_priority, id);
        } else {
          if(plot_sub && !window.sub[half_x]) self.screen.output.plot_sub(half_x, mosaic_color, mosaic_priority, id);
        }
      }
    }
  }
}

PPU::Background::Background(PPU& self, unsigned id) : self(self), id(id) {
  priority0_enable = true;
  priority1_enable = true;

  opt_valid_bit = (id == ID::BG1 ? 0x2000 : id == ID::BG2 ? 0x4000 : 0x0000);

  mosaic_table = new uint16*[16];
  for(unsigned m = 0; m < 16; m++) {
    mosaic_table[m] = new uint16[4096];
    for(unsigned x = 0; x < 4096; x++) {
      mosaic_table[m][x] = (x / (m + 1)) * (m + 1);
    }
  }
}

PPU::Background::~Background() {
  for(unsigned m = 0; m < 16; m++) delete[] mosaic_table[m];
  delete[] mosaic_table;
}

#endif
