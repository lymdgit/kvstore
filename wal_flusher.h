/**
 * @file wal_flusher.h
 * @brief Background WAL flush thread
 *
 * This module provides a background thread that periodically
 * flushes the WAL buffer to disk. Similar to Redis AOF with
 * appendfsync everysec.
 */

#ifndef WAL_FLUSHER_H
#define WAL_FLUSHER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flusher configuration
 */
typedef struct {
  int flush_interval_ms;   // Flush interval (default: 1000ms = 1 second)
  int flush_threshold_pct; // Flush if buffer above this % (default: 75)
  int use_fsync;           // Call fsync after flush (default: 1)
} wal_flusher_config_t;

/**
 * @brief Start the background flusher thread
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, negative on error
 */
int wal_flusher_start(const wal_flusher_config_t *config);

/**
 * @brief Stop the background flusher thread
 *
 * This will signal the thread to stop and wait for it to finish.
 */
void wal_flusher_stop(void);

/**
 * @brief Force an immediate flush
 *
 * Use this during shutdown to ensure all data is persisted.
 *
 * @return Bytes flushed, negative on error
 */
int wal_flusher_force_flush(void);

/**
 * @brief Check if flusher is running
 * @return 1 if running, 0 if stopped
 */
int wal_flusher_is_running(void);

/**
 * @brief Get flush statistics
 * @param total_flushes Output: total flush count
 * @param total_bytes Output: total bytes flushed
 */
void wal_flusher_get_stats(unsigned long *total_flushes,
                           unsigned long *total_bytes);

#ifdef __cplusplus
}
#endif

#endif /* WAL_FLUSHER_H */
