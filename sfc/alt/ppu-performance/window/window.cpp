#ifdef PPU_CPP

void PPU::LayerWindow::render(bool screen) {
  bool one_enable_ = one_enable, one_invert_ = one_invert;
  bool two_enable_ = two_enable, two_invert_ = two_invert;
  unsigned mask_ = mask;
  bool main_enable_ = main_enable, sub_enable_ = sub_enable;
  const auto& pregs = ppu.render_regs();

  if(ppu.render_src) {
    const LayerWindowSnap* s = nullptr;
    if(this == &ppu.bg1.window) s = &ppu.render_src->bg[0].window;
    else if(this == &ppu.bg2.window) s = &ppu.render_src->bg[1].window;
    else if(this == &ppu.bg3.window) s = &ppu.render_src->bg[2].window;
    else if(this == &ppu.bg4.window) s = &ppu.render_src->bg[3].window;
    else if(this == &ppu.sprite.window) s = &ppu.render_src->sprite_window;
    if(s) {
      one_enable_ = s->one_enable; one_invert_ = s->one_invert;
      two_enable_ = s->two_enable; two_invert_ = s->two_invert;
      mask_ = s->mask;
      main_enable_ = s->main_enable; sub_enable_ = s->sub_enable;
    }
  }

  uint8* output;
  if(screen == 0) {
    output = main;
    if(main_enable_ == false) {
      memset(output, 0, 256);
      return;
    }
  } else {
    output = sub;
    if(sub_enable_ == false) {
      memset(output, 0, 256);
      return;
    }
  }

  if(one_enable_ == false && two_enable_ == false) {
    memset(output, 0, 256);
    return;
  }

  if(one_enable_ == true && two_enable_ == false) {
    bool set = 1 ^ one_invert_, clr = !set;
    unsigned left = pregs.window_one_left, right = pregs.window_one_right;
    memset(output, clr, 256);
    if(left <= right && right < 256) memset(output + left, set, right - left + 1);
    return;
  }

  if(one_enable_ == false && two_enable_ == true) {
    bool set = 1 ^ two_invert_, clr = !set;
    unsigned left = pregs.window_two_left, right = pregs.window_two_right;
    memset(output, clr, 256);
    if(left <= right && right < 256) memset(output + left, set, right - left + 1);
    return;
  }

  for(unsigned x = 0; x < 256; x++) {
    bool one_mask = (x >= pregs.window_one_left && x <= pregs.window_one_right) ^ one_invert_;
    bool two_mask = (x >= pregs.window_two_left && x <= pregs.window_two_right) ^ two_invert_;
    switch(mask_) {
    case 0: output[x] =  (one_mask | two_mask); break;
    case 1: output[x] =  (one_mask & two_mask); break;
    case 2: output[x] =  (one_mask ^ two_mask); break;
    case 3: output[x] = !(one_mask ^ two_mask); break;
    }
  }
}

//

void PPU::ColorWindow::render(bool screen) {
  bool one_enable_ = one_enable, one_invert_ = one_invert;
  bool two_enable_ = two_enable, two_invert_ = two_invert;
  unsigned mask_ = mask, main_mask_ = main_mask, sub_mask_ = sub_mask;
  const auto& pregs = ppu.render_regs();
  if(ppu.render_src) {
    const auto& s = ppu.render_src->color_window;
    one_enable_ = s.one_enable; one_invert_ = s.one_invert;
    two_enable_ = s.two_enable; two_invert_ = s.two_invert;
    mask_ = s.mask; main_mask_ = s.main_mask; sub_mask_ = s.sub_mask;
  }

  uint8* output = (screen == 0 ? main : sub);
  bool set = 1, clr = 0;

  switch(screen == 0 ? main_mask_ : sub_mask_) {
  case 0: memset(output, 1, 256); return;  //always
  case 1: set = 1, clr = 0; break;         //inside window only
  case 2: set = 0, clr = 1; break;         //outside window only
  case 3: memset(output, 0, 256); return;  //never
  }

  if(one_enable_ == false && two_enable_ == false) {
    memset(output, clr, 256);
    return;
  }

  if(one_enable_ == true && two_enable_ == false) {
    if(one_invert_) { set ^= 1; clr ^= 1; }
    unsigned left = pregs.window_one_left, right = pregs.window_one_right;
    memset(output, clr, 256);
    if(left <= right && right < 256) memset(output + left, set, right - left + 1);
    return;
  }

  if(one_enable_ == false && two_enable_ == true) {
    if(two_invert_) { set ^= 1; clr ^= 1; }
    unsigned left = pregs.window_two_left, right = pregs.window_two_right;
    memset(output, clr, 256);
    if(left <= right && right < 256) memset(output + left, set, right - left + 1);
    return;
  }

  for(unsigned x = 0; x < 256; x++) {
    bool one_mask = (x >= pregs.window_one_left && x <= pregs.window_one_right) ^ one_invert_;
    bool two_mask = (x >= pregs.window_two_left && x <= pregs.window_two_right) ^ two_invert_;
    switch(mask_) {
      case 0: output[x] =  (one_mask | two_mask) ? set : clr; break;
      case 1: output[x] =  (one_mask & two_mask) ? set : clr; break;
      case 2: output[x] =  (one_mask ^ two_mask) ? set : clr; break;
      case 3: output[x] = !(one_mask ^ two_mask) ? set : clr; break;
    }
  }
}

#endif
