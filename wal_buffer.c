/**
 * @file wal_buffer.c
 * @brief WAL buffer implementation with io_uring support
 */

#include "wal_buffer.h"

#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Default configuration
#define DEFAULT_CAPACITY (16 * 1024 * 1024) // 16MB
#define DEFAULT_DATA_DIR "./data"
#define WAL_FILE_NAME "wal.log"

// Global WAL buffer state
static struct {
  char *data;       // Buffer memory
  size_t capacity;  // Total capacity
  size_t write_pos; // Current write position
  size_t flush_pos; // Last flushed position

  pthread_mutex_t lock;        // Lock for buffer access
  pthread_spinlock_t spinlock; // Fast lock for append

  char data_dir[256]; // Data directory
  int wal_fd;         // WAL file descriptor

  struct io_uring ring; // io_uring instance
  int use_iouring;      // Flag to indicate if io_uring is available

  int initialized;
} g_wal = {0};

// Get current timestamp in microseconds
static uint64_t get_timestamp_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

int wal_buffer_init(const wal_buffer_config_t *config) {
  if (g_wal.initialized) {
    return 0; // Already initialized
  }

  // Set defaults
  size_t capacity =
      config && config->capacity > 0 ? config->capacity : DEFAULT_CAPACITY;
  const char *data_dir =
      config && config->data_dir ? config->data_dir : DEFAULT_DATA_DIR;

  // Create data directory if needed
  mkdir(data_dir, 0755);

  // Allocate buffer
  g_wal.data = (char *)malloc(capacity);
  if (!g_wal.data) {
    perror("wal_buffer malloc");
    return -1;
  }

  g_wal.capacity = capacity;
  g_wal.write_pos = 0;
  g_wal.flush_pos = 0;

  // Initialize locks
  pthread_mutex_init(&g_wal.lock, NULL);
  pthread_spin_init(&g_wal.spinlock, PTHREAD_PROCESS_PRIVATE);

  // Store data directory
  strncpy(g_wal.data_dir, data_dir, sizeof(g_wal.data_dir) - 1);
  g_wal.data_dir[sizeof(g_wal.data_dir) - 1] = '\0';

  // Open WAL file (append mode)
  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s/%s", data_dir, WAL_FILE_NAME);

  g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_wal.wal_fd < 0) {
    perror("wal file open");
    free(g_wal.data);
    g_wal.data = NULL;
    return -1;
  }

  // Initialize io_uring
  if (io_uring_queue_init(1024, &g_wal.ring, 0) < 0) {
    fprintf(stderr, "Failed to initialize io_uring for WAL, falling back to "
                    "standard syscalls\n");
    g_wal.use_iouring = 0;
  } else {
    g_wal.use_iouring = 1;
    printf("[WAL] io_uring initialized for persistence\n");
  }

  g_wal.initialized = 1;
  printf("[WAL] Initialized with %zu MB buffer\n", capacity / (1024 * 1024));

  return 0;
}

void wal_buffer_shutdown(void) {
  if (!g_wal.initialized) {
    return;
  }

  // Flush remaining data
  wal_buffer_flush();

  // Close io_uring
  if (g_wal.use_iouring) {
    io_uring_queue_exit(&g_wal.ring);
    g_wal.use_iouring = 0;
  }

  // Close file
  if (g_wal.wal_fd >= 0) {
    fsync(g_wal.wal_fd);
    close(g_wal.wal_fd);
    g_wal.wal_fd = -1;
  }

  // Free buffer
  if (g_wal.data) {
    free(g_wal.data);
    g_wal.data = NULL;
  }

  pthread_mutex_destroy(&g_wal.lock);
  pthread_spin_destroy(&g_wal.spinlock);

  g_wal.initialized = 0;
  printf("[WAL] Shutdown complete\n");
}
// 向内存中的buffer中追加数据
// 如果内存满了，就先刷盘
// 涉及到了spinlock和mutex两者的使用方式
int wal_buffer_append(const char *key, size_t key_len, const char *value,
                      size_t value_len) {
  if (!g_wal.initialized || !key) {
    return -1;
  }

  // Calculate entry size
  size_t entry_size = sizeof(wal_entry_header_t) + key_len + value_len;

  // Fast path: use spinlock for quick append
  pthread_spin_lock(&g_wal.spinlock);

  // Check if we have space
  size_t pending = g_wal.write_pos - g_wal.flush_pos;
  if (pending + entry_size > g_wal.capacity) {
    pthread_spin_unlock(&g_wal.spinlock);

    // Buffer full - need to wait for flush
    // In production, you'd signal the flusher and wait
    // For now, do a blocking flush
    pthread_mutex_lock(&g_wal.lock);
    wal_buffer_flush();
    pthread_mutex_unlock(&g_wal.lock);

    pthread_spin_lock(&g_wal.spinlock);

    // Re-check after flush
    pending = g_wal.write_pos - g_wal.flush_pos;
    if (pending + entry_size > g_wal.capacity) {
      pthread_spin_unlock(&g_wal.spinlock);
      return -1; // Entry too large even after flush
    }
  }

  // Append entry
  size_t write_offset = g_wal.write_pos % g_wal.capacity;

  // Bug fix: 检查是否会跨越缓冲区末尾（环绕写入情况）
  if (write_offset + entry_size > g_wal.capacity) {
    pthread_spin_unlock(&g_wal.spinlock);

    // 数据会跨界，需要先刷盘使 write_pos 重置到 0
    pthread_mutex_lock(&g_wal.lock);
    wal_buffer_flush();
    pthread_mutex_unlock(&g_wal.lock);

    pthread_spin_lock(&g_wal.spinlock);
    write_offset = g_wal.write_pos % g_wal.capacity;
    // 如果刷盘后仍然跨界，说明 entry 太大，无法写入
    if (write_offset + entry_size > g_wal.capacity) {
      pthread_spin_unlock(&g_wal.spinlock);
      return -1;
    }
  }

  char *ptr = g_wal.data + write_offset;

  // Write header
  wal_entry_header_t header;
  header.key_len = (uint32_t)key_len;
  header.value_len = (uint32_t)value_len;
  header.timestamp = get_timestamp_us();

  memcpy(ptr, &header, sizeof(header));
  ptr += sizeof(header);

  // Write key
  memcpy(ptr, key, key_len);
  ptr += key_len;

  // Write value
  if (value && value_len > 0) {
    memcpy(ptr, value, value_len);
  }

  g_wal.write_pos += entry_size;

  pthread_spin_unlock(&g_wal.spinlock);

  return 0;
}
// 获取未刷盘的数据量
size_t wal_buffer_pending_size(void) {
  if (!g_wal.initialized) {
    return 0;
  }
  return g_wal.write_pos - g_wal.flush_pos;
}

int wal_buffer_fill_percent(void) {
  if (!g_wal.initialized || g_wal.capacity == 0) {
    return 0;
  }
  size_t pending = g_wal.write_pos - g_wal.flush_pos;
  return (int)((pending * 100) / g_wal.capacity);
}

int wal_buffer_needs_flush(int threshold_percent) {
  return wal_buffer_fill_percent() >= threshold_percent;
}

// Internal standard write fallback
// 标准的写操作，其实redis采用的是后台线程是每秒进行fsync一次
// 每秒之间的数据扔到page cache中，fsyn是一个非常昂贵的IO操作
// Bug fix: 使用循环处理 short write
static int wal_buffer_flush_std(char *data, size_t len) {
  size_t total_written = 0;
  while (total_written < len) {
    ssize_t written = write(g_wal.wal_fd, data + total_written, len - total_written);
    if (written < 0) {
      if (errno == EINTR) continue; // 被信号中断，重试
      perror("wal write");
      return -1;
    }
    total_written += written;
  }
  fsync(g_wal.wal_fd);
  return (int)total_written;
}

// Internal io_uring flush
// 1. 准备写
// 2. 准备fsync
// 3. 提交
// 4. 等待完成
// 5. 检查结果
// 6. 推进CQ ring
static int wal_buffer_flush_uring(char *data, size_t len) {
  struct io_uring_sqe *sqe_write;
  struct io_uring_sqe *sqe_fsync;

  // We need 2 SQEs: Write + Fsync
  // They must be linked so fsync executes after write
  // 确保队列里至少有两个空位
  if (io_uring_sq_space_left(&g_wal.ring) < 2) {
    // Should not happen in this simplified design, but handle it
    io_uring_submit(&g_wal.ring); // Try to clear space
  }

  // 1. Prepare Write
  sqe_write = io_uring_get_sqe(&g_wal.ring);
  if (!sqe_write)
    return wal_buffer_flush_std(data, len); // Fallback

  // Bug fix: 使用 lseek 获取当前文件末尾偏移，避免 O_APPEND + offset=-1 的不确定行为
  off_t cur_offset = lseek(g_wal.wal_fd, 0, SEEK_END);
  io_uring_prep_write(sqe_write, g_wal.wal_fd, data, len, cur_offset >= 0 ? cur_offset : 0);
  sqe_write->flags |= IOSQE_IO_LINK; // Important: Link to next

  // 2. Prepare Fsync
  sqe_fsync = io_uring_get_sqe(&g_wal.ring);
  if (!sqe_fsync) {
    // Bug fix: 第一个 SQE 已经准备好了，不能直接回退到 std 写（会导致双重写入）
    // 提交已有的 write SQE 并等待完成，然后手动 fsync
    int submit_ret = io_uring_submit_and_wait(&g_wal.ring, 1);
    if (submit_ret < 0) {
      return wal_buffer_flush_std(data, len);
    }
    struct io_uring_cqe *fallback_cqe;
    io_uring_wait_cqe(&g_wal.ring, &fallback_cqe);
    int write_res = fallback_cqe->res;
    io_uring_cqe_seen(&g_wal.ring, fallback_cqe);
    if (write_res < 0) {
      fprintf(stderr, "io_uring write failed in fallback: %s\n", strerror(-write_res));
      return -1;
    }
    fsync(g_wal.wal_fd);
    return (int)len;
  }

  io_uring_prep_fsync(sqe_fsync, g_wal.wal_fd, IORING_FSYNC_DATASYNC);
  // Default flags for last one

  // 3. Submit and Wait
  int ret = io_uring_submit_and_wait(&g_wal.ring, 2);
  if (ret < 0) {
    perror("io_uring_submit_and_wait");
    return wal_buffer_flush_std(data, len);
  }

  // 4. Check CQEs
  struct io_uring_cqe *cqe;
  unsigned head;
  int count = 0;
  int io_err = 0;

  io_uring_for_each_cqe(&g_wal.ring, head, cqe) {
    if (cqe->res < 0) {
      io_err = cqe->res;
    }
    count++;
  }

  // Advance CQ ring
  // 告诉内核我看过这些通知了，CQ队列可以推进了
  io_uring_cq_advance(&g_wal.ring, count);

  if (io_err < 0) {
    fprintf(stderr, "io_uring operation failed: %s\n", strerror(-io_err));
    return -1;
  }

  return (int)len;
}
// 调用io_uring进行刷盘
// 1. 获取未刷盘的数据量
// 2. 计算刷盘偏移
// 3. 调用io_uring进行刷盘
// 4. 更新刷盘位置
// 5. 重置位置（如果缓冲区为空）
int wal_buffer_flush(void) {
  if (!g_wal.initialized) {
    return -1;
  }

  // 获取未刷盘的数据量
  // 全局变量的简单读，使用spinlock
  pthread_spin_lock(&g_wal.spinlock);
  size_t write_pos = g_wal.write_pos;
  size_t flush_pos = g_wal.flush_pos;
  pthread_spin_unlock(&g_wal.spinlock);

  if (write_pos == flush_pos) {
    return 0; // Nothing to flush
  }
  // pending:需要进行刷盘的大小
  size_t pending = write_pos - flush_pos;
  // flush_offset:刷盘的偏移量
  size_t flush_offset = flush_pos % g_wal.capacity;

  // Bug fix: 检查数据是否跨越缓冲区末尾，只刷到末尾
  if (flush_offset + pending > g_wal.capacity) {
    pending = g_wal.capacity - flush_offset;
  }

  // Actual Flush Logic
  int written = 0;
  if (g_wal.use_iouring) {
    written = wal_buffer_flush_uring(g_wal.data + flush_offset, pending);
  } else {
    written = wal_buffer_flush_std(g_wal.data + flush_offset, pending);
  }

  if (written < 0) {
    return -1;
  }

  // Update flush position
  // 刷盘成功，更新刷盘位置
  pthread_spin_lock(&g_wal.spinlock);
  g_wal.flush_pos = write_pos;

  // Reset positions if buffer is empty (avoid overflow)
  // 如果缓冲区为空，重置位置（避免溢出）
  if (g_wal.flush_pos == g_wal.write_pos) {
    g_wal.flush_pos = 0;
    g_wal.write_pos = 0;
  }
  pthread_spin_unlock(&g_wal.spinlock);

  return written;
}
// 回放硬盘里的数据
// 1. 打开WAL文件
// 2. 获取文件大小
// 3. 读取文件内容
// 4. 解析WAL文件
// 5. 调用回调函数
// 6. 关闭文件
int wal_buffer_replay(wal_replay_callback_t callback, void *user_data) {
  if (!callback) {
    return -1;
  }

  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s/%s",
           g_wal.data_dir[0] ? g_wal.data_dir : DEFAULT_DATA_DIR,
           WAL_FILE_NAME);

  int fd = open(wal_path, O_RDONLY);
  if (fd < 0) {
    // No WAL file - fresh start
    return 0;
  }

  // Get file size
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }

  if (st.st_size == 0) {
    close(fd);
    return 0;
  }

  // Read entire file
  // For replay, standard read is fine as it happens once at startup
  char *data = (char *)malloc(st.st_size);
  if (!data) {
    close(fd);
    return -1;
  }

  ssize_t bytes_read = read(fd, data, st.st_size);
  close(fd);

  if (bytes_read != st.st_size) {
    free(data);
    return -1;
  }

  // Parse entries
  int count = 0;
  size_t offset = 0;

  while (offset + sizeof(wal_entry_header_t) <= (size_t)st.st_size) {
    wal_entry_header_t *header = (wal_entry_header_t *)(data + offset);

    size_t entry_size =
        sizeof(wal_entry_header_t) + header->key_len + header->value_len;

    if (offset + entry_size > (size_t)st.st_size) {
      break; // Incomplete entry (crash during write)
    }

    const char *key = data + offset + sizeof(wal_entry_header_t);
    const char *value = key + header->key_len;
    // 这里利用了生产者-消费者模式，本函数式生产者，进行解析，数据扔给回调函数（消费者）
    // 消费者是去如何写到内存中我不管
    callback(key, header->key_len, header->value_len > 0 ? value : NULL,
             header->value_len, user_data);

    count++;
    offset += entry_size;
  }

  free(data);

  printf("[WAL] Replayed %d entries from %s\n", count, wal_path);
  return count;
}

// =============================================================================
// WAL Compaction: deduplicate entries, keep only latest value per key
// =============================================================================

// Internal hash map for compaction (self-contained, used only during compact)
typedef struct _compact_node {
  char *key;
  size_t key_len;
  char *value;       // NULL = tombstone (deleted)
  size_t value_len;
  uint64_t timestamp;
  struct _compact_node *next;
} compact_node_t;

#define COMPACT_BUCKETS 16384

static unsigned int compact_hash_fn(const char *key, size_t len) {
  unsigned int h = 5381;
  for (size_t i = 0; i < len; i++) {
    h = ((h << 5) + h) + (unsigned char)key[i];
  }
  return h % COMPACT_BUCKETS;
}

// Free all nodes in the compaction hash map
static void compact_free_map(compact_node_t **buckets) {
  if (!buckets) return;
  for (int i = 0; i < COMPACT_BUCKETS; i++) {
    compact_node_t *node = buckets[i];
    while (node) {
      compact_node_t *next = node->next;
      free(node->key);
      if (node->value) free(node->value);
      free(node);
      node = next;
    }
  }
  free(buckets);
}

// Write data to fd with short-write retry
static int compact_write_all(int fd, const void *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t ret = write(fd, (const char *)buf + written, len - written);
    if (ret < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    written += ret;
  }
  return 0;
}

int wal_buffer_compact(void) {
  if (!g_wal.initialized) {
    return -1;
  }

  char wal_path[512];
  snprintf(wal_path, sizeof(wal_path), "%s/%s",
           g_wal.data_dir[0] ? g_wal.data_dir : DEFAULT_DATA_DIR,
           WAL_FILE_NAME);

  // Lock to prevent flusher from writing during compaction
  pthread_mutex_lock(&g_wal.lock);

  // 1. Close current WAL fd
  if (g_wal.wal_fd >= 0) {
    fsync(g_wal.wal_fd);
    close(g_wal.wal_fd);
    g_wal.wal_fd = -1;
  }

  // 2. Read entire WAL file
  int fd = open(wal_path, O_RDONLY);
  if (fd < 0) {
    // No WAL file, just reopen for writing
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return 0;
  }

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size == 0) {
    close(fd);
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return 0;
  }

  size_t file_size = (size_t)st.st_size;
  char *data = (char *)malloc(file_size);
  if (!data) {
    close(fd);
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return -1;
  }

  ssize_t bytes_read = read(fd, data, file_size);
  close(fd);

  if (bytes_read != (ssize_t)file_size) {
    free(data);
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return -1;
  }

  // 3. Parse all entries, deduplicate into hash map
  compact_node_t **buckets =
      (compact_node_t **)calloc(COMPACT_BUCKETS, sizeof(compact_node_t *));
  if (!buckets) {
    free(data);
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return -1;
  }

  int total_entries = 0;
  size_t offset = 0;

  while (offset + sizeof(wal_entry_header_t) <= file_size) {
    wal_entry_header_t *header = (wal_entry_header_t *)(data + offset);
    size_t entry_size =
        sizeof(wal_entry_header_t) + header->key_len + header->value_len;

    if (offset + entry_size > file_size) break; // Incomplete entry

    const char *key = data + offset + sizeof(wal_entry_header_t);
    const char *value =
        (header->value_len > 0) ? (key + header->key_len) : NULL;

    unsigned int idx = compact_hash_fn(key, header->key_len);

    // Find existing entry for this key
    compact_node_t *found = NULL;
    compact_node_t *node = buckets[idx];
    while (node) {
      if (node->key_len == header->key_len &&
          memcmp(node->key, key, header->key_len) == 0) {
        found = node;
        break;
      }
      node = node->next;
    }

    if (found) {
      // Update existing: replace with latest value
      if (found->value) {
        free(found->value);
        found->value = NULL;
      }
      found->value_len = 0;

      if (value && header->value_len > 0) {
        found->value = (char *)malloc(header->value_len);
        memcpy(found->value, value, header->value_len);
        found->value_len = header->value_len;
      }
      found->timestamp = header->timestamp;
    } else {
      // Insert new entry
      compact_node_t *new_node =
          (compact_node_t *)malloc(sizeof(compact_node_t));
      new_node->key = (char *)malloc(header->key_len);
      memcpy(new_node->key, key, header->key_len);
      new_node->key_len = header->key_len;

      if (value && header->value_len > 0) {
        new_node->value = (char *)malloc(header->value_len);
        memcpy(new_node->value, value, header->value_len);
        new_node->value_len = header->value_len;
      } else {
        new_node->value = NULL;
        new_node->value_len = 0;
      }
      new_node->timestamp = header->timestamp;
      new_node->next = buckets[idx];
      buckets[idx] = new_node;
    }

    total_entries++;
    offset += entry_size;
  }

  free(data);

  // 4. Write compacted entries to temp file
  char tmp_path[512];
  snprintf(tmp_path, sizeof(tmp_path), "%s/%s.compact",
           g_wal.data_dir[0] ? g_wal.data_dir : DEFAULT_DATA_DIR,
           WAL_FILE_NAME);

  int tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (tmp_fd < 0) {
    perror("wal compact: open temp file");
    compact_free_map(buckets);
    g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    pthread_mutex_unlock(&g_wal.lock);
    return -1;
  }

  int live_entries = 0;
  for (int i = 0; i < COMPACT_BUCKETS; i++) {
    compact_node_t *node = buckets[i];
    while (node) {
      // Skip tombstones (deleted keys: value == NULL)
      if (node->value && node->value_len > 0) {
        wal_entry_header_t hdr;
        hdr.key_len = (uint32_t)node->key_len;
        hdr.value_len = (uint32_t)node->value_len;
        hdr.timestamp = node->timestamp;

        compact_write_all(tmp_fd, &hdr, sizeof(hdr));
        compact_write_all(tmp_fd, node->key, node->key_len);
        compact_write_all(tmp_fd, node->value, node->value_len);
        live_entries++;
      }
      node = node->next;
    }
  }

  fsync(tmp_fd);
  close(tmp_fd);

  // 5. Atomically replace old WAL with compacted version
  rename(tmp_path, wal_path);

  // 6. Cleanup hash map
  compact_free_map(buckets);

  // 7. Reopen WAL file for append
  g_wal.wal_fd = open(wal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_wal.wal_fd < 0) {
    perror("wal compact: reopen");
    pthread_mutex_unlock(&g_wal.lock);
    return -1;
  }

  pthread_mutex_unlock(&g_wal.lock);

  printf("[WAL] Compaction complete: %d entries -> %d live entries\n",
         total_entries, live_entries);

  return live_entries;
}

int wal_buffer_get_fd(void) { return g_wal.wal_fd; }
