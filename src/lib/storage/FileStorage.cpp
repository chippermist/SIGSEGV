#include "FileStorage.h"

FileStorage::FileStorage(const char* filename, uint64_t nblocks): file(filename) {
  size = nblocks;
}

FileStorage::~FileStorage() {
  file.close();
}

void FileStorage::get(Block::ID id, Block& dst) {
  if(id >= size) {
    throw std::length_error("Block read out of range.");
  }

  file.seekg(id * Block::BLOCK_SIZE);
  file.read(dst.data, Block::BLOCK_SIZE);
}

void FileStorage::set(Block::ID id, const Block& src) {
  if(id >= size) {
    throw std::length_error("Block write out of range.");
  }

  file.seekp(id * Block::BLOCK_SIZE);
  file.write(src.data, Block::BLOCK_SIZE);
  file.flush();
}
