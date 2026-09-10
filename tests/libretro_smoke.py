#!/usr/bin/env python3
"""Exercise the public libretro ABI with a generated, redistributable test ROM."""
import ctypes as C
import hashlib
import json
import sys
import threading

core = C.CDLL(sys.argv[1])
thread_mode = sys.argv[2] if len(sys.argv) > 2 else 'enabled'
class Variable(C.Structure):
    _fields_ = [('key', C.c_char_p), ('value', C.c_char_p)]
class Game(C.Structure):
    _fields_ = [('path', C.c_char_p), ('data', C.c_void_p), ('size', C.c_size_t), ('meta', C.c_char_p)]
class Descriptor(C.Structure):
    _fields_ = [('flags', C.c_uint64), ('ptr', C.c_void_p), ('offset', C.c_size_t),
                ('start', C.c_size_t), ('select', C.c_size_t), ('disconnect', C.c_size_t),
                ('len', C.c_size_t), ('addrspace', C.c_char_p)]
class MemoryMap(C.Structure):
    _fields_ = [('descriptors', C.POINTER(Descriptor)), ('count', C.c_uint)]
options = {b'bsnes_speed_profile': b'full', b'bsnes_ppu_thread': thread_mode.encode(),
           b'bsnes_frameskip': b'0', b'bsnes_region': b'auto'}
maps = []
video_hash = hashlib.sha256()
audio_hash = hashlib.sha256()
video_frames = 0
audio_frames = 0
@C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
def environment(cmd, data):
    if cmd == 15:
        v = C.cast(data, C.POINTER(Variable)).contents
        v.value = options.get(v.key)
        return v.value is not None
    if cmd == 17:
        C.cast(data, C.POINTER(C.c_bool))[0] = False
        return True
    if cmd == 10:
        return C.cast(data, C.POINTER(C.c_int))[0] == 1  # XRGB8888
    if cmd == 36 | 0x10000:
        m = C.cast(data, C.POINTER(MemoryMap)).contents
        maps[:] = [(m.descriptors[i].start, m.descriptors[i].len) for i in range(m.count)]
        return True
    if cmd in (16, 35, 37, 42):
        return True
    return False
@C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
def video(data, width, height, pitch):
    global video_frames
    assert data
    video_frames += 1
    for y in range(height):
        video_hash.update(C.string_at(data + y * pitch, width * 4))
@C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
def audio(data, frames):
    global audio_frames
    audio_frames += frames
    audio_hash.update(C.string_at(data, frames * 4))
    return frames
@C.CFUNCTYPE(None)
def poll():
    pass
@C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
def input_state(port, device, index, key):
    return int(port == 0 and key == 0)
for name, callback in [('environment', environment), ('video_refresh', video),
                       ('audio_sample_batch', audio), ('input_poll', poll), ('input_state', input_state)]:
    getattr(core, 'retro_set_' + name)(callback)
core.retro_load_game.argtypes = [C.POINTER(Game)]
core.retro_load_game.restype = C.c_bool
core.retro_get_memory_data.argtypes = [C.c_uint]
core.retro_get_memory_data.restype = C.c_void_p
core.retro_get_memory_size.argtypes = [C.c_uint]
core.retro_get_memory_size.restype = C.c_size_t
core.retro_serialize_size.restype = C.c_size_t
for name in ('retro_serialize', 'retro_unserialize'):
    getattr(core, name).argtypes = [C.c_void_p, C.c_size_t]
    getattr(core, name).restype = C.c_bool
# Emulation-mode 65816: enable display and auto joypad polling; wait for each
# vblank, increment WRAM, and copy the controller result into WRAM.
code = bytes.fromhex('78 d8 a9 80 8d 00 21 9c 21 21 a9 1f 8d 22 21 9c 22 21 a9 0f 8d 00 21 a9 01 8d 00 42')
loop = 0x8000 + len(code)
code += bytes.fromhex('ad 12 42 30 fb ad 12 42 10 fb ee 00 00 ad 18 42 8d 02 00')
code += bytes([0x4c, loop & 255, loop >> 8])
rom = bytearray(32768)
rom[:len(code)] = code
rom[0x7fc0:0x7fd5] = b'YAGE ABI SMOKE TEST'.ljust(21)
rom[0x7fd5:0x7fdc] = bytes([0x20, 0x02, 5, 2, 1, 0x33, 0])
rom[0x7fdc:0x7fe0] = bytes([0xff, 0xff, 0, 0])
rom[0x7ffc:0x7ffe] = bytes([0, 0x80])
assert len(rom) == 32768
buf = C.create_string_buffer(bytes(rom))
core.retro_init()
assert core.retro_load_game(C.byref(Game(b'smoke.sfc', C.addressof(buf), len(rom), None)))
assert core.retro_get_memory_size(2) == 131072
assert core.retro_get_memory_size(0) == 4096
assert (0x1000000, 4096) in maps, 'RetroAchievements SRAM mapping missing'
for _ in range(90):
    core.retro_run()
size = core.retro_serialize_size()
state = C.create_string_buffer(size)
assert core.retro_serialize(state, size)
# Save-state continuation must survive a restore while the worker/cache is live.
def continuation():
    global video_hash, audio_hash, video_frames, audio_frames
    video_hash = hashlib.sha256(); audio_hash = hashlib.sha256()
    video_frames = audio_frames = 0
    for _ in range(60):
        core.retro_run()
    return {'video': video_hash.hexdigest(), 'audio': audio_hash.hexdigest(),
            'video_frames': video_frames, 'audio_frames': audio_frames,
            'wram': hashlib.sha256(C.string_at(core.retro_get_memory_data(2), 131072)).hexdigest()}
assert core.retro_unserialize(state, size)
a = continuation()
assert core.retro_unserialize(state, size)
b = continuation()
assert a == b, (a, b)
assert a['video_frames'] == 60
assert 31000 < a['audio_frames'] < 33000
# YAGE stops and recreates its frame-loop thread around save-state operations.
assert core.retro_unserialize(state, size)
thread_result = []
worker = threading.Thread(target=lambda: thread_result.append(continuation()))
worker.start(); worker.join()
assert thread_result == [a]
assert core.retro_serialize(state, size)
print(json.dumps(dict(a, state_size=size, memory_maps=maps), sort_keys=True))
core.retro_unload_game()
core.retro_deinit()
