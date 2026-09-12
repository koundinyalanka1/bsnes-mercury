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
  // Use the process leader's allowed CPUs: the frontend may have narrowed
  // only its emulation thread's affinity. Let the OS choose among the other
  // eligible CPUs instead of guessing that caller+1 is a suitable worker.
  cpu_set_t set;
  CPU_ZERO(&set);
  if(sched_getaffinity(getpid(), sizeof(set), &set) != 0) {
    render_thread_affinity_cpu = -2;
    return false;
  }
  int caller = sched_getcpu();
  if(caller >= 0 && caller < CPU_SETSIZE && CPU_COUNT(&set) > 1)
    CPU_CLR(caller, &set);
  if(CPU_COUNT(&set) == 0) {
    render_thread_affinity_cpu = -2;
    return false;
  }
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
  // -3 denotes an OS-scheduled set, rather than one fixed CPU.
  render_thread_affinity_cpu = -3;
  if(CPU_COUNT(&set) == 1)
    for(int cpu = 0; cpu < CPU_SETSIZE; cpu++)
      if(CPU_ISSET(cpu, &set)) { render_thread_affinity_cpu = cpu; break; }
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
  memset(vram_dirty_tiles, 0xff, sizeof(vram_dirty_tiles));
}

void PPU::mark_vram_dirty(unsigned addr) {
  vram_dirty = true;
  const unsigned tile = addr >> 4;
  vram_dirty_tiles[tile >> 5] |= uint32(1) << (tile & 31);
}

uint8* PPU::acquire_vram_slot() {
  if(!vram_dirty && vram_slot_current >= 0) {
    vram_slot_ref[vram_slot_current].fetch_add(1, std::memory_order_relaxed);
    return vram_slot[vram_slot_current];
  }

  //The worker's release of a slot is ordered after its last read of that slot, so
  //seeing zero here is enough to reuse the buffer.
  int free_slot = -1;
  for(unsigned i = 0; i < VramSlots; i++) {
    if(vram_slot_ref[i].load(std::memory_order_acquire) == 0) { free_slot = (int)i; break; }
  }
  if(free_slot < 0) {
    drain_render();
    for(unsigned i = 0; i < VramSlots; i++) {
      if(vram_slot_ref[i].load(std::memory_order_acquire) == 0) { free_slot = (int)i; break; }
    }
  }
  if(free_slot < 0) free_slot = 0;

  memcpy(vram_slot[free_slot], vram, 64 * 1024);
  memcpy(vram_slot_dirty_tiles[free_slot], vram_dirty_tiles, sizeof(vram_dirty_tiles));
  memset(vram_dirty_tiles, 0, sizeof(vram_dirty_tiles));
  vram_dirty = false;
  vram_slot_current = free_slot;
  vram_slot_ref[free_slot].fetch_add(1, std::memory_order_relaxed);
  vram_gen++;
  return vram_slot[free_slot];
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
  job_read = job_write = 0;
  job_count.store(0, std::memory_order_relaxed);
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
  job_read = job_write = 0;
  job_count.store(0, std::memory_order_relaxed);
  for(unsigned i = 0; i < VramSlots; i++) vram_slot_ref[i].store(0, std::memory_order_relaxed);
  sync_cache_allocation();
}

void PPU::drain_render() {
  if(!render_thread_running) return;
  std::unique_lock<std::mutex> lock(render_mutex);
  while(job_count.load(std::memory_order_relaxed)) render_cv_empty.wait(lock);
}

//Block until the next fill slot is free. Slots the worker owns are [job_read,
//job_read + job_count); the ones already buffered here are the job_pending that
//follow, so both count as taken. The caller then fills its slot without the lock;
//only the index handoff in publish_pending_jobs() is synchronized.
void PPU::wait_for_job_slot() {
  if(job_count.load(std::memory_order_relaxed) < JobSlots) return;
  std::unique_lock<std::mutex> lock(render_mutex);
  while(job_count.load(std::memory_order_relaxed) == JobSlots) render_cv_empty.wait(lock);
}

//Hands the filled slot over. Holding scanlines back to amortize this lock starves
//the worker for longer than the lock costs, so each one is published as it lands.
void PPU::publish_line_job() {
  jobs[job_write].occupied = true;
  job_write = (job_write + 1) % JobSlots;
  bool wake_worker;
  {
    std::lock_guard<std::mutex> lock(render_mutex);
    wake_worker = job_count.load(std::memory_order_relaxed) == 0;
    job_count.fetch_add(1, std::memory_order_relaxed);
  }
  if(wake_worker) render_cv_fill.notify_one();
}

void PPU::worker_loop() {
  while(true) {
    unsigned slot, count;
    {
      std::unique_lock<std::mutex> lock(render_mutex);
      while(!job_count.load(std::memory_order_relaxed) && !render_thread_stop)
        render_cv_fill.wait(lock);
      count = job_count.load(std::memory_order_relaxed);
      if(!count && render_thread_stop) return;
      slot = job_read;
    }

    // Render the published batch in place. Its slots and VRAM references remain
    // owned by the worker until the completion handoff below.
    for(unsigned i = 0; i < count; i++)
      apply_line_job(jobs[(slot + i) % JobSlots]);

    //Release the snapshots before the queue slots: a producer that then sees a
    //slot free must also see this thread's last read of its buffer.
    for(unsigned i = 0; i < count; i++) {
      LineJob& job = jobs[(slot + i) % JobSlots];
      job.occupied = false;
      vram_slot_ref[job.vram_slot].fetch_sub(1, std::memory_order_release);
    }

    bool wake_caller;
    {
      std::lock_guard<std::mutex> lock(render_mutex);
      unsigned live = job_count.load(std::memory_order_relaxed);
      wake_caller = live == JobSlots || live == count;
      job_read = (job_read + count) % JobSlots;
      job_count.store(live - count, std::memory_order_relaxed);
    }
    // Wake a full-queue producer or a drain waiter only when it can progress.
    if(wake_caller) render_cv_empty.notify_one();
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
    if(job.vram_gen != worker_cache_gen + 1) {
      worker_cache.invalidate();
    } else {
      const uint32* dirty = vram_slot_dirty_tiles[job.vram_slot];
      for(unsigned word = 0; word < 128; word++) {
        uint32 bits = dirty[word];
        for(unsigned bit = 0; bits; bit++, bits >>= 1) {
          if(!(bits & 1)) continue;
          const unsigned tile = word * 32 + bit;
          worker_cache.tilevalid[0][tile] = 0;
          worker_cache.tilevalid[1][tile >> 1] = 0;
          worker_cache.tilevalid[2][tile >> 2] = 0;
        }
      }
    }
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
