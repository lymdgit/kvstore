/**
 * @file test_persistence.c
 * @brief 持久化层测试程序
 *
 * 测试流程:
 * 1. Phase 1: 写入测试数据到磁盘
 * 2. Phase 2: 模拟重启，从磁盘恢复数据
 * 3. Phase 3: 验证恢复的数据是否正确
 *
 * 编译:
 *   cd persistence && make
 *   cd .. && gcc -o test_persistence test_persistence.c \
 *       -I./persistence/include -L./persistence/lib \
 *       -lkvs_persistence -luring -lstdc++ -lpthread
 *
 * 运行:
 *   ./test_persistence
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kvs_persistence.h"

#define DATA_DIR "./test_data"
#define TEST_COUNT 100

// 测试用的简单索引 (只记录 key -> disk_loc)
typedef struct {
  char key[64];
  kvs_disk_loc_t loc;
  int valid;
} test_entry_t;

static test_entry_t test_index[TEST_COUNT];
static int recovered_count = 0;
static int write_completed = 0;

// ============================================================================
// 回调函数
// ============================================================================

// 写入完成回调
void on_write_complete(int result, kvs_disk_loc_t loc, void *user_data) {
  int idx = (int)(long)user_data;

  if (result == 0) {
    test_index[idx].loc = loc;
    test_index[idx].valid = 1;
    printf("[WRITE] Key '%s' -> file:%u offset:%lu size:%u\n",
           test_index[idx].key, loc.file_id, loc.offset, loc.value_size);
  } else {
    printf("[WRITE] FAILED for key '%s': error=%d\n", test_index[idx].key,
           result);
  }

  write_completed++;
}

// 恢复回调
void on_recover(const char *key, size_t key_len, kvs_disk_loc_t loc,
                int is_delete, void *user_data) {
  (void)user_data;

  if (is_delete) {
    printf("[RECOVER] Key '%.*s' DELETED\n", (int)key_len, key);
    return;
  }

  printf("[RECOVER] Key '%.*s' -> file:%u offset:%lu size:%u\n", (int)key_len,
         key, loc.file_id, loc.offset, loc.value_size);

  // 存储到临时索引
  if (recovered_count < TEST_COUNT) {
    memset(test_index[recovered_count].key, 0, sizeof(test_index[0].key));
    memcpy(test_index[recovered_count].key, key, key_len < 63 ? key_len : 63);
    test_index[recovered_count].loc = loc;
    test_index[recovered_count].valid = 1;
    recovered_count++;
  }
}

// 读取完成回调
typedef struct {
  char expected_value[128];
  char actual_value[128];
  int done;
  int success;
} read_ctx_t;

void on_read_complete(int result, const char *value, size_t value_len,
                      void *user_data) {
  read_ctx_t *ctx = (read_ctx_t *)user_data;

  if (result == 0 && value && value_len > 0) {
    memcpy(ctx->actual_value, value, value_len < 127 ? value_len : 127);
    ctx->actual_value[value_len < 127 ? value_len : 127] = '\0';
    ctx->success = 1;
  } else {
    ctx->success = 0;
  }
  ctx->done = 1;
}

// ============================================================================
// 辅助函数
// ============================================================================

// 清理测试目录
void cleanup_test_dir(void) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", DATA_DIR);
  system(cmd);
}

// 创建测试目录
void create_test_dir(void) { mkdir(DATA_DIR, 0755); }

// 检查数据文件是否存在
int check_data_file_exists(void) {
  char path[256];
  snprintf(path, sizeof(path), "%s/data1.log", DATA_DIR);
  return access(path, F_OK) == 0;
}

// 获取数据文件大小
long get_data_file_size(void) {
  char path[256];
  struct stat st;
  snprintf(path, sizeof(path), "%s/data1.log", DATA_DIR);
  if (stat(path, &st) == 0) {
    return st.st_size;
  }
  return -1;
}

// ============================================================================
// 测试阶段
// ============================================================================

int phase1_write_data(void) {
  printf("\n========================================\n");
  printf("Phase 1: 写入测试数据\n");
  printf("========================================\n\n");

  // 初始化持久化层
  int ret = kvs_persistence_init(DATA_DIR);
  if (ret < 0) {
    printf("ERROR: kvs_persistence_init failed: %d\n", ret);
    return -1;
  }
  printf("[INFO] Persistence layer initialized\n");

  // 初始化测试索引
  memset(test_index, 0, sizeof(test_index));
  write_completed = 0;

  // 写入测试数据
  for (int i = 0; i < TEST_COUNT; i++) {
    char key[64], value[128];
    snprintf(key, sizeof(key), "test_key_%04d", i);
    snprintf(value, sizeof(value), "test_value_%04d_with_some_extra_data", i);

    strcpy(test_index[i].key, key);

    ret = kvs_persistence_write_async(key, strlen(key), value, strlen(value),
                                      on_write_complete, (void *)(long)i);
    if (ret < 0) {
      printf("ERROR: write_async failed for key %s: %d\n", key, ret);
      return -1;
    }
  }

  printf("[INFO] Submitted %d write requests\n", TEST_COUNT);

  // 等待所有写入完成
  while (write_completed < TEST_COUNT) {
    int n = kvs_persistence_wait_completion();
    if (n < 0) {
      printf("ERROR: wait_completion failed: %d\n", n);
      break;
    }
  }

  printf("[INFO] All writes completed: %d/%d\n", write_completed, TEST_COUNT);

  // 检查数据文件
  long file_size = get_data_file_size();
  printf("[INFO] Data file size: %ld bytes\n", file_size);

  // 关闭持久化层 (模拟正常关闭)
  kvs_persistence_shutdown();
  printf("[INFO] Persistence layer shutdown\n");

  return 0;
}

int phase2_recover_data(void) {
  printf("\n========================================\n");
  printf("Phase 2: 从磁盘恢复数据\n");
  printf("========================================\n\n");

  // 检查数据文件是否存在
  if (!check_data_file_exists()) {
    printf("ERROR: Data file not found!\n");
    return -1;
  }
  printf("[INFO] Data file exists\n");

  // 清空测试索引
  memset(test_index, 0, sizeof(test_index));
  recovered_count = 0;

  // 重新初始化持久化层
  int ret = kvs_persistence_init(DATA_DIR);
  if (ret < 0) {
    printf("ERROR: kvs_persistence_init failed: %d\n", ret);
    return -1;
  }
  printf("[INFO] Persistence layer re-initialized\n");

  // 恢复数据
  ret = kvs_persistence_recover(on_recover, NULL);
  if (ret < 0) {
    printf("ERROR: recover failed: %d\n", ret);
    return -1;
  }

  printf("[INFO] Recovered %d entries\n", recovered_count);

  return 0;
}

int phase3_verify_data(void) {
  printf("\n========================================\n");
  printf("Phase 3: 验证恢复的数据\n");
  printf("========================================\n\n");

  int success_count = 0;
  int fail_count = 0;

  // 验证恢复的数量
  if (recovered_count != TEST_COUNT) {
    printf("WARNING: Recovered count mismatch: expected %d, got %d\n",
           TEST_COUNT, recovered_count);
  }

  // 尝试读取部分数据验证
  printf("[INFO] Verifying first 5 entries by reading from disk...\n");

  for (int i = 0; i < 5 && i < recovered_count; i++) {
    char expected_value[128];
    // 根据 key 推算期望的 value
    int key_num;
    sscanf(test_index[i].key, "test_key_%d", &key_num);
    snprintf(expected_value, sizeof(expected_value),
             "test_value_%04d_with_some_extra_data", key_num);

    read_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.expected_value, expected_value);

    int ret =
        kvs_persistence_read_async(&test_index[i].loc, on_read_complete, &ctx);
    if (ret < 0) {
      printf("  [FAIL] Key '%s': read_async failed\n", test_index[i].key);
      fail_count++;
      continue;
    }

    // 等待读取完成
    while (!ctx.done) {
      kvs_persistence_wait_completion();
    }

    if (ctx.success && strcmp(ctx.actual_value, ctx.expected_value) == 0) {
      printf("  [PASS] Key '%s' = '%s'\n", test_index[i].key, ctx.actual_value);
      success_count++;
    } else {
      printf("  [FAIL] Key '%s': expected '%s', got '%s'\n", test_index[i].key,
             ctx.expected_value,
             ctx.success ? ctx.actual_value : "(read failed)");
      fail_count++;
    }
  }

  printf("\n[RESULT] Verified: %d pass, %d fail\n", success_count, fail_count);

  return fail_count == 0 ? 0 : -1;
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char *argv[]) {
  int ret;
  int skip_cleanup = 0;

  // 解析参数
  if (argc > 1 && strcmp(argv[1], "--keep") == 0) {
    skip_cleanup = 1;
  }

  printf("===========================================\n");
  printf("     KVStore Persistence Test Program\n");
  printf("===========================================\n");

  // 清理并创建测试目录
  if (!skip_cleanup) {
    cleanup_test_dir();
  }
  create_test_dir();

  // Phase 1: 写入数据
  ret = phase1_write_data();
  if (ret < 0) {
    printf("\n[FAILED] Phase 1 failed\n");
    return 1;
  }

  printf("\n[INFO] Simulating restart... (persistence layer was shutdown)\n");

  // Phase 2: 恢复数据
  ret = phase2_recover_data();
  if (ret < 0) {
    printf("\n[FAILED] Phase 2 failed\n");
    return 1;
  }

  // Phase 3: 验证数据
  ret = phase3_verify_data();

  // 关闭
  kvs_persistence_shutdown();

  // 最终结果
  printf("\n===========================================\n");
  if (ret == 0) {
    printf("  ✓ ALL TESTS PASSED!\n");
    printf("  Data was successfully persisted and recovered.\n");
  } else {
    printf("  ✗ SOME TESTS FAILED!\n");
  }
  printf("===========================================\n");

  // 提示查看数据文件
  printf("\n[TIP] Check the data file:\n");
  printf("  hexdump -C %s/data1.log | head -50\n", DATA_DIR);
  printf("\n[TIP] Run with --keep to preserve data between runs.\n");

  return ret == 0 ? 0 : 1;
}
