struct LayerWindowSnap {
  bool one_enable;
  bool one_invert;
  bool two_enable;
  bool two_invert;
  unsigned mask;
  bool main_enable;
  bool sub_enable;
};

struct ColorWindowSnap {
  bool one_enable;
  bool one_invert;
  bool two_enable;
  bool two_invert;
  unsigned mask;
  unsigned main_mask;
  unsigned sub_mask;
};

struct BgSnap {
  Background::Regs regs;
  bool hires;
  signed width;
  unsigned tile_width;
  unsigned tile_height;
  unsigned mask_x;
  unsigned mask_y;
  unsigned scx;
  unsigned scy;
  unsigned mosaic_voffset;
  bool priority0_enable;
  bool priority1_enable;
  LayerWindowSnap window;
};

struct LineJob {
  bool occupied;
  uint8* vram;
  unsigned vram_slot;
  unsigned vram_gen;
  alignas(uint16) uint8 cgram[512];

  unsigned vcounter;
  bool field;
  bool interlace;
  unsigned display_width;
  bool display_disable;

  Regs regs;
  BgSnap bg[4];

  Sprite::Regs sprite_regs;
  Sprite::TileList tilelist[34];
  bool sprite_priority0_enable;
  bool sprite_priority1_enable;
  bool sprite_priority2_enable;
  bool sprite_priority3_enable;
  LayerWindowSnap sprite_window;

  Screen::Regs screen_regs;
  ColorWindowSnap color_window;
};

enum RenderThreadMode : unsigned {
  RenderThreadAuto = 0,
  RenderThreadEnabled = 1,
  RenderThreadDisabled = 2
};

bool ppu_fast_paths;
LineJob* render_src;
unsigned render_thread_mode;
bool render_thread_running;
bool render_thread_stop;
unsigned worker_cache_gen;

// Buffer enough scanlines to amortize worker wakeups without buffering a frame.
enum : unsigned { VramSlots = 3, JobSlots = 16 };
// One bit per 16-byte tile block. Snapshots carry changes since the previous
// generation so unchanged decoded tiles survive small VRAM uploads.
uint32 vram_dirty_tiles[128];
uint32 vram_slot_dirty_tiles[VramSlots][128];
uint8* vram_slot[VramSlots];
unsigned vram_slot_ref[VramSlots];
int vram_slot_current;
bool vram_dirty;
unsigned vram_gen;

LineJob jobs[JobSlots];
unsigned job_read;
unsigned job_write;
//Counts jobs published but not yet finished rendering, so it serves as both the
//queue-full predicate and the drain predicate.
unsigned job_count;

std::thread render_thread;
std::mutex render_mutex;
std::condition_variable render_cv_fill;
std::condition_variable render_cv_empty;
int render_thread_affinity_cpu;
