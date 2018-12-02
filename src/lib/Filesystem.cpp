#include "Filesystem.h"
#include "FSExceptions.h"
#include "Superblock.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stack>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <unistd.h>

#if defined(__linux__)
  #include <sys/statfs.h>
  #include <sys/vfs.h>
  #include <sys/statvfs.h>
#endif

#include <fuse.h>


Filesystem::Filesystem(BlockManager& block_manager, INodeManager& inode_manager) {
  this->block_manager = &block_manager;
  this->inode_manager = &inode_manager;

  uint64_t ipb   = Block::SIZE / sizeof(Block::ID);
  max_file_size  = INode::DIRECT_POINTERS;
  max_file_size += INode::SINGLE_INDIRECT_POINTERS * ipb;
  max_file_size += INode::DOUBLE_INDIRECT_POINTERS * ipb * ipb;
  max_file_size += INode::TRIPLE_INDIRECT_POINTERS * ipb * ipb * ipb;
  max_file_size *= Block::SIZE;
}

Filesystem::~Filesystem() {
  // Nothing to do.
}

void Filesystem::mkfs(uint64_t nblocks, uint64_t niblocks) {
  Block block;
  Superblock* superblock = (Superblock*) block.data;
  std::memset(block.data, 0, Block::SIZE);

  superblock->magic       = 3199905246;
  superblock->block_size  = Block::SIZE;
  superblock->block_count = nblocks;

  superblock->inode_block_start = 1;
  superblock->inode_block_count = niblocks;
  superblock->data_block_start  = niblocks + 1;
  superblock->data_block_count  = nblocks - niblocks - 1;

  block_manager->set(0, block);
  inode_manager->mkfs();
  block_manager->mkfs();

  INode::ID id = inode_manager->getRoot();
  INode inode(FileType::DIRECTORY, 0777);
  if(uid_t uid = getuid()) {
    inode.uid = uid;
  }
  if(gid_t gid = getgid()) {
    inode.gid = gid;
  }
  inode.links = 2;
  save(id, inode);

  Directory root(id, id);
  save(root);
}

void Filesystem::statfs(struct statvfs* info) {
  Block block;
  Superblock* superblock = (Superblock*) block.data;
  block_manager->get(0, block);

  // Based on http://pubs.opengroup.org/onlinepubs/009604599/basedefs/sys/statvfs.h.html
  // Also see http://man7.org/linux/man-pages/man3/statvfs.3.html
  info->f_bsize   = superblock->block_size; // File system block size.
  info->f_frsize  = superblock->block_size; // Fundamental file system block size.
  info->f_fsid    = superblock->magic;      // File system ID.
  info->f_flag    = 0;                      // Bit mask of f_flag values.
  info->f_namemax = 256;                    // Maximum filename length.
}

int Filesystem::mount(char* program, fuse_operations* ops) {
  if(mount_point == NULL) {
    std::cerr << "No mount point given.\n";
    exit(1);
  }

  char s[] = "-s"; // Use a single thread.
  char d[] = "-d"; // Print debuging output.
  char f[] = "-f"; // Run in the foreground.
  char o[] = "-o"; // Other options
  char p[] = "default_permissions"; // Defer permissions checks to kernel
  char r[] = "allow_other"; // Allow all users to (try to) access files

  int argc = 0;
  char* argv[12] = {0};

  argv[argc++] = program;
  if(!parallel) argv[argc++] = s;
  if(debug)     argv[argc++] = d;
  argv[argc++] = f;
  argv[argc++] = mount_point;
  argv[argc++] = o;
  argv[argc++] = p;
  argv[argc++] = o;
  argv[argc++] = r;

  return fuse_main(argc, argv, ops, 0);
}

Directory Filesystem::getDirectory(INode::ID id) {
  INode inode;
  inode_manager->get(id, inode);
  if(inode.type != FileType::DIRECTORY) {
    throw NotADirectory();
  }

  char* buffer = new char[inode.size];
  read(id, buffer, inode.size, 0);

  Directory directory(id, buffer, inode.size);
  delete [] buffer;
  return directory;
}

Directory Filesystem::getDirectory(const std::string& path) {
  return getDirectory(getINodeID(path));
}

INode Filesystem::getINode(INode::ID id) {
  if(id == 0) throw NoSuchEntry();

  INode inode;
  inode_manager->get(id, inode);
  return inode;
}

INode Filesystem::getINode(const std::string& path) {
  return getINode(getINodeID(path));
}

INode::ID Filesystem::getINodeID(const std::string& path) {
  INode::ID id = inode_manager->getRoot();
  if(path == "/") return id;

  size_t start = 1;
  size_t found = 0;

  while(found != std::string::npos) {
    found = path.find('/', start);
    std::string component = path.substr(start, found - start);
    start = found + 1;

    Directory dir = getDirectory(id);
    id = dir.search(component);
  }

  return id;
}

INode::ID Filesystem::newINodeID() {
  return inode_manager->reserve();
}

void Filesystem::save(const Directory& directory) {
  std::vector<char> data = directory.serialize();
  write(directory.id(), &data[0], data.size(), 0);

  INode inode = getINode(directory.id());
  if(inode.size > data.size()) {
    truncate(directory.id(), data.size());
  }
}

void Filesystem::save(INode::ID id, const INode& inode) {
  inode_manager->set(id, inode);
}

/**
 * Writes size bytes from buf into a file, starting at the given offset.
 *
 * Inserts NULL filler if the starting offset is beyond the file's size,
 * and this affects the returned total bytes written.
 *
 * Returns -1 on error.
 * - No such file
 * - Gave name of directory
 * - TODO: Incorrect ownership/permissions
 */
int Filesystem::write(INode::ID file_inode_num, const char *buf, size_t size, size_t offset) {
  if(offset > max_file_size || size > max_file_size - offset) {
    throw FileTooBig();
  }

  // Read the file's inode and do some sanity checks
  INode file_inode;
  this->inode_manager->get(file_inode_num, file_inode);

  file_inode.mtime = time(NULL);
  file_inode.ctime = file_inode.mtime;
  file_inode.atime = file_inode.mtime;

  size_t total_written = 0;
  // 1. If we are overwriting any data in the file, do that first.
  while (offset < file_inode.size && size > 0) {

    // Read in block
    Block block;
    Block::ID block_num = blockAt(file_inode, offset);
    this->block_manager->get(block_num, block);

    /*
      How many bytes to write?
      a) Normally write whole blocks at a time.
      b) If the offset isn't block aligned though
         (which could happen for the first block),
         then only write until the end of the block.
      c) Don't write past the end of the file (handled in step 3).
      d) Of course, don't write more than what was asked!
    */
    size_t to_write = Block::SIZE - (offset % Block::SIZE);

    if (offset + to_write > file_inode.size) {
      to_write = file_inode.size - offset;
    }

    if (to_write > size) {
      to_write = size;
    }

    // Copy the data and write to disk
    memcpy(block.data + (offset % Block::SIZE), buf, to_write);
    this->block_manager->set(block_num, block);

    // Update offset, buf pointer, and num bytes left to write
    offset += to_write;
    buf += to_write;
    size -= to_write;

    total_written += to_write;
  }

  if (size == 0) {
    return total_written;
  }

  // 2. If the offset > file size, insert NULL filler.
  size_t null_filler = offset - file_inode.size;
  if (null_filler > 0) {
      total_written += appendData(file_inode, buf, null_filler, file_inode.size, true);
  }

  // 3. Write actual data from offset (which should be file size).
  total_written += appendData(file_inode, buf, size, offset, false);

  // 4. Write back changes to file_inode
  this->inode_manager->set(file_inode_num, file_inode);
  return total_written - null_filler;
}

/**
 * Appends data to the end of the file, either from buf or just NULL filler.
 * Automatically allocates 1 extra block each time a block is needed.
 *
 * Invariant upon entering the function:
 * - Offset == file size, since we are adding to the end of the file.
 */
size_t Filesystem::appendData(INode& file_inode, const char *buf, size_t size, size_t offset, bool null_filler) {

  assert(offset == file_inode.size);
  size_t total_written = 0;

  // 1. Fill in the last block already allocated if it has space.
  if (offset % Block::SIZE != 0) {
    Block block;
    Block::ID block_num = blockAt(file_inode, offset);
    this->block_manager->get(block_num, block);

    /*
      How many bytes to write?
      a) Write the remainder of the block.
      b) Of course, don't write more than what was asked!
    */
    size_t to_write = Block::SIZE - (offset % Block::SIZE);
    if (to_write > size) {
      to_write = size;
    }

    // Copy the data and write to disk
    if (null_filler) {
      memset(block.data + (offset % Block::SIZE), 0, to_write);
    } else {
      memcpy(block.data  + (offset % Block::SIZE), buf, to_write);
    }
    this->block_manager->set(block_num, block);

    // Update offset, buf pointer, and num bytes left to write
    offset += to_write;
    buf += to_write;
    size -= to_write;

    file_inode.size += to_write;
    total_written += to_write;
  }

  if (size == 0) {
    return total_written;
  }

  // 2. Need to allocate new blocks.
  while (size > 0) {

    // Should be block-aligned now
    assert(offset % Block::SIZE == 0);

    // Allocate the next data block
    Block::ID block_num = allocateNextBlock(file_inode);
    Block block;

    /*
      How many bytes to write?
      a) Write the whole block (offset should be block-aligned).
      b) Of course, don't write more than what was asked!
    */
    size_t to_write = Block::SIZE;
    if (to_write > size) {
      to_write = size;
    }

    // Copy the data and write to disk
    if (null_filler) {
      memset(block.data, 0, to_write);
    } else {
      memcpy(block.data, buf, to_write);
    }
    this->block_manager->set(block_num, block);

    // Update offset, buf pointer, and num bytes left to write
    offset += to_write;
    buf += to_write;
    size -= to_write;

    file_inode.size += to_write;
    total_written += to_write;
  }
  return total_written;
}

/**
 * Allocates a new data block for a file's inode.
 * Also allocates any needed new blocks for indirect pointers.
 */
Block::ID Filesystem::allocateNextBlock(INode& file_inode) {

  size_t scale = Block::SIZE / sizeof(Block::ID);
  size_t logical_blk_num = file_inode.blocks + 1;
  Block::ID data_block_num = 0;

  if (logical_blk_num <= INode::DIRECT_POINTERS) {

    // Direct block

    // 1. Just allocate in inode
    data_block_num = this->block_manager->reserve();
    file_inode.block_pointers[file_inode.blocks] = data_block_num;

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale) {

    // Single-indirect

    // 1. Check if need block for the direct pointers
    if (logical_blk_num == INode::DIRECT_POINTERS + 1) {
      file_inode.block_pointers[INode::DIRECT_POINTERS] = this->block_manager->reserve();
    }

    // 2. Load in first level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS], direct_ptrs_blk);

    // 3. Allocate the direct block
    logical_blk_num -= INode::DIRECT_POINTERS;
    data_block_num = this->block_manager->reserve();
    direct_ptrs[logical_blk_num - 1] = data_block_num;
    this->block_manager->set(file_inode.block_pointers[INode::DIRECT_POINTERS], direct_ptrs_blk);

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale + (scale * scale)) {

    // Double-indirect

    // 1. Check if need block for single-indirect pointers
    if (logical_blk_num == INode::DIRECT_POINTERS + scale + 1) {
      file_inode.block_pointers[INode::DIRECT_POINTERS + 1] = this->block_manager->reserve();
    }

    // 2. Load in first level block
    Block single_indirect_ptrs_blk;
    Block::ID *single_indirect_ptrs = (Block::ID *) &single_indirect_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS + 1], single_indirect_ptrs_blk);

    // 3. Check if need block for direct pointers
    Block::ID block_idx_in_level = logical_blk_num - INode::DIRECT_POINTERS - scale - 1;
    if (block_idx_in_level % scale == 0) {
      single_indirect_ptrs[block_idx_in_level / scale] = this->block_manager->reserve();
      this->block_manager->set(file_inode.block_pointers[INode::DIRECT_POINTERS + 1], single_indirect_ptrs_blk);
    }

    // 4. Load in second level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(single_indirect_ptrs[block_idx_in_level / scale], direct_ptrs_blk);

    // 5. Allocate the direct block
    data_block_num = this->block_manager->reserve();
    direct_ptrs[block_idx_in_level % scale] = data_block_num;
    this->block_manager->set(single_indirect_ptrs[block_idx_in_level / scale], direct_ptrs_blk);

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale + (scale * scale) + (scale * scale * scale)) {
    // Triple-indirect

    // 1. Check if need block for double-indirect pointers
    if (logical_blk_num == INode::DIRECT_POINTERS + scale + (scale * scale) + 1) {
      file_inode.block_pointers[INode::DIRECT_POINTERS + 2] = this->block_manager->reserve();
    }

    // 2. Load in first level block
    Block double_indirect_ptrs_blk;
    Block::ID *double_indirect_ptrs = (Block::ID *) &double_indirect_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS + 2], double_indirect_ptrs_blk);

    // 3. Check if need block for single-indirect pointers
    Block::ID block_idx_in_level = logical_blk_num - INode::DIRECT_POINTERS - scale - scale * scale - 1;
    if (block_idx_in_level % (scale * scale) == 0) {
      double_indirect_ptrs[block_idx_in_level / (scale * scale)] = this->block_manager->reserve();
      this->block_manager->set(file_inode.block_pointers[INode::DIRECT_POINTERS + 2], double_indirect_ptrs_blk);
    }

    // 4. Load in second level block
    Block single_indirect_ptrs_blk;
    Block::ID *single_indirect_ptrs = (Block::ID *) &single_indirect_ptrs_blk;
    this->block_manager->get(double_indirect_ptrs[block_idx_in_level / (scale * scale)], single_indirect_ptrs_blk);

    // 5. Check if need block for direct pointers
    Block::ID block_idx_in_level_two = block_idx_in_level % (scale * scale);
    if (block_idx_in_level_two % scale == 0) {
      single_indirect_ptrs[block_idx_in_level_two / scale] = this->block_manager->reserve();
      this->block_manager->set(double_indirect_ptrs[block_idx_in_level / (scale * scale)], single_indirect_ptrs_blk);
    }

    // 6. Load in third level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(single_indirect_ptrs[block_idx_in_level_two / scale], direct_ptrs_blk);

    // 7. Allocate direct block
    data_block_num = this->block_manager->reserve();
    direct_ptrs[block_idx_in_level_two % scale] = data_block_num;
    this->block_manager->set(single_indirect_ptrs[block_idx_in_level_two / scale], direct_ptrs_blk);

  } else {
    // Can't allocate any more blocks for this file!
    throw std::out_of_range("Reached max number of blocks allocated for a single file!");
  }

  // Update the number of allocated data blocks in this inode
  file_inode.blocks++;
  return data_block_num;
}

int Filesystem::read(INode::ID file_inode_num, char *buf, size_t size, size_t offset) {

  // Read the file's inode and do some sanity checks
  INode file_inode = getINode(file_inode_num);
  if (offset >= file_inode.size) {
    return 0; // Can't begin reading from after file
  }

  file_inode.atime = time(NULL);

  // Only read until the end of the file
  if (offset + size > file_inode.size) {
    size = file_inode.size - offset;
  }

  size_t total_read = 0;
  while (size > 0) {

    // Get datablock of current offset
    Block::ID cur_block_id = blockAt(file_inode, offset);
    Block block;
    this->block_manager->get(cur_block_id, block);

    /*
      How many bytes to read?
      a) Normally read whole blocks at a time.
      b) If the offset isn't block aligned though
         (which could happen for the first block),
         then only read until the end of the block.
      c) Don't read past the end of the file.
      d) Of course, don't read more than what was asked!
    */
    size_t to_read = Block::SIZE - (offset % Block::SIZE);

    if (offset + to_read > file_inode.size) {
      to_read = file_inode.size - offset;
    }

    if (to_read > size) {
      to_read = size;
    }

    // Copy data from block into buffer
    memcpy(buf, block.data + (offset % Block::SIZE), to_read);

    // Update offset, buf pointer, and num bytes left to read
    offset += to_read;
    buf += to_read;
    size -= to_read;

    total_read += to_read;
  }
  // Write back changes to file_inode
  this->inode_manager->set(file_inode_num, file_inode);
  return total_read;
}

Block::ID Filesystem::blockAt(const INode& inode, uint64_t offset) {
  // This function only gets existing blocks.
  // It might need reworking for writes.
  if (offset >= inode.blocks * Block::SIZE) {
    throw std::out_of_range("Offset greater than file size.");
  }

  if (offset < INode::DIRECT_POINTERS * Block::SIZE) {
    return inode.block_pointers[offset / Block::SIZE];
  }

  uint64_t size  = Block::SIZE;
  uint64_t scale = Block::SIZE / sizeof(Block::ID);
  offset -= INode::DIRECT_POINTERS * Block::SIZE;
  for (int i = 0; i < 3; ++i) {
    if (offset < size * scale) {
      Block::ID bid = inode.block_pointers[INode::DIRECT_POINTERS + i];
      return indirectBlockAt(bid, offset, size);
    }

    size   *= scale;
    offset -= size;
  }

  // If we get here, something has gone very wrong.
  throw std::out_of_range("Offset greater than maximum file size!");
}

Block::ID Filesystem::indirectBlockAt(Block::ID bid, uint64_t offset, uint64_t size) {
  Block block;
  this->block_manager->get(bid, block);
  Block::ID* refs = (Block::ID*) &block;

  uint64_t index = offset / size;
  if(size == Block::SIZE) {
    return refs[index];
  }

  uint64_t scale = Block::SIZE / sizeof(Block::ID);
  return indirectBlockAt(refs[index], offset % size, size / scale);
}

int Filesystem::truncate(INode::ID file_inode_num, size_t length) {
  if(length > max_file_size) {
    throw FileTooBig();
  }

  // Read the file's inode and do some sanity checks
  INode file_inode = getINode(file_inode_num);
  if (file_inode.size == length) {
    return 0;
  }

  file_inode.mtime = time(NULL);
  file_inode.ctime = file_inode.mtime;
  file_inode.atime = file_inode.mtime;

  // If increasing size, fill with NULL bytes
  if (length > file_inode.size) {
    appendData(file_inode, NULL, length - file_inode.size, file_inode.size, true);
    // Write back changes to file_inode
    this->inode_manager->set(file_inode_num, file_inode);
    return 0;
  } else {

    // Remove data from last block
    if (file_inode.size % Block::SIZE != 0) {

      // If removing remainder of last block is too much, just truncate to the desired length
      if (file_inode.size % Block::SIZE > file_inode.size - length) {
        file_inode.size = length;
        // Write back changes to file_inode
        this->inode_manager->set(file_inode_num, file_inode);
        return 0;
      }

      file_inode.size -= file_inode.size % Block::SIZE;
      deallocateLastBlock(file_inode);

      if (file_inode.size == length) {
        // Write back changes to file_inode
        this->inode_manager->set(file_inode_num, file_inode);
        return 0;
      }
    }

    // Remove other blocks
    assert(file_inode.size % Block::SIZE == 0);
    while (file_inode.size - Block::SIZE >= length && file_inode.size > 0) {
      deallocateLastBlock(file_inode);
      file_inode.size -= Block::SIZE;
    }

    // Remove remainder of first block, if any
    if (file_inode.size > length) {
      file_inode.size = length;
    }
    // Write back changes to file_inode
    this->inode_manager->set(file_inode_num, file_inode);
    return 0;
  }
}

std::string Filesystem::dirname(const char* path_cstring) {
  std::string path(path_cstring);
  // Remove all repeated /'s
  path.erase(std::unique(path.begin(), path.end(), [](char &a, char &b) {
    return a == '/' && b == '/';
  }), path.end());

  std::stack<std::string> folder_names;

  for(size_t i=0; i<path.size(); ++i) {
    if(i == 0 && path[i] == '/') {
      continue;
    }

    std::string str;
    while(path[i] != '/' && i < path.length()) {
      str += path[i++];
    }
    //cout << str << endl;
    if(str == ".") {
      continue;
    }
    else if (str == ".." && !folder_names.empty()) {
      folder_names.pop();
      continue;
    }
    else if (str == "..") {
      continue;
    }
    folder_names.push(str);
  }
  folder_names.pop(); // Removing the filename

  std::string dir_path;
  while(!folder_names.empty()) {
    dir_path = "/" + folder_names.top() + dir_path;
    folder_names.pop();
  }
  if (dir_path.length() == 0) {
    dir_path = "/";
  }
  return dir_path;
}

std::string Filesystem::basename(const char* path_cstring) {
  std::string path(path_cstring);
  size_t loc = path.find_last_of('/');
  if (loc != std::string::npos) {
    return (path.substr(loc + 1));
  }
  return "";
}


void Filesystem::deallocateLastBlock(INode& file_inode) {

  size_t scale = Block::SIZE / sizeof(Block::ID);
  size_t logical_blk_num = file_inode.blocks;

  if (logical_blk_num <= INode::DIRECT_POINTERS) {

    // Direct block

    // 1. Just deallocate in inode
    this->block_manager->release(file_inode.block_pointers[file_inode.blocks - 1]);

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale) {

    // Single-indirect

    // 1. Load in first level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS], direct_ptrs_blk);

    // 2. Dellocate the direct block
    this->block_manager->release(direct_ptrs[logical_blk_num - INode::DIRECT_POINTERS - 1]);

    // 3. If first block in first level, relesae the first level block as well
    if (logical_blk_num == INode::DIRECT_POINTERS + 1) {
      this->block_manager->release(file_inode.block_pointers[INode::DIRECT_POINTERS]);
    }

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale + (scale * scale)) {

    // Double-indirect

    // 1. Load in first level block
    Block single_indirect_ptrs_blk;
    Block::ID *single_indirect_ptrs = (Block::ID *) &single_indirect_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS + 1], single_indirect_ptrs_blk);

    Block::ID block_idx_in_level = logical_blk_num - INode::DIRECT_POINTERS - scale - 1;

    // 2. Load in second level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(single_indirect_ptrs[block_idx_in_level / scale], direct_ptrs_blk);

    // 3. Deallocate the direct block
    this->block_manager->release(direct_ptrs[block_idx_in_level % scale]);

    // 4. Check if first block in second level
    if (block_idx_in_level % scale == 0) {
      this->block_manager->release(single_indirect_ptrs[block_idx_in_level / scale]);
    }

    // 5. Check if first block in first level
    if (logical_blk_num == INode::DIRECT_POINTERS + scale + 1) {
      this->block_manager->release(file_inode.block_pointers[INode::DIRECT_POINTERS + 1]);
    }

  } else if (logical_blk_num <= INode::DIRECT_POINTERS + scale + (scale * scale) + (scale * scale * scale)) {
    // Triple-indirect

    // 1. Load in first level block
    Block double_indirect_ptrs_blk;
    Block::ID *double_indirect_ptrs = (Block::ID *) &double_indirect_ptrs_blk;
    this->block_manager->get(file_inode.block_pointers[INode::DIRECT_POINTERS + 2], double_indirect_ptrs_blk);

    Block::ID block_idx_in_level = logical_blk_num - INode::DIRECT_POINTERS - scale - scale * scale - 1;

    // 2. Load in second level block
    Block single_indirect_ptrs_blk;
    Block::ID *single_indirect_ptrs = (Block::ID *) &single_indirect_ptrs_blk;
    this->block_manager->get(double_indirect_ptrs[block_idx_in_level / (scale * scale)], single_indirect_ptrs_blk);

    Block::ID block_idx_in_level_two = block_idx_in_level % (scale * scale);

    // 3. Load in third level block
    Block direct_ptrs_blk;
    Block::ID *direct_ptrs = (Block::ID *) &direct_ptrs_blk;
    this->block_manager->get(single_indirect_ptrs[block_idx_in_level_two / scale], direct_ptrs_blk);

    // 4. Deallocate direct block
    this->block_manager->release(direct_ptrs[block_idx_in_level_two % scale]);

    // 5. Check if first block in third level
    if (block_idx_in_level_two % scale == 0) {
      this->block_manager->release(single_indirect_ptrs[block_idx_in_level_two / scale]);
    }

    // 6. Check if first block in second level
    if (block_idx_in_level % (scale * scale) == 0) {
      this->block_manager->release(double_indirect_ptrs[block_idx_in_level / (scale * scale)]);
    }

    // 7. Check if first block in first level
    if (logical_blk_num == INode::DIRECT_POINTERS + scale + (scale * scale) + 1) {
      this->block_manager->release(file_inode.block_pointers[INode::DIRECT_POINTERS + 2]);
    }

  } else {
    // Can't deallocate such a large block! Must be some kind of error in setting inode's blocks
    throw std::out_of_range("file_inode.blocks is too high; can't deallocate!");
  }

  // Update the number of allocated data blocks in this inode
  file_inode.blocks--;
}

void Filesystem::unlink(INode::ID id) {
  INode inode = getINode(id);
  if(inode.links < 2) {
    truncate(id, 0);
    inode_manager->release(id);
  }
  else {
    inode.ctime = time(NULL);
    inode.links -= 1;
    save(id, inode);
  }
}
