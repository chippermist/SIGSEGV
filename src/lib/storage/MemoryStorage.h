#pragma once

#include "../Storage.h"

class MemoryStorage: public Storage {
  char*    data;
  uint64_t size;
public:
  MemoryStorage(uint64_t nblocks);
  ~MemoryStorage();

  void get(Block::ID id, Block& dst);
  void set(Block::ID id, const Block& src);
};
