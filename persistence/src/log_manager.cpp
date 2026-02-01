/**
 * @file log_manager.cpp
 * @brief Implementation of async log manager using io_uring
 */

#include "../include/log_manager.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>

namespace kvs {
namespace persistence {

LogManager::LogManager()
    : initialized_(false), current_fd_(-1), current_file_id_(0),
      write_offset_(0), ring_initialized_(false), pending_ios_(0),
      pending_sqes_(0) {}

LogManager::~LogManager() { shutdown(); }

int LogManager::init(const LogManagerConfig &config) {
  if (initialized_) {
    return 0;
  }

  config_ = config;

  // Create data directory if it doesn't exist
  struct stat st;
  if (stat(config_.data_dir.c_str(), &st) != 0) {
    if (mkdir(config_.data_dir.c_str(), 0755) != 0) {
      perror("mkdir");
      return -1;
    }
  }

  // Initialize io_uring
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));

  // Try SQPOLL mode if requested (zero-syscall submission)
  if (config_.use_sqpoll) {
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = config_.sqpoll_idle_ms;

    int ret = io_uring_queue_init_params(config_.ring_size, &ring_, &params);
    if (ret < 0) {
      // SQPOLL requires root or CAP_SYS_ADMIN, fall back to regular mode
      fprintf(stderr, "SQPOLL init failed (%s), falling back to regular mode\n",
              strerror(-ret));
      memset(&params, 0, sizeof(params));
      ret = io_uring_queue_init_params(config_.ring_size, &ring_, &params);
      if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        return ret;
      }
    } else {
      printf("[PERSISTENCE] SQPOLL mode enabled (zero-syscall I/O)\n");
    }
  } else {
    int ret = io_uring_queue_init_params(config_.ring_size, &ring_, &params);
    if (ret < 0) {
      fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
      return ret;
    }
  }
  ring_initialized_ = true;

  // Find the latest log file or create new one
  current_file_id_ = 1;

  // Scan for existing log files
  DIR *dir = opendir(config_.data_dir.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      std::string name = entry->d_name;
      if (name.find(config_.file_prefix) == 0 &&
          name.find(".log") != std::string::npos) {
        // Extract file ID
        size_t start = config_.file_prefix.length();
        size_t end = name.find(".log");
        if (end > start) {
          try {
            uint32_t id = std::stoul(name.substr(start, end - start));
            if (id >= current_file_id_) {
              current_file_id_ = id;
            }
          } catch (...) {
          }
        }
      }
    }
    closedir(dir);
  }

  // Open or create the current log file
  int ret = openLogFile(current_file_id_, true);
  if (ret < 0) {
    io_uring_queue_exit(&ring_);
    ring_initialized_ = false;
    return ret;
  }

  initialized_ = true;
  return 0;
}

void LogManager::shutdown() {
  if (!initialized_) {
    return;
  }

  // Wait for pending I/O operations
  while (pending_ios_.load() > 0) {
    waitForCompletion();
  }

  // Close log file
  if (current_fd_ >= 0) {
    close(current_fd_);
    current_fd_ = -1;
  }

  // Cleanup io_uring
  if (ring_initialized_) {
    io_uring_queue_exit(&ring_);
    ring_initialized_ = false;
  }

  initialized_ = false;
}

std::string LogManager::getLogFilePath(uint32_t file_id) const {
  char filename[256];
  snprintf(filename, sizeof(filename), "%s/%s%u.log", config_.data_dir.c_str(),
           config_.file_prefix.c_str(), file_id);
  return std::string(filename);
}

int LogManager::openLogFile(uint32_t file_id, bool create) {
  std::string path = getLogFilePath(file_id);

  int flags = O_RDWR;
  if (create) {
    flags |= O_CREAT;
  }
  if (config_.use_direct_io) {
    flags |= O_DIRECT;
  }

  int fd = open(path.c_str(), flags, 0644);
  if (fd < 0) {
    perror("open log file");
    return -1;
  }

  // Get current file size for write offset
  struct stat st;
  if (fstat(fd, &st) == 0) {
    write_offset_.store(st.st_size);
  }

  if (current_fd_ >= 0) {
    close(current_fd_);
  }

  current_fd_ = fd;
  current_file_id_ = file_id;

  return 0;
}

int LogManager::rotateIfNeeded(size_t write_size) {
  if (write_offset_.load() + write_size > config_.max_file_size) {
    // Close current file and open a new one
    return openLogFile(current_file_id_ + 1, true);
  }
  return 0;
}

int LogManager::appendAsync(
    const std::string &key, const std::string &value,
    std::function<void(bool, const DiskPointer &)> callback) {
  if (!initialized_) {
    return -EINVAL;
  }

  // Create log entry
  LogEntry entry(key, value);

  // Allocate write offset atomically
  size_t entry_size = entry.diskSize();

  // Check if rotation is needed (simplified - not thread-safe rotation)
  if (rotateIfNeeded(entry_size) < 0) {
    return -EIO;
  }

  uint64_t offset;
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    offset = write_offset_.load();
    write_offset_.fetch_add(entry_size);
  }

  // Create async context
  AsyncContext *ctx = new AsyncContext();
  ctx->type = RequestType::WRITE;
  ctx->buffer = entry.encode();
  ctx->fd = current_fd_;
  ctx->offset = offset;
  ctx->expected_size = entry_size;
  ctx->key = key;
  ctx->disk_ptr =
      DiskPointer(current_file_id_, offset, static_cast<uint32_t>(value.size()),
                  entry.header.timestamp);
  ctx->write_callback = callback;

  return submitWrite(ctx);
}

int LogManager::appendAsyncC(const char *key, size_t key_len, const char *value,
                             size_t value_len,
                             void (*callback)(int result, kvs_disk_loc_t loc,
                                              void *user_data),
                             void *user_data) {
  if (!initialized_) {
    return -EINVAL;
  }

  std::string k(key, key_len);
  std::string v(value ? value : "", value_len);

  // Create log entry
  LogEntry entry(k, v);

  size_t entry_size = entry.diskSize();

  if (rotateIfNeeded(entry_size) < 0) {
    return -EIO;
  }

  uint64_t offset;
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    offset = write_offset_.load();
    write_offset_.fetch_add(entry_size);
  }

  AsyncContext *ctx = new AsyncContext();
  ctx->type = RequestType::WRITE;
  ctx->buffer = entry.encode();
  ctx->fd = current_fd_;
  ctx->offset = offset;
  ctx->expected_size = entry_size;
  ctx->key = k;
  ctx->disk_ptr =
      DiskPointer(current_file_id_, offset, static_cast<uint32_t>(value_len),
                  entry.header.timestamp);
  ctx->c_write_callback = callback;
  ctx->user_data = user_data;

  return submitWrite(ctx);
}

int LogManager::readAsync(
    const DiskPointer &ptr,
    std::function<void(bool, const std::string &)> callback) {
  if (!initialized_) {
    return -EINVAL;
  }

  // For now, only support reading from current file
  // TODO: Support reading from older files
  if (ptr.file_id != current_file_id_) {
    // Would need to open the old file
    return -ENOENT;
  }

  // Need to read header + key + value
  // For simplicity, read the entire entry
  size_t read_size =
      sizeof(LogEntryHeader) + ptr.value_size + 256; // Assume max key size

  AsyncContext *ctx = new AsyncContext();
  ctx->type = RequestType::READ;
  ctx->buffer.resize(read_size);
  ctx->fd = current_fd_;
  ctx->offset = ptr.offset;
  ctx->expected_size = read_size;
  ctx->disk_ptr = ptr;
  ctx->read_callback = callback;

  return submitRead(ctx);
}

int LogManager::readAsyncC(const kvs_disk_loc_t *ptr,
                           void (*callback)(int result, const char *value,
                                            size_t value_len, void *user_data),
                           void *user_data) {
  if (!initialized_ || !ptr) {
    return -EINVAL;
  }

  if (ptr->file_id != current_file_id_) {
    return -ENOENT;
  }

  size_t read_size = sizeof(LogEntryHeader) + ptr->value_size + 256;

  AsyncContext *ctx = new AsyncContext();
  ctx->type = RequestType::READ;
  ctx->buffer.resize(read_size);
  ctx->fd = current_fd_;
  ctx->offset = ptr->offset;
  ctx->expected_size = read_size;
  ctx->disk_ptr =
      DiskPointer(ptr->file_id, ptr->offset, ptr->value_size, ptr->timestamp);
  ctx->c_read_callback = callback;
  ctx->user_data = user_data;

  return submitRead(ctx);
}

int LogManager::submitWrite(AsyncContext *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    delete ctx;
    return -EAGAIN;
  }

  io_uring_prep_write(sqe, ctx->fd, ctx->buffer.data(), ctx->buffer.size(),
                      ctx->offset);
  io_uring_sqe_set_data(sqe, ctx);

  pending_ios_.fetch_add(1);
  pending_sqes_.fetch_add(1);

  // Batch mode: only submit when we have enough SQEs or explicitly flushed
  if (pending_sqes_.load() >= BATCH_SUBMIT_THRESHOLD) {
    int ret = io_uring_submit(&ring_);
    if (ret > 0) {
      pending_sqes_.store(0);
    }
  }

  return 0;
}

int LogManager::flushSubmissions() {
  if (pending_sqes_.load() > 0) {
    int ret = io_uring_submit(&ring_);
    if (ret > 0) {
      pending_sqes_.store(0);
    }
    return ret;
  }
  return 0;
}

int LogManager::submitRead(AsyncContext *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    delete ctx;
    return -EAGAIN;
  }

  io_uring_prep_read(sqe, ctx->fd, ctx->buffer.data(), ctx->buffer.size(),
                     ctx->offset);
  io_uring_sqe_set_data(sqe, ctx);

  pending_ios_.fetch_add(1);

  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    pending_ios_.fetch_sub(1);
    delete ctx;
    return ret;
  }

  return 0;
}

int LogManager::submitFsync(AsyncContext *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    delete ctx;
    return -EAGAIN;
  }

  io_uring_prep_fsync(sqe, ctx->fd, 0);
  io_uring_sqe_set_data(sqe, ctx);

  pending_ios_.fetch_add(1);

  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    pending_ios_.fetch_sub(1);
    delete ctx;
    return ret;
  }

  return 0;
}

void LogManager::handleCompletion(struct io_uring_cqe *cqe) {
  AsyncContext *ctx = static_cast<AsyncContext *>(io_uring_cqe_get_data(cqe));
  if (!ctx) {
    return;
  }

  int result = cqe->res;

  switch (ctx->type) {
  case RequestType::WRITE: {
    bool success = (result == static_cast<int>(ctx->buffer.size()));

    if (ctx->write_callback) {
      ctx->write_callback(success, ctx->disk_ptr);
    } else if (ctx->c_write_callback) {
      kvs_disk_loc_t loc;
      loc.file_id = ctx->disk_ptr.file_id;
      loc.offset = ctx->disk_ptr.offset;
      loc.value_size = ctx->disk_ptr.value_size;
      loc.timestamp = ctx->disk_ptr.timestamp;
      ctx->c_write_callback(success ? 0 : -EIO, loc, ctx->user_data);
    }
    break;
  }

  case RequestType::READ: {
    if (result > 0) {
      // Decode the entry
      LogEntry entry = LogEntry::decode(ctx->buffer.data(), result);

      // Verify CRC
      LogEntry temp(std::string(entry.key.begin(), entry.key.end()),
                    std::string(entry.value.begin(), entry.value.end()),
                    entry.header.timestamp);

      bool crc_valid = (temp.header.crc == entry.header.crc);

      if (ctx->read_callback) {
        if (crc_valid && !entry.value.empty()) {
          std::string value(entry.value.begin(), entry.value.end());
          ctx->read_callback(true, value);
        } else {
          ctx->read_callback(false, "");
        }
      } else if (ctx->c_read_callback) {
        if (crc_valid && !entry.value.empty()) {
          ctx->c_read_callback(0, entry.value.data(), entry.value.size(),
                               ctx->user_data);
        } else {
          ctx->c_read_callback(-EIO, nullptr, 0, ctx->user_data);
        }
      }
    } else {
      if (ctx->read_callback) {
        ctx->read_callback(false, "");
      } else if (ctx->c_read_callback) {
        ctx->c_read_callback(result, nullptr, 0, ctx->user_data);
      }
    }
    break;
  }

  case RequestType::FSYNC: {
    if (ctx->generic_callback) {
      ctx->generic_callback(result);
    }
    break;
  }
  }

  delete ctx;
  pending_ios_.fetch_sub(1);
}

int LogManager::processCompletions(int max_completions) {
  if (!initialized_) {
    return -EINVAL;
  }

  int count = 0;
  struct io_uring_cqe *cqe;

  while (true) {
    if (max_completions > 0 && count >= max_completions) {
      break;
    }

    int ret = io_uring_peek_cqe(&ring_, &cqe);
    if (ret < 0) {
      break; // No more completions
    }

    handleCompletion(cqe);
    io_uring_cqe_seen(&ring_, cqe);
    count++;
  }

  return count;
}

int LogManager::waitForCompletion() {
  if (!initialized_) {
    return -EINVAL;
  }

  struct io_uring_cqe *cqe;
  int ret = io_uring_wait_cqe(&ring_, &cqe);
  if (ret < 0) {
    return ret;
  }

  handleCompletion(cqe);
  io_uring_cqe_seen(&ring_, cqe);

  return 1;
}

int LogManager::recover(RecoverCallback callback) {
  if (!callback) {
    return -EINVAL;
  }

  int total_entries = 0;

  // Scan all log files
  for (uint32_t file_id = 1; file_id <= current_file_id_; file_id++) {
    std::string path = getLogFilePath(file_id);

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      continue;
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) != 0) {
      close(fd);
      continue;
    }

    uint64_t offset = 0;
    std::vector<char> buffer(sizeof(LogEntryHeader) +
                             1024 * 1024); // 1MB buffer

    while (offset < static_cast<uint64_t>(st.st_size)) {
      // Read header
      ssize_t n = pread(fd, buffer.data(), sizeof(LogEntryHeader), offset);
      if (n < static_cast<ssize_t>(sizeof(LogEntryHeader))) {
        break;
      }

      LogEntryHeader header;
      std::memcpy(&header, buffer.data(), sizeof(LogEntryHeader));

      // Sanity check
      size_t entry_size =
          sizeof(LogEntryHeader) + header.key_sz + header.value_sz;
      if (entry_size > buffer.size()) {
        buffer.resize(entry_size);
      }

      // Read full entry
      n = pread(fd, buffer.data(), entry_size, offset);
      if (n < static_cast<ssize_t>(entry_size)) {
        // Truncated entry - truncate file here
        if (file_id == current_file_id_) {
          ftruncate(current_fd_, offset);
          write_offset_.store(offset);
        }
        break;
      }

      // Decode and verify CRC
      LogEntry entry = LogEntry::decode(buffer.data(), entry_size);

      // Recalculate CRC
      LogEntry temp(std::string(entry.key.begin(), entry.key.end()),
                    std::string(entry.value.begin(), entry.value.end()),
                    entry.header.timestamp);

      if (temp.header.crc != entry.header.crc) {
        // CRC mismatch - truncate here
        if (file_id == current_file_id_) {
          ftruncate(current_fd_, offset);
          write_offset_.store(offset);
        }
        break;
      }

      // Valid entry - call callback
      DiskPointer ptr(file_id, offset, header.value_sz, header.timestamp);
      std::string key(entry.key.begin(), entry.key.end());
      bool is_delete = (header.value_sz == 0);

      callback(key, ptr, is_delete);
      total_entries++;

      offset += entry_size;
    }

    close(fd);
  }

  return total_entries;
}

int LogManager::syncAsync(std::function<void(int)> callback) {
  if (!initialized_) {
    return -EINVAL;
  }

  AsyncContext *ctx = new AsyncContext();
  ctx->type = RequestType::FSYNC;
  ctx->fd = current_fd_;
  ctx->generic_callback = callback;

  return submitFsync(ctx);
}

int LogManager::getRingFd() const {
  if (!ring_initialized_) {
    return -1;
  }
  return ring_.ring_fd;
}

} // namespace persistence
} // namespace kvs
