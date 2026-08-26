struct PPU : Thread, public PPUcounter {
  uint8 vram[64 * 1024];
  uint8 oam[544];
  uint8 cgram[512];

  enum : bool { Threaded = true };
  alwaysinline void step(unsigned clocks);
  alwaysinline void synchronize_cpu();

  void latch_counters();
  bool interlace() const;
  bool overscan() const;
  bool hires() const;

  void enter();
  void enable();
  void power();
  void reset();
  void scanline();
  void frame();

  void layer_enable(unsigned layer, unsigned priority, bool enable);
  void set_frameskip(unsigned frameskip);
  void set_render_thread_mode(unsigned mode);
  void drain_render();
  bool render_thread_active() const;
  void set_ppu_fast(bool enable) { ppu_fast_paths = enable; }
  uint32 framebuffer_hash() const;

  void serialize(serializer&);
  PPU();
  ~PPU();

private:
  uint32* surface;
  uint32* output;

  #include "mmio/mmio.hpp"
  #include "window/window.hpp"
  #include "cache/cache.hpp"
  #include "background/background.hpp"
  #include "sprite/sprite.hpp"
  #include "screen/screen.hpp"

  Cache cache;
  Cache worker_cache;
  Background bg1;
  Background bg2;
  Background bg3;
  Background bg4;
  Sprite sprite;
  Screen screen;

  #include "render-thread.hpp"

  struct Display {
    bool interlace;
    bool overscan;
    unsigned width;
    unsigned height;
    unsigned frameskip;
    unsigned framecounter;
  } display;

  static void Enter();
  void add_clocks(unsigned clocks);
  void render_scanline();
  void render_scanline_inline();
  void capture_line_job(LineJob&);
  void apply_line_job(LineJob&);
  void snap_layer_window(LayerWindowSnap&, const LayerWindow&);
  void snap_color_window(ColorWindowSnap&, const ColorWindow&);
  void snap_background(BgSnap&, const Background&);
  void mark_vram_dirty();
  uint8* acquire_vram_slot();
  void release_vram_slot(unsigned slot);
  void start_render_thread();
  void stop_render_thread();
  void enqueue_line_job(const LineJob&);
  void worker_loop();

  alwaysinline uint8* vram_data() { return render_src ? render_src->vram : vram; }
  alwaysinline const uint8* cgram_data() const { return render_src ? render_src->cgram : cgram; }
  alwaysinline const Regs& render_regs() const { return render_src ? render_src->regs : regs; }
  alwaysinline unsigned render_vcounter() const { return render_src ? render_src->vcounter : vcounter(); }
  alwaysinline bool render_field() const { return render_src ? render_src->field : field(); }
  alwaysinline bool render_interlace() const { return render_src ? render_src->interlace : interlace(); }
  Cache& render_cache() { return render_src ? worker_cache : cache; }

  alwaysinline bool can_skip_sub_screen() const {
    if(!ppu_fast_paths) return false;
    const auto& pregs = render_regs();
    if(pregs.pseudo_hires || pregs.bgmode == 5 || pregs.bgmode == 6) return false;
    const auto& sr = render_src ? render_src->screen_regs : screen.regs;
    for(unsigned n = 0; n < 7; n++) if(sr.color_enable[n]) return false;
    return true;
  }

  friend class PPU::Cache;
  friend class PPU::Background;
  friend class PPU::Sprite;
  friend class PPU::Screen;
  friend class Video;
};

extern PPU ppu;
