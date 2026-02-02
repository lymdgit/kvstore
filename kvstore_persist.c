
#include "kvstore.h"
#include "wal_buffer.h"

#if ENABLE_PERSISTENCE
// 中间层：连接了wbuffer和kv引擎层
static kv_engine_type_t g_current_engine = KVS_ENGINE_HASH; // Default

void kvs_persist_set_engine(kv_engine_type_t type) { g_current_engine = type; }

int kvs_persist_create(void) {
  // In-memory engines are already created by init_kvengine in kvstore.c
  // We just set the default here if needed, or do nothing.
  return 0;
}

void kvs_persist_destroy(void) {
  // Nothing specific to destroy for the generic driver
}

int kvs_persist_set(char *key, char *value) {
  if (!key || !value)
    return -1;

  // 1. Write to WAL
  wal_buffer_append(key, strlen(key), value, strlen(value));

  // 2. Update In-Memory Engine
  switch (g_current_engine) {
  case KVS_ENGINE_HASH:
    return kvs_hash_set(&Hash, key, value);
  case KVS_ENGINE_RBTREE:
    return kvs_rbtree_set(&Tree, key, value);
  case KVS_ENGINE_SKIPLIST:
    return kvs_skiptable_set(&Skiplist, key, value);
  case KVS_ENGINE_BTREE:
    return kvs_btree_set(&Btree, key, value);
  default:
    return -1;
  }
}

char *kvs_persist_get(char *key) {
  if (!key)
    return NULL;

  // Read directly from In-Memory Engine (WAL is for recovery only)
  switch (g_current_engine) {
  case KVS_ENGINE_HASH:
    return kvs_hash_get(&Hash, key);
  case KVS_ENGINE_RBTREE:
    return kvs_rbtree_get(&Tree, key);
  case KVS_ENGINE_SKIPLIST:
    return kvs_skiptable_get(&Skiplist, key);
  case KVS_ENGINE_BTREE:
    return kvs_btree_get(&Btree, key);
  default:
    return NULL;
  }
}

int kvs_persist_delete(char *key) {
  if (!key)
    return -1;

  // 1. Write Tombstone to WAL (empty value)
  wal_buffer_append(key, strlen(key), NULL, 0);

  // 2. Update In-Memory Engine
  switch (g_current_engine) {
  case KVS_ENGINE_HASH:
    return kvs_hash_delete(&Hash, key);
  case KVS_ENGINE_RBTREE:
    return kvs_rbtree_delete(&Tree, key);
  case KVS_ENGINE_SKIPLIST:
    return kvs_skiptable_delete(&Skiplist, key);
  case KVS_ENGINE_BTREE:
    return kvs_btree_delete(&Btree, key);
  default:
    return -1;
  }
}

int kvs_persist_modify(char *key, char *value) {
  // Modify is same as Set for core log-structure
  return kvs_persist_set(key, value);
}

int kvs_persist_count(void) {
  switch (g_current_engine) {
  case KVS_ENGINE_HASH:
    return kvs_hash_count(&Hash);
  case KVS_ENGINE_RBTREE:
    return kvs_rbtree_count(&Tree);
  case KVS_ENGINE_SKIPLIST:
    return kvs_skiptable_count(&Skiplist);
  case KVS_ENGINE_BTREE:
    return kvs_btree_count(&Btree);
  default:
    return 0;
  }
}

// WAL Replay Callback
void kvs_persist_wal_recover(const char *key, size_t key_len, const char *value,
                             size_t value_len, void *user_data) {

  // Convert to null-terminated strings
  char *k = (char *)malloc(key_len + 1);
  memcpy(k, key, key_len);
  k[key_len] = '\0';

  char *v = NULL;
  if (value && value_len > 0) {
    v = (char *)malloc(value_len + 1);
    memcpy(v, value, value_len);
    v[value_len] = '\0';
  }

  // Apply to current engine (bypass WAL logging during recovery)
  if (v) {
    // Set/Modify
    switch (g_current_engine) {
    case KVS_ENGINE_HASH:
      kvs_hash_set(&Hash, k, v);
      break;
    case KVS_ENGINE_RBTREE:
      kvs_rbtree_set(&Tree, k, v);
      break;
    case KVS_ENGINE_SKIPLIST:
      kvs_skiptable_set(&Skiplist, k, v);
      break;
    case KVS_ENGINE_BTREE:
      kvs_btree_set(&Btree, k, v);
      break;
    }
  } else {
    // Delete
    switch (g_current_engine) {
    case KVS_ENGINE_HASH:
      kvs_hash_delete(&Hash, k);
      break;
    case KVS_ENGINE_RBTREE:
      kvs_rbtree_delete(&Tree, k);
      break;
    case KVS_ENGINE_SKIPLIST:
      kvs_skiptable_delete(&Skiplist, k);
      break;
    case KVS_ENGINE_BTREE:
      kvs_btree_delete(&Btree, k);
      break;
    }
  }

  free(k);
  if (v)
    free(v);
}

int kvs_persist_recover(void) {
  // Replay WAL
  int count = wal_buffer_replay(kvs_persist_wal_recover, NULL);
  return count;
}

#endif
