/**
 * @file kvs_persistence.h
 * @brief C API for KVStore persistence layer
 *
 * This header provides a C-compatible interface for the C++ persistence
 * library, allowing integration with existing C code.
 */

#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * @brief Disk location pointer
 *
 * This structure represents where a value is stored on disk.
 * It should be stored in the in-memory index instead of the actual value.
 */
typedef struct kvs_disk_loc {
  uint32_t file_id;    /**< Log file ID */
  uint64_t offset;     /**< Byte offset in file */
  uint32_t value_size; /**< Size of value in bytes */
  uint64_t timestamp;  /**< Entry timestamp */
} kvs_disk_loc_t;

/**
 * @brief Configuration for persistence layer
 */
typedef struct kvs_persistence_config {
  const char *data_dir;    /**< Directory for data files (default: "./data") */
  const char *file_prefix; /**< Log file prefix (default: "data") */
  size_t max_file_size;    /**< Max file size before rotation (default: 1GB) */
  uint32_t ring_size;      /**< io_uring queue depth (default: 256) */
  int use_direct_io;       /**< Use O_DIRECT (default: 0) */
  int use_sqpoll; /**< Use SQPOLL for zero-syscall I/O (default: 0, requires
                     root) */
  uint32_t sqpoll_idle_ms; /**< SQPOLL idle timeout in ms (default: 2000) */
} kvs_persistence_config_t;

/* ============================================================================
 * Callback Types
 * ============================================================================
 */

/**
 * @brief Write completion callback
 * @param result 0 on success, negative on error
 * @param loc Disk location of written data
 * @param user_data User-provided context
 */
typedef void (*kvs_write_callback_t)(int result, kvs_disk_loc_t loc,
                                     void *user_data);

/**
 * @brief Read completion callback
 * @param result 0 on success, negative on error
 * @param value Pointer to value data (valid only during callback)
 * @param value_len Length of value data
 * @param user_data User-provided context
 */
typedef void (*kvs_read_callback_t)(int result, const char *value,
                                    size_t value_len, void *user_data);

/**
 * @brief Recovery callback
 * @param key Key string
 * @param key_len Key length
 * @param loc Disk location
 * @param is_delete 1 if this is a tombstone (delete marker)
 * @param user_data User-provided context
 */
typedef void (*kvs_recover_callback_t)(const char *key, size_t key_len,
                                       kvs_disk_loc_t loc, int is_delete,
                                       void *user_data);

/**
 * @brief Sync completion callback
 * @param result 0 on success, negative on error
 * @param user_data User-provided context
 */
typedef void (*kvs_sync_callback_t)(int result, void *user_data);

/* ============================================================================
 * Initialization & Shutdown
 * ============================================================================
 */

/**
 * @brief Initialize persistence layer with default configuration
 * @param data_dir Directory for data files (NULL for default "./data")
 * @return 0 on success, negative on error
 */
int kvs_persistence_init(const char *data_dir);

/**
 * @brief Initialize persistence layer with custom configuration
 * @param config Configuration structure
 * @return 0 on success, negative on error
 */
int kvs_persistence_init_config(const kvs_persistence_config_t *config);

/**
 * @brief Shutdown persistence layer
 *
 * Waits for pending I/O operations to complete.
 */
void kvs_persistence_shutdown(void);

/**
 * @brief Check if persistence layer is initialized
 * @return 1 if initialized, 0 otherwise
 */
int kvs_persistence_is_initialized(void);

/* ============================================================================
 * Async Write Operations
 * ============================================================================
 */

/**
 * @brief Async write a key-value pair
 *
 * The callback will be invoked when the write completes.
 *
 * @param key Key data
 * @param key_len Key length
 * @param value Value data
 * @param value_len Value length
 * @param callback Completion callback
 * @param user_data User context for callback
 * @return 0 on successful submission, negative on error
 */
int kvs_persistence_write_async(const char *key, size_t key_len,
                                const char *value, size_t value_len,
                                kvs_write_callback_t callback, void *user_data);

/**
 * @brief Async write a delete tombstone
 *
 * @param key Key to delete
 * @param key_len Key length
 * @param callback Completion callback
 * @param user_data User context for callback
 * @return 0 on successful submission, negative on error
 */
int kvs_persistence_delete_async(const char *key, size_t key_len,
                                 kvs_write_callback_t callback,
                                 void *user_data);

/* ============================================================================
 * Async Read Operations
 * ============================================================================
 */

/**
 * @brief Async read a value from disk
 *
 * @param loc Disk location from index
 * @param callback Completion callback
 * @param user_data User context for callback
 * @return 0 on successful submission, negative on error
 */
int kvs_persistence_read_async(const kvs_disk_loc_t *loc,
                               kvs_read_callback_t callback, void *user_data);

/* ============================================================================
 * Event Loop Integration
 * ============================================================================
 */

/**
 * @brief Process pending I/O completions
 *
 * Call this in your event loop to handle completed I/O operations.
 * Non-blocking - returns immediately if no completions are ready.
 *
 * @return Number of completions processed, negative on error
 */
int kvs_persistence_process_completions(void);

/**
 * @brief Wait for at least one I/O completion
 *
 * Blocks until at least one I/O operation completes.
 *
 * @return Number of completions processed, negative on error
 */
int kvs_persistence_wait_completion(void);

/**
 * @brief Get the io_uring file descriptor
 *
 * Can be used to integrate with epoll/select for event notification.
 *
 * @return File descriptor, or negative on error
 */
int kvs_persistence_get_ring_fd(void);

/**
 * @brief Get count of pending I/O operations
 * @return Number of pending operations
 */
int kvs_persistence_pending_count(void);

/**
 * @brief Flush pending SQE submissions
 *
 * Forces submission of any batched but not yet submitted io_uring requests.
 * Call this periodically in your event loop for optimal batching.
 *
 * @return Number of submissions, negative on error
 */
int kvs_persistence_flush_submissions(void);

/* ============================================================================
 * Sync & Recovery
 * ============================================================================
 */

/**
 * @brief Force sync all pending writes to disk
 *
 * @param callback Completion callback (can be NULL for fire-and-forget)
 * @param user_data User context for callback
 * @return 0 on successful submission, negative on error
 */
int kvs_persistence_sync_async(kvs_sync_callback_t callback, void *user_data);

/**
 * @brief Recover index from log files
 *
 * This is a synchronous operation that reads all log entries
 * and calls the callback for each one. Should be called at startup.
 *
 * @param callback Called for each log entry
 * @param user_data User context for callback
 * @return Number of entries recovered, negative on error
 */
int kvs_persistence_recover(kvs_recover_callback_t callback, void *user_data);

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/**
 * @brief Check if a disk location is valid
 * @param loc Disk location to check
 * @return 1 if valid, 0 otherwise
 */
int kvs_disk_loc_is_valid(const kvs_disk_loc_t *loc);

/**
 * @brief Check if a disk location represents a deleted entry
 * @param loc Disk location to check
 * @return 1 if deleted, 0 otherwise
 */
int kvs_disk_loc_is_deleted(const kvs_disk_loc_t *loc);

/**
 * @brief Get current write offset
 * @return Current write offset in bytes
 */
uint64_t kvs_persistence_get_write_offset(void);

/**
 * @brief Get current log file ID
 * @return Current file ID
 */
uint32_t kvs_persistence_get_file_id(void);

/* ============================================================================
 * Hint File (Fast Startup)
 * ============================================================================
 */

/**
 * @brief Hint file entry callback
 * @param key Key string
 * @param key_len Key length
 * @param loc Disk location
 * @param user_data User context
 */
typedef void (*kvs_hint_load_callback_t)(const char *key, size_t key_len,
                                         kvs_disk_loc_t loc, void *user_data);

/**
 * @brief Hint file save iterator callback
 * @param key Output: key buffer (caller must fill)
 * @param key_len Output: key length
 * @param loc Output: disk location
 * @param user_data User context
 * @return 1 if more entries, 0 if done
 */
typedef int (*kvs_hint_save_iterator_t)(char *key, size_t *key_len,
                                        kvs_disk_loc_t *loc, void *user_data);

/**
 * @brief Check if hint file exists
 * @param data_dir Data directory path (NULL for default)
 * @return 1 if exists, 0 otherwise
 */
int kvs_hint_file_exists(const char *data_dir);

/**
 * @brief Load index from hint file
 *
 * Use this for fast startup instead of scanning logs.
 *
 * @param data_dir Data directory path (NULL for default)
 * @param callback Called for each loaded entry
 * @param user_data User context
 * @return Number of entries loaded, negative on error
 */
int kvs_hint_file_load(const char *data_dir, kvs_hint_load_callback_t callback,
                       void *user_data);

/**
 * @brief Save current index to hint file
 *
 * Call this during shutdown for fast startup next time.
 *
 * @param data_dir Data directory path (NULL for default)
 * @param iterator Iterator function that yields key-pointer pairs
 * @param user_data User context
 * @return Number of entries saved, negative on error
 */
int kvs_hint_file_save(const char *data_dir, kvs_hint_save_iterator_t iterator,
                       void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* KVS_PERSISTENCE_H */
