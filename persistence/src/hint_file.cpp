/**
 * @file hint_file.cpp
 * @brief Hint file implementation for fast startup
 */

#include "../include/hint_file.hpp"
#include "../include/storage_format.hpp"

#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace kvs {
namespace persistence {

// Hint file header
struct HintFileHeader {
  uint32_t magic;
  uint32_t version;
  uint64_t entry_count;
};

std::string HintFile::getDefaultPath(const std::string &data_dir) {
  return data_dir + "/index.hint";
}

bool HintFile::exists(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return false;
  }

  // Check minimum size for valid hint file
  if (st.st_size <
      static_cast<off_t>(sizeof(HintFileHeader) + sizeof(uint32_t))) {
    return false;
  }

  // Check magic number
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }

  HintFileHeader header;
  ssize_t n = read(fd, &header, sizeof(header));
  close(fd);

  if (n < static_cast<ssize_t>(sizeof(header))) {
    return false;
  }

  return (header.magic == HINT_MAGIC && header.version == HINT_VERSION);
}

int HintFile::save(
    const std::string &path,
    std::function<bool(std::string &key, DiskPointer &ptr)> iterator) {
  // Create temporary file first
  std::string tmp_path = path + ".tmp";

  int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("hint file create");
    return -1;
  }

  // Write placeholder header (will update entry_count later)
  HintFileHeader header;
  header.magic = HINT_MAGIC;
  header.version = HINT_VERSION;
  header.entry_count = 0;

  if (write(fd, &header, sizeof(header)) != sizeof(header)) {
    close(fd);
    unlink(tmp_path.c_str());
    return -1;
  }

  // Buffer for batch writing
  std::vector<char> buffer;
  buffer.reserve(64 * 1024); // 64KB buffer

  uint64_t count = 0;
  uint32_t crc = 0;

  std::string key;
  DiskPointer ptr;

  while (iterator(key, ptr)) {
    // Skip deleted entries
    if (ptr.value_size == 0) {
      continue;
    }

    // Encode entry: [key_len][key][file_id][offset][value_size][timestamp]
    uint32_t key_len = static_cast<uint32_t>(key.size());

    size_t entry_size = sizeof(uint32_t) + key_len + sizeof(uint32_t) +
                        sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t);

    // Flush buffer if needed
    if (buffer.size() + entry_size > buffer.capacity()) {
      if (write(fd, buffer.data(), buffer.size()) !=
          static_cast<ssize_t>(buffer.size())) {
        close(fd);
        unlink(tmp_path.c_str());
        return -1;
      }
      buffer.clear();
    }

    // Append to buffer
    size_t offset = buffer.size();
    buffer.resize(offset + entry_size);
    char *p = buffer.data() + offset;

    memcpy(p, &key_len, sizeof(key_len));
    p += sizeof(key_len);
    memcpy(p, key.data(), key_len);
    p += key_len;
    memcpy(p, &ptr.file_id, sizeof(ptr.file_id));
    p += sizeof(ptr.file_id);
    memcpy(p, &ptr.offset, sizeof(ptr.offset));
    p += sizeof(ptr.offset);
    memcpy(p, &ptr.value_size, sizeof(ptr.value_size));
    p += sizeof(ptr.value_size);
    memcpy(p, &ptr.timestamp, sizeof(ptr.timestamp));

    // Update CRC
    crc = CRC32::update(crc, buffer.data() + offset, entry_size);

    count++;
  }

  // Flush remaining buffer
  if (!buffer.empty()) {
    if (write(fd, buffer.data(), buffer.size()) !=
        static_cast<ssize_t>(buffer.size())) {
      close(fd);
      unlink(tmp_path.c_str());
      return -1;
    }
  }

  // Write CRC
  if (write(fd, &crc, sizeof(crc)) != sizeof(crc)) {
    close(fd);
    unlink(tmp_path.c_str());
    return -1;
  }

  // Update header with entry count
  header.entry_count = count;
  lseek(fd, 0, SEEK_SET);
  if (write(fd, &header, sizeof(header)) != sizeof(header)) {
    close(fd);
    unlink(tmp_path.c_str());
    return -1;
  }

  // Sync and close
  fsync(fd);
  close(fd);

  // Atomic rename
  if (rename(tmp_path.c_str(), path.c_str()) != 0) {
    perror("hint file rename");
    unlink(tmp_path.c_str());
    return -1;
  }

  printf("[HINT] Saved %lu entries to %s\n", count, path.c_str());
  return static_cast<int>(count);
}

int HintFile::load(const std::string &path, HintLoadCallback callback) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return -1;
  }

  // Get file size
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }

  // Read header
  HintFileHeader header;
  if (read(fd, &header, sizeof(header)) != sizeof(header)) {
    close(fd);
    return -1;
  }

  // Validate header
  if (header.magic != HINT_MAGIC || header.version != HINT_VERSION) {
    close(fd);
    return -1;
  }

  // Read all entries
  size_t data_size = st.st_size - sizeof(header) - sizeof(uint32_t);
  std::vector<char> data(data_size);

  if (read(fd, data.data(), data_size) != static_cast<ssize_t>(data_size)) {
    close(fd);
    return -1;
  }

  // Read and verify CRC
  uint32_t stored_crc;
  if (read(fd, &stored_crc, sizeof(stored_crc)) != sizeof(stored_crc)) {
    close(fd);
    return -1;
  }

  uint32_t computed_crc = CRC32::calculate(data.data(), data_size);
  if (computed_crc != stored_crc) {
    close(fd);
    fprintf(stderr, "[HINT] CRC mismatch, hint file corrupted\n");
    return -1;
  }

  close(fd);

  // Parse entries
  size_t offset = 0;
  uint64_t count = 0;

  while (offset < data_size && count < header.entry_count) {
    // Read key length
    if (offset + sizeof(uint32_t) > data_size)
      break;
    uint32_t key_len;
    memcpy(&key_len, data.data() + offset, sizeof(key_len));
    offset += sizeof(key_len);

    // Sanity check
    if (key_len > 65536)
      break;

    // Read key
    if (offset + key_len > data_size)
      break;
    std::string key(data.data() + offset, key_len);
    offset += key_len;

    // Read disk pointer
    if (offset + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) +
            sizeof(uint64_t) >
        data_size)
      break;

    DiskPointer ptr;
    memcpy(&ptr.file_id, data.data() + offset, sizeof(ptr.file_id));
    offset += sizeof(ptr.file_id);
    memcpy(&ptr.offset, data.data() + offset, sizeof(ptr.offset));
    offset += sizeof(ptr.offset);
    memcpy(&ptr.value_size, data.data() + offset, sizeof(ptr.value_size));
    offset += sizeof(ptr.value_size);
    memcpy(&ptr.timestamp, data.data() + offset, sizeof(ptr.timestamp));
    offset += sizeof(ptr.timestamp);

    // Invoke callback
    callback(key, ptr);
    count++;
  }

  printf("[HINT] Loaded %lu entries from %s\n", count, path.c_str());
  return static_cast<int>(count);
}

} // namespace persistence
} // namespace kvs
