#include "kvstore.h"
#include <string.h>

#if ENABLE_PERSISTENCE
#include "wal_buffer.h"
#include "wal_flusher.h"
#endif

#define KVSTORE_MAX_TOKENS 128

const char *commands[] = {
    "SET",    "GET",    "DEL",  "MOD",    "COUNT", "RSET",   "RGET",   "RDEL",
    "RMOD",   "RCOUNT", "HSET", "HGET",   "HDEL",  "HMOD",   "HCOUNT", "SSET",
    "SGET",   "SDEL",   "SMOD", "SCOUNT", "BSET",  "BGET",   "BDEL",   "BMOD",
    "BCOUNT", "PSET",   "PGET", "PDEL",   "PMOD",  "PCOUNT", // 持久化命令
};

enum {
  KVS_CMD_START = 0,
  KVS_CMD_SET = KVS_CMD_START,
  KVS_CMD_GET,
  KVS_CMD_DEL,
  KVS_CMD_MOD,
  KVS_CMD_COUNT,

  KVS_CMD_RSET,
  KVS_CMD_RGET,
  KVS_CMD_RDEL,
  KVS_CMD_RMOD,
  KVS_CMD_RCOUNT,

  KVS_CMD_HSET,
  KVS_CMD_HGET,
  KVS_CMD_HDEL,
  KVS_CMD_HMOD,
  KVS_CMD_HCOUNT,

  KVS_CMD_SSET,
  KVS_CMD_SGET,
  KVS_CMD_SDEL,
  KVS_CMD_SMOD,
  KVS_CMD_SCOUNT,

  KVS_CMD_BSET,
  KVS_CMD_BGET,
  KVS_CMD_BDEL,
  KVS_CMD_BMOD,
  KVS_CMD_BCOUNT,

  // Persistence commands (PSET/PGET/PDEL/PMOD/PCOUNT)
  KVS_CMD_PSET,
  KVS_CMD_PGET,
  KVS_CMD_PDEL,
  KVS_CMD_PMOD,
  KVS_CMD_PCOUNT,

  KVS_CMD_SIZE,
};

// 内存分配
void *kvstore_malloc(size_t size) {
#if ENABLE_MEM_POOL
  return slab_alloc(size);
#else
  return malloc(size);
#endif
}

void kvstore_free(void *ptr) {
#if ENABLE_MEM_POOL
  // 使用 slab_free_ptr 自动判断 ptr 是 slab 分配还是 malloc 分配
  if (ptr)
    slab_free_ptr(ptr);
#else
  free(ptr);
#endif
}

// hash
#if ENABLE_HASH_KVENGINE

int kvstore_hash_set(char *key, char *value) {
  return kvs_hash_set(&Hash, key, value);
}
char *kvstore_hash_get(char *key) { return kvs_hash_get(&Hash, key); }
int kvstore_hash_delete(char *key) { return kvs_hash_delete(&Hash, key); }
int kvstore_hash_modify(char *key, char *value) {
  return kvs_hash_modify(&Hash, key, value);
}
int kvstore_hash_count(void) { return kvs_hash_count(&Hash); }

#endif

// skip list
#if ENABLE_SKIPTABLE_KVENGINE

int kvstore_skiptable_set(char *key, char *value) {
  return kvs_skiptable_set(&Skiplist, key, value);
}
char *kvstore_skiptable_get(char *key) {
  return kvs_skiptable_get(&Skiplist, key);
}
int kvstore_skiptable_delete(char *key) {
  return kvs_skiptable_delete(&Skiplist, key);
}
int kvstore_skiptable_modify(char *key, char *value) {
  return kvs_skiptable_modify(&Skiplist, key, value);
}
int kvstore_skiptable_count(void) { return kvs_skiptable_count(&Skiplist); }

#endif
// btree
#if ENABLE_BTREE_KVENGINE

int kvstore_btree_set(char *key, char *value) {
  return kvs_btree_set(&Btree, key, value);
}
char *kvstore_btree_get(char *key) { return kvs_btree_get(&Btree, key); }
int kvstore_btree_delete(char *key) { return kvs_btree_delete(&Btree, key); }
int kvstore_btree_modify(char *key, char *value) {
  return kvs_btree_modify(&Btree, key, value);
}
int kvstore_btree_count(void) { return kvs_btree_count(&Btree); }

#endif

#if ENABLE_RBTREE_KVENGINE

// red black tree
int kvstore_rbtree_set(char *key, char *value) {
  return kvs_rbtree_set(&Tree, key, value);
}

char *kvstore_rbtree_get(char *key) { return kvs_rbtree_get(&Tree, key); }
int kvstore_rbtree_delete(char *key) { return kvs_rbtree_delete(&Tree, key); }
int kvstore_rbtree_modify(char *key, char *value) {
  return kvs_rbtree_modify(&Tree, key, value);
}

int kvstore_rbtree_count(void) { return kvs_rbtree_count(&Tree); }

#endif

#if ENABLE_ARRAY_KVENGINE
// array
int kvstore_array_set(char *key, char *value) {
  return kvs_array_set(&Array, key, value);
}
char *kvstore_array_get(char *key) { return kvs_array_get(&Array, key); }
int kvstore_array_delete(char *key) { return kvs_array_delete(&Array, key); }
int kvstore_array_modify(char *key, char *value) {
  return kvs_array_modify(&Array, key, value);
}
int kvstore_array_count(void) { return kvs_array_count(&Array); }

#endif

// 对接收到信息进行按照空格进行分割
int kvstore_split_token(char *msg, char **tokens) {

  if (msg == NULL || tokens == NULL)
    return -1;

  int idx = 0;

  char *token = strtok(msg, " ");

  while (token != NULL) {
    tokens[idx++] = token;
    token = strtok(NULL, " ");
  }
  return idx;
}

// 对分割好的信息进行解析
int kvstore_parser_protocol(struct conn_item *item, char **tokens, int count) {

  if (item == NULL || count == 0 || tokens == NULL || tokens[0] == NULL) {
    if (item != NULL) {
      memset(item->wbuffer, 0, BUFFER_LENGTH);
      snprintf(item->wbuffer, BUFFER_LENGTH, "ERROR: empty command");
    }
    return -1;
  }

  int cmd = KVS_CMD_START;
  // 看一下是不是我们协议中的命令
  for (cmd = KVS_CMD_START; cmd < KVS_CMD_SIZE; cmd++) {
    // printf("cmd: %s, %s, %ld, %ld\n",commands[cmd], tokens[0],
    // strlen(commands[cmd]), strlen(tokens[0]));
    if (strcmp(commands[cmd], tokens[0]) == 0) {
      break;
    }
  }
  // 匹配到了支持的命令
  char *msg = item->wbuffer;
  char *key = tokens[1];
  char *value = tokens[2];
  memset(msg, 0, BUFFER_LENGTH);
  // 看看是什么那种数组结构的命令，根据命令处理
  switch (cmd) {
  // array
  case KVS_CMD_SET: {
    int res = kvstore_array_set(key, value);
    if (!res) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    // LOG("set: %d\n", res);

    break;
  }
  case KVS_CMD_GET: {
    char *val = kvstore_array_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    // printf("get: %s\n", val);

    break;
  }
  case KVS_CMD_DEL: {
    // printf("del\n");

    int res = kvstore_array_delete(key);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }
  case KVS_CMD_MOD: {
    // printf("mod\n");

    int res = kvstore_array_modify(key, value);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }

  case KVS_CMD_COUNT: {
    int count = kvstore_array_count();
    if (count < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }

  // rbtree
  case KVS_CMD_RSET: {

    int res = kvstore_rbtree_set(key, value);
    if (!res) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    break;
  }
  case KVS_CMD_RGET: {

    char *val = kvstore_rbtree_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }
  case KVS_CMD_RDEL: {

    int res = kvstore_rbtree_delete(key);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }
  case KVS_CMD_RMOD: {

    int res = kvstore_rbtree_modify(key, value);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }

  case KVS_CMD_RCOUNT: {
    int count = kvstore_rbtree_count();
    if (count < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }

  case KVS_CMD_HSET: {

    int res = kvstore_hash_set(key, value);
    if (!res) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    break;
  }
  // hash
  case KVS_CMD_HGET: {

    char *val = kvstore_hash_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }
  case KVS_CMD_HDEL: {

    int res = kvstore_hash_delete(key);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }
  case KVS_CMD_HMOD: {

    int res = kvstore_hash_modify(key, value);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }

    break;
  }

  case KVS_CMD_HCOUNT: {
    int count = kvstore_hash_count();
    if (count < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }

  // skip list
  case KVS_CMD_SSET: {
    int res = kvstore_skiptable_set(key, value);
    if (!res) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    break;
  }
  case KVS_CMD_SGET: {
    char *val = kvstore_skiptable_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_SDEL: {
    int res = kvstore_skiptable_delete(key);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_SMOD: {
    int res = kvstore_skiptable_modify(key, value);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_SCOUNT: {
    int count = kvstore_skiptable_count();
    if (count < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }

  // B-tree
  case KVS_CMD_BSET: {
    int res = kvstore_btree_set(key, value);
    if (!res) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    break;
  }
  case KVS_CMD_BGET: {
    char *val = kvstore_btree_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_BDEL: {
    int res = kvstore_btree_delete(key);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_BMOD: {
    int res = kvstore_btree_modify(key, value);
    if (res < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "%s", "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_BCOUNT: {
    int count = kvstore_btree_count();
    if (count < 0) { // server
      snprintf(msg, BUFFER_LENGTH, "%s", "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }

#if ENABLE_PERSISTENCE
  // ============ Persistence Commands ============
  case KVS_CMD_PSET: {
    if (key == NULL || value == NULL) {
      snprintf(msg, BUFFER_LENGTH, "FAILED: missing key or value");
      break;
    }
    int res = kvs_persist_set(key, value);
    if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else {
      snprintf(msg, BUFFER_LENGTH, "FAILED");
    }
    break;
  }
  case KVS_CMD_PGET: {
    if (key == NULL) {
      snprintf(msg, BUFFER_LENGTH, "FAILED: missing key");
      break;
    }
    char *val = kvs_persist_get(key);
    if (val) {
      snprintf(msg, BUFFER_LENGTH, "%s", val);
    } else {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    }
    break;
  }
  case KVS_CMD_PDEL: {
    if (key == NULL) {
      snprintf(msg, BUFFER_LENGTH, "FAILED: missing key");
      break;
    }
    int res = kvs_persist_delete(key);
    if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else if (res > 0) {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    } else {
      snprintf(msg, BUFFER_LENGTH, "ERROR");
    }
    break;
  }
  case KVS_CMD_PMOD: {
    if (key == NULL || value == NULL) {
      snprintf(msg, BUFFER_LENGTH, "FAILED: missing key or value");
      break;
    }
    int res = kvs_persist_modify(key, value);
    if (res == 0) {
      snprintf(msg, BUFFER_LENGTH, "SUCCESS");
    } else if (res > 0) {
      snprintf(msg, BUFFER_LENGTH, "NO EXIST");
    } else {
      snprintf(msg, BUFFER_LENGTH, "ERROR");
    }
    break;
  }
  case KVS_CMD_PCOUNT: {
    int count = kvs_persist_count();
    if (count < 0) {
      snprintf(msg, BUFFER_LENGTH, "ERROR");
    } else {
      snprintf(msg, BUFFER_LENGTH, "%d", count);
    }
    break;
  }
#endif // ENABLE_PERSISTENCE

  default: {
    // 未知命令，返回错误消息而不是崩溃
    printf("Unknown cmd: %s (cmd_id=%d)\n", tokens[0], cmd);
    snprintf(msg, BUFFER_LENGTH, "ERROR: unknown command '%s'", tokens[0]);
    break;
  }
  }
}

// 处理cli的请求，按 \n 分割多条消息，逐条解析处理
int kvstore_request(struct conn_item *item) {

  char *buffer = item->rbuffer;
  char *line_start = buffer;
  char *newline;

  // 清空写缓冲区
  memset(item->wbuffer, 0, BUFFER_LENGTH);
  int wpos = 0;

  // 使用静态缓冲区避免协程栈溢出
  static char temp_msg[512];
  static struct conn_item temp_item;

  // 按 \n 分割，逐条处理消息
  while ((newline = strchr(line_start, '\n')) != NULL) {
    *newline = '\0'; // 用 \0 截断当前消息

    // 跳过空消息
    if (strlen(line_start) == 0) {
      line_start = newline + 1;
      continue;
    }

    // 复制当前消息到临时缓冲区
    memset(temp_msg, 0, sizeof(temp_msg));
    strncpy(temp_msg, line_start, sizeof(temp_msg) - 1);

    // 分割 token
    char *tokens[KVSTORE_MAX_TOKENS] = {0};
    int count = kvstore_split_token(temp_msg, tokens);

    // 使用静态 temp_item 处理
    memset(&temp_item, 0, sizeof(temp_item));
    kvstore_parser_protocol(&temp_item, tokens, count);

    // 把响应追加到 wbuffer，加上 \n
    int response_len = strlen(temp_item.wbuffer);
    if (wpos + response_len + 1 < BUFFER_LENGTH) {
      memcpy(item->wbuffer + wpos, temp_item.wbuffer, response_len);
      wpos += response_len;
      item->wbuffer[wpos++] = '\n';
    }

    line_start = newline + 1; // 移动到下一条消息
  }

  // 处理最后一条没有 \n 结尾的消息
  if (strlen(line_start) > 0) {
    memset(temp_msg, 0, sizeof(temp_msg));
    strncpy(temp_msg, line_start, sizeof(temp_msg) - 1);

    char *tokens[KVSTORE_MAX_TOKENS] = {0};
    int count = kvstore_split_token(temp_msg, tokens);

    memset(&temp_item, 0, sizeof(temp_item));
    kvstore_parser_protocol(&temp_item, tokens, count);

    int response_len = strlen(temp_item.wbuffer);
    if (wpos + response_len + 1 < BUFFER_LENGTH) {
      memcpy(item->wbuffer + wpos, temp_item.wbuffer, response_len);
      wpos += response_len;
      item->wbuffer[wpos++] = '\n';
    }
  }

  item->wlen = wpos;
  return 0;
}
// 初始化 kvengine
int init_kvengine(void) {

#if ENABLE_ARRAY_KVENGINE
  kvstore_array_create(&Array);
#endif

#if ENABLE_RBTREE_KVENGINE
  kvstore_rbtree_create(&Tree);
#endif

#if ENABLE_HASH_KVENGINE
  kvstore_hash_create(&Hash);
#endif

#if ENABLE_SKIPTABLE_KVENGINE
  kvstore_skiptable_create(&Skiplist);
#endif

#if ENABLE_BTREE_KVENGINE
  kvstore_btree_create(&Btree);
#endif

#if ENABLE_PERSISTENCE
  // 初始化 WAL 缓冲区
  wal_buffer_config_t wal_config = {.capacity = 16 * 1024 * 1024, // 16MB buffer
                                    .data_dir = "./data"};
  if (wal_buffer_init(&wal_config) < 0) {
    printf("ERROR: Failed to initialize WAL buffer\n");
    return -1;
  }

  // 启动后台刷盘线程
  wal_flusher_config_t flusher_config = {
      .flush_interval_ms = 1000, // 每秒刷一次
      .flush_threshold_pct = 75, // 缓冲区 75% 时提前刷
      .use_fsync = 1};
  if (wal_flusher_start(&flusher_config) < 0) {
    printf("ERROR: Failed to start WAL flusher\n");
    return -1;
  }
  /*
    typedef enum {
      KVS_ENGINE_RBTREE = 0,
      KVS_ENGINE_HASH,
      KVS_ENGINE_SKIPLIST,
      KVS_ENGINE_BTREE
    } kv_engine_type_t;
  */
  /******************************切换持久化引擎************************************/
  // 设置默认持久化引擎 (Hash Table)
  kvs_persist_set_engine(KVS_ENGINE_RBTREE);

  // 初始化通用持久化层
  kvs_persist_create();

  // 从 WAL 恢复数据 (Generic)
  int recovered = kvs_persist_recover();
  printf("[PERSISTENCE] Recovered %d entries from WAL to Hash Engine\n",
         recovered);
#endif

  return 0;
}
// 退出 kvengine
int exit_kvengine(void) {

#if ENABLE_ARRAY_KVENGINE
  kvstore_array_destory(&Array);
#endif

#if ENABLE_RBTREE_KVENGINE
  kvstore_rbtree_destory(&Tree);
#endif

#if ENABLE_HASH_KVENGINE
  kvstore_hash_destory(&Hash);
#endif

#if ENABLE_SKIPTABLE_KVENGINE
  kvstore_skiptable_destory(&Skiplist);
#endif

#if ENABLE_BTREE_KVENGINE
  kvstore_btree_destory(&Btree);
#endif

#if ENABLE_PERSISTENCE
  // 停止后台刷盘线程
  wal_flusher_stop();
  // 确保所有数据落盘
  wal_flusher_force_flush();
  // 关闭 WAL 缓冲区
  wal_buffer_shutdown();
  // 销毁持久化层
  kvs_persist_destroy();
  printf("[PERSISTENCE] Shutdown complete\n");
#endif

  return 0;
}

int init_ctx(void) {

#if ENABLE_MEM_POOL
  slab_init();
#endif

  return 0;
}

void exit_ctx(void) {

#if ENABLE_MEM_POOL
  slab_dest();
#endif
}

int main() {

  init_ctx();      // 初始化内存池
  init_kvengine(); // 初始化kvengine && WAL
// 调用具体的网络模型
#if (ENABLE_NETWORK_SELECT == NETWORK_EPOLL)
  epoll_entry();
#elif (ENABLE_NETWORK_SELECT == NETWORK_NTYCO)
  ntyco_entry();
#elif (ENABLE_NETWORK_SELECT == NETWORK_IOURING)
  iouring_entry();
#endif

  exit_kvengine();
  exit_ctx();

  return 0;
}
