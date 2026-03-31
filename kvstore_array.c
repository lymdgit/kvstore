
#include "kvstore.h"
#include <stdlib.h>
#include <string.h>

array_t Array;

// create
int kvstore_array_create(array_t *arr) {

  if (!arr)
    return -1;

  arr->array_table =
      kvstore_malloc(KVS_ARRAY_SIZE * sizeof(struct kvs_array_item));
  if (!arr->array_table) {
    return -1;
  }
  memset(arr->array_table, 0, KVS_ARRAY_SIZE * sizeof(struct kvs_array_item));

  arr->array_idx = 0;

  return 0;
}

// destory
void kvstore_array_destory(array_t *arr) {

  if (!arr)
    return;

  if (arr->array_table)
    kvstore_free(arr->array_table);
}

int kvs_array_set(array_t *arr, char *key, char *value) {

  if (arr == NULL || key == NULL || value == NULL)
    return -1;

  if (arr->array_idx > KVS_ARRAY_SIZE) { // Safety check
    return -1;
  }

  // Pre-allocate new value/key (can be optimized but keeping style)
  char *kcopy = kvstore_malloc(strlen(key) + 1);
  if (kcopy == NULL)
    return -1;
  strcpy(kcopy, key);

  char *vcopy = kvstore_malloc(strlen(value) + 1);
  if (vcopy == NULL) {
    kvstore_free(kcopy);
    return -1;
  }
  strcpy(vcopy, value);

  int i = 0;
  int empty_idx = -1;

  // Scan for existence or empty slot
  for (i = 0; i < arr->array_idx && i < KVS_ARRAY_SIZE; i++) {
    if (arr->array_table[i].key == NULL) {
      if (empty_idx == -1)
        empty_idx = i;
      continue;
    }
    // Found existing key -> Update
    if (strcmp(arr->array_table[i].key, key) == 0) {
      kvstore_free(arr->array_table[i].value); // Free old value
      arr->array_table[i].value = vcopy;

      // We don't need kcopy since key is same
      kvstore_free(kcopy);
      return 0;
    }
  }

  // Not found. Insert.

  // 1. Reuse empty slot if available
  if (empty_idx != -1) {
    arr->array_table[empty_idx].key = kcopy;
    arr->array_table[empty_idx].value = vcopy;
    // Do NOT increment array_idx
    return 0;
  }

  // 2. Append if space available
  if (arr->array_idx < KVS_ARRAY_SIZE) {
    arr->array_table[arr->array_idx].key = kcopy;
    arr->array_table[arr->array_idx].value = vcopy;
    arr->array_idx++;
    return 0;
  }

  // Full
  kvstore_free(kcopy);
  kvstore_free(vcopy);
  return -1;
}

char *kvs_array_get(array_t *arr, char *key) {

  int i = 0;
  if (arr == NULL || key == NULL)
    return NULL;

  for (i = 0; i < arr->array_idx && i < KVS_ARRAY_SIZE; i++) {
    if (arr->array_table[i].key == NULL) {
      continue; // Skip empty slots
    }
    if (strcmp(arr->array_table[i].key, key) == 0) {
      return arr->array_table[i].value;
    }
  }

  return NULL;
}

// i > 0 : no exist
int kvs_array_delete(array_t *arr, char *key) {

  int i = 0;
  if (arr == NULL || key == NULL)
    return -1;

  for (i = 0; i < arr->array_idx && i < KVS_ARRAY_SIZE; i++) {

    // 跳过已删除的空槽位
    if (arr->array_table[i].key == NULL) {
      continue;
    }

    if (strcmp(arr->array_table[i].key, key) == 0) {

      kvstore_free(arr->array_table[i].value);
      arr->array_table[i].value = NULL;

      kvstore_free(arr->array_table[i].key);
      arr->array_table[i].key = NULL;

      return 0;
    }
  }

  return 1;
}

int kvs_array_modify(array_t *arr, char *key, char *value) {

  int i = 0;
  if (arr == NULL || key == NULL || value == NULL)
    return -1;

  for (i = 0; i < arr->array_idx && i < KVS_ARRAY_SIZE; i++) {

    // 跳过已删除的空槽位
    if (arr->array_table[i].key == NULL) {
      continue;
    }

    if (strcmp(arr->array_table[i].key, key) == 0) {

      kvstore_free(arr->array_table[i].value);
      arr->array_table[i].value = NULL;

      char *vcopy = kvstore_malloc(strlen(value) + 1);
      if (vcopy == NULL)
        return -1;
      strcpy(vcopy, value);

      arr->array_table[i].value = vcopy;

      return 0;
    }
  }

  return 1;
}

int kvs_array_count(array_t *arr) {
  if (!arr)
    return -1;

  // Bug fix: 遍历计数实际有效元素，而非返回高水位线
  // 因为 delete 操作将 key 设为 NULL 但不减少 array_idx
  int count = 0;
  for (int i = 0; i < arr->array_idx && i < KVS_ARRAY_SIZE; i++) {
    if (arr->array_table[i].key != NULL) {
      count++;
    }
  }
  return count;
}
