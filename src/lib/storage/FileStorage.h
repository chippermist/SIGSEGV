#pragma once

#include "../Storage.h"
#include <fstream>

class FileStorage: public Storage {
  std::fstream file;
  uint64_t     size;
public:
  FileStorage(const char* filename, uint64_t nblocks);
  ~FileStorage();

  void get(Block::ID id, Block& dst);
  void set(Block::ID id, const Block& src);
};
