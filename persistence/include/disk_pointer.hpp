/**
 * @file disk_pointer.hpp
 * @brief Disk location pointer for in-memory index
 *
 * This structure is stored in the in-memory index (Skiplist, RBTree, etc.)
 * instead of the actual value, enabling crash recovery from disk.
 */

#ifndef DISK_POINTER_HPP
#define DISK_POINTER_HPP

#include <cstdint>

namespace kvs {
namespace persistence {

/**
 * @brief Pointer to data location on disk
 *
 * This is what gets stored in the in-memory index.
 * The actual value is stored on disk and can be read using this pointer.
 */
struct DiskPointer {
  uint32_t file_id;    // File ID (for multi-file log rotation)
  uint64_t offset;     // Byte offset within the file
  uint32_t value_size; // Size of the value in bytes
  uint64_t timestamp;  // Version timestamp for conflict resolution

  DiskPointer() : file_id(0), offset(0), value_size(0), timestamp(0) {}

  DiskPointer(uint32_t fid, uint64_t off, uint32_t vsz, uint64_t ts = 0)
      : file_id(fid), offset(off), value_size(vsz), timestamp(ts) {}

  // Check if this is a valid pointer
  bool isValid() const { return file_id > 0 || offset > 0 || value_size > 0; }

  // Check if this represents a deleted entry
  bool isDeleted() const { return value_size == 0 && timestamp > 0; }
};

} // namespace persistence
} // namespace kvs

#endif // DISK_POINTER_HPP
