# 后端工程师面试准备指南 - KVStore 项目

## 项目概览 (自我介绍话术)
"我独立设计并实现了一个基于 C 语言的高性能 Key-Value 存储系统。通过引入 **io_uring** 异步 I/O 技术，在单机环境下实现了超过 **36w QPS** 的吞吐量（性能是同配置 Redis 的 3.8 倍）。项目核心亮点在于实现了 **Epoll、NtyCo 协程、io_uring** 三种不同的网络模型，并支持红黑树、哈希表、BTree 多种存储引擎，旨在深度探索 Linux 系统编程与高性能网络架构的极限。"

---

## 🚀 架构与 I/O 模型 (核心考点)

### Q1: 你为什么实现了三种网络模型？它们有什么本质区别？(Epoll vs NtyCo vs io_uring)
**回答思路：**
*   **Epoll (Reactor 模型):** 工业界标准（Redis/Nginx 使用）。
    *   *原理:* 操作系统通知可以读写了，用户线程再去执行读写。
    *   *瓶颈:* 每次读写都需要系统调用 (syscall)，高并发下上下文切换开销大。
*   **NtyCo (用户态协程):** 以“同步的编程方式”实现“异步的性能”。
    *   *优势:* 代码逻辑清晰，没有回调地狱 (Callback Hell)。
    *   *原理:* `yield`/`resume` 切换用户栈，底层通过 Hook 系统调用 + Epoll 实现非阻塞。
*   **io_uring (Proactor 模型):** 真正的异步 I/O，本项目性能飞跃的关键。
    *   *核心区别:* Epoll 告诉你“什么时候可以做”，io_uring 帮你“做完”并告诉你结果。
    *   *优势:* 提交队列 (SQ) 和完成队列 (CQ) 处于用户态与内核态共享内存中，大幅减少了系统调用次数（Syscall Batching）和内存拷贝。

### Q2: 为什么 io_uring 比 Epoll 快这么多？
**关键点：**
1.  **系统调用批处理 (Syscall Batching):** Epoll 模式下，`epoll_wait`、`read`、`write` 都是独立的系统调用。io_uring 可以一次性提交多个请求，合并系统调用开销。
2.  **零拷贝机制 (Zero Copy / Architecture):** 利用 `mmap` 共享内存环 (Ring Buffer)，提交 IO 请求时无需在用户态和内核态之间拷贝数据结构。
3.  **异步设计:** 也就是 Proactor 模式，内核完成数据拷贝后再通知用户，用户态完全不需要阻塞去等待数据准备好。

### Q3: 详细解释一下 NtyCo 协程是如何工作的？上下文切换做了什么？
**技术细节 (基于 `nty_coroutine.c`):**
*   **上下文 (Context):** 每个协程有独立的栈空间和寄存器状态（如栈指针 `%rsp`，指令指针 `%rip` 等）。
*   **切换 (Switch):** 核心是 `_switch(new_ctx, cur_ctx)` 函数，保存当前 CPU 寄存器到当前协程结构体，并加载目标协程的寄存器。
*   **Hook 机制:** 我们 Hook 了 `read`、`write` 等系统调用。
    *   当协程调用 `read` 阻塞时，框架会自动将其加入 Epoll 监控列表。
    *   调用 `nty_coroutine_yield` 让出 CPU 给调度器。
    *   调度器选择下一个“就绪”的协程执行。
    *   当 Epoll 事件触发，调度器恢复 (Resume) 原协程执行。

---

## 🐛 亮点 Bug：高并发竞态条件解决

### Q4: 开发过程中遇到过最难的 Bug 是什么？(必问)
**背景:** 在高并发压测（benchmarking）时，NtyCo 模型不仅性能没上去，服务端还频繁崩溃，报错 `Assertion 'co_tmp == NULL' failed`。
**定位过程:**
1.  **调试:** 使用 GDB 抓取 Core Dump，发现崩溃在红黑树插入操作 `nty_schedule_sched_wait`。原因是尝试向等待队列插入一个已经存在的 FD（文件描述符）。
2.  **根因分析:** 这是典型的**栈变量生命周期**导致的竞态条件。
    *   **错误代码:**
        ```c
        // BUG
        int cli_fd = accept(...); // cli_fd 是栈变量
        nty_coroutine_create(&read_co, server_reader, &cli_fd); // 传入了栈变量的地址
        ```
    *   **现象:** 主线程循环处理连接非常快。在第一个协程还没来得及执行并读取 `*arg` (即 `cli_fd`) 之前，`accept` 又返回了新的连接，**覆盖了** 栈上 `cli_fd` 的值。导致两个不同的协程拿到了同一个 FD 值，引发内部数据结构冲突。
3.  **解决方案:** 堆内存分配（Heap Allocation）。
    *   **修复代码:**
        ```c
        // FIX
        int *fd_ptr = malloc(sizeof(int)); // 申请堆内存
        *fd_ptr = cli_fd;
        nty_coroutine_create(&read_co, server_reader, fd_ptr);
        // ... 在协程函数内部: 读取完 fd 后 free(fd_ptr);
        ```
**结果:** 修复后系统稳定，且能够支撑高并发压测。这体现了对**内存模型**和**并发时序**的深刻理解。

---

## 💾 数据结构与存储引擎

### Q5: 为什么实现了三种存储结构？(RBTree vs Hash vs BTree)
*   **红黑树 (RBTree):**
    *   *特点:* 自平衡二叉搜索树，插入/查找/删除均为 O(log N)。
    *   *场景:* 适合需要 Range Scan（范围查询）的场景。如果你问我为什么选它做默认？因为 Linux 内核（epoll, 调度器）都在用，标准且性能稳定。
*   **哈希表 (Hash Table):**
    *   *特点:* 理论 O(1)。
    *   *缺点:* 扩容 (Rehash) 时会有性能抖动（Stop-the-world 风显），存在哈希冲突（链地址法解决）。
*   **B-Tree:**
    *   *特点:* 多路搜索树，层高低。
    *   *优势:* **缓存友好性 (Cache Locality)**。相比红黑树，B-Tree 节点利用了 CPU Cache Line，访问内存次数更少（虽然本项目是纯内存版，但原理通用）。

---

## 📊 性能分析

### Q6: 压测数据如何？瓶颈在哪里？
*   **数据:** 
    *   Redis (Pipeline=16): ~9w QPS
    *   KVStore (io_uring): ~36w QPS
*   **为什么快 4 倍？**
    *   Redis 是单线程 Epoll。KVStore 在 io_uring 模式下虽然也是单线程处理 I/O 提交（或者是使用了内核轮询），但极大减少了系统调用上下文切换。
    *   **多线程:** 我们目前的 Benchmark 工具支持多线程压测，如果不开启 Pipeline，网络往返时间 (RTT) 是主要瓶颈。开启 Pipeline 后，CPU/内存拷贝成为瓶颈。
*   **当前瓶颈:** 
    *   **内存拷贝:** 即使网络层零拷贝了，KV 存储层还是要把数据从 buffer 拷贝到数据结构中。
    *   **锁竞争:** 如果开启多线程 worker 处理请求，全局的 KV 存储锁（如 RBTree 的互斥锁）会是最大瓶颈。

---

## 🧠 行为与反思

### Q7: 如果还有时间，你会优化什么？
*   **持久化:** 目前是纯内存的。我会模仿 Redis 实现 AOF (Append Only File) 或 RDB 快照。也可以考虑使用 `mmap` 做内存映射文件。
*   **分布式:** 引入 Raft 协议实现多副本一致性，或者引入一致性哈希实现分片集群。
*   **内存池:** 引入 `jemalloc` 或 `tcmalloc` 替换默认的 `malloc`，减少内存碎片。

---

## 📝 面试突击检查清单 (必背!)
*   [ ] **Reactor 与 Proactor 的区别** (I/O多路复用 vs 异步I/O)
*   [ ] **用户态 vs 内核态** (Ring 0 vs Ring 3, 切换开销)
*   [ ] **进程 vs 线程 vs 协程** (拥有资源的单位 vs 调度的单位 vs 用户态线程)
*   [ ] **TCP 三次握手与四次挥手** (特别是 TIME_WAIT 和 CLOSE_WAIT 状态)
*   [ ] **零拷贝 (Zero Copy)** 原理 (sendfile, splice, mmap)
