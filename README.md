# KVStore - 高性能键值存储系统

## 项目简介

KVStore 是一个高性能、多存储引擎的网络键值存储系统，使用纯 C 语言实现。支持 **5 种数据结构**作为底层存储引擎，**3 种网络 I/O 模型**，以及基于 **WAL (Write-Ahead Logging)** 的异步持久化机制，并提供 Docker 容器化部署方案。

### 核心特性

- 🚀 **多存储引擎**：数组、红黑树、哈希表、跳表、B树
- 🔌 **多网络模型**：epoll、协程(NtyCo)、io_uring (SQPOLL)
- 💾 **WAL 异步持久化**：Redis AOF everysec 策略，后台线程批量刷盘 + fsync，支持崩溃恢复
- 🔄 **WAL Compaction**：启动时自动合并去重，防止日志无限增长
- 🧠 **内存池优化**：Slab 分配器，减少内存碎片
- 🐳 **Docker 支持**：多阶段构建，一键部署，Volume 数据持久化
- ⚡ **高性能**：io_uring 模式单机 100w+ QPS（Pipeline 模式，2000 并发）

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      客户端请求                              │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                    网络模型层                                │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   epoll     │  │   NtyCo     │  │     io_uring        │  │
│  │  (多路复用)  │  │  (协程)     │  │  (异步I/O+SQPOLL)   │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                   协议解析层 (kvstore.c)                     │
│            SET/GET/DEL/MOD/COUNT 命令解析                    │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                   持久化中间层 (kvstore_persist.c)           │
│               WAL 写入 + 内存引擎更新                        │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                    存储引擎层                                │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐    │
│  │ Array  │ │ RBTree │ │  Hash  │ │SkipList│ │ B-Tree │    │
│  │ O(n)   │ │O(logn) │ │ O(1)   │ │O(logn) │ │O(logn) │    │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                   WAL 持久化层                               │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  wal_buffer.c  : 环形缓冲区 + write 刷盘 + 压缩      │   │
│  │  wal_flusher.c : 后台刷盘线程 (定时/阈值触发+fsync)   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                    内存管理层                                │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Slab Allocator (16/32/64/128/256/512/1024 bytes)   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 高并发优化

### io_uring 网络层

针对 2000+ 并发连接场景，io_uring 事件循环做了以下优化：

| 优化项 | 默认值/旧值 | 优化后 | 说明 |
|--------|-----------|--------|------|
| SQ Ring Size | 4096 | **16384** | 避免高并发下 SQE 溢出导致连接丢弃 |
| listen backlog | 512 | **4096** | 支持数千连接同时建立 |
| CQE 处理方式 | 全部处理完再 submit | **每 256 个穿插 submit** | 减少尾部延迟，新 I/O 操作不再等整批完 |

### WAL 热路径优化

| 优化项 | 旧方案 | 新方案 |
|--------|--------|--------|
| Buffer 满时 | `write()` + `fsync()` 同步阻塞 | 仅 `write()` 到 Page Cache |
| fsync 时机 | 每次 flush 都 fsync | 后台线程每秒 fsync（everysec） |
| flush 线程安全 | 外部加锁，flusher 无锁 | 内部 Mutex 自动序列化 |
| flush_pos 更新 | 跳跃到 write_pos | `+=written` 增量推进 |

## 支持的数据结构

| 数据结构 | 命令前缀 | 查找复杂度 | 插入复杂度 | 适用场景 |
|---------|---------|-----------|-----------|---------|
| 数组 | (无) | O(n) | O(1) | 小规模数据，简单场景 |
| 红黑树 | R | O(log n) | O(log n) | 有序数据，范围查询 |
| 哈希表 | H | O(1) | O(1) | 快速随机访问 |
| 跳表 | S | O(log n) | O(log n) | 有序数据，实现简单 |
| B树 | B | O(log n) | O(log n) | 大规模数据，磁盘友好 |

## Docker 部署

### 前置条件

- 宿主机 Linux 内核 ≥ 5.6（io_uring 依赖宿主机内核，非容器内核）
- 已安装 Docker 和 Docker Compose

```bash
# 检查内核版本
uname -r
```

### 一键部署

```bash
# 构建镜像（多阶段构建，最终镜像不包含编译工具）
docker compose build

# 启动服务
docker compose up -d

# 查看日志
docker logs -f kvstore-server

# 查看运行状态
docker compose ps
```

### 镜像构成

采用多阶段构建，最终运行时镜像仅包含：

| 包含 | 不包含 |
|------|--------|
| Ubuntu 22.04 用户空间 | gcc / make / 编译工具 |
| liburing 运行时库 | liburing-dev 头文件 |
| kvstore / kv_benchmark 二进制 | 源代码 / .o 中间文件 |

### 数据持久化

WAL 日志通过 Docker Volume 持久化，容器重建后数据不丢失：

```yaml
volumes:
  - kvstore-data:/app/data    # WAL 数据持久化到 Docker Volume
```

```bash
# 重启后验证数据恢复
docker compose restart kvstore
docker logs kvstore-server --tail 5
# 应看到: [PERSISTENCE] Recovered N entries from WAL
# 以及: [WAL] Compaction complete: N entries -> M live entries
```

### SQPOLL 模式

io_uring SQPOLL 模式需要特权：

```yaml
# docker-compose.yml 中已配置
privileged: true    # 启用 SQPOLL 内核轮询线程
```

如不需要 SQPOLL，注释掉 `privileged: true`，程序会自动 fallback 到标准 io_uring 模式。

### 运行 Benchmark

```bash
# 使用 docker-compose profile 启动测试容器
docker compose --profile test up benchmark
```

### 常用运维命令

```bash
# 停止服务（触发优雅关闭，WAL 会完成最终 flush + fsync）
docker compose down

# 清理数据卷（注意：会丢失 WAL 数据）
docker compose down -v

# 代码修改后重新构建
docker compose build --no-cache && docker compose up -d
```

## 本地编译运行

### 环境要求

- **操作系统**：Linux（推荐 Ubuntu 20.04+）
- **内核版本**：≥ 5.6（io_uring 支持）
- **编译器**：GCC 7+
- **依赖库**：pthread, dl, liburing

### 安装依赖

```bash
sudo apt-get install build-essential liburing-dev
```

### 编译与运行

```bash
git clone https://github.com/lymdgit/kvstore.git
cd kvstore
make

# 启动服务端（默认监听端口 2048-2067）
./kvstore

# 客户端测试
echo "HSET mykey myvalue" | nc localhost 2048
echo "HGET mykey" | nc localhost 2048
```

## 配置选项

在 `kvstore.h` 中通过宏定义配置：

### 网络模型选择

```c
#define ENABLE_NETWORK_SELECT    NETWORK_EPOLL    // epoll 多路复用
#define ENABLE_NETWORK_SELECT    NETWORK_NTYCO    // NtyCo 协程
#define ENABLE_NETWORK_SELECT    NETWORK_IOURING  // io_uring 异步 I/O
```

### 存储引擎开关

```c
#define ENABLE_ARRAY_KVENGINE      1  // 数组
#define ENABLE_RBTREE_KVENGINE     1  // 红黑树
#define ENABLE_HASH_KVENGINE       1  // 哈希表
#define ENABLE_SKIPTABLE_KVENGINE  1  // 跳表
#define ENABLE_BTREE_KVENGINE      1  // B树
```

### 持久化与内存池

```c
#define ENABLE_PERSISTENCE    1  // WAL 异步持久化
#define ENABLE_MEM_POOL       1  // Slab 内存池
```

### io_uring 参数（iouring_entry.c）

```c
#define IOURING_ENTRIES           16384  // SQ ring size, 支撑 4000+ 并发连接
#define CQE_BATCH_SUBMIT_INTERVAL 256    // 每处理 256 个 CQE 穿插一次 submit
#define IOURING_PORT_COUNT        20     // 监听端口数
```

### WAL 参数（kvstore.c）

```c
wal_buffer_config_t wal_config = {
    .capacity = 16 * 1024 * 1024,   // 16MB 环形缓冲区
    .data_dir = "./data"
};
wal_flusher_config_t flusher_config = {
    .flush_interval_ms = 1000,      // 每秒刷盘
    .flush_threshold_pct = 75,      // 75% 满时提前触发
    .use_fsync = 1
};
```

## 命令格式

| 操作 | 数组 | 红黑树 | 哈希表 | 跳表 | B树 | 持久化 |
|------|------|--------|--------|------|-----|--------|
| 设置 | SET | RSET | HSET | SSET | BSET | PSET |
| 获取 | GET | RGET | HGET | SGET | BGET | PGET |
| 删除 | DEL | RDEL | HDEL | SDEL | BDEL | PDEL |
| 修改 | MOD | RMOD | HMOD | SMOD | BMOD | PMOD |
| 计数 | COUNT | RCOUNT | HCOUNT | SCOUNT | BCOUNT | PCOUNT |

> `P` 前缀的命令会先写 WAL 再更新内存引擎，保证数据可恢复。

## 性能测试

### Benchmark 工具 (kv_benchmark)

```bash
./kv_benchmark [options]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-h` | 服务器 IP | 127.0.0.1 |
| `-p` | 服务器端口 | 2048 |
| `-c` | 并发连接数 | 50 |
| `-n` | 总请求数 | 100000 |
| `-P` | Pipeline 批处理大小 | 1 |
| `-d` | Value 数据大小 (bytes) | 64 |
| `-t` | 存储类型 (rbtree/hash/skip/btree/persist) | rbtree |

```bash
# 基本测试
./kv_benchmark -h 127.0.0.1 -p 2048 -c 50 -n 100000

# 高吞吐 Pipeline 测试
./kv_benchmark -c 100 -n 1000000 -P 16 -d 64 -t persist

# 高并发压力测试
./kv_benchmark -c 2000 -n 1000000 -P 16 -d 64 -t persist
```

> 延迟 histogram 量程 0-2000ms，分辨率 100μs。

## 网络模型对比

| 特性 | epoll | NtyCo 协程 | io_uring |
|------|-------|------------|----------|
| 编程复杂度 | 中等 | 低 | 高 |
| 性能 | 高 | 高 | 极高 |
| 内存占用 | 低 | 中 | 低 |
| 系统调用次数 | 多 | 多 | 少 (SQPOLL 可降至 0) |
| 内核要求 | 2.6+ | 2.6+ | 5.6+ |
| 适用场景 | 通用 | 高并发连接 | 高吞吐 I/O |

## 项目结构

```
kvstore/
├── kvstore.c              # 核心逻辑、协议解析、命令分发
├── kvstore.h              # 头文件、配置宏、API 声明
├── kvstore_persist.c      # 持久化中间层 (WAL + 引擎分发)
│
├── kvstore_array.c        # 数组存储引擎
├── kvstore_rbtree.c       # 红黑树存储引擎
├── kvstore_hash.c         # 哈希表存储引擎 (FNV-1a)
├── kvstore_skiptable.c    # 跳表存储引擎
├── kvstore_btree.c        # B树存储引擎
├── kvstore_mp.c           # Slab 内存池
│
├── wal_buffer.c/h         # WAL 环形缓冲区 + 线程安全 flush + Compaction
├── wal_flusher.c/h        # 后台刷盘线程 (write + fsync everysec)
│
├── epoll_entry.c          # epoll 网络模型
├── ntyco_entry.c          # NtyCo 协程网络模型
├── iouring_entry.c        # io_uring 网络模型 (SQPOLL, ring=16384)
│
├── kv_benchmark.c         # 性能测试工具 (histogram 0-2000ms)
├── Makefile               # 编译脚本
├── NtyCo/                 # 协程库 (第三方)
│
├── Dockerfile             # 多阶段构建
├── docker-compose.yml     # 容器编排 + Volume 持久化
├── .dockerignore           # 构建上下文过滤
└── docker/
    └── entrypoint.sh      # 容器入口 + 信号处理
```

## 许可证

MIT License
