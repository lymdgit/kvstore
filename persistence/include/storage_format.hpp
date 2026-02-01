/**
 * @file storage_format.hpp
 * @brief Bitcask-style disk storage format definitions
 *
 * Disk Layout per record:
 * [ CRC32 (4B) ][ Timestamp (8B) ][ Key_Size (4B) ][ Value_Size (4B) ][ Key ][
 * Value ]
 *
 * Header is 20 bytes fixed, followed by variable-length key and value.
 */

#ifndef STORAGE_FORMAT_HPP
#define STORAGE_FORMAT_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kvs {
namespace persistence {

// Log entry header (20 bytes fixed)
#pragma pack(push, 1)
struct LogEntryHeader {
  uint32_t crc; // CRC32 checksum (covers timestamp + key_sz + value_sz + key +
                // value)
  uint64_t timestamp; // Write timestamp in microseconds
  uint32_t key_sz;    // Key length in bytes
  uint32_t value_sz;  // Value length in bytes (0 means tombstone/delete)
};
#pragma pack(pop)

static_assert(sizeof(LogEntryHeader) == 20, "LogEntryHeader must be 20 bytes");

// Complete log entry for in-memory operations
class LogEntry {
public:
  LogEntryHeader header;
  std::vector<char> key;
  std::vector<char> value;

  LogEntry() = default;

  LogEntry(const std::string &k, const std::string &v, uint64_t ts = 0);

  // Serialize to binary buffer for disk write
  std::vector<char> encode() const;

  // Deserialize from binary buffer
  static LogEntry decode(const char *data, size_t len);

  // Decode header only (for recovery scanning)
  static bool decodeHeader(const char *data, size_t len,
                           LogEntryHeader &header);

  // Calculate total size on disk
  size_t diskSize() const {
    return sizeof(LogEntryHeader) + key.size() + value.size();
  }

  // Check if this is a tombstone (delete marker)
  bool isTombstone() const { return header.value_sz == 0; }

private:
  // Calculate CRC32 for the entry (excluding the CRC field itself)
  uint32_t calculateCRC() const;
};

// CRC32 utility functions
class CRC32 {
public:
  static uint32_t calculate(const void *data, size_t len);
  static uint32_t update(uint32_t crc, const void *data, size_t len);

private:
  static const uint32_t table[256];
  static void initTable();
};

// Get current timestamp in microseconds
uint64_t getCurrentTimestamp();

} // namespace persistence
} // namespace kvs

#endif // STORAGE_FORMAT_HPP
