# KVStore - 高性能键值存储系统

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C-orange.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

## 项目简介

KVStore 是一个**高性能、多存储引擎**的网络键值存储系统。支持 **5 种数据结构**作为底层存储引擎，**3 种网络模型**，以及基于 **io_uring 的 WAL 异步持久化**。

### 核心特性

- 🚀 **多存储引擎**：数组、红黑树、哈希表、跳表、B树
- 🔌 **多网络模型**：epoll、协程(NtyCo)、io_uring
- 💾 **WAL 异步持久化**：基于 io_uring，后台线程批量刷盘，支持崩溃恢复
- 🧠 **内存池优化**：Slab 分配器，减少内存碎片
- ⚡ **高性能**：单机 5w+ QPS（Pipeline 模式）
- 🔧 **模块化设计**：通过宏定义灵活切换配置

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
│  │  (多路复用)  │  │  (协程)     │  │  (异步I/O)          │  │
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
│  │  wal_buffer.c    : 内存环形缓冲区 + io_uring 刷盘     │   │
│  │  wal_flusher.c   : 后台刷盘线程 (每秒/阈值触发)       │   │
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

## WAL 持久化设计

本项目采用类似 Redis AOF 的 **Write-Ahead Logging (WAL)** 机制实现数据持久化：

### 核心流程

1. **写入请求到达** → 数据先追加到内存中的 WAL Buffer（环形缓冲区）
2. **后台线程刷盘** → 每隔 1 秒或缓冲区达到 90% 时，由 `wal_flusher` 线程批量写入磁盘
3. **io_uring 异步 I/O** → 使用 Linux 最新的 io_uring 接口，Write + Fsync 链式提交，减少系统调用开销
4. **崩溃恢复** → 重启时通过 `wal_buffer_replay` 回放日志，重建内存数据

### 关键技术点

- **Spinlock + Mutex 混合锁**：热路径（内存写入）用 Spinlock，冷路径（磁盘刷盘）用 Mutex
- **环形缓冲区**：通过取余运算实现空间复用，避免频繁内存分配
- **可中断休眠**：后台线程支持紧急唤醒（缓冲区满）和优雅停机

### 文件说明

| 文件 | 功能 |
|------|------|
| `wal_buffer.c/h` | WAL 缓冲区管理、io_uring 刷盘、日志回放 |
| `wal_flusher.c/h` | 后台刷盘线程、定时/阈值触发 |
| `kvstore_persist.c` | 持久化中间层，连接 WAL 与各存储引擎 |

## 支持的数据结构

| 数据结构 | 前缀 | 查找复杂度 | 插入复杂度 | 适用场景 |
|---------|------|-----------|-----------|---------| 
| 数组 | - | O(n) | O(1) | 小规模数据，简单场景 |
| 红黑树 | R | O(log n) | O(log n) | 有序数据，范围查询 |
| 哈希表 | H | O(1) | O(1) | 快速随机访问 |
| 跳表 | S | O(log n) | O(log n) | 有序数据，实现简单 |
| B树 | B | O(log n) | O(log n) | 大规模数据，磁盘友好 |

## 快速开始

### 环境要求

- **操作系统**：Linux (推荐 Ubuntu 20.04+)
- **内核版本**：5.1+ (io_uring 支持)
- **编译器**：GCC 7+
- **依赖库**：pthread, dl, liburing

### 安装依赖

```bash
# 基础依赖
sudo apt-get install build-essential

# io_uring 支持
sudo apt-get install liburing-dev
```

### 编译

```bash
git clone <repository-url>
cd kvstore
make
```

### 运行

```bash
# 启动服务端 (默认端口 2048)
./kvstore

# 运行测试客户端 (Pipeline 模式)
./testcase -s 127.0.0.1 -p 2048 -m 30
```

## 配置选项

在 `kvstore.h` 中通过宏定义配置：

### 网络模型选择

```c
#define ENABLE_NETWORK_SELECT    NETWORK_EPOLL    // epoll 模型
#define ENABLE_NETWORK_SELECT    NETWORK_NTYCO    // 协程模型
#define ENABLE_NETWORK_SELECT    NETWORK_IOURING  // io_uring 模型
```

### 存储引擎开关

```c
#define ENABLE_ARRAY_KVENGINE      1  // 数组
#define ENABLE_RBTREE_KVENGINE     1  // 红黑树
#define ENABLE_HASH_KVENGINE       1  // 哈希表
#define ENABLE_SKIPTABLE_KVENGINE  1  // 跳表
#define ENABLE_BTREE_KVENGINE      1  // B树
```

### 持久化开关

```c
#define ENABLE_PERSISTENCE    1  // 0=关闭, 1=启用 WAL 持久化
```

### 内存池开关

```c
#define ENABLE_MEM_POOL    1  // 0=关闭, 1=启用 Slab 分配器
```

## 命令格式

### 基本命令

| 操作 | 数组 | 红黑树 | 哈希表 | 跳表 | B树 |
|------|------|--------|--------|------|-----|
| 设置 | SET | RSET | HSET | SSET | BSET |
| 获取 | GET | RGET | HGET | SGET | BGET |
| 删除 | DEL | RDEL | HDEL | SDEL | BDEL |
| 修改 | MOD | RMOD | HMOD | SMOD | BMOD |
| 计数 | COUNT | RCOUNT | HCOUNT | SCOUNT | BCOUNT |

### 使用示例

```bash
# 使用 netcat 测试
echo "SET name Alice\n" | nc 127.0.0.1 2048    # SUCCESS
echo "GET name\n" | nc 127.0.0.1 2048          # Alice
echo "HSET age 25\n" | nc 127.0.0.1 2048       # SUCCESS (哈希表)
echo "RSET score 100\n" | nc 127.0.0.1 2048    # SUCCESS (红黑树)
```

## 性能测试

### 高并发基准测试 (kv_benchmark)

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

**测试示例**：

```bash
# 基本测试 (50并发，10万请求)
./kv_benchmark -h 127.0.0.1 -p 2048 -c 50 -n 100000

# 高吞吐测试 (Pipeline模式)
./kv_benchmark -c 50 -n 1000000 -P 16
```

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
├── wal_buffer.c/h         # WAL 缓冲区 + io_uring 刷盘
├── wal_flusher.c/h        # 后台刷盘线程
│
├── epoll_entry.c          # epoll 网络模型
├── ntyco_entry.c          # 协程网络模型
├── iouring_entry.c        # io_uring 网络模型
│
├── kv_benchmark.c         # 性能测试工具
├── testcase.c             # 功能测试客户端
├── Makefile               # 编译脚本
├── NtyCo/                 # 协程库子模块
└── README.md              # 项目说明
```

## 网络模型对比

| 特性 | epoll | NtyCo 协程 | io_uring |
|------|-------|------------|----------|
| 编程复杂度 | 中等 | 低 | 高 |
| 性能 | 高 | 高 | 极高 |
| 内存占用 | 低 | 中 | 低 |
| 系统调用次数 | 多 | 多 | 少 |
| 内核要求 | 2.6+ | 2.6+ | 5.1+ |
| 适用场景 | 通用 | 高并发连接 | 高吞吐IO |

## 技术亮点

1. **多种存储引擎**：红黑树保证 O(log n) 平衡性能，哈希表提供 O(1) 快速查找
2. **WAL 异步持久化**：类 Redis AOF 设计，io_uring 批量刷盘，支持崩溃恢复
3. **协程支持**：基于 NtyCo 协程库，简化异步编程模型
4. **io_uring 支持**：利用 Linux 5.1+ 最新异步 IO 接口
5. **Slab 内存池**：7 级大小分类，减少内存碎片和分配开销
6. **模块化设计**：通过宏定义灵活配置各组件

## 面试深入：NtyCo 协程竞态条件分析

在开发过程中，我们在高并发压测（50并发，Pipeline模式）下遇到了 NtyCo 协程库的崩溃问题。这个问题非常经典，涉及并发编程中的**竞态条件（Race Condition）**和**防御性编程**。

### 问题现象

```
kvstore: nty_schedule.c:172: nty_schedule_sched_wait: Assertion `co_tmp == NULL' failed.
```

### 根本原因

Bug 的根源位于连接接受循环中的**栈变量竞态**：

```c
// 错误代码
while (1) {
    int cli_fd = accept(fd, ...); // cli_fd 是栈上的局部变量
    nty_coroutine_create(&read_co, server_reader, &cli_fd); // 传递栈地址
}
```

协程创建与执行是解耦的。在协程 A 尚未运行时，`cli_fd` 可能已被下一次 `accept` 覆盖，导致多个协程操作同一个 fd。

### 解决方案

```c
// 修复后：动态分配内存
while (1) {
    int cli_fd = accept(fd, ...);
    int *fd_ptr = malloc(sizeof(int));
    *fd_ptr = cli_fd;
    nty_coroutine_create(&read_co, server_reader, fd_ptr);
}

void server_reader(void *arg) {
    int fd = *(int *)arg;
    free(arg); // 释放内存
    // ...
}
```

## 许可证

MIT License
