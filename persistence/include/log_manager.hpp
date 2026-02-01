/**
 * @file log_manager.hpp
 * @brief Async log file manager using io_uring
 *
 * Manages append-only log files for Bitcask-style persistence.
 * All disk I/O is async via io_uring.
 */

#ifndef LOG_MANAGER_HPP
#define LOG_MANAGER_HPP

#include <atomic>
#include <functional>
#include <liburing.h>
#include <mutex>
#include <string>

#include "async_context.hpp"
#include "disk_pointer.hpp"
#include "kvs_persistence.h"
#include "storage_format.hpp"

namespace kvs {
namespace persistence {

// Configuration for LogManager
struct LogManagerConfig {
  std::string data_dir = "./data";           // Directory for log files
  std::string file_prefix = "data";          // Log file name prefix
  size_t max_file_size = 1024 * 1024 * 1024; // 1GB max file size
  uint32_t ring_size = 256;                  // io_uring queue depth
  bool use_direct_io = false;     // Use O_DIRECT (requires aligned buffers)
  bool use_sqpoll = false;        // Use SQPOLL mode (zero-syscall submission)
  uint32_t sqpoll_idle_ms = 2000; // SQPOLL idle timeout in milliseconds
};

// Batch submission threshold - submit when this many SQEs are pending
static constexpr int BATCH_SUBMIT_THRESHOLD = 32;

// Recovery callback type
using RecoverCallback = std::function<void(
    const std::string &key, const DiskPointer &ptr, bool is_delete)>;

/**
 * @brief Async log file manager
 *
 * Provides async append and read operations using io_uring.
 * Supports crash recovery by replaying log entries.
 */
class LogManager {
public:
  LogManager();
  ~LogManager();

  // Non-copyable
  LogManager(const LogManager &) = delete;
  LogManager &operator=(const LogManager &) = delete;

  /**
   * @brief Initialize the log manager
   * @param config Configuration options
   * @return 0 on success, negative on error
   */
  int init(const LogManagerConfig &config = LogManagerConfig());

  /**
   * @brief Shutdown the log manager
   *
   * Waits for pending I/O operations to complete.
   */
  void shutdown();

  /**
   * @brief Check if manager is initialized
   */
  bool isInitialized() const { return initialized_; }

  /**
   * @brief Async append a key-value pair to log
   *
   * @param key Key string
   * @param value Value string (empty for delete/tombstone)
   * @param callback Called when write completes
   * @return 0 on success (submission), negative on error
   */
  int appendAsync(const std::string &key, const std::string &value,
                  std::function<void(bool, const DiskPointer &)> callback);

  /**
   * @brief Async append with C-style callback
   */
  int appendAsyncC(const char *key, size_t key_len, const char *value,
                   size_t value_len,
                   void (*callback)(int result, kvs_disk_loc_t loc,
                                    void *user_data),
                   void *user_data);

  /**
   * @brief Async read value from disk
   *
   * @param ptr Disk pointer from index
   * @param callback Called when read completes
   * @return 0 on success (submission), negative on error
   */
  int readAsync(const DiskPointer &ptr,
                std::function<void(bool, const std::string &)> callback);

  /**
   * @brief Async read with C-style callback
   */
  int readAsyncC(const kvs_disk_loc_t *ptr,
                 void (*callback)(int result, const char *value,
                                  size_t value_len, void *user_data),
                 void *user_data);

  /**
   * @brief Process completed I/O operations
   *
   * Call this from the main event loop to handle CQEs.
   *
   * @param max_completions Maximum number of completions to process (0 = all
   * available)
   * @return Number of completions processed
   */
  int processCompletions(int max_completions = 0);

  /**
   * @brief Wait for and process at least one completion
   *
   * Blocks until at least one I/O operation completes.
   *
   * @return Number of completions processed, negative on error
   */
  int waitForCompletion();

  /**
   * @brief Flush pending SQE submissions
   *
   * Forces submission of any batched but not yet submitted requests.
   *
   * @return Number of submissions, negative on error
   */
  int flushSubmissions();

  /**
   * @brief Recover index from log files
   *
   * Reads all log entries and calls the callback for each.
   * This is synchronous and should be called at startup.
   *
   * @param callback Called for each valid log entry
   * @return Number of entries recovered, negative on error
   */
  int recover(RecoverCallback callback);

  /**
   * @brief Sync all pending writes to disk
   *
   * @param callback Called when fsync completes
   * @return 0 on success, negative on error
   */
  int syncAsync(std::function<void(int)> callback);

  /**
   * @brief Get the io_uring ring fd for integration with epoll/select
   */
  int getRingFd() const;

  /**
   * @brief Get current write offset
   */
  uint64_t getWriteOffset() const { return write_offset_.load(); }

  /**
   * @brief Get current file ID
   */
  uint32_t getCurrentFileId() const { return current_file_id_; }

  /**
   * @brief Get pending I/O count
   */
  int getPendingCount() const { return pending_ios_.load(); }

private:
  // Open a log file
  int openLogFile(uint32_t file_id, bool create);

  // Generate log file path
  std::string getLogFilePath(uint32_t file_id) const;

  // Rotate to a new log file if needed
  int rotateIfNeeded(size_t write_size);

  // Submit an io_uring SQE
  int submitWrite(AsyncContext *ctx);
  int submitRead(AsyncContext *ctx);
  int submitFsync(AsyncContext *ctx);

  // Handle a completed CQE
  void handleCompletion(struct io_uring_cqe *cqe);

private:
  bool initialized_;
  LogManagerConfig config_;

  // Current log file
  int current_fd_;
  uint32_t current_file_id_;
  std::atomic<uint64_t> write_offset_;

  // io_uring instance
  struct io_uring ring_;
  bool ring_initialized_;

  // Pending I/O counter
  std::atomic<int> pending_ios_;

  // Mutex for write offset allocation
  std::mutex write_mutex_;

  // Pending SQE counter for batch submission
  std::atomic<int> pending_sqes_;
};

} // namespace persistence
} // namespace kvs

#endif // LOG_MANAGER_HPP
