# KVStore - 基于多种数据结构的键值存储系统

## 项目简介

KVStore 是一个基于网络的键值存储系统，支持多种数据结构作为底层存储引擎。用户可以通过简单的命令行接口或自定义客户端与服务端进行交互，实现键值对的增删改查操作。

## 支持的数据结构

| 数据结构 | 模式值 | 前缀 | 说明 |
|---------|--------|------|------|
| 数组 | 0x01 | - | 简单数组实现，适用于小规模数据 |
| 红黑树 | 0x02 | R | 平衡树结构，适用于有序数据操作 |
| 哈希表 | 0x04 | H | 哈希表实现，适用于快速查找 |
| 跳表 | 0x08 | S | 跳表实现，平衡查找和插入性能 |
| B树 | 0x10 | B | B树实现，适用于大规模数据存储 |

## 编译和安装

### 依赖

- GCC 编译器
- pthread 库
- dl 库
- NtyCo 协程库（作为子模块包含在项目中）

### 编译步骤

1. 克隆项目到本地

```bash
git clone <repository-url>
cd kvstore
```

2. 编译项目

```bash
make
```

这将编译服务端程序 `kvstore` 和测试客户端 `testcase`。

3. 清理编译结果（可选）

```bash
make clean
```

## 运行方法

### 启动服务端

```bash
./kvstore
```

服务端默认监听端口 9096。

### 运行测试客户端

```bash
./testcase -s <ip> -p <port> -m <mode>
```

参数说明：
- `-s <ip>`：服务端 IP 地址
- `-p <port>`：服务端端口号，默认为 9096
- `-m <mode>`：测试模式，使用位掩码选择要测试的数据结构
  - 0x01：测试数组
  - 0x02：测试红黑树
  - 0x04：测试哈希表
  - 0x08：测试跳表
  - 0x10：测试 B 树
  - 0x31：测试所有数据结构

示例：

```bash
# 测试所有数据结构
./testcase -s 127.0.0.1 -p 9096 -m 31

# 只测试跳表
./testcase -s 127.0.0.1 -p 9096 -m 8

# 测试红黑树和 B 树
./testcase -s 127.0.0.1 -p 9096 -m 18
```

## 命令格式

服务端支持以下命令格式：

### 通用命令（数组）

- `SET <key> <value>`：设置键值对
- `GET <key>`：获取键对应的值
- `DEL <key>`：删除键值对
- `MOD <key> <new-value>`：修改键对应的值
- `COUNT`：获取键值对数量

### 红黑树命令

- `RSET <key> <value>`：设置键值对
- `RGET <key>`：获取键对应的值
- `RDEL <key>`：删除键值对
- `RMOD <key> <new-value>`：修改键对应的值
- `RCOUNT`：获取键值对数量

### 哈希表命令

- `HSET <key> <value>`：设置键值对
- `HGET <key>`：获取键对应的值
- `HDEL <key>`：删除键值对
- `HMOD <key> <new-value>`：修改键对应的值
- `HCOUNT`：获取键值对数量

### 跳表命令

- `SSET <key> <value>`：设置键值对
- `SGET <key>`：获取键对应的值
- `SDEL <key>`：删除键值对
- `SMOD <key> <new-value>`：修改键对应的值
- `SCOUNT`：获取键值对数量

### B树命令

- `BSET <key> <value>`：设置键值对
- `BGET <key>`：获取键对应的值
- `BDEL <key>`：删除键值对
- `BMOD <key> <new-value>`：修改键对应的值
- `BCOUNT`：获取键值对数量

## 性能测试

测试客户端会自动执行性能测试，并输出每个数据结构的执行时间和 QPS（每秒查询数）。

示例输出：

```
book@100ask:~/Desktop/0voice/github/kvstore$ ./testcase -s 192.168.72.145 -p 9096 -m 1
array testcase--> time_used: 21366, qps: 28081
book@100ask:~/Desktop/0voice/github/kvstore$ ./testcase -s 192.168.72.145 -p 9096 -m 2
rbtree testcase-->  time_used: 7095, qps: 28188
book@100ask:~/Desktop/0voice/github/kvstore$ ./testcase -s 192.168.72.145 -p 9096 -m 4
hash testcase-->  time_used: 7068, qps: 28296
book@100ask:~/Desktop/0voice/github/kvstore$ ./testcase -s 192.168.72.145 -p 9096 -m 8
skiptable testcase-->  time_used: 7031, qps: 28445
book@100ask:~/Desktop/0voice/github/kvstore$ ./testcase -s 192.168.72.145 -p 9096 -m 16
btree testcase-->  time_used: 7106, qps: 28145

```



## 项目结构

```
.
├── kvstore.c          # 核心逻辑
├── kvstore.h          # 头文件
├── kvstore_array.c    # 数组实现
├── kvstore_rbtree.c   # 红黑树实现
├── kvstore_hash.c     # 哈希表实现
├── kvstore_skiptable.c # 跳表实现
├── kvstore_btree.c    # B树实现
├── ntyco_entry.c      # NtyCo 网络接口
├── epoll_entry.c      # Epoll 网络接口
├── testcase.c         # 测试客户端
├── Makefile           # 编译脚本
├── NtyCo/             # 协程库子模块
└── README.md          # 项目说明
```

## 网络模型

项目支持多种网络模型，可在 `kvstore.h` 中通过 `ENABLE_NETWORK_SELECT` 宏进行选择：

- `NETWORK_EPOLL`：基于 epoll 的网络模型
- `NETWORK_NTYCO`：基于 NtyCo 协程库的网络模型
- `NETWORK_IOURING`：基于 io_uring 的网络模型（暂未实现）

默认使用 `NETWORK_NTYCO` 网络模型。

## 许可证

MIT License
