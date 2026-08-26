#ifdef PPU_CPP

#define Clip(x) (((x) & 0x2000) ? ((x) | ~0x03ff) : ((x) & 0x03ff))

void PPU::Background::render_mode7() {
  signed px, py;
  signed tx, ty, tile, palette;

  const auto& pregs = self.render_regs();
  const BgSnap* snap = self.render_src ? &self.render_src->bg[id] : nullptr;
  const auto& r = snap ? snap->regs : regs;
  const bool pri0_en = snap ? snap->priority0_enable : priority0_enable;
  const bool pri1_en = snap ? snap->priority1_enable : priority1_enable;
  const auto& bg1r = self.render_src ? self.render_src->bg[ID::BG1].regs : self.bg1.regs;
  const auto& bg2r = self.render_src ? self.render_src->bg[ID::BG2].regs : self.bg2.regs;
  const uint8* vram = self.vram_data();
  const bool direct_color = self.render_src ? self.render_src->screen_regs.direct_color : self.screen.regs.direct_color;

  signed a = sclip<16>(pregs.m7a);
  signed b = sclip<16>(pregs.m7b);
  signed c = sclip<16>(pregs.m7c);
  signed d = sclip<16>(pregs.m7d);

  signed cx = sclip<13>(pregs.m7x);
  signed cy = sclip<13>(pregs.m7y);
  signed hofs = sclip<13>(pregs.mode7_hoffset);
  signed vofs = sclip<13>(pregs.mode7_voffset);

  signed y = (pregs.mode7_vflip == false ? self.render_vcounter() : 255 - self.render_vcounter());

  uint16* mosaic_x;
  uint16* mosaic_y;
  if(id == ID::BG1) {
    mosaic_x = mosaic_table[bg1r.mosaic];
    mosaic_y = mosaic_table[bg1r.mosaic];
  } else {
    mosaic_x = mosaic_table[bg2r.mosaic];
    mosaic_y = mosaic_table[bg1r.mosaic];
  }

  unsigned priority0 = (pri0_en ? r.priority0 : 0);
  unsigned priority1 = (pri1_en ? r.priority1 : 0);
  if(priority0 + priority1 == 0) return;

  signed psx = ((a * Clip(hofs - cx)) & ~63) + ((b * Clip(vofs - cy)) & ~63) + ((b * mosaic_y[y]) & ~63) + (cx << 8);
  signed psy = ((c * Clip(hofs - cx)) & ~63) + ((d * Clip(vofs - cy)) & ~63) + ((d * mosaic_y[y]) & ~63) + (cy << 8);
  for(signed x = 0; x < 256; x++) {
    px = (psx + (a * mosaic_x[x])) >> 8;
    py = (psy + (c * mosaic_x[x])) >> 8;

    switch(pregs.mode7_repeat) {
    case 0: case 1: {
      px &= 1023;
      py &= 1023;
      tx = ((px >> 3) & 127);
      ty = ((py >> 3) & 127);
      tile = vram[(ty * 128 + tx) << 1];
      palette = vram[(((tile << 6) + ((py & 7) << 3) + (px & 7)) << 1) + 1];
      break;
    }

    case 2: {
      if((px | py) & ~1023) {
        palette = 0;
      } else {
        px &= 1023;
        py &= 1023;
        tx = ((px >> 3) & 127);
        ty = ((py >> 3) & 127);
        tile = vram[(ty * 128 + tx) << 1];
        palette = vram[(((tile << 6) + ((py & 7) << 3) + (px & 7)) << 1) + 1];
      }
      break;
    }

    case 3: {
      if((px | py) & ~1023) {
        tile = 0;
      } else {
        px &= 1023;
        py &= 1023;
        tx = ((px >> 3) & 127);
        ty = ((py >> 3) & 127);
        tile = vram[(ty * 128 + tx) << 1];
      }
      palette = vram[(((tile << 6) + ((py & 7) << 3) + (px & 7)) << 1) + 1];
      break;
    }
    }

    unsigned priority;
    if(id == ID::BG1) {
      priority = priority0;
    } else {
      priority = (palette & 0x80 ? priority1 : priority0);
      palette &= 0x7f;
    }

    if(palette == 0) continue;
    unsigned plot_x = (pregs.mode7_hflip == false ? x : 255 - x);

    unsigned color;
    if(direct_color && id == ID::BG1) {
      color = self.screen.get_direct_color(0, palette);
    } else {
      color = self.screen.get_palette(palette);
    }

    if(r.main_enable && !window.main[plot_x]) self.screen.output.plot_main(plot_x, color, priority, id);
    if(r.sub_enable && !self.can_skip_sub_screen() && !window.sub[plot_x]) self.screen.output.plot_sub(plot_x, color, priority, id);
  }
}

#undef Clip

#endif
