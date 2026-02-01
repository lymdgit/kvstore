# Async Persistence Layer for KVStore

基于 io_uring 的异步持久化层，采用 Bitcask 日志结构存储模型。

## 特性

- **异步 I/O**: 使用 io_uring 实现磁盘读写异步化，不阻塞网络线程
- **Bitcask 模型**: Append-only 日志 + 内存索引，写入性能极高
- **CRC32 校验**: 每条记录都有 CRC 校验，保证数据完整性
- **崩溃恢复**: 启动时自动扫描日志文件，重建内存索引
- **C API**: 提供 C 语言接口，可直接集成到现有 C 项目

## 文件结构

```
persistence/
├── include/
│   ├── storage_format.hpp    # 磁盘数据格式定义
│   ├── disk_pointer.hpp      # 磁盘位置指针
│   ├── async_context.hpp     # 异步操作上下文
│   ├── log_manager.hpp       # 日志管理器
│   └── kvs_persistence.h     # C API 头文件
├── src/
│   ├── storage_format.cpp    # 序列化/CRC 实现
│   ├── log_manager.cpp       # io_uring 异步操作
│   └── kvs_persistence.cpp   # C API 实现
├── Makefile
└── README.md
```

## 编译

```bash
# 在 persistence 目录下
make

# 输出
# lib/libkvs_persistence.a  - 静态库
# lib/libkvs_persistence.so - 动态库
```

## 依赖

- Linux (kernel 5.1+) 
- liburing (`apt install liburing-dev` 或 `yum install liburing-devel`)
- g++ (支持 C++17)

## 使用示例

### C 程序集成

```c
#include "kvs_persistence.h"

// 写入回调
void on_write(int result, kvs_disk_loc_t loc, void* user_data) {
    if (result == 0) {
        printf("Write success at offset %lu\n", loc.offset);
        // 在这里更新内存索引
    }
}

// 读取回调
void on_read(int result, const char* value, size_t len, void* user_data) {
    if (result == 0) {
        printf("Read value: %.*s\n", (int)len, value);
    }
}

int main() {
    // 初始化
    kvs_persistence_init("./data");
    
    // 恢复索引
    kvs_persistence_recover(my_recover_callback, my_index);
    
    // 异步写入
    kvs_persistence_write_async("key1", 4, "value1", 6, on_write, NULL);
    
    // 处理完成事件
    while (kvs_persistence_pending_count() > 0) {
        kvs_persistence_wait_completion();
    }
    
    // 关闭
    kvs_persistence_shutdown();
    return 0;
}
```

### 与 io_uring 网络层集成

```c
// 在主事件循环中处理磁盘 I/O 完成事件
while (1) {
    // 处理网络事件
    io_uring_wait_cqe(&network_ring, &cqe);
    // ...
    
    // 处理磁盘事件 (非阻塞)
    kvs_persistence_process_completions();
}
```

## 数据格式

每条日志记录格式：

```
[ CRC32 (4B) ][ Timestamp (8B) ][ Key_Size (4B) ][ Value_Size (4B) ][ Key ][ Value ]
```

- **CRC32**: 校验码，覆盖 timestamp 到 value 的所有字段
- **Timestamp**: 微秒时间戳，用于版本控制
- **Key_Size**: Key 长度
- **Value_Size**: Value 长度 (0 表示删除标记)

## 性能特点

1. **写入**: O(1) 追加写，无随机 I/O
2. **读取**: 热数据走内存缓存；冷数据异步读盘
3. **恢复**: 启动时顺序扫描日志，O(n) 时间复杂度

## 后续优化方向

- [ ] 日志文件 Compaction (合并压缩)
- [ ] O_DIRECT 支持 (绕过 Page Cache)
- [ ] 批量提交优化 (Batch Commit)
- [ ] 多文件滚动支持
