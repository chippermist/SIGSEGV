#include "MemoryStorage.h"

#include <cstring>
#include <stdexcept>

MemoryStorage::MemoryStorage(uint64_t nblocks) {
  data = new char[nblocks * Block::BLOCK_SIZE];
  size = nblocks;
}

MemoryStorage::~MemoryStorage() {
  delete [] data;
}

void MemoryStorage::get(Block::ID id, Block& dst) {
  if(id >= size) {
    throw std::length_error("Block read out of range.");
  }

  const char* src = &data[id * Block::BLOCK_SIZE];
  std::memcpy(dst.data, src, Block::BLOCK_SIZE);
}

void MemoryStorage::set(Block::ID id, const Block& src) {
  if(id >= size) {
    throw std::length_error("Block write out of range.");
  }

  char* dst = &data[id * Block::BLOCK_SIZE];
  std::memcpy(dst, src.data, Block::BLOCK_SIZE);
}
