/**
 * @file wal_buffer.h
 * @brief Write-Ahead Log buffer for async persistence
 *
 * This module provides a ring buffer for pending WAL entries.
 * Writes are appended to the buffer and flushed to disk by a
 * background thread, achieving high throughput with eventual
 * persistence (similar to Redis AOF with appendfsync everysec).
 */

#ifndef WAL_BUFFER_H
#define WAL_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WAL entry header (stored in buffer)
 */
typedef struct {
  uint32_t key_len;
  uint32_t value_len;
  uint64_t timestamp;
  // Followed by: key[key_len] + value[value_len]
} wal_entry_header_t;

/**
 * @brief WAL buffer configuration
 */
typedef struct {
  size_t capacity;      // Buffer capacity in bytes (default: 16MB)
  const char *data_dir; // Directory for WAL files
} wal_buffer_config_t;

/**
 * @brief Initialize WAL buffer
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, negative on error
 */
int wal_buffer_init(const wal_buffer_config_t *config);

/**
 * @brief Shutdown WAL buffer
 */
void wal_buffer_shutdown(void);

/**
 * @brief Append entry to WAL buffer
 *
 * This function is thread-safe and non-blocking.
 * Returns immediately after appending to memory buffer.
 *
 * @param key Key data
 * @param key_len Key length
 * @param value Value data (NULL for delete)
 * @param value_len Value length (0 for delete)
 * @return 0 on success, negative on error
 */
int wal_buffer_append(const char *key, size_t key_len, const char *value,
                      size_t value_len);

/**
 * @brief Get pending data size in buffer
 * @return Bytes pending flush
 */
size_t wal_buffer_pending_size(void);

/**
 * @brief Get buffer fill percentage
 * @return 0-100 percentage
 */
int wal_buffer_fill_percent(void);

/**
 * @brief Check if buffer needs flush (above threshold)
 * @param threshold_percent Threshold percentage (e.g., 75)
 * @return 1 if needs flush, 0 otherwise
 */
int wal_buffer_needs_flush(int threshold_percent);

/**
 * @brief Flush buffer to disk (called by flusher thread)
 *
 * This writes all pending entries to the WAL file.
 *
 * @return Number of bytes written, negative on error
 */
int wal_buffer_flush(void);

/**
 * @brief Replay WAL file on startup
 *
 * Calls the callback for each entry in the WAL file.
 *
 * @param callback Function to call for each entry
 * @param user_data User context
 * @return Number of entries replayed, negative on error
 */
typedef void (*wal_replay_callback_t)(const char *key, size_t key_len,
                                      const char *value, size_t value_len,
                                      void *user_data);
int wal_buffer_replay(wal_replay_callback_t callback, void *user_data);

/**
 * @brief Get current WAL file descriptor (for io_uring)
 * @return File descriptor, or -1 if not open
 */
int wal_buffer_get_fd(void);

/**
 * @brief Compact WAL file on startup
 *
 * Reads the entire WAL, deduplicates entries (keeps only latest
 * value per key, removes tombstones), and rewrites a compact WAL.
 * This prevents unbounded WAL growth from repeated SET/DEL operations.
 *
 * Should be called AFTER wal_buffer_replay() completes.
 *
 * @return Number of live entries after compaction, negative on error
 */
int wal_buffer_compact(void);

#ifdef __cplusplus
}
#endif

#endif /* WAL_BUFFER_H */
