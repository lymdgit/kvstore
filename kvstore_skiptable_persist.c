/**
 * @file kvstore_skiptable_persist.c
 * @brief Skiplist implementation with disk persistence support
 *
 * This version stores DiskPointer in nodes instead of actual values.
 * Values are persisted to disk via the persistence layer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kvstore.h"
#include "persistence/include/kvs_persistence.h"
#include "wal_buffer.h"

#ifdef ENABLE_PERSISTENCE

#define MAX_LEVEL 16
#define MAX_KEY_LEN 256

/**
 * @brief Skiplist node with disk pointer
 */
typedef struct _skiplist_persist_node {
  char *key;
  kvs_disk_loc_t disk_loc; // Where the value is on disk
  char *cached_value;      // Optional: cached value in memory
  struct _skiplist_persist_node **forward;
} skiplist_persist_node;

/**
 * @brief Persistent skiplist structure
 */
typedef struct _skiplist_persist {
  int level;
  struct _skiplist_persist_node *header;
  int count;
  int recovery_mode; // 1 during recovery, 0 normal operation
} skiplist_persist;

// Global instance
skiplist_persist PersistSkiplist = {0};

// ===========================================================================
// Internal Helper Functions
// ===========================================================================

static int random_level(void) {
  int level = 1;
  while ((rand() & 0xFFFF) < (0.5 * 0xFFFF) && level < MAX_LEVEL) {
    level++;
  }
  return level;
}

static skiplist_persist_node *create_persist_node(int level, const char *key) {
  skiplist_persist_node *node =
      (skiplist_persist_node *)kvstore_malloc(sizeof(skiplist_persist_node));
  if (!node)
    return NULL;

  node->forward = (skiplist_persist_node **)kvstore_malloc(
      sizeof(skiplist_persist_node *) * level);
  if (!node->forward) {
    kvstore_free(node);
    return NULL;
  }

  node->key = (char *)kvstore_malloc(strlen(key) + 1);
  if (!node->key) {
    kvstore_free(node->forward);
    kvstore_free(node);
    return NULL;
  }
  strcpy(node->key, key);

  // Initialize disk location as empty
  memset(&node->disk_loc, 0, sizeof(kvs_disk_loc_t));
  node->cached_value = NULL;

  for (int i = 0; i < level; i++) {
    node->forward[i] = NULL;
  }

  return node;
}

static void destroy_persist_node(skiplist_persist_node *node) {
  if (!node)
    return;
  kvstore_free(node->key);
  kvstore_free(node->cached_value);
  kvstore_free(node->forward);
  kvstore_free(node);
}

// ===========================================================================
// Write Callback Context
// ===========================================================================

typedef struct {
  skiplist_persist_node *node;
  char *value_copy;
} write_context_t;

// Callback when async write completes
static void on_write_complete(int result, kvs_disk_loc_t loc, void *user_data) {
  write_context_t *ctx = (write_context_t *)user_data;

  if (result == 0 && ctx && ctx->node) {
    // Update node's disk location
    ctx->node->disk_loc = loc;

    // Optionally cache the value
    if (ctx->value_copy && !ctx->node->cached_value) {
      ctx->node->cached_value = ctx->value_copy;
      ctx->value_copy = NULL; // Ownership transferred
    }
  }

  // Cleanup
  if (ctx) {
    kvstore_free(ctx->value_copy);
    kvstore_free(ctx);
  }
}

// ===========================================================================
// Read Callback Context
// ===========================================================================

typedef struct {
  skiplist_persist_node *node;
  char *out_buffer;
  size_t buffer_size;
  volatile int *done_flag;
  volatile int *result_flag;
} read_context_t;

// Callback when async read completes
static void on_read_complete(int result, const char *value, size_t value_len,
                             void *user_data) {
  read_context_t *ctx = (read_context_t *)user_data;

  if (ctx) {
    if (result == 0 && value && value_len > 0) {
      // Cache the value in the node
      if (ctx->node && !ctx->node->cached_value) {
        ctx->node->cached_value = (char *)kvstore_malloc(value_len + 1);
        if (ctx->node->cached_value) {
          memcpy(ctx->node->cached_value, value, value_len);
          ctx->node->cached_value[value_len] = '\0';
        }
      }

      // Copy to output buffer if provided
      if (ctx->out_buffer && ctx->buffer_size > 0) {
        size_t copy_len =
            value_len < ctx->buffer_size - 1 ? value_len : ctx->buffer_size - 1;
        memcpy(ctx->out_buffer, value, copy_len);
        ctx->out_buffer[copy_len] = '\0';
      }

      if (ctx->result_flag)
        *ctx->result_flag = 1;
    } else {
      if (ctx->result_flag)
        *ctx->result_flag = 0;
    }

    if (ctx->done_flag)
      *ctx->done_flag = 1;
    kvstore_free(ctx);
  }
}

// ===========================================================================
// Public API
// ===========================================================================

int kvstore_skiptable_persist_create(skiplist_persist *sl) {
  if (!sl)
    return -1;

  memset(sl, 0, sizeof(skiplist_persist));

  sl->level = 1;
  sl->count = 0;
  sl->recovery_mode = 0;

  sl->header = create_persist_node(MAX_LEVEL, "");
  if (!sl->header)
    return -1;

  srand(time(NULL));
  return 0;
}

void kvstore_skiptable_persist_destroy(skiplist_persist *sl) {
  if (!sl || !sl->header)
    return;

  skiplist_persist_node *node = sl->header->forward[0];
  while (node != NULL) {
    skiplist_persist_node *tmp = node;
    node = node->forward[0];
    destroy_persist_node(tmp);
  }

  destroy_persist_node(sl->header);
  memset(sl, 0, sizeof(skiplist_persist));
}

/**
 * @brief Search for a node by key
 */
static skiplist_persist_node *skiplist_persist_search(skiplist_persist *sl,
                                                      const char *key) {
  if (!sl || !key || !sl->header)
    return NULL;

  skiplist_persist_node *x = sl->header;
  for (int i = sl->level - 1; i >= 0; i--) {
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
      x = x->forward[i];
    }
  }

  x = x->forward[0];

  if (x != NULL && strcmp(x->key, key) == 0) {
    return x;
  }

  return NULL;
}

/**
 * @brief Insert a key with disk location (used during recovery)
 */
int kvs_skiptable_persist_insert_loc(skiplist_persist *sl, const char *key,
                                     kvs_disk_loc_t loc) {
  if (!sl || !key)
    return -1;

  skiplist_persist_node *update[MAX_LEVEL];
  skiplist_persist_node *x = sl->header;

  for (int i = sl->level - 1; i >= 0; i--) {
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
      x = x->forward[i];
    }
    update[i] = x;
  }

  x = x->forward[0];

  if (x != NULL && strcmp(x->key, key) == 0) {
    // Key exists - update disk location (newer version)
    if (loc.timestamp > x->disk_loc.timestamp) {
      x->disk_loc = loc;
      kvstore_free(x->cached_value);
      x->cached_value = NULL;
    }
    return 1; // Updated existing
  }

  // Insert new node
  int level = random_level();

  if (level > sl->level) {
    for (int i = sl->level; i < level; i++) {
      update[i] = sl->header;
    }
    sl->level = level;
  }

  x = create_persist_node(level, key);
  if (!x)
    return -1;

  x->disk_loc = loc;

  for (int i = 0; i < level; i++) {
    x->forward[i] = update[i]->forward[i];
    update[i]->forward[i] = x;
  }

  sl->count++;
  return 0;
}

/**
 * @brief Set a key-value pair (async write to disk)
 */
int kvs_skiptable_persist_set(skiplist_persist *sl, char *key, char *value) {
  if (!sl || !key || !value)
    return -1;

  // First, insert/find the node in memory
  skiplist_persist_node *update[MAX_LEVEL];
  skiplist_persist_node *x = sl->header;

  for (int i = sl->level - 1; i >= 0; i--) {
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
      x = x->forward[i];
    }
    update[i] = x;
  }

  x = x->forward[0];

  skiplist_persist_node *target_node;

  if (x != NULL && strcmp(x->key, key) == 0) {
    // Key exists - update
    target_node = x;
    kvstore_free(x->cached_value);
    x->cached_value = NULL;
  } else {
    // Insert new node
    int level = random_level();

    if (level > sl->level) {
      for (int i = sl->level; i < level; i++) {
        update[i] = sl->header;
      }
      sl->level = level;
    }

    target_node = create_persist_node(level, key);
    if (!target_node)
      return -1;

    for (int i = 0; i < level; i++) {
      target_node->forward[i] = update[i]->forward[i];
      update[i]->forward[i] = target_node;
    }

    sl->count++;
  }

  // Cache the value in memory for fast reads
  size_t value_len = strlen(value);
  target_node->cached_value = (char *)kvstore_malloc(value_len + 1);
  if (target_node->cached_value) {
    memcpy(target_node->cached_value, value, value_len + 1);
  }

  // ASYNC PERSISTENCE: Append to WAL buffer (non-blocking!)
  // The background flusher thread will write to disk later
  wal_buffer_append(key, strlen(key), value, value_len);

  return 0;
}

/**
 * @brief Get value by key
 *
 * If cached, returns immediately. Otherwise triggers async read.
 * For simplicity, this version does a synchronous wait for the read.
 */
char *kvs_skiptable_persist_get(skiplist_persist *sl, char *key) {
  if (!sl || !key)
    return NULL;

  skiplist_persist_node *node = skiplist_persist_search(sl, key);
  if (!node)
    return NULL;

  // Check if value is deleted
  if (kvs_disk_loc_is_deleted(&node->disk_loc)) {
    return NULL;
  }

  // Check cache first
  if (node->cached_value) {
    return node->cached_value;
  }

  // Need to read from disk
  if (!kvs_persistence_is_initialized()) {
    return NULL;
  }

  // Allocate buffer for result
  static char result_buffer[4096]; // Thread-unsafe but simple
  volatile int done = 0;
  volatile int success = 0;

  read_context_t *ctx =
      (read_context_t *)kvstore_malloc(sizeof(read_context_t));
  if (!ctx)
    return NULL;

  ctx->node = node;
  ctx->out_buffer = result_buffer;
  ctx->buffer_size = sizeof(result_buffer);
  ctx->done_flag = &done;
  ctx->result_flag = &success;

  int ret = kvs_persistence_read_async(&node->disk_loc, on_read_complete, ctx);
  if (ret < 0) {
    kvstore_free(ctx);
    return NULL;
  }

  // Wait for completion (simplified synchronous wait)
  while (!done) {
    kvs_persistence_wait_completion();
  }

  if (success && node->cached_value) {
    return node->cached_value;
  }

  return NULL;
}

/**
 * @brief Delete a key (writes tombstone)
 */
int kvs_skiptable_persist_delete(skiplist_persist *sl, char *key) {
  if (!sl || !key)
    return -1;

  skiplist_persist_node *node = skiplist_persist_search(sl, key);
  if (!node)
    return -1; // Key not found

  if (!kvs_persistence_is_initialized()) {
    return -1;
  }

  // Write tombstone (empty value)
  kvs_persistence_delete_async(key, strlen(key), NULL, NULL);

  // Mark as deleted
  node->disk_loc.value_size = 0;
  kvstore_free(node->cached_value);
  node->cached_value = NULL;

  // Note: We don't actually remove the node here for simplicity
  // In a production system, you'd want compaction to clean up

  return 0;
}

/**
 * @brief Modify existing key's value
 */
int kvs_skiptable_persist_modify(skiplist_persist *sl, char *key, char *value) {
  // Modify is just set with existing key
  return kvs_skiptable_persist_set(sl, key, value);
}

/**
 * @brief Count of entries
 */
int kvs_skiptable_persist_count(skiplist_persist *sl) {
  if (!sl)
    return 0;
  return sl->count;
}

// ===========================================================================
// Recovery Support
// ===========================================================================

/**
 * @brief Recovery callback - called for each log entry during startup
 */
static void recovery_callback(const char *key, size_t key_len,
                              kvs_disk_loc_t loc, int is_delete,
                              void *user_data) {
  skiplist_persist *sl = (skiplist_persist *)user_data;

  if (!sl || !key)
    return;

  // Create null-terminated key
  char *key_copy = (char *)kvstore_malloc(key_len + 1);
  if (!key_copy)
    return;
  memcpy(key_copy, key, key_len);
  key_copy[key_len] = '\0';

  if (is_delete) {
    // Mark as deleted
    skiplist_persist_node *node = skiplist_persist_search(sl, key_copy);
    if (node) {
      node->disk_loc.value_size = 0;
      node->disk_loc.timestamp = loc.timestamp;
    }
  } else {
    // Insert or update
    kvs_skiptable_persist_insert_loc(sl, key_copy, loc);
  }

  kvstore_free(key_copy);
}

/**
 * @brief Initialize skiplist from disk logs
 */
int kvstore_skiptable_persist_recover(skiplist_persist *sl) {
  if (!sl)
    return -1;

  if (!kvs_persistence_is_initialized()) {
    return -1;
  }

  sl->recovery_mode = 1;
  int count = kvs_persistence_recover(recovery_callback, sl);
  sl->recovery_mode = 0;

  return count;
}

/**
 * @brief WAL recovery callback - called for each entry during WAL replay
 */
void kvstore_skiptable_persist_wal_recover(const char *key, size_t key_len,
                                           const char *value, size_t value_len,
                                           void *user_data) {
  skiplist_persist *sl = (skiplist_persist *)user_data;

  if (!sl || !key)
    return;

  // Create null-terminated key
  char *key_copy = (char *)kvstore_malloc(key_len + 1);
  if (!key_copy)
    return;
  memcpy(key_copy, key, key_len);
  key_copy[key_len] = '\0';

  // Find or insert node
  skiplist_persist_node *update[MAX_LEVEL];
  skiplist_persist_node *x = sl->header;

  for (int i = sl->level - 1; i >= 0; i--) {
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key_copy) < 0) {
      x = x->forward[i];
    }
    update[i] = x;
  }

  x = x->forward[0];

  skiplist_persist_node *target_node;

  if (x != NULL && strcmp(x->key, key_copy) == 0) {
    // Key exists - update cached value
    target_node = x;
    kvstore_free(x->cached_value);
    x->cached_value = NULL;
  } else {
    // Insert new node
    int level = random_level();

    if (level > sl->level) {
      for (int i = sl->level; i < level; i++) {
        update[i] = sl->header;
      }
      sl->level = level;
    }

    target_node = create_persist_node(level, key_copy);
    if (!target_node) {
      kvstore_free(key_copy);
      return;
    }

    for (int i = 0; i < level; i++) {
      target_node->forward[i] = update[i]->forward[i];
      update[i]->forward[i] = target_node;
    }

    sl->count++;
  }

  // Cache the value
  if (value && value_len > 0) {
    target_node->cached_value = (char *)kvstore_malloc(value_len + 1);
    if (target_node->cached_value) {
      memcpy(target_node->cached_value, value, value_len);
      target_node->cached_value[value_len] = '\0';
    }
  }

  kvstore_free(key_copy);
}

#endif // ENABLE_PERSISTENCE
