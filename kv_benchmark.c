/**
 * kv_benchmark.c - Redis-benchmark style high-concurrency testing tool
 *
 * Usage: ./kv_benchmark -h <host> -p <port> -c <clients> -n <requests> -P
 * <pipeline>
 *
 * Options:
 *   -h  Target host IP (default: 127.0.0.1)
 *   -p  Target port (default: 2048)
 *   -c  Concurrent connections (default: 50)
 *   -n  Total requests (default: 100000)
 *   -P  Pipeline batch size (default: 1)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// ===================== Configuration =====================

#define MAX_BUFFER_SIZE 65536
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 2048
#define DEFAULT_CLIENTS 50
#define DEFAULT_REQUESTS 100000
#define DEFAULT_PIPELINE 1
#define DEFAULT_DATA_SIZE 64

// Histogram configuration
// 0-200ms range with 10us resolution
// Bucket 0: 0-9us
// Bucket 1: 10-19us
// ...
// Bucket 20000: >= 200000us (200ms)
#define HISTOGRAM_BUCKETS 20001
#define HISTOGRAM_RESOLUTION_US 10

// 存储类型
typedef enum {
  STORE_RBTREE = 0, // 红黑树 (RSET/RGET) - 默认
  STORE_HASH,       // 哈希表 (HSET/HGET)
  STORE_SKIP,       // 跳表 (SSET/SGET)
  STORE_BTREE,      // B树 (BSET/BGET)
  STORE_PERSIST     // 持久化 (PSET/PGET)
} store_type_t;

// 存储类型名称
static const char *store_names[] = {"RBTree", "Hash", "SkipTable", "BTree",
                                    "Persist"};
static const char *store_set_cmds[] = {"RSET", "HSET", "SSET", "BSET", "PSET"};
static const char *store_get_cmds[] = {"RGET", "HGET", "SGET", "BGET", "PGET"};

// ===================== Global State =====================

typedef struct {
  char host[64];
  int port;
  int num_clients;
  long total_requests;
  int pipeline_size;
  int data_size;           // 数据大小（value长度）
  store_type_t store_type; // 存储类型
} benchmark_config_t;

// 命令类型
typedef enum { CMD_SET = 0, CMD_GET = 1 } cmd_type_t;

typedef struct {
  int thread_id;
  benchmark_config_t *config;
  cmd_type_t cmd_type;               // 命令类型
  long requests_to_send;             // 该线程需要发送的请求数
  long requests_completed;           // 该线程完成的请求数
  long long latency_sum_us;          // 延迟总和(微秒)
  int errors;                        // 错误计数
  long histogram[HISTOGRAM_BUCKETS]; // Latency histogram
} thread_context_t;

// 全局原子计数器，用于精确统计
static atomic_long g_completed_requests = 0;
static atomic_long g_total_errors = 0;

// ===================== Utility Functions =====================

// 获取当前时间(微秒)
static long long get_time_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// 创建TCP连接
static int connect_to_server(const char *host, int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
    perror("inet_pton");
    close(sockfd);
    return -1;
  }

  if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("connect");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

// 发送数据，确保全部发送完成
static int send_all(int sockfd, const char *buf, int len) {
  int total_sent = 0;
  while (total_sent < len) {
    int n = send(sockfd, buf + total_sent, len - total_sent, 0);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      return -1;
    }
    total_sent += n;
  }
  return total_sent;
}

// 接收响应，直到收到预期数量的响应
static int recv_responses(int sockfd, char *buf, int buf_size,
                          int expected_count) {
  int total_recv = 0;
  int response_count = 0;

  while (response_count < expected_count && total_recv < buf_size - 1) {
    int n = recv(sockfd, buf + total_recv, buf_size - 1 - total_recv, 0);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      return -1;
    }

    // 统计收到的响应数量(以换行符为界)
    for (int i = total_recv; i < total_recv + n; i++) {
      if (buf[i] == '\n') {
        response_count++;
      }
    }
    total_recv += n;
  }

  buf[total_recv] = '\0';
  return response_count;
}

// ===================== Benchmark Worker =====================

void *benchmark_worker(void *arg) {
  thread_context_t *ctx = (thread_context_t *)arg;
  benchmark_config_t *config = ctx->config;

  // Initialize histogram
  memset(ctx->histogram, 0, sizeof(ctx->histogram));

  // 创建连接
  int sockfd = connect_to_server(config->host, config->port);
  if (sockfd < 0) {
    ctx->errors++;
    atomic_fetch_add(&g_total_errors, 1);
    return NULL;
  }

  // 分配收发缓冲区
  char *send_buf = malloc(MAX_BUFFER_SIZE);
  char *recv_buf = malloc(MAX_BUFFER_SIZE);
  if (!send_buf || !recv_buf) {
    perror("malloc");
    close(sockfd);
    free(send_buf);
    free(recv_buf);
    return NULL;
  }

  // 预生成固定长度的 value 数据
  char *value_data = malloc(config->data_size + 1);
  if (!value_data) {
    perror("malloc value_data");
    close(sockfd);
    free(send_buf);
    free(recv_buf);
    return NULL;
  }
  // 用可打印字符填充 value (循环使用 'a'-'z')
  for (int i = 0; i < config->data_size; i++) {
    value_data[i] = 'a' + (i % 26);
  }
  value_data[config->data_size] = '\0';

  long requests_sent = 0;
  long requests_completed = 0;
  long long latency_sum = 0;
  int pipeline = config->pipeline_size;

  while (requests_sent < ctx->requests_to_send) {
    // 计算本批次发送的命令数
    int batch_size = pipeline;
    if (requests_sent + batch_size > ctx->requests_to_send) {
      batch_size = (int)(ctx->requests_to_send - requests_sent);
    }

    // 构造批量命令 (SET 或 GET)
    int pos = 0;
    const char *set_cmd = store_set_cmds[config->store_type];
    const char *get_cmd = store_get_cmds[config->store_type];

    for (int i = 0; i < batch_size; i++) {
      long key_id = requests_sent + i + ctx->thread_id * config->total_requests;
      if (ctx->cmd_type == CMD_SET) {
        // 使用固定长度的 value 数据
        pos += snprintf(send_buf + pos, MAX_BUFFER_SIZE - pos, "%s key%ld %s\n",
                        set_cmd, key_id, value_data);
      } else {
        pos += snprintf(send_buf + pos, MAX_BUFFER_SIZE - pos, "%s key%ld\n",
                        get_cmd, key_id);
      }
    }

    // 记录发送时间
    long long start_us = get_time_us();

    // 发送批量命令
    if (send_all(sockfd, send_buf, pos) < 0) {
      ctx->errors++;
      atomic_fetch_add(&g_total_errors, 1);
      break;
    }

    // 接收响应
    int recv_count =
        recv_responses(sockfd, recv_buf, MAX_BUFFER_SIZE, batch_size);

    // 记录结束时间
    long long end_us = get_time_us();

    if (recv_count < 0) {
      ctx->errors++;
      atomic_fetch_add(&g_total_errors, 1);
      break;
    }

    // 更新统计
    int completed = (recv_count > 0) ? recv_count : batch_size;
    requests_sent += batch_size;
    requests_completed += completed;

    // Average pipeline latency per request for histogram?
    // Usually benchmark tools count per-pipeline-batch latency or average.
    // Redis-benchmark measures round-trip of the pipeline batch.
    long long batch_latency = end_us - start_us;
    latency_sum += batch_latency;

    // Record in histogram (batch latency)
    int bucket = batch_latency / HISTOGRAM_RESOLUTION_US;
    if (bucket >= HISTOGRAM_BUCKETS) {
      bucket = HISTOGRAM_BUCKETS - 1;
    }
    ctx->histogram[bucket]++;

    // 原子更新全局计数
    atomic_fetch_add(&g_completed_requests, completed);
  }

  // 保存线程统计
  ctx->requests_completed = requests_completed;
  ctx->latency_sum_us = latency_sum;

  // 清理
  free(value_data);
  free(send_buf);
  free(recv_buf);
  close(sockfd);

  return NULL;
}

// ===================== Benchmark Runner =====================

typedef struct {
  long completed;
  long errors;
  double elapsed_sec;
  double qps;
  double avg_latency_ms;
  double p50_ms;
  double p99_ms;
  double p999_ms;
} benchmark_result_t;

// Calculate percentile from histogram
static double calculate_percentile(long *histogram, long total_samples,
                                   double percentile) {
  long threshold = (long)(total_samples * percentile / 100.0);
  long count_so_far = 0;

  for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
    count_so_far += histogram[i];
    if (count_so_far >= threshold) {
      // Bucket center value in ms
      return (i * HISTOGRAM_RESOLUTION_US + HISTOGRAM_RESOLUTION_US / 2.0) /
             1000.0;
    }
  }
  // Max value (200ms+)
  return (HISTOGRAM_BUCKETS * HISTOGRAM_RESOLUTION_US) / 1000.0;
}

void run_benchmark(benchmark_config_t *config, const char *test_name,
                   cmd_type_t cmd_type, benchmark_result_t *result) {
  int num_clients = config->num_clients;
  long total_requests = config->total_requests;

  // 分配线程和上下文
  pthread_t *threads = malloc(sizeof(pthread_t) * num_clients);
  thread_context_t *contexts = malloc(sizeof(thread_context_t) * num_clients);
  long *global_histogram = malloc(sizeof(long) * HISTOGRAM_BUCKETS);

  if (!threads || !contexts || !global_histogram) {
    perror("malloc");
    free(threads);
    free(contexts);
    free(global_histogram);
    result->completed = 0;
    result->qps = 0;
    return;
  }
  memset(global_histogram, 0, sizeof(long) * HISTOGRAM_BUCKETS);

  // 重置全局计数器
  atomic_store(&g_completed_requests, 0);
  atomic_store(&g_total_errors, 0);

  // 计算每个线程的请求数
  long requests_per_thread = total_requests / num_clients;
  long remainder = total_requests % num_clients;

  // 初始化线程上下文
  for (int i = 0; i < num_clients; i++) {
    contexts[i].thread_id = i;
    contexts[i].config = config;
    contexts[i].cmd_type = cmd_type;
    contexts[i].requests_to_send =
        requests_per_thread + (i < remainder ? 1 : 0);
    contexts[i].requests_completed = 0;
    contexts[i].latency_sum_us = 0;
    contexts[i].errors = 0;
  }

  // 记录开始时间
  long long start_time = get_time_us();

  // 创建工作线程
  for (int i = 0; i < num_clients; i++) {
    if (pthread_create(&threads[i], NULL, benchmark_worker, &contexts[i]) !=
        0) {
      perror("pthread_create");
      contexts[i].errors++;
    }
  }

  // 等待所有线程完成
  for (int i = 0; i < num_clients; i++) {
    pthread_join(threads[i], NULL);
  }

  // 记录结束时间
  long long end_time = get_time_us();

  // 计算统计结果
  long total_completed = atomic_load(&g_completed_requests);
  long total_errors = atomic_load(&g_total_errors);
  long long total_latency = 0;
  long total_samples = 0;

  for (int i = 0; i < num_clients; i++) {
    total_latency += contexts[i].latency_sum_us;

    // Merge histograms
    for (int j = 0; j < HISTOGRAM_BUCKETS; j++) {
      global_histogram[j] += contexts[i].histogram[j];
    }

    // Count total batches sent (samples)
    // Note: total_completed is requests, but latency is per batch/pipeline
    long batches =
        (contexts[i].requests_completed + config->pipeline_size - 1) /
        config->pipeline_size;
    total_samples += batches;
  }

  // 计算精确的时间差(秒)
  double elapsed_sec = (double)(end_time - start_time) / 1000000.0;

  // 防止除零
  if (elapsed_sec < 0.000001) {
    elapsed_sec = 0.000001;
  }

  // 计算QPS: 完成请求数 / 总耗时(秒)
  double qps = (double)total_completed / elapsed_sec;

  // 计算平均延迟(毫秒)
  double avg_latency_ms =
      (total_samples > 0) ? (double)total_latency / total_samples / 1000.0 : 0;

  // Calculate percentiles
  double p50 = calculate_percentile(global_histogram, total_samples, 50.0);
  double p99 = calculate_percentile(global_histogram, total_samples, 99.0);
  double p999 = calculate_percentile(global_histogram, total_samples, 99.9);

  // 填充结果
  result->completed = total_completed;
  result->errors = total_errors;
  result->elapsed_sec = elapsed_sec;
  result->qps = qps;
  result->avg_latency_ms = avg_latency_ms;
  result->p50_ms = p50;
  result->p99_ms = p99;
  result->p999_ms = p999;

  // 释放资源
  free(threads);
  free(contexts);
  free(global_histogram);
}

// ===================== Output Formatting =====================

void print_result(const char *test_name, benchmark_config_t *config,
                  benchmark_result_t *result) {
  printf("\n");
  printf("====== %s ======\n", test_name);
  printf("  %ld requests completed in %.2f seconds\n", result->completed,
         result->elapsed_sec);
  printf("  %d parallel clients\n", config->num_clients);
  printf("  Pipeline: %d\n", config->pipeline_size);
  printf("\n");
  printf("  Throughput: %.2f requests/second\n", result->qps);
  printf("  Avg Latency: %.3f ms\n", result->avg_latency_ms);
  printf("  p50 Latency: %.3f ms\n", result->p50_ms);
  printf("  p99 Latency: %.3f ms\n", result->p99_ms);
  printf("  p99.9 Latency: %.3f ms\n", result->p999_ms);

  if (result->errors > 0) {
    printf("  Errors: %ld\n", result->errors);
  }
  printf("\n");
}

void print_usage(const char *prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("\n");
  printf("Options:\n");
  printf("  -h <hostname>   Server hostname (default: %s)\n", DEFAULT_HOST);
  printf("  -p <port>       Server port (default: %d)\n", DEFAULT_PORT);
  printf("  -c <clients>    Number of parallel connections (default: %d)\n",
         DEFAULT_CLIENTS);
  printf("  -n <requests>   Total number of requests (default: %d)\n",
         DEFAULT_REQUESTS);
  printf("  -P <pipeline>   Pipeline requests (default: %d)\n",
         DEFAULT_PIPELINE);
  printf("  -d <size>       Data size of value in bytes (default: %d)\n",
         DEFAULT_DATA_SIZE);
  printf("  -t <type>       Storage type: rbtree, hash, skip, btree, persist "
         "(default: "
         "rbtree)\n");
  printf("  -?              Show this help message\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s -h 127.0.0.1 -p 2048 -c 50 -n 100000\n", prog);
  printf("  %s -c 100 -n 500000 -P 10 -t hash\n", prog);
}

// ===================== Main Entry =====================

int main(int argc, char *argv[]) {
  // 默认配置
  benchmark_config_t config;
  strcpy(config.host, DEFAULT_HOST);
  config.port = DEFAULT_PORT;
  config.num_clients = DEFAULT_CLIENTS;
  config.total_requests = DEFAULT_REQUESTS;
  config.pipeline_size = DEFAULT_PIPELINE;
  config.data_size = DEFAULT_DATA_SIZE;
  config.store_type = STORE_RBTREE; // 默认使用红黑树

  // 解析命令行参数
  int opt;
  while ((opt = getopt(argc, argv, "h:p:c:n:P:d:t:?")) != -1) {
    switch (opt) {
    case 'h':
      strncpy(config.host, optarg, sizeof(config.host) - 1);
      config.host[sizeof(config.host) - 1] = '\0';
      break;
    case 'p':
      config.port = atoi(optarg);
      break;
    case 'c':
      config.num_clients = atoi(optarg);
      if (config.num_clients < 1)
        config.num_clients = 1;
      break;
    case 'n':
      config.total_requests = atol(optarg);
      if (config.total_requests < 1)
        config.total_requests = 1;
      break;
    case 'P':
      config.pipeline_size = atoi(optarg);
      if (config.pipeline_size < 1)
        config.pipeline_size = 1;
      break;
    case 'd':
      config.data_size = atoi(optarg);
      if (config.data_size < 1)
        config.data_size = 1;
      if (config.data_size > 65535)
        config.data_size = 65535; // 限制最大值，避免溢出
      break;
    case 't':
      if (strcmp(optarg, "rbtree") == 0 || strcmp(optarg, "r") == 0) {
        config.store_type = STORE_RBTREE;
      } else if (strcmp(optarg, "hash") == 0 || strcmp(optarg, "h") == 0) {
        config.store_type = STORE_HASH;
      } else if (strcmp(optarg, "skip") == 0 || strcmp(optarg, "s") == 0) {
        config.store_type = STORE_SKIP;
      } else if (strcmp(optarg, "btree") == 0 || strcmp(optarg, "b") == 0) {
        config.store_type = STORE_BTREE;
      } else if (strcmp(optarg, "persist") == 0 || strcmp(optarg, "p") == 0) {
        config.store_type = STORE_PERSIST;
      } else {
        fprintf(stderr, "Unknown storage type: %s\n", optarg);
        fprintf(stderr, "Valid types: rbtree, hash, skip, btree, persist\n");
        return 1;
      }
      break;
    case '?':
    default:
      print_usage(argv[0]);
      return (opt == '?') ? 0 : 1;
    }
  }

  // 打印配置信息
  printf("KV-Store Benchmark\n");
  printf("==================\n");
  printf("Host: %s\n", config.host);
  printf("Port: %d\n", config.port);
  printf("Storage: %s\n", store_names[config.store_type]);
  printf("Clients: %d\n", config.num_clients);
  printf("Requests: %ld\n", config.total_requests);
  printf("Pipeline: %d\n", config.pipeline_size);
  printf("Data size: %d bytes\n", config.data_size);
  printf("==================\n");

  // 运行SET测试
  benchmark_result_t set_result;
  printf("\nRunning SET benchmark...\n");
  run_benchmark(&config, "SET", CMD_SET, &set_result);
  print_result("SET", &config, &set_result);

  // 运行GET测试 (读取刚才SET的数据)
  benchmark_result_t get_result;
  printf("Running GET benchmark...\n");
  run_benchmark(&config, "GET", CMD_GET, &get_result);
  print_result("GET", &config, &get_result);

  // 总结(Updated table format)
  printf("==================\n");
  printf("Benchmark Complete\n");
  printf("==================\n");
  printf("\n");
  printf("%-10s %12s %12s %12s %12s %12s\n", "Test", "Requests", "Time(s)",
         "QPS", "p50(ms)", "p99(ms)");
  printf("---------- ------------ ------------ ------------ ------------ "
         "------------\n");
  printf("%-10s %12ld %12.2f %12.2f %12.3f %12.3f\n", "SET",
         set_result.completed, set_result.elapsed_sec, set_result.qps,
         set_result.p50_ms, set_result.p99_ms);
  printf("%-10s %12ld %12.2f %12.2f %12.3f %12.3f\n", "GET",
         get_result.completed, get_result.elapsed_sec, get_result.qps,
         get_result.p50_ms, get_result.p99_ms);
  printf("\n");

  return 0;
}
