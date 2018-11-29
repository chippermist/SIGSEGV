#pragma once

#include <cstring>
#include <stdexcept>
#include <cassert>
#include "../Superblock.h"
#include "../INodeManager.h"
#include "../Storage.h"
#include "../Block.h"

class LinearINodeManager: public INodeManager {
public:
  LinearINodeManager(Storage& storage);
  ~LinearINodeManager();

  void mkfs();
  void statfs(struct statvfs* info);
  INode::ID getRoot();

  void get(INode::ID id, INode& dst);
  void set(INode::ID id, const INode& src);

  INode::ID reserve();
  void release(INode::ID id);

  void reconfigure();

private:
  static const uint64_t root = 1;

  Storage*  disk;
  uint64_t  num_inodes;
  Block::ID start_block;
  uint64_t  block_count;
};
