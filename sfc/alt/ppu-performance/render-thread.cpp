#ifdef PPU_CPP

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#endif

void PPU::snap_layer_window(LayerWindowSnap& d, const LayerWindow& s) {
  d.one_enable = s.one_enable;
  d.one_invert = s.one_invert;
  d.two_enable = s.two_enable;
  d.two_invert = s.two_invert;
  d.mask = s.mask;
  d.main_enable = s.main_enable;
  d.sub_enable = s.sub_enable;
}

void PPU::snap_color_window(ColorWindowSnap& d, const ColorWindow& s) {
  d.one_enable = s.one_enable;
  d.one_invert = s.one_invert;
  d.two_enable = s.two_enable;
  d.two_invert = s.two_invert;
  d.mask = s.mask;
  d.main_mask = s.main_mask;
  d.sub_mask = s.sub_mask;
}

void PPU::snap_background(BgSnap& d, const Background& bg) {
  d.regs = bg.regs;
  d.hires = bg.hires;
  d.width = bg.width;
  d.tile_width = bg.tile_width;
  d.tile_height = bg.tile_height;
  d.mask_x = bg.mask_x;
  d.mask_y = bg.mask_y;
  d.scx = bg.scx;
  d.scy = bg.scy;
  d.mosaic_voffset = bg.mosaic_voffset;
  d.priority0_enable = bg.priority0_enable;
  d.priority1_enable = bg.priority1_enable;
  snap_layer_window(d.window, bg.window);
}

bool PPU::render_thread_active() const {
  return render_thread_running;
}

int PPU::render_thread_cpu() const {
  return render_thread_affinity_cpu;
}

bool PPU::pin_worker_off_caller() {
  unsigned cores = std::thread::hardware_concurrency();
  if(cores < 2 || !render_thread.joinable()) {
    render_thread_affinity_cpu = -1;
    return false;
  }

#if defined(__linux__)
  int caller = sched_getcpu();
  if(caller < 0) caller = 0;
  int worker = (caller + 1) % (int)cores;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(worker, &set);
#if defined(__ANDROID__)
  pid_t tid = pthread_gettid_np(render_thread.native_handle());
  if(tid <= 0 || sched_setaffinity((int)tid, sizeof(set), &set) != 0) {
    render_thread_affinity_cpu = -2;
    return false;
  }
#else
  if(pthread_setaffinity_np(render_thread.native_handle(), sizeof(set), &set) != 0) {
    render_thread_affinity_cpu = -2;
    return false;
  }
#endif
  render_thread_affinity_cpu = worker;
  return true;
#else
  render_thread_affinity_cpu = -1;
  return true;
#endif
}

void PPU::set_render_thread_mode(unsigned mode) {
  render_thread_mode = mode;
  bool want = false;
  if(mode == RenderThreadEnabled) want = true;
  else if(mode == RenderThreadDisabled) want = false;
  else {
    unsigned cores = std::thread::hardware_concurrency();
    want = cores > 1;
  }

  if(want) {
    start_render_thread();
    if(render_thread_running) {
      bool pinned = pin_worker_off_caller();
#if defined(__ANDROID__)
      if(mode == RenderThreadAuto && !pinned) stop_render_thread();
#else
      (void)pinned;
#endif
    }
  } else {
    stop_render_thread();
  }
}

void PPU::mark_vram_dirty() {
  vram_dirty = true;
}

uint8* PPU::acquire_vram_slot() {
  if(!vram_dirty && vram_slot_current >= 0) {
    std::lock_guard<std::mutex> lock(render_mutex);
    vram_slot_ref[vram_slot_current]++;
    return vram_slot[vram_slot_current];
  }

  int free_slot = -1;
  {
    std::lock_guard<std::mutex> lock(render_mutex);
    for(unsigned i = 0; i < VramSlots; i++) {
      if(vram_slot_ref[i] == 0) { free_slot = (int)i; break; }
    }
  }
  if(free_slot < 0) {
    drain_render();
    for(unsigned i = 0; i < VramSlots; i++) {
      if(vram_slot_ref[i] == 0) { free_slot = (int)i; break; }
    }
  }
  if(free_slot < 0) free_slot = 0;

  memcpy(vram_slot[free_slot], vram, 64 * 1024);
  vram_dirty = false;
  vram_slot_current = free_slot;
  {
    std::lock_guard<std::mutex> lock(render_mutex);
    vram_slot_ref[free_slot]++;
  }
  vram_gen++;
  return vram_slot[free_slot];
}

void PPU::release_vram_slot(unsigned slot) {
  std::lock_guard<std::mutex> lock(render_mutex);
  if(slot < VramSlots && vram_slot_ref[slot]) vram_slot_ref[slot]--;
}

//Only one tile cache is ever live: the worker renders from worker_cache while the render
//thread runs, and render_scanline_inline() uses cache when it does not. The idle one drops
//its 448KB of tiledata. tilevalid (7KB) stays allocated in both, because MMIO VRAM writes
//and PPU::Cache::serialize poke it without knowing which renderer is active.
void PPU::sync_cache_allocation() {
  if(render_thread_running) {
    worker_cache.allocate();
    worker_cache_gen = ~0u;
    cache.release();
  } else {
    cache.allocate();
    worker_cache.release();
  }
}

void PPU::start_render_thread() {
  if(render_thread_running) return;
  render_thread_stop = false;
  job_read = job_write = job_count = 0;
  render_thread_affinity_cpu = -1;
  try {
    render_thread = std::thread(&PPU::worker_loop, this);
    render_thread_running = true;
  } catch(...) {
    render_thread_running = false;
    render_thread_affinity_cpu = -1;
  }
  sync_cache_allocation();
}

void PPU::stop_render_thread() {
  if(!render_thread_running) return;
  {
    std::lock_guard<std::mutex> lock(render_mutex);
    render_thread_stop = true;
  }
  render_cv_fill.notify_all();
  if(render_thread.joinable()) render_thread.join();
  render_thread_running = false;
  render_src = nullptr;
  job_read = job_write = job_count = 0;
  for(unsigned i = 0; i < VramSlots; i++) vram_slot_ref[i] = 0;
  sync_cache_allocation();
}

void PPU::drain_render() {
  if(!render_thread_running) return;
  std::unique_lock<std::mutex> lock(render_mutex);
  while(job_count) render_cv_empty.wait(lock);
}

//Block until jobs[job_write] is free. That slot sits outside the [job_read, job_read +
//job_count) window the worker owns, so the caller can then fill it in place without the
//lock; only the index handoff in publish_line_job() is synchronized.
void PPU::wait_for_job_slot() {
  std::unique_lock<std::mutex> lock(render_mutex);
  while(job_count == JobSlots) render_cv_empty.wait(lock);
}

void PPU::publish_line_job() {
  {
    std::lock_guard<std::mutex> lock(render_mutex);
    jobs[job_write].occupied = true;
    job_write = (job_write + 1) % JobSlots;
    job_count++;
  }
  render_cv_fill.notify_one();
}

void PPU::worker_loop() {
  while(true) {
    unsigned slot;
    {
      std::unique_lock<std::mutex> lock(render_mutex);
      while(!job_count && !render_thread_stop) render_cv_fill.wait(lock);
      if(!job_count && render_thread_stop) return;
      slot = job_read;
    }

    //Rendered in place: the slot stays counted in job_count until the line is finished,
    //which is what keeps the producer from overwriting it.
    LineJob& job = jobs[slot];
    apply_line_job(job);
    release_vram_slot(job.vram_slot);

    {
      std::lock_guard<std::mutex> lock(render_mutex);
      job.occupied = false;
      job_read = (job_read + 1) % JobSlots;
      job_count--;
    }
    //notify_all because both wait_for_job_slot() and drain_render() wait here on
    //different predicates.
    render_cv_empty.notify_all();
  }
}

//Every field below is assigned unconditionally, so there is nothing to pre-clear; the
//caller passes the ring slot itself, which the worker then renders in place.
void PPU::capture_line_job(LineJob& job) {
  job.vram = acquire_vram_slot();
  job.vram_slot = (unsigned)vram_slot_current;
  job.vram_gen = vram_gen;
  memcpy(job.cgram, cgram, 512);

  job.vcounter = vcounter();
  job.field = field();
  job.interlace = interlace();
  job.display_width = display.width;
  job.display_disable = regs.display_disable;
  job.regs = regs;

  snap_background(job.bg[0], bg1);
  snap_background(job.bg[1], bg2);
  snap_background(job.bg[2], bg3);
  snap_background(job.bg[3], bg4);

  job.sprite_regs = sprite.regs;
  memcpy(job.tilelist, sprite.tilelist, sizeof(job.tilelist));
  job.sprite_priority0_enable = sprite.priority0_enable;
  job.sprite_priority1_enable = sprite.priority1_enable;
  job.sprite_priority2_enable = sprite.priority2_enable;
  job.sprite_priority3_enable = sprite.priority3_enable;
  snap_layer_window(job.sprite_window, sprite.window);

  job.screen_regs = screen.regs;
  snap_color_window(job.color_window, screen.window);
}

void PPU::apply_line_job(LineJob& job) {
  render_src = &job;
  if(job.vram_gen != worker_cache_gen) {
    worker_cache.invalidate();
    worker_cache_gen = job.vram_gen;
  }
  if(job.display_disable) {
    screen.render_black();
    render_src = nullptr;
    return;
  }
  screen.scanline();
  bg1.render();
  bg2.render();
  bg3.render();
  bg4.render();
  sprite.plot();
  screen.render();
  render_src = nullptr;
}

#endif
