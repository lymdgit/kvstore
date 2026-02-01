/**
 * @file hint_file.hpp
 * @brief Hint file for fast startup - stores index snapshot
 *
 * Instead of scanning all log files on startup, we can dump the
 * in-memory index to a "hint" file on shutdown. On next startup,
 * we load this hint file directly - achieving sub-second startup
 * even with millions of keys.
 *
 * Hint file format:
 *   [Magic: 4 bytes] [Version: 4 bytes] [Entry Count: 8 bytes]
 *   [Entry 1] [Entry 2] ... [Entry N]
 *   [CRC32: 4 bytes]
 *
 * Each entry:
 *   [Key Length: 4 bytes] [Key: variable]
 *   [File ID: 4 bytes] [Offset: 8 bytes] [Value Size: 4 bytes] [Timestamp: 8
 * bytes]
 */

#ifndef HINT_FILE_HPP
#define HINT_FILE_HPP

#include <cstdint>
#include <functional>
#include <string>

#include "disk_pointer.hpp"

namespace kvs {
namespace persistence {

// Hint file magic number: "KVSH"
static constexpr uint32_t HINT_MAGIC = 0x4853564B;
static constexpr uint32_t HINT_VERSION = 1;

// Callback for loading hint entries
using HintLoadCallback =
    std::function<void(const std::string &key, const DiskPointer &ptr)>;

/**
 * @brief Hint file manager
 *
 * Provides fast startup by storing/loading index snapshots.
 */
class HintFile {
public:
  /**
   * @brief Save index to hint file
   *
   * Call this during shutdown to create a snapshot.
   *
   * @param path Path to hint file
   * @param iterator Function that yields all key-pointer pairs
   * @return 0 on success, negative on error
   */
  static int
  save(const std::string &path,
       std::function<bool(std::string &key, DiskPointer &ptr)> iterator);

  /**
   * @brief Load index from hint file
   *
   * Call this during startup for fast recovery.
   *
   * @param path Path to hint file
   * @param callback Called for each loaded entry
   * @return Number of entries loaded, negative on error
   */
  static int load(const std::string &path, HintLoadCallback callback);

  /**
   * @brief Check if hint file exists and is valid
   *
   * @param path Path to hint file
   * @return true if valid hint file exists
   */
  static bool exists(const std::string &path);

  /**
   * @brief Get default hint file path for a data directory
   */
  static std::string getDefaultPath(const std::string &data_dir);
};

} // namespace persistence
} // namespace kvs

#endif // HINT_FILE_HPP
