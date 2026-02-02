/**
 * @file wal_flusher.c
 * @brief Background WAL flush thread implementation
 */

#include "wal_flusher.h"
#include "wal_buffer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Default configuration
#define DEFAULT_FLUSH_INTERVAL_MS 1000
#define DEFAULT_FLUSH_THRESHOLD_PCT 75
#define DEFAULT_USE_FSYNC 1

// Global flusher state
static struct {
  pthread_t thread;
  int running;
  int stop_requested;

  // Configuration
  int flush_interval_ms;
  int flush_threshold_pct;
  int use_fsync;

  // Statistics
  unsigned long total_flushes;
  unsigned long total_bytes;

  pthread_mutex_t lock;
} g_flusher = {0};

// Sleep for milliseconds
// 计算所需的ms和ns
// 告诉CPU，我接下来一段时间不需要你管我了，你可以去干别的了
static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

// Background thread function
// 落盘线程
static void *flusher_thread_func(void *arg) {
  (void)arg;

  printf("[WAL-FLUSHER] Started (interval=%dms, threshold=%d%%)\n",
         g_flusher.flush_interval_ms, g_flusher.flush_threshold_pct);

  while (!g_flusher.stop_requested) {
    // Check if we need to flush
    int should_flush = 0;

    // Time-based flush
    should_flush = 1;

    // Threshold-based flush (immediate if buffer is getting full)
    if (wal_buffer_needs_flush(g_flusher.flush_threshold_pct)) {
      should_flush = 1;
    }

    if (should_flush) {
      // 调用wal_buffer中的实际落盘函数
      int bytes = wal_buffer_flush();
      if (bytes > 0) {
        pthread_mutex_lock(&g_flusher.lock);
        g_flusher.total_flushes++;
        g_flusher.total_bytes += bytes;
        pthread_mutex_unlock(&g_flusher.lock);
      }
    }

    // Sleep for flush interval
    // Use shorter sleeps to check stop_requested more frequently
    // 设置1000ms的休眠
    int remaining_ms = g_flusher.flush_interval_ms;
    while (remaining_ms > 0 && !g_flusher.stop_requested) {
      int sleep_time = remaining_ms > 100 ? 100 : remaining_ms;
      // 执行真正的内核休眠
      sleep_ms(sleep_time);
      remaining_ms -= sleep_time;

      // Check threshold during sleep
      // 每100ms查看一次wal_buffer是否需要落盘（指标就是90%）
      if (wal_buffer_needs_flush(90)) { // 90% = urgent
        break;
      }
    }
  }

  // Final flush before exit
  int bytes = wal_buffer_flush();
  if (bytes > 0) {
    pthread_mutex_lock(&g_flusher.lock);
    g_flusher.total_flushes++;
    g_flusher.total_bytes += bytes;
    pthread_mutex_unlock(&g_flusher.lock);
  }

  printf("[WAL-FLUSHER] Stopped (total flushes=%lu, bytes=%lu)\n",
         g_flusher.total_flushes, g_flusher.total_bytes);

  return NULL;
}
// 创建刷盘的线程
// 传入配置并做mutex的初始化
int wal_flusher_start(const wal_flusher_config_t *config) {
  if (g_flusher.running) {
    return 0; // Already running
  }

  // Apply configuration
  g_flusher.flush_interval_ms = config && config->flush_interval_ms > 0
                                    ? config->flush_interval_ms
                                    : DEFAULT_FLUSH_INTERVAL_MS;
  g_flusher.flush_threshold_pct = config && config->flush_threshold_pct > 0
                                      ? config->flush_threshold_pct
                                      : DEFAULT_FLUSH_THRESHOLD_PCT;
  g_flusher.use_fsync = config ? config->use_fsync : DEFAULT_USE_FSYNC;

  g_flusher.stop_requested = 0;
  g_flusher.total_flushes = 0;
  g_flusher.total_bytes = 0;

  pthread_mutex_init(&g_flusher.lock, NULL);

  // Start thread
  int ret = pthread_create(&g_flusher.thread, NULL, flusher_thread_func, NULL);
  if (ret != 0) {
    fprintf(stderr, "[WAL-FLUSHER] Failed to create thread: %d\n", ret);
    return -1;
  }

  g_flusher.running = 1;
  return 0;
}
// 停止刷盘线程
void wal_flusher_stop(void) {
  if (!g_flusher.running) {
    return;
  }

  g_flusher.stop_requested = 1;

  // Wait for thread to finish
  pthread_join(g_flusher.thread, NULL);

  pthread_mutex_destroy(&g_flusher.lock);

  g_flusher.running = 0;
}

int wal_flusher_force_flush(void) { return wal_buffer_flush(); }

int wal_flusher_is_running(void) { return g_flusher.running; }

void wal_flusher_get_stats(unsigned long *total_flushes,
                           unsigned long *total_bytes) {
  pthread_mutex_lock(&g_flusher.lock);
  if (total_flushes)
    *total_flushes = g_flusher.total_flushes;
  if (total_bytes)
    *total_bytes = g_flusher.total_bytes;
  pthread_mutex_unlock(&g_flusher.lock);
}
