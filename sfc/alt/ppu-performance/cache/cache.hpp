struct Cache {
  uint8* tiledata[3];
  uint8* tilevalid[3];

  uint8* tile_2bpp(unsigned tile);
  uint8* tile_4bpp(unsigned tile);
  uint8* tile_8bpp(unsigned tile);
  uint8* tile(unsigned bpp, unsigned tile);

  void serialize(serializer&);
  void invalidate();
  //tiledata is claimed only by whichever renderer is live; see PPU::sync_cache_allocation.
  //tilevalid is always allocated, so MMIO invalidation stays branch-free.
  void allocate();
  void release();
  Cache(PPU& self);
  ~Cache();

  PPU& self;
  friend class PPU;
};
