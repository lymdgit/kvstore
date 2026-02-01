/**
 * @file kvs_persistence.cpp
 * @brief C API implementation for persistence layer
 */

#include "../include/kvs_persistence.h"
#include "../include/disk_pointer.hpp"
#include "../include/hint_file.hpp"
#include "../include/log_manager.hpp"

#include <cstring>

// Global LogManager instance
static kvs::persistence::LogManager *g_log_manager = nullptr;

extern "C" {

int kvs_persistence_init(const char *data_dir) {
  if (g_log_manager) {
    return 0; // Already initialized
  }

  g_log_manager = new kvs::persistence::LogManager();

  kvs::persistence::LogManagerConfig config;
  if (data_dir) {
    config.data_dir = data_dir;
  }

  int ret = g_log_manager->init(config);
  if (ret < 0) {
    delete g_log_manager;
    g_log_manager = nullptr;
    return ret;
  }

  return 0;
}

int kvs_persistence_init_config(const kvs_persistence_config_t *config) {
  if (g_log_manager) {
    return 0;
  }

  g_log_manager = new kvs::persistence::LogManager();

  kvs::persistence::LogManagerConfig cfg;
  if (config) {
    if (config->data_dir) {
      cfg.data_dir = config->data_dir;
    }
    if (config->file_prefix) {
      cfg.file_prefix = config->file_prefix;
    }
    if (config->max_file_size > 0) {
      cfg.max_file_size = config->max_file_size;
    }
    if (config->ring_size > 0) {
      cfg.ring_size = config->ring_size;
    }
    cfg.use_direct_io = config->use_direct_io != 0;
    cfg.use_sqpoll = config->use_sqpoll != 0;
    if (config->sqpoll_idle_ms > 0) {
      cfg.sqpoll_idle_ms = config->sqpoll_idle_ms;
    }
  }

  int ret = g_log_manager->init(cfg);
  if (ret < 0) {
    delete g_log_manager;
    g_log_manager = nullptr;
    return ret;
  }

  return 0;
}

void kvs_persistence_shutdown(void) {
  if (g_log_manager) {
    g_log_manager->shutdown();
    delete g_log_manager;
    g_log_manager = nullptr;
  }
}

int kvs_persistence_is_initialized(void) {
  return (g_log_manager && g_log_manager->isInitialized()) ? 1 : 0;
}

int kvs_persistence_write_async(const char *key, size_t key_len,
                                const char *value, size_t value_len,
                                kvs_write_callback_t callback,
                                void *user_data) {
  if (!g_log_manager) {
    return -1;
  }

  return g_log_manager->appendAsyncC(key, key_len, value, value_len, callback,
                                     user_data);
}

int kvs_persistence_delete_async(const char *key, size_t key_len,
                                 kvs_write_callback_t callback,
                                 void *user_data) {
  if (!g_log_manager) {
    return -1;
  }

  // Delete is just a write with empty value (tombstone)
  return g_log_manager->appendAsyncC(key, key_len, nullptr, 0, callback,
                                     user_data);
}

int kvs_persistence_read_async(const kvs_disk_loc_t *loc,
                               kvs_read_callback_t callback, void *user_data) {
  if (!g_log_manager || !loc) {
    return -1;
  }

  return g_log_manager->readAsyncC(loc, callback, user_data);
}

int kvs_persistence_process_completions(void) {
  if (!g_log_manager) {
    return -1;
  }

  return g_log_manager->processCompletions(0);
}

int kvs_persistence_wait_completion(void) {
  if (!g_log_manager) {
    return -1;
  }

  return g_log_manager->waitForCompletion();
}

int kvs_persistence_get_ring_fd(void) {
  if (!g_log_manager) {
    return -1;
  }

  return g_log_manager->getRingFd();
}

int kvs_persistence_pending_count(void) {
  if (!g_log_manager) {
    return 0;
  }

  return g_log_manager->getPendingCount();
}

int kvs_persistence_flush_submissions(void) {
  if (!g_log_manager) {
    return -1;
  }

  return g_log_manager->flushSubmissions();
}

// Wrapper struct for C callback adaptation
struct SyncCallbackWrapper {
  kvs_sync_callback_t callback;
  void *user_data;
};

static void sync_callback_adapter(int result) {
  // This is a simplified version - in production you'd need proper context
  // management
}

int kvs_persistence_sync_async(kvs_sync_callback_t callback, void *user_data) {
  if (!g_log_manager) {
    return -1;
  }

  // Wrap callback
  auto cpp_callback = [callback, user_data](int result) {
    if (callback) {
      callback(result, user_data);
    }
  };

  return g_log_manager->syncAsync(cpp_callback);
}

int kvs_persistence_recover(kvs_recover_callback_t callback, void *user_data) {
  if (!g_log_manager || !callback) {
    return -1;
  }

  auto cpp_callback = [callback,
                       user_data](const std::string &key,
                                  const kvs::persistence::DiskPointer &ptr,
                                  bool is_delete) {
    kvs_disk_loc_t loc;
    loc.file_id = ptr.file_id;
    loc.offset = ptr.offset;
    loc.value_size = ptr.value_size;
    loc.timestamp = ptr.timestamp;

    callback(key.c_str(), key.size(), loc, is_delete ? 1 : 0, user_data);
  };

  return g_log_manager->recover(cpp_callback);
}

int kvs_disk_loc_is_valid(const kvs_disk_loc_t *loc) {
  if (!loc)
    return 0;
  return (loc->file_id > 0 || loc->offset > 0 || loc->value_size > 0) ? 1 : 0;
}

int kvs_disk_loc_is_deleted(const kvs_disk_loc_t *loc) {
  if (!loc)
    return 0;
  return (loc->value_size == 0 && loc->timestamp > 0) ? 1 : 0;
}

uint64_t kvs_persistence_get_write_offset(void) {
  if (!g_log_manager) {
    return 0;
  }
  return g_log_manager->getWriteOffset();
}

uint32_t kvs_persistence_get_file_id(void) {
  if (!g_log_manager) {
    return 0;
  }
  return g_log_manager->getCurrentFileId();
}

// ============================================================================
// Hint File Functions
// ============================================================================

int kvs_hint_file_exists(const char *data_dir) {
  std::string dir = data_dir ? data_dir : "./data";
  std::string path = kvs::persistence::HintFile::getDefaultPath(dir);
  return kvs::persistence::HintFile::exists(path) ? 1 : 0;
}

int kvs_hint_file_load(const char *data_dir, kvs_hint_load_callback_t callback,
                       void *user_data) {
  if (!callback) {
    return -1;
  }

  std::string dir = data_dir ? data_dir : "./data";
  std::string path = kvs::persistence::HintFile::getDefaultPath(dir);

  auto cpp_callback = [callback,
                       user_data](const std::string &key,
                                  const kvs::persistence::DiskPointer &ptr) {
    kvs_disk_loc_t loc;
    loc.file_id = ptr.file_id;
    loc.offset = ptr.offset;
    loc.value_size = ptr.value_size;
    loc.timestamp = ptr.timestamp;
    callback(key.c_str(), key.size(), loc, user_data);
  };

  return kvs::persistence::HintFile::load(path, cpp_callback);
}

int kvs_hint_file_save(const char *data_dir, kvs_hint_save_iterator_t iterator,
                       void *user_data) {
  if (!iterator) {
    return -1;
  }

  std::string dir = data_dir ? data_dir : "./data";
  std::string path = kvs::persistence::HintFile::getDefaultPath(dir);

  // Adapter from C iterator to C++ iterator
  auto cpp_iterator = [iterator,
                       user_data](std::string &key,
                                  kvs::persistence::DiskPointer &ptr) -> bool {
    char key_buf[4096];
    size_t key_len = 0;
    kvs_disk_loc_t loc;

    int result = iterator(key_buf, &key_len, &loc, user_data);
    if (result <= 0) {
      return false;
    }

    key = std::string(key_buf, key_len);
    ptr.file_id = loc.file_id;
    ptr.offset = loc.offset;
    ptr.value_size = loc.value_size;
    ptr.timestamp = loc.timestamp;
    return true;
  };

  return kvs::persistence::HintFile::save(path, cpp_iterator);
}

} // extern "C"
