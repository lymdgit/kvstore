# KVStore - 高性能键值存储系统

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C-orange.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)]()

## 项目简介

KVStore 是一个**高性能、多存储引擎**的网络键值存储系统。支持 **5 种数据结构**作为底层存储引擎，**3 种网络模型**，以及可选的 **Slab 内存池**优化。

### 核心特性

- 🚀 **多存储引擎**：数组、红黑树、哈希表、跳表、B树
- 🔌 **多网络模型**：epoll、协程(NtyCo)、io_uring
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
│                    存储引擎层                                │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐    │
│  │ Array  │ │ RBTree │ │  Hash  │ │SkipList│ │ B-Tree │    │
│  │ O(n)   │ │O(logn) │ │ O(1)   │ │O(logn) │ │O(logn) │    │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘    │
└─────────────────────┬───────────────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────────────┐
│                    内存管理层                                │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Slab Allocator (16/32/64/128/256/512/1024 bytes)   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

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
- **内核版本**：5.1+ (使用 io_uring 时)
- **编译器**：GCC 7+
- **依赖库**：pthread, dl, liburing (可选)

### 安装依赖

```bash
# 基础依赖
sudo apt-get install build-essential

# io_uring 支持 (可选)
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
#define ENABLE_NETWORK_SELECT    NETWORK_NTYCO    // 协程模型 (默认)
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

### 内存池开关

```c
#define ENABLE_MEM_POOL    0  // 0=关闭, 1=启用 Slab 分配器
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

### 参数说明

```bash
./testcase -s <服务器IP> -p <端口> -m <模式>
```

| 参数 | 说明 | 示例 |
|------|------|------|
| `-s` | 服务器 IP 地址 | `127.0.0.1` |
| `-p` | 服务器端口 | `2048` |
| `-m` | 测试模式 (位掩码) | `31` (测试全部) |

### 测试模式 (-m) 位掩码对照表

| 值 | 二进制 | 测试的数据结构 | 模式 |
|----|--------|----------------|------|
| 1  | 0x01   | 数组 (Array) | Pipeline |
| 2  | 0x02   | 红黑树 (RBTree) | Pipeline |
| 4  | 0x04   | 哈希表 (Hash) | Pipeline |
| 8  | 0x08   | 跳表 (SkipTable) | Pipeline |
| 16 | 0x10   | B树 (BTree) | Pipeline |
| 31 | 0x1F   | 全部数据结构 Pipeline |

> **提示**：可以组合多个值来同时测试多种数据结构，例如 `-m 3` 表示同时测试数组和红黑树 (1+2=3)

### 单独测试各数据结构

首先启动服务端：

```bash
./kvstore
```

然后在**另一个终端**运行测试：

```bash
# 测试全部数据结构
./testcase -s 127.0.0.1 -p 2048 -m 31

# 仅测试数组 (Array)
./testcase -s 127.0.0.1 -p 2048 -m 1

# 仅测试红黑树 (RBTree)
./testcase -s 127.0.0.1 -p 2048 -m 2

# 仅测试哈希表 (Hash)
./testcase -s 127.0.0.1 -p 2048 -m 4

# 仅测试跳表 (SkipTable)
./testcase -s 127.0.0.1 -p 2048 -m 8

# 仅测试 B树 (BTree)
./testcase -s 127.0.0.1 -p 2048 -m 16
```

### 组合测试示例

```bash
# 测试数组 + 红黑树 (1 + 2 = 3)
./testcase -s 127.0.0.1 -p 2048 -m 3

# 测试哈希表 + 跳表 + B树 (4 + 8 + 16 = 28)
./testcase -s 127.0.0.1 -p 2048 -m 28

# 测试红黑树 + 哈希表 (2 + 4 = 6)
./testcase -s 127.0.0.1 -p 2048 -m 6
```

### 测试结果示例

```
array testcase    --> time_used: 21366ms, qps: 28081
rbtree testcase   --> time_used: 7095ms,  qps: 28188
hash testcase     --> time_used: 7068ms,  qps: 28296
skiptable testcase--> time_used: 7031ms,  qps: 28445
### 高并发基准测试 (kv_benchmark)

为了更真实地模拟生产环境的高并发压力（类似 redis-benchmark），可以使用 `kv_benchmark` 工具：

**编译**：
```bash
make clean && make
```

**参数说明**：
```bash
./kv_benchmark [options]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-h` | 服务器 IP | 127.0.0.1 |
| `-p` | 服务器端口 | 2048 |
| `-c` | 并发连接数 (Clients) | 50 |
| `-n` | 总请求数 (Requests) | 100000 |
| `-P` | Pipeline 批处理大小 | 1 |

**测试示例**：

1. **基本测试** (50并发，10万请求)
   ```bash
   ./kv_benchmark -h 127.0.0.1 -p 2048 -c 50 -n 100000
   ```

2. **高吞吐测试** (Pipeline模式，推荐)
   ```bash
   ./kv_benchmark -c 50 -n 1000000 -P 16
   ```

3. **高压测试** (100并发)
   ```bash
   ./kv_benchmark -c 100 -n 1000000 -P 64
   ```

## 项目结构

```
kvstore/
├── kvstore.c           # 核心逻辑、协议解析
├── kvstore.h           # 头文件、配置宏
├── kvstore_array.c     # 数组存储引擎
├── kvstore_rbtree.c    # 红黑树存储引擎
├── kvstore_hash.c      # 哈希表存储引擎 (FNV-1a)
├── kvstore_skiptable.c # 跳表存储引擎
├── kvstore_btree.c     # B树存储引擎
├── kvstore_mp.c        # Slab 内存池
├── epoll_entry.c       # epoll 网络模型
├── ntyco_entry.c       # 协程网络模型
├── iouring_entry.c     # io_uring 网络模型
├── testcase.c          # 测试客户端
├── Makefile            # 编译脚本
├── NtyCo/              # 协程库子模块
└── README.md           # 项目说明
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
2. **协程支持**：基于 NtyCo 协程库，简化异步编程模型
3. **io_uring 支持**：利用 Linux 5.1+ 最新异步 IO 接口
4. **Slab 内存池**：7 级大小分类，减少内存碎片和分配开销
5. **模块化设计**：通过宏定义灵活配置各组件

## 许可证

MIT License

## 面试深入：NtyCo 协程竞态条件分析

在开发过程中，我们在高并发压测（50并发，Pipeline模式）下遇到了 NtyCo 协程库的崩溃问题。这个问题非常经典，涉及并发编程中的**竞态条件（Race Condition）**和**防御性编程**，是面试中展示技术深度的绝佳案例。

### 1. 问题现象

在使用 `kv_benchmark` 进行高并发测试时，服务端抛出断言错误导致崩溃：

```
kvstore: nty_schedule.c:172: nty_schedule_sched_wait: Assertion `co_tmp == NULL' failed.
Program received signal SIGABRT, Aborted.
```

### 2. 问题分析

**（1）追踪崩溃点**

通过 GDB 调试发现崩溃发生 `nty_schedule.c` 的 172 行，即红黑树插入操作：

```c
// logic in nty_schedule.c
nty_coroutine *co_tmp = RB_INSERT(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
assert(co_tmp == NULL); // 崩溃点
```

`assert(co_tmp == NULL)` 意味着我们试图向等待队列（waiting tree）中插入一个已经存在的节点。换句话说，**同一个 fd 被多个协程同时等待**。在 NtyCo 的设计中，一个 fd 同一时刻只能被一个协程等待，这是为了避免惊群和数据混乱。

**（2）定位根本原因（Root Cause）**

Bug 的根源位于 `ntyco_entry.c` 的连接接受循环中：

```c
// 错误代码示例
while (1) {
    int cli_fd = accept(fd, ...); // cli_fd 是栈上的局部变量
    
    // 问题所在：直接将栈变量的地址传递给了协程
    nty_coroutine_create(&read_co, server_reader, &cli_fd); 
}
```

这是一个典型的 **Use-After-Write** 竞态条件：

1. **主线程**：`accept` 返回新的 socket `fd=5`，将 `cli_fd` 赋值为 5。
2. **主线程**：调用 `nty_coroutine_create`，将 `&cli_fd`（栈地址）传给协程 A，但协程 A 此时**尚未运行**（仅添加到就绪队列）。
3. **主线程**：`accept` 再次返回，`cli_fd` 被更新为 `fd=6`。**注意：协程 A 持有的指针指向的内存内容被改变了！**
4. **协程 A 运行**：从 `arg` 指针读取 fd，此时得到的是 **6** 而不是原本的 5。
5. **协程 B 运行**：处理 `fd=6`。
6. **结果**：协程 A 和 协程 B 都在操作同一个 `fd=6`。当它们同时调用 `recv` 等待事件时，触发了 `nty_schedule` 中的重复插入检测，导致断言失败。

### 3. 解决方案

修复的核心思路是**消除数据共享**，为每个协程提供独立的 fd 副本。我们采用了**动态内存分配（Heap Allocation）**的方案：

```c
// 修复后的代码
while (1) {
    int cli_fd = accept(fd, ...);
    
    // 动态分配内存，拷贝 fd 的值
    int *fd_ptr = malloc(sizeof(int));
    *fd_ptr = cli_fd;
    
    // 传递堆内存地址，每个协程通过指针独占一份数据
    nty_coroutine_create(&read_co, server_reader, fd_ptr);
}

// 在消费者函数中释放内存
void server_reader(void *arg) {
    int fd = *(int *)arg;
    free(arg); // 必须释放，防止内存泄漏
    // ...
}
```

### 4. 技术总结（面试关键词）

*   **竞态条件 (Race Condition)**：多个执行流访问共享资源（栈变量），且执行顺序不确定导致的问题。
*   **栈内存 vs 堆内存**：栈变量生命周期随作用域结束，且易被复用覆盖；堆内存生命周期可控，适合跨上下文传递数据。
*   **协程调度机制**：协程创建与执行是解耦的（异步），不能假设参数传递后立即消费。
*   **防御性编程**：NtyCo 的 `assert` 机制在定位此类并发 Bug 中起到了关键作用，防止了更严重的数据损坏。

## 性能对比分析 (Benchmarking)

我们在以下环境进行了高并发压力测试，对比了 KVStore (io_uring) 与 Redis (v6.x) 的吞吐量性能。

*   **测试条件**: 50 并发连接, 100万请求, Pipeline机制
*   **硬件环境**: Linux Server (epoll/io_uring supported)

![Benchmark rbtree](https://disk.0voice.com/p/2c)
![Benchmark io_uring](https://disk.0voice.com/p/2d)

### 核心数据 (QPS)

| 网络模型 | 数据结构 | SET QPS | GET QPS | 相对 Redis 提升 |
| :--- | :--- | :--- | :--- | :--- |
| **io_uring** | RBTree | **29.6w** | **36.1w** | **3.8x** |
| **io_uring** | Hash | 31.3w | 34.1w | 3.6x |
| **epoll** | RBTree | 26.4w | 29.6w | 3.1x |
| **NtyCo** | RBTree | 22.1w | 24.5w | 2.6x |
| **Redis** | Default | 9.1w | 9.3w | 1.0 (基准) |

> **面试核心点**：
> 1. **io_uring 的降维打击**：利用 Linux 异步 IO 机制（Submission/Completion Queue）大幅减少系统调用和上下文切换，实现性能飞跃。
> 2. **轻量级特定优化**：Redis 功能大而全，而本项目专注于核心 KV 路径，指令数更少，缓存亲和性更好。


