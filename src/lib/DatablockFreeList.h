#ifndef SIGSEGV_DATABLOCKFREELIST_H
#define SIGSEGV_DATABLOCKFREELIST_H

#include <cstdint>

struct DatablockNode {
  uint64_t next_block = 0;
  uint64_t prev_block = 0;
  uint64_t free_blocks[(4096 / sizeof(uint64_t)) - 2];
};

#endif
