#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kvstore.h"

/* --- Configuration and Constants --- */
#define MAX_LEVEL 16       // Maximum height of the skip list
#define MAX_KEY_LEN 256    // Maximum length of the key
#define MAX_VALUE_LEN 1024 // Maximum length of the value

#define ENABLE_KEY_CHAR 1 // Toggle for key type: 1 for string, 0 for integer

/* Define KEY_TYPE based on configuration */
#if ENABLE_KEY_CHAR
typedef char *KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif

/**
 * Skip List Node Structure
 */
typedef struct _skiplist_node {
  KEY_TYPE key; // Key of the node
  void *value;  // Value pointer (can store string data)
  struct _skiplist_node *
      *forward; // Array of forward pointers for different levels
} skiplist_node;

/**
 * Skip List Control Structure
 */
typedef struct _skiplist {
  int level; // Current maximum level of the entire skip list
  struct _skiplist_node *header; // Pointer to the dummy head node
  int count;                     // Total number of nodes in the skip list
} skiplist;

skiplist Skiplist;

/**
 * Generates a random level for a new node using a geometric distribution.
 * P = 0.5 that the level will be increased.
 */
static int random_level() {
  int level = 1;
  while ((rand() & 0xFFFF) < (0.5 * 0xFFFF) && level < MAX_LEVEL) {
    level++;
  }
  return level;
}

/**
 * Creates and initializes a new skip list node.
 * Allocates memory for the node, its forward pointers, and copies key/value if
 * needed.
 */
static skiplist_node *create_node(int level, KEY_TYPE key, void *value) {
  skiplist_node *node = (skiplist_node *)kvstore_malloc(sizeof(skiplist_node));
  if (!node)
    return NULL;

  // Allocate array of forward pointers for the given level height
  node->forward =
      (skiplist_node **)kvstore_malloc(sizeof(skiplist_node *) * level);
  if (!node->forward) {
    kvstore_free(node);
    return NULL;
  }

#if ENABLE_KEY_CHAR
  // For string keys: allocate memory and copy the string
  node->key = kvstore_malloc(strlen(key) + 1);
  if (!node->key) {
    kvstore_free(node->forward);
    kvstore_free(node);
    return NULL;
  }
  strcpy(node->key, key);

  // For string values: allocate memory and copy if value is not NULL
  if (value) {
    node->value = kvstore_malloc(strlen((char *)value) + 1);
    if (!node->value) {
      kvstore_free(node->key);
      kvstore_free(node->forward);
      kvstore_free(node);
      return NULL;
    }
    strcpy((char *)node->value, (char *)value);
  } else {
    node->value = NULL;
  }
#else
  // For non-string keys/values (e.g. integer), direct assignment
  node->key = key;
  node->value = value;
#endif

  return node;
}

/**
 * Initializes the skip list structure.
 * Sets initial level, count, and creates the header node.
 */
int skiplist_init(skiplist *sl) {
  if (!sl)
    return -1;

  sl->level = 1;
  sl->count = 0;

  // Create header with MAX_LEVEL height to serve as starting point for all
  // levels
  sl->header = create_node(MAX_LEVEL, "", NULL);
  if (!sl->header)
    return -1;

  for (int i = 0; i < MAX_LEVEL; i++) {
    sl->header->forward[i] = NULL;
  }

  srand(time(NULL));
  return 0;
}

/**
 * Destroys the skip list and frees all allocated memory.
 */
void skiplist_destory(skiplist *sl) {
  if (!sl)
    return;

  // Traverse the base level (level 0) and free all nodes
  skiplist_node *node = sl->header->forward[0];
  while (node != NULL) {
    skiplist_node *tmp = node;
    node = node->forward[0];

    kvstore_free(tmp->key);
    if (tmp->value) {
      kvstore_free(tmp->value);
    }
    kvstore_free(tmp->forward);
    kvstore_free(tmp);
  }

  // Free the header node
  kvstore_free(sl->header->key);
  if (sl->header->value) {
    kvstore_free(sl->header->value);
  }
  kvstore_free(sl->header->forward);
  kvstore_free(sl->header);
}

/**
 * Searches for a node with the given key.
 * Strategy: Start from top level and jump down when next node's key is larger.
 * Returns: pointer to the node if found, NULL otherwise.
 */
skiplist_node *skiplist_search(skiplist *sl, KEY_TYPE key) {
  if (!sl || !key)
    return NULL;

  skiplist_node *x = sl->header;
  // Iterate from current highest level down to level 0
  for (int i = sl->level - 1; i >= 0; i--) {
#if ENABLE_KEY_CHAR
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
#else
    while (x->forward[i] != NULL && x->forward[i]->key < key) {
#endif
      // 只要下个节点存在，且下个节点的值 < 目标值，就往右走
      x = x->forward[i];
    }
  }

  // Move to the next node at level 0 (the potential match)
  //
  x = x->forward[0];

#if ENABLE_KEY_CHAR
  if (x != NULL && strcmp(x->key, key) == 0) {
#else
  if (x != NULL && x->key == key) {
#endif
    return x;
  }

  return NULL;
}

/**
 * Inserts a new key-value pair into the skip list.
 * Returns: 0 on success, 1 if key exists, -1 on failure.
 */
int skiplist_insert(skiplist *sl, KEY_TYPE key, void *value) {
  if (!sl || !key)
    return -1;

  // update array stores the nodes that will be before the new node at each
  // level
  skiplist_node *update[MAX_LEVEL];
  skiplist_node *x = sl->header;

  // Step 1: Find the insertion position at each level
  // Traverse from the highest level down to level 0.
  // At each level, move forward as long as the next node's key is less than the
  // new key. Store the last node visited at each level in the 'update' array.
  // These 'update' nodes will be the predecessors of the new node.
  for (int i = sl->level - 1; i >= 0; i--) {
#if ENABLE_KEY_CHAR
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
#else
    while (x->forward[i] != NULL && x->forward[i]->key < key) {
#endif
      x = x->forward[i];
    }
    update[i] = x;
  }

  // Move to the next node at level 0. This is the potential insertion point or
  // existing node.
  x = x->forward[0];

  // Step 2: Check if the key already exists
  // If x is not NULL and its key matches the new key, the key already exists.
#if ENABLE_KEY_CHAR
  if (x != NULL && strcmp(x->key, key) == 0) {
#else
  if (x != NULL && x->key == key) {
#endif
    return 1; // key already exists
  }

  // Step 3: Determine the level for the new node
  // Generate a random level for the new node.
  int level = random_level();

  // If the new node's level is higher than the current maximum level of the
  // skip list, extend the 'update' array to point to the header for these new
  // higher levels. This ensures the header's forward pointers are correctly set
  // for the new levels.
  if (level > sl->level) {
    for (int i = sl->level; i < level; i++) {
      update[i] = sl->header;
    }
    sl->level = level; // Update the skip list's maximum level
  }

  // Step 4: Create new node and update pointers
  // Allocate and initialize the new node with the generated level, key, and
  // value.
  x = create_node(level, key, value);
  if (!x)
    return -1;

  // Link the new node into each level's list up to its randomly determined
  // level. For each level 'i' from 0 up to 'level-1':
  // 1. The new node's forward[i] pointer points to the node that 'update[i]'
  // previously pointed to.
  // 2. The 'update[i]' node's forward[i] pointer is updated to point to the new
  // node.
  for (int i = 0; i < level; i++) {
    x->forward[i] = update[i]->forward[i];
    update[i]->forward[i] = x;
  }

  // sl->count++ is now handled in kvs_skiptable_set
  return 0;
}

/**
 * Deletes a key from the skip list.
 * Returns: 0 on success, -1 if key not found.
 */
int skiplist_delete(skiplist *sl, KEY_TYPE key) {
  if (!sl || !key)
    return -1;

  skiplist_node *update[MAX_LEVEL];
  skiplist_node *x = sl->header;

  // Step 1: Find the node to be deleted and track predecessors
  // Traverse from the highest level down to level 0.
  // At each level, move forward as long as the next node's key is less than the
  // key to be deleted. Store the last node visited at each level in the
  // 'update' array. These 'update' nodes will be the predecessors of the node
  // to be deleted.
  for (int i = sl->level - 1; i >= 0; i--) {
#if ENABLE_KEY_CHAR
    while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
#else
    while (x->forward[i] != NULL && x->forward[i]->key < key) {
#endif
      x = x->forward[i];
    }
    update[i] = x;
  }

  // Move to the next node at level 0. This is the potential node to be deleted.
  x = x->forward[0];

  // Step 2: Check if key exists
  // If x is NULL or its key does not match the key to be deleted, the key was
  // not found.
#if ENABLE_KEY_CHAR
  if (x == NULL || strcmp(x->key, key) != 0) {
#else
  if (x == NULL || x->key != key) {
#endif
    return -1; // key not found
  }

  // Step 3: Remove the node from each level
  // For each level 'i' from 0 up to the current max level of the skip list:
  // If the 'update[i]' node's forward[i] pointer points to 'x' (the node to be
  // deleted), then bypass 'x' by making 'update[i]->forward[i]' point to
  // 'x->forward[i]'. If 'update[i]->forward[i]' is not 'x', it means 'x' was
  // not present at this level (or we've already processed all levels 'x' was
  // on), so we can break.
  for (int i = 0; i < sl->level; i++) {
    if (update[i]->forward[i] != x) {
      break;
    }
    update[i]->forward[i] = x->forward[i];
  }

  // Step 4: Update the max level of the skip list if it has shrunk
  // After deletion, check if the highest levels of the skip list have become
  // empty. If the header's forward pointer at a certain level is NULL, it means
  // that level is now empty. Decrement the skip list's max level until a
  // non-empty level is found or the level becomes 1.
  while (sl->level > 1 && sl->header->forward[sl->level - 1] == NULL) {
    sl->level--;
  }

  // Step 5: Free node memory
  // Free the memory associated with the deleted node: its key, value, forward
  // pointers array, and the node itself.
  kvstore_free(x->key);
  kvstore_free(x->value);
  kvstore_free(x->forward);
  kvstore_free(x);

  sl->count--; // Decrement the total count of nodes in the skip list
  return 0;
}

/**
 * Modifies the value of an existing key.
 * Strategy: Search for node and update its value.
 */
int skiplist_modify(skiplist *sl, KEY_TYPE key, void *value) {
  if (!sl || !key || !value)
    return -1;

  // Step 1: Search for the node with the given key.
  skiplist_node *node = skiplist_search(sl, key);
  if (!node) {
    return -1; // key not found, cannot modify
  }

  // Step 2: Free the memory of the old value.
  // It's important to free the old value to prevent memory leaks,
  // especially if the old value was dynamically allocated (as is the case for
  // string values).
  kvstore_free(node->value);

  // Step 3: Allocate new memory for the updated value.
  // For string values, allocate enough space for the new string plus a null
  // terminator.
  node->value = kvstore_malloc(strlen((char *)value) + 1);
  if (!node->value) {
    // If allocation fails, the node now has a NULL value pointer, which might
    // be problematic. Depending on error handling strategy, one might want to
    // restore the old value or mark the node as invalid. For simplicity, we
    // return -1 here.
    return -1;
  }

  // Step 4: Copy the new value into the allocated memory.
  strcpy((char *)node->value, (char *)value);
  return 0; // Modification successful
}

/* --- Skip List Public API functions --- */

/**
 * Wrapper for initializing a skip list.
 */
int kvstore_skiptable_create(skiplist *sl) {
  if (!sl)
    return -1;
  memset(sl, 0, sizeof(skiplist));

  return skiplist_init(sl);
}

/**
 * Wrapper for destroying a skip list.
 */
void kvstore_skiptable_destory(skiplist *sl) {
  if (!sl)
    return;

  skiplist_destory(sl);
  memset(sl, 0, sizeof(skiplist));
}

/**
 * High-level SET operation.
 * If key doesn't exist, inserts it.
 * If key exists, modifies its value.
 */
int kvs_skiptable_set(skiplist *sl, char *key, char *value) {
  if (!sl || !key || !value)
    return -1;

  int res = skiplist_insert(sl, key, value);
  if (res == 0) {
    // New key inserted, increment count
    sl->count++;
  } else if (res == 1) {
    // Key already exists, update its value
    return skiplist_modify(sl, key, value);
  }
  return 0;
}

/**
 * High-level GET operation.
 * Returns: pointer to the value string, or NULL if not found.
 */
char *kvs_skiptable_get(skiplist *sl, char *key) {
  if (!sl || !key)
    return NULL;

  skiplist_node *node = skiplist_search(sl, key);
  if (!node) {
    return NULL;
  }

  return node->value;
}

/**
 * High-level DELETE operation.
 */
int kvs_skiptable_delete(skiplist *sl, char *key) {
  if (!sl || !key)
    return -1;

  return skiplist_delete(sl, key);
}

/**
 * High-level MODIFY operation.
 */
int kvs_skiptable_modify(skiplist *sl, char *key, char *value) {
  if (!sl || !key || !value)
    return -1;

  return skiplist_modify(sl, key, value);
}

/**
 * Returns the total number of elements in the skip list.
 */
int kvs_skiptable_count(skiplist *sl) {
  if (!sl)
    return 0;

  return sl->count;
}