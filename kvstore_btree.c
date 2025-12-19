#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"

#define MAX_KEY_LEN     256
#define MAX_VALUE_LEN   1024
#define DEGREE          3 // B-tree of degree 3 (Min degree t=3, Max keys = 2t-1 = 5)

#define ENABLE_KEY_CHAR 1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif

typedef struct _btree_node {
    int leaf;
    int n;
    KEY_TYPE *keys;
    void **values;
    struct _btree_node **children;
} btree_node;

typedef struct _btree {
    btree_node *root;
    int count;
} btree;

btree Btree;

// --- Helper Functions Declaration ---
static btree_node *create_node(int leaf);
static void split_child(btree_node *x, int i);
static void insert_nonfull(btree_node *x, KEY_TYPE k, void *v);
static void _btree_delete(btree_node *x, KEY_TYPE k);
static void _btree_merge(btree_node *x, int i);
static void _btree_borrow_from_prev(btree_node *x, int i);
static void _btree_borrow_from_next(btree_node *x, int i);
static void _btree_fill(btree_node *x, int i);
static KEY_TYPE _btree_get_pred(btree_node *x, int i);
static KEY_TYPE _btree_get_succ(btree_node *x, int i);

// --- Implementation ---

static btree_node *create_node(int leaf) {
    btree_node *node = (btree_node *)kvstore_malloc(sizeof(btree_node));
    if (!node) return NULL;

    node->leaf = leaf;
    node->n = 0;
    // Keys: max 2*DEGREE - 1
    node->keys = (KEY_TYPE *)kvstore_malloc(sizeof(KEY_TYPE) * (2 * DEGREE - 1));
    // Values: matches keys
    node->values = (void **)kvstore_malloc(sizeof(void *) * (2 * DEGREE - 1));
    // Children: max 2*DEGREE
    node->children = (btree_node **)kvstore_malloc(sizeof(btree_node *) * (2 * DEGREE));

    if (!node->keys || !node->values || !node->children) {
        if (node->keys) kvstore_free(node->keys);
        if (node->values) kvstore_free(node->values);
        if (node->children) kvstore_free(node->children);
        kvstore_free(node);
        return NULL;
    }

    // Initialize pointers to NULL for safety
    memset(node->keys, 0, sizeof(KEY_TYPE) * (2 * DEGREE - 1));
    memset(node->values, 0, sizeof(void *) * (2 * DEGREE - 1));
    memset(node->children, 0, sizeof(btree_node *) * (2 * DEGREE));

    return node;
}

static void split_child(btree_node *x, int i) {
    btree_node *y = x->children[i];
    btree_node *z = create_node(y->leaf);
    z->n = DEGREE - 1;

    // Copy the last (DEGREE-1) keys/values from y to z
    for (int j = 0; j < DEGREE - 1; j++) {
#if ENABLE_KEY_CHAR
        z->keys[j] = y->keys[j + DEGREE]; // Pointer copy is enough during split, ownership transfers
        z->values[j] = y->values[j + DEGREE];
        // Don't free y here, we are moving pointers
#else
        z->keys[j] = y->keys[j + DEGREE];
        z->values[j] = y->values[j + DEGREE];
#endif
    }

    if (!y->leaf) {
        for (int j = 0; j < DEGREE; j++) {
            z->children[j] = y->children[j + DEGREE];
        }
    }

    y->n = DEGREE - 1;

    // Shift children of x
    for (int j = x->n; j >= i + 1; j--) {
        x->children[j + 1] = x->children[j];
    }
    x->children[i + 1] = z;

    // Shift keys of x
    for (int j = x->n - 1; j >= i; j--) {
        x->keys[j + 1] = x->keys[j];
        x->values[j + 1] = x->values[j];
    }

    // Move middle key to x
    x->keys[i] = y->keys[DEGREE - 1];
    x->values[i] = y->values[DEGREE - 1];
    
    x->n++;
}

static void insert_nonfull(btree_node *x, KEY_TYPE k, void *v) {
    int i = x->n - 1;

    if (x->leaf) {
#if ENABLE_KEY_CHAR
        while (i >= 0 && strcmp(k, x->keys[i]) < 0) {
            x->keys[i + 1] = x->keys[i];
            x->values[i + 1] = x->values[i];
            i--;
        }
        
        x->keys[i + 1] = kvstore_malloc(strlen(k) + 1);
        strcpy(x->keys[i + 1], k);
        x->values[i + 1] = kvstore_malloc(strlen((char*)v) + 1);
        strcpy((char*)x->values[i + 1], (char*)v);
#else
        while (i >= 0 && k < x->keys[i]) {
            x->keys[i + 1] = x->keys[i];
            x->values[i + 1] = x->values[i];
            i--;
        }
        x->keys[i + 1] = k;
        x->values[i + 1] = v;
#endif
        x->n++;
    } else {
#if ENABLE_KEY_CHAR
        while (i >= 0 && strcmp(k, x->keys[i]) < 0) {
            i--;
        }
#else
        while (i >= 0 && k < x->keys[i]) {
            i--;
        }
#endif
        i++;
        
        if (x->children[i]->n == 2 * DEGREE - 1) {
            split_child(x, i);
#if ENABLE_KEY_CHAR
            if (strcmp(k, x->keys[i]) > 0) {
                i++;
            }
#else
            if (k > x->keys[i]) {
                i++;
            }
#endif
        }
        insert_nonfull(x->children[i], k, v);
    }
}

// Helper: search
static btree_node *search_node(btree_node *x, KEY_TYPE k, int *idx) {
    int i = 0;
    while (i < x->n && 
#if ENABLE_KEY_CHAR
           strcmp(k, x->keys[i]) > 0
#else
           k > x->keys[i]
#endif
          ) {
        i++;
    }
    
    if (i < x->n && 
#if ENABLE_KEY_CHAR
        strcmp(k, x->keys[i]) == 0
#else
        k == x->keys[i]
#endif
       ) {
        *idx = i;
        return x;
    }
    
    if (x->leaf) return NULL;
    
    return search_node(x->children[i], k, idx);
}


// --- Deletion Helpers ---

// Borrow from prev (left sibling)
static void _btree_borrow_from_prev(btree_node *x, int i) {
    btree_node *child = x->children[i];
    btree_node *sibling = x->children[i - 1];

    // Shift child's keys/values right to make room for 1
    for (int j = child->n - 1; j >= 0; j--) {
        child->keys[j + 1] = child->keys[j];
        child->values[j + 1] = child->values[j];
    }

    if (!child->leaf) {
        for (int j = child->n; j >= 0; j--) {
            child->children[j + 1] = child->children[j];
        }
    }

    // Move parent's key[i-1] to child[0]
    child->keys[0] = x->keys[i - 1];
    child->values[0] = x->values[i - 1];

    if (!child->leaf) {
        child->children[0] = sibling->children[sibling->n];
    }

    // Move sibling's last key to parent
    x->keys[i - 1] = sibling->keys[sibling->n - 1];
    x->values[i - 1] = sibling->values[sibling->n - 1];

    child->n += 1;
    sibling->n -= 1;
}

// Borrow from next (right sibling)
static void _btree_borrow_from_next(btree_node *x, int i) {
    btree_node *child = x->children[i];
    btree_node *sibling = x->children[i + 1];

    // Move parent's key[i] to child's end
    child->keys[child->n] = x->keys[i];
    child->values[child->n] = x->values[i];

    if (!child->leaf) {
        child->children[child->n + 1] = sibling->children[0];
    }

    // Move sibling's first key to parent
    x->keys[i] = sibling->keys[0];
    x->values[i] = sibling->values[0];

    // Shift sibling left
    for (int j = 1; j < sibling->n; j++) {
        sibling->keys[j - 1] = sibling->keys[j];
        sibling->values[j - 1] = sibling->values[j];
    }

    if (!sibling->leaf) {
        for (int j = 1; j <= sibling->n; j++) {
            sibling->children[j - 1] = sibling->children[j];
        }
    }

    child->n += 1;
    sibling->n -= 1;
}

// Merge child[i] and child[i+1]
static void _btree_merge(btree_node *x, int i) {
    btree_node *child = x->children[i];
    btree_node *sibling = x->children[i + 1];

    // Pull down key from parent
    child->keys[DEGREE - 1] = x->keys[i];
    child->values[DEGREE - 1] = x->values[i];

    // Copy keys/values from sibling to child
    for (int j = 0; j < sibling->n; j++) {
        child->keys[j + DEGREE] = sibling->keys[j];
        child->values[j + DEGREE] = sibling->values[j];
    }

    // Copy children pointers
    if (!child->leaf) {
        for (int j = 0; j <= sibling->n; j++) {
            child->children[j + DEGREE] = sibling->children[j];
        }
    }

    // Shift parent keys/children left
    for (int j = i + 1; j < x->n; j++) {
        x->keys[j - 1] = x->keys[j];
        x->values[j - 1] = x->values[j];
    }

    for (int j = i + 2; j <= x->n; j++) {
        x->children[j - 1] = x->children[j];
    }

    child->n += sibling->n + 1;
    x->n--;

    // Free sibling struct (keys moved, so just free container)
    kvstore_free(sibling->keys);
    kvstore_free(sibling->values);
    kvstore_free(sibling->children);
    kvstore_free(sibling);
}

static void _btree_fill(btree_node *x, int i) {
    if (i != 0 && x->children[i - 1]->n >= DEGREE) {
        _btree_borrow_from_prev(x, i);
    } else if (i != x->n && x->children[i + 1]->n >= DEGREE) {
        _btree_borrow_from_next(x, i);
    } else {
        if (i != x->n) {
            _btree_merge(x, i);
        } else {
            _btree_merge(x, i - 1);
        }
    }
}

// Get predecessor key (rightmost key of left child)
static KEY_TYPE _btree_get_pred(btree_node *x, int i) {
    btree_node *cur = x->children[i];
    while (!cur->leaf) {
        cur = cur->children[cur->n];
    }
    // Return a copy because the original might be freed or moved
#if ENABLE_KEY_CHAR
    char *ret = kvstore_malloc(strlen(cur->keys[cur->n - 1]) + 1);
    strcpy(ret, cur->keys[cur->n - 1]);
    return ret;
#else
    return cur->keys[cur->n - 1];
#endif
}

// Get successor key (leftmost key of right child)
static KEY_TYPE _btree_get_succ(btree_node *x, int i) {
    btree_node *cur = x->children[i + 1];
    while (!cur->leaf) {
        cur = cur->children[0];
    }
#if ENABLE_KEY_CHAR
    char *ret = kvstore_malloc(strlen(cur->keys[0]) + 1);
    strcpy(ret, cur->keys[0]);
    return ret;
#else
    return cur->keys[0];
#endif
}

static void _btree_delete(btree_node *x, KEY_TYPE k) {
    int i = 0;
    while (i < x->n && 
#if ENABLE_KEY_CHAR
           strcmp(k, x->keys[i]) > 0
#else
           k > x->keys[i]
#endif
          ) {
        i++;
    }

    // Case 1: Key found in current node x
    if (i < x->n && 
#if ENABLE_KEY_CHAR
        strcmp(k, x->keys[i]) == 0
#else
        k == x->keys[i]
#endif
       ) {
        
        if (x->leaf) {
            // Case 1a: x is leaf -> just delete
#if ENABLE_KEY_CHAR
            kvstore_free(x->keys[i]);
            kvstore_free(x->values[i]);
#endif
            for (int j = i + 1; j < x->n; j++) {
                x->keys[j - 1] = x->keys[j];
                x->values[j - 1] = x->values[j];
            }
            x->n--;
        } else {
            // Case 1b: x is internal node
            KEY_TYPE k_copy = x->keys[i]; // Alias for clarity

            if (x->children[i]->n >= DEGREE) {
                // Predecessor is abundant
                KEY_TYPE pred = _btree_get_pred(x, i);
                
                // Replace k with pred
#if ENABLE_KEY_CHAR
                kvstore_free(x->keys[i]); // Free old key
                // Note: values need to be updated too, but we usually store value with key.
                // In a true key-value store, we should probably fetch the predecessor's VALUE too.
                // For simplicity here, assuming we delete the key and 'swap' logic handles it recursively.
                // Wait! B-tree deletion via predecessor swap: we replace key[i] with pred, 
                // then recursively delete pred from child[i].
                // We MUST update the value associated with key[i] to be the value of pred!
                
                // Correction: Get predecessor value as well
                btree_node *curr = x->children[i];
                while (!curr->leaf) curr = curr->children[curr->n];
                void *pred_val_ptr = curr->values[curr->n - 1];
                void *pred_val = kvstore_malloc(strlen((char*)pred_val_ptr) + 1);
                strcpy((char*)pred_val, (char*)pred_val_ptr);
                
                kvstore_free(x->values[i]);
                x->keys[i] = pred; // Owned by x now
                x->values[i] = pred_val;
#else
                x->keys[i] = pred;
                // Value copy logic needed for int keys too if values are pointers
#endif
                _btree_delete(x->children[i], pred);
#if ENABLE_KEY_CHAR
                // The _btree_get_pred returned a malloc'd copy, which is now in x->keys[i].
                // The recursive delete removed the original from the leaf.
                // Correct.
#endif

            } else if (x->children[i + 1]->n >= DEGREE) {
                // Successor is abundant
                KEY_TYPE succ = _btree_get_succ(x, i);
                
                // Get successor value
                btree_node *curr = x->children[i+1];
                while (!curr->leaf) curr = curr->children[0];
#if ENABLE_KEY_CHAR
                void *succ_val_ptr = curr->values[0];
                void *succ_val = kvstore_malloc(strlen((char*)succ_val_ptr) + 1);
                strcpy((char*)succ_val, (char*)succ_val_ptr);

                kvstore_free(x->keys[i]);
                kvstore_free(x->values[i]);
                x->keys[i] = succ;
                x->values[i] = succ_val;
#else
                x->keys[i] = succ;
#endif
                _btree_delete(x->children[i + 1], succ);

            } else {
                // Both children have DEGREE-1 keys. Merge them.
                _btree_merge(x, i);
                // Now key k is in x->children[i] (which is the merged node)
                _btree_delete(x->children[i], k);
            }
        }
    } else {
        // Case 2: Key not found in x. Determine which subtree to descend.
        if (x->leaf) {
            // Key not in tree.
            // This should have been caught by the API wrapper kvs_btree_delete checking existence first.
            return;
        }

        int flag = (i == x->n) ? 1 : 0;

        if (x->children[i]->n < DEGREE) {
            _btree_fill(x, i);
        }

        // If the last child was merged, it merged into the previous child (index i-1)
        if (flag && i > x->n) {
            _btree_delete(x->children[i - 1], k);
        } else {
            _btree_delete(x->children[i], k);
        }
    }
}

// --- API Implementation ---

int kvstore_btree_create(btree *tree) {
    if (!tree) return -1;
    memset(tree, 0, sizeof(btree));
    
    tree->root = create_node(1); // Root starts as leaf
    if (!tree->root) return -1;
    
    return 0;
}

static void _btree_destory_node(btree_node *node) {
    if (!node) return;
    
    if (!node->leaf) {
        for (int i = 0; i <= node->n; i++) {
            _btree_destory_node(node->children[i]);
        }
    }
    
    for (int i = 0; i < node->n; i++) {
#if ENABLE_KEY_CHAR
        kvstore_free(node->keys[i]);
        kvstore_free(node->values[i]);
#endif
    }
    
    kvstore_free(node->keys);
    kvstore_free(node->values);
    kvstore_free(node->children);
    kvstore_free(node);
}

void kvstore_btree_destory(btree *tree) {
    if (!tree || !tree->root) return;
    
    _btree_destory_node(tree->root);
    tree->root = NULL;
    tree->count = 0;
}

int kvs_btree_set(btree *tree, char *key, char *value) {
    if (!tree || !key || !value) return -1;

    // Check update
    char *existing = kvs_btree_get(tree, key);
    if (existing) {
        return kvs_btree_modify(tree, key, value);
    }

    btree_node *r = tree->root;
    if (r->n == 2 * DEGREE - 1) {
        btree_node *s = create_node(0);
        tree->root = s;
        s->children[0] = r;
        split_child(s, 0);
        insert_nonfull(s, key, value);
    } else {
        insert_nonfull(r, key, value);
    }

    tree->count++;
    return 0;
}

char *kvs_btree_get(btree *tree, char *key) {
    if (!tree || !tree->root || !key) return NULL;
    
    int idx = 0;
    btree_node *node = search_node(tree->root, key, &idx);
    
    if (node) {
        return node->values[idx];
    }
    return NULL;
}

int kvs_btree_modify(btree *tree, char *key, char *value) {
    if (!tree || !key || !value) return -1;

    int idx = 0;
    btree_node *node = search_node(tree->root, key, &idx);

    if (node) {
#if ENABLE_KEY_CHAR
        kvstore_free(node->values[idx]);
        node->values[idx] = kvstore_malloc(strlen(value) + 1);
        strcpy((char*)node->values[idx], value);
#else
        // If int keys and pointer values, update pointer
        node->values[idx] = value;
#endif
        return 0;
    }
    return -1;
}

int kvs_btree_delete(btree *tree, char *key) {
    if (!tree || !tree->root || !key) return -1;

    // Must check existence first, because _btree_delete logic assumes 
    // it will find the key if it descends into the tree, or handles "not found" silently.
    if (kvs_btree_get(tree, key) == NULL) {
        return -1; // Not found
    }

    _btree_delete(tree->root, key);

    // If root becomes empty (but not NULL, meaning it was internal and merged), replace it
    if (tree->root->n == 0) {
        btree_node *tmp = tree->root;
        if (tree->root->leaf) {
            // Tree is now completely empty
            // Keep the empty root node to act as a placeholder for next insert
            // or free it? Usually we keep an empty leaf as root for a clear tree.
            // But if we free it, insert must handle null root.
            // Let's keep it as an empty leaf.
        } else {
            tree->root = tree->root->children[0];
            
            kvstore_free(tmp->keys);
            kvstore_free(tmp->values);
            kvstore_free(tmp->children);
            kvstore_free(tmp);
        }
    }

    tree->count--;
    return 0;
}

int kvs_btree_count(btree *tree) {
    return tree ? tree->count : 0;
}