// Portable libretro benchmark. No display, audio device, affinity or model tuning.
// c++ -O2 -std=c++11 -I. tests/core_benchmark.cpp -ldl -o core-benchmark
// core-benchmark CORE ROM [thread=disabled] [profile=full] [frames=600]
#include "target-libretro/libretro.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <vector>

static const char* thread_mode = "disabled";
static const char* profile = "full";
static bool checking;
static uint32_t video_hash, audio_hash, wram_hash;
static size_t audio_frames, video_frames, nonzero_samples;
static void hash_bytes(uint32_t& hash, const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for(size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
}
static bool environment(unsigned cmd, void* data) {
  if(cmd == RETRO_ENVIRONMENT_GET_VARIABLE) {
    retro_variable* v = static_cast<retro_variable*>(data);
    if(!strcmp(v->key, "bsnes_ppu_thread")) v->value = thread_mode;
    else if(!strcmp(v->key, "bsnes_speed_profile")) v->value = profile;
    else if(!strcmp(v->key, "bsnes_chip_hle")) v->value = "HLE";
    else if(!strcmp(v->key, "bsnes_violate_accuracy")) v->value = "enabled";
    else if(!strcmp(v->key, "bsnes_frameskip")) v->value = "0";
    else if(!strcmp(v->key, "bsnes_region")) v->value = "auto";
    else { v->value = nullptr; return false; }
    return true;
  }
  if(cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) {
    *static_cast<bool*>(data) = false;
    return true;
  }
  if(cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
    return *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
  return cmd == RETRO_ENVIRONMENT_SET_VARIABLES ||
         cmd == RETRO_ENVIRONMENT_SET_CONTROLLER_INFO ||
         cmd == RETRO_ENVIRONMENT_SET_GEOMETRY ||
         cmd == 42; // SET_SUPPORT_ACHIEVEMENTS (newer than the bundled header)
}
static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
  if(!checking) return;
  video_frames++;
  if(data) for(unsigned y = 0; y < height; y++)
    hash_bytes(video_hash, static_cast<const uint8_t*>(data) + y * pitch, width * 4);
}
static size_t audio(const int16_t* data, size_t frames) {
  if(checking) {
    audio_frames += frames;
    hash_bytes(audio_hash, data, frames * 4);
    for(size_t i = 0; i < frames * 2; i++) nonzero_samples += data[i] != 0;
  }
  return frames;
}
static void poll() {}
static int16_t input(unsigned, unsigned, unsigned, unsigned) { return 0; }

int main(int argc, char** argv) {
  if(argc < 3) { fprintf(stderr, "Usage: %s CORE ROM [thread] [profile] [frames]\n", argv[0]); return 2; }
  if(argc > 3) thread_mode = argv[3];
  if(argc > 4) profile = argv[4];
  unsigned frames = argc > 5 ? strtoul(argv[5], nullptr, 10) : 600;
  if(!frames) return 2;
  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if(!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define API(name) auto name = reinterpret_cast<decltype(&::name)>(dlsym(lib, #name)); \
  if(!name) { fprintf(stderr, "Missing %s\n", #name); return 1; }
  API(retro_set_environment); API(retro_set_video_refresh); API(retro_set_audio_sample_batch);
  API(retro_set_input_poll); API(retro_set_input_state); API(retro_init); API(retro_load_game);
  API(retro_run); API(retro_get_memory_data); API(retro_get_memory_size);
  API(retro_serialize_size); API(retro_serialize); API(retro_unserialize);
  API(retro_unload_game); API(retro_deinit);
#undef API
  retro_set_environment(environment); retro_set_video_refresh(video);
  retro_set_audio_sample_batch(audio); retro_set_input_poll(poll); retro_set_input_state(input);
  std::ifstream file(argv[2], std::ios::binary);
  std::vector<char> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if(rom.empty()) { fprintf(stderr, "Cannot read ROM\n"); return 1; }
  retro_game_info game = {argv[2], rom.data(), rom.size(), nullptr};
  retro_init();
  if(!retro_load_game(&game)) return 1;
  for(unsigned i = 0; i < 600; i++) retro_run();
  std::vector<double> times;
  double total = 0;
  for(unsigned i = 0; i < frames; i++) {
    auto start = std::chrono::steady_clock::now();
    retro_run();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    times.push_back(ms); total += ms;
  }
  std::sort(times.begin(), times.end());
  std::vector<char> state(retro_serialize_size());
  if(!retro_serialize(state.data(), state.size())) return 1;
  uint32_t expected_video = 0, expected_audio = 0, expected_wram = 0;
  size_t expected_frames = 0;
  for(unsigned pass = 0; pass < 2; pass++) {
    if(!retro_unserialize(state.data(), state.size())) return 1;
    video_hash = audio_hash = wram_hash = 2166136261u;
    video_frames = audio_frames = nonzero_samples = 0;
    checking = true;
    for(unsigned i = 0; i < 120; i++) retro_run();
    checking = false;
    hash_bytes(wram_hash, retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM),
               retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
    if(pass && (video_hash != expected_video || audio_hash != expected_audio ||
                wram_hash != expected_wram || audio_frames != expected_frames)) {
      fprintf(stderr, "Save-state continuation differs\n"); return 1;
    }
    expected_video = video_hash; expected_audio = audio_hash;
    expected_wram = wram_hash; expected_frames = audio_frames;
  }
  printf("{\"mean_ms\":%.6f,\"median_ms\":%.6f,\"p95_ms\":%.6f,"
         "\"frames\":%u,\"video\":\"%08x\",\"audio\":\"%08x\",\"wram\":\"%08x\","
         "\"audio_frames\":%zu,\"video_frames\":%zu,\"nonzero_samples\":%zu,\"state_size\":%zu}\n",
         total / frames, times[frames / 2], times[frames * 95 / 100], frames,
         video_hash, audio_hash, wram_hash, audio_frames, video_frames, nonzero_samples, state.size());
  retro_unload_game(); retro_deinit(); dlclose(lib);
}
