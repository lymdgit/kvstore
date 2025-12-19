#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kvstore.h"

#define MAX_LEVEL		16
#define MAX_KEY_LEN		256
#define MAX_VALUE_LEN	1024

#define ENABLE_KEY_CHAR	1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif

typedef struct _skiplist_node {
    KEY_TYPE key;
    void *value;
    struct _skiplist_node **forward;
} skiplist_node;

typedef struct _skiplist {
    int level;
    struct _skiplist_node *header;
    int count;
} skiplist;

skiplist Skiplist;

static int random_level() {
    int level = 1;
    while ((rand() & 0xFFFF) < (0.5 * 0xFFFF) && level < MAX_LEVEL) {
        level++;
    }
    return level;
}

static skiplist_node *create_node(int level, KEY_TYPE key, void *value) {
    skiplist_node *node = (skiplist_node *)kvstore_malloc(sizeof(skiplist_node));
    if (!node) return NULL;

    node->forward = (skiplist_node **)kvstore_malloc(sizeof(skiplist_node *) * level);
    if (!node->forward) {
        kvstore_free(node);
        return NULL;
    }

#if ENABLE_KEY_CHAR
    node->key = kvstore_malloc(strlen(key) + 1);
    if (!node->key) {
        kvstore_free(node->forward);
        kvstore_free(node);
        return NULL;
    }
    strcpy(node->key, key);

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
    node->key = key;
    node->value = value;
#endif

    return node;
}

int skiplist_init(skiplist *sl) {
    if (!sl) return -1;

    sl->level = 1;
    sl->count = 0;

    sl->header = create_node(MAX_LEVEL, "", NULL);
    if (!sl->header) return -1;

    for (int i = 0; i < MAX_LEVEL; i++) {
        sl->header->forward[i] = NULL;
    }

    srand(time(NULL));
    return 0;
}

void skiplist_destory(skiplist *sl) {
    if (!sl) return;

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

    kvstore_free(sl->header->key);
    if (sl->header->value) {
        kvstore_free(sl->header->value);
    }
    kvstore_free(sl->header->forward);
    kvstore_free(sl->header);
}

skiplist_node *skiplist_search(skiplist *sl, KEY_TYPE key) {
    if (!sl || !key) return NULL;

    skiplist_node *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
#if ENABLE_KEY_CHAR
        while (x->forward[i] != NULL && strcmp(x->forward[i]->key, key) < 0) {
#else
        while (x->forward[i] != NULL && x->forward[i]->key < key) {
#endif
            x = x->forward[i];
        }
    }

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

int skiplist_insert(skiplist *sl, KEY_TYPE key, void *value) {
	if (!sl || !key) return -1;

	skiplist_node *update[MAX_LEVEL];
	skiplist_node *x = sl->header;

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

	x = x->forward[0];

#if ENABLE_KEY_CHAR
	if (x != NULL && strcmp(x->key, key) == 0) {
#else
	if (x != NULL && x->key == key) {
#endif
		return 1; // key already exists
	}

	int level = random_level();

	if (level > sl->level) {
		for (int i = sl->level; i < level; i++) {
			update[i] = sl->header;
		}
		sl->level = level;
	}

	x = create_node(level, key, value);
	if (!x) return -1;

	for (int i = 0; i < level; i++) {
		x->forward[i] = update[i]->forward[i];
		update[i]->forward[i] = x;
	}

	// sl->count++ is now handled in kvs_skiptable_set
	return 0;
}

int skiplist_delete(skiplist *sl, KEY_TYPE key) {
    if (!sl || !key) return -1;

    skiplist_node *update[MAX_LEVEL];
    skiplist_node *x = sl->header;

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

    x = x->forward[0];

#if ENABLE_KEY_CHAR
    if (x == NULL || strcmp(x->key, key) != 0) {
#else
    if (x == NULL || x->key != key) {
#endif
        return -1; // key not found
    }

    for (int i = 0; i < sl->level; i++) {
        if (update[i]->forward[i] != x) {
            break;
        }
        update[i]->forward[i] = x->forward[i];
    }

    // Update the level of the skip list
    while (sl->level > 1 && sl->header->forward[sl->level - 1] == NULL) {
        sl->level--;
    }

    kvstore_free(x->key);
    kvstore_free(x->value);
    kvstore_free(x->forward);
    kvstore_free(x);

    sl->count--;
    return 0;
}

int skiplist_modify(skiplist *sl, KEY_TYPE key, void *value) {
    if (!sl || !key || !value) return -1;

    skiplist_node *node = skiplist_search(sl, key);
    if (!node) {
        return -1; // key not found
    }

    kvstore_free(node->value);

    node->value = kvstore_malloc(strlen((char *)value) + 1);
    if (!node->value) {
        return -1;
    }

    strcpy((char *)node->value, (char *)value);
    return 0;
}

// Skip List API functions
int kvstore_skiptable_create(skiplist *sl) {
    if (!sl) return -1;
    memset(sl, 0, sizeof(skiplist));
    
    return skiplist_init(sl);
}

void kvstore_skiptable_destory(skiplist *sl) {
    if (!sl) return;
    
    skiplist_destory(sl);
    memset(sl, 0, sizeof(skiplist));
}

int kvs_skiptable_set(skiplist *sl, char *key, char *value) {
	if (!sl || !key || !value) return -1;
	
	int res = skiplist_insert(sl, key, value);
	if (res == 0) {
		// New key inserted, increment count
		sl->count++;
	} else if (res == 1) {
		// Key already exists, update it
		return skiplist_modify(sl, key, value);
	}
	return 0;
}

char *kvs_skiptable_get(skiplist *sl, char *key) {
    if (!sl || !key) return NULL;
    
    skiplist_node *node = skiplist_search(sl, key);
    if (!node) {
        return NULL;
    }
    
    return node->value;
}

int kvs_skiptable_delete(skiplist *sl, char *key) {
    if (!sl || !key) return -1;
    
    return skiplist_delete(sl, key);
}

int kvs_skiptable_modify(skiplist *sl, char *key, char *value) {
    if (!sl || !key || !value) return -1;
    
    return skiplist_modify(sl, key, value);
}

int kvs_skiptable_count(skiplist *sl) {
    if (!sl) return 0;
    
    return sl->count;
}