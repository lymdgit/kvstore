/**
 * @file async_context.hpp
 * @brief Async I/O context for io_uring operations
 *
 * Manages buffer lifecycle for async operations. The buffer must remain
 * valid until io_uring completes the operation.
 */

#ifndef ASYNC_CONTEXT_HPP
#define ASYNC_CONTEXT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "disk_pointer.hpp"
#include "kvs_persistence.h"

namespace kvs {
namespace persistence {

// Request type enumeration
enum class RequestType { WRITE, READ, FSYNC };

// Forward declaration
class LogManager;

/**
 * @brief Context for async I/O operations
 *
 * This structure holds all data needed for an async operation,
 * including the buffer that must stay alive until completion.
 */
struct AsyncContext {
  RequestType type;

  // Buffer for I/O - MUST remain valid until CQE is processed
  std::vector<char> buffer;

  // File descriptor for the operation
  int fd;

  // Offset in file
  uint64_t offset;

  // Expected size of operation
  size_t expected_size;

  // For write operations: the resulting disk pointer
  DiskPointer disk_ptr;

  // Key for this operation (used in callbacks)
  std::string key;

  // Write completion callback: (success, disk_pointer)
  std::function<void(bool, const DiskPointer &)> write_callback;

  // Read completion callback: (success, value)
  std::function<void(bool, const std::string &)> read_callback;

  // Generic completion callback: (result_code)
  std::function<void(int)> generic_callback;

  // User data pointer for C callbacks
  void *user_data;

  // C-style write callback
  void (*c_write_callback)(int result, kvs_disk_loc_t loc, void *user_data);

  // C-style read callback
  void (*c_read_callback)(int result, const char *value, size_t value_len,
                          void *user_data);

  AsyncContext()
      : type(RequestType::WRITE), fd(-1), offset(0), expected_size(0),
        user_data(nullptr), c_write_callback(nullptr),
        c_read_callback(nullptr) {}

  ~AsyncContext() = default;

  // Non-copyable, movable
  AsyncContext(const AsyncContext &) = delete;
  AsyncContext &operator=(const AsyncContext &) = delete;
  AsyncContext(AsyncContext &&) = default;
  AsyncContext &operator=(AsyncContext &&) = default;
};

// Smart pointer type for context management
using AsyncContextPtr = std::unique_ptr<AsyncContext>;

} // namespace persistence
} // namespace kvs

#endif // ASYNC_CONTEXT_HPP
