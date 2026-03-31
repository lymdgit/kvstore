

#include "kvstore.h"

#if (ENABLE_NETWORK_SELECT == NETWORK_IOURING)

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <liburing.h>
#include <sys/time.h>

// ============================================================================
// Configuration
// ============================================================================
#define IOURING_ENTRIES 4096      // 更大的 SQ/CQ ring，避免高并发下溢出
#define IOURING_PORT_COUNT 20
#define IOURING_BASE_PORT 2048

// SQPOLL mode configuration
#define ENABLE_SQPOLL 1
#define SQPOLL_IDLE_MS 2000 // Kernel thread sleeps after 2s idle

// fd 关闭标志：防止 fd 复用后 use-after-close
#define FD_CLOSED_FLAG -1

// io_uring event types
enum {
  EVENT_TYPE_ACCEPT = 0,
  EVENT_TYPE_READ,
  EVENT_TYPE_WRITE,
};

// ============================================================================
// Global State
// ============================================================================

// io_uring context
struct io_uring ring;

// Connection management - pre-allocated to avoid malloc per request
struct conn_item uring_connlist[1048576] = {0};

// Special conn_item entries for accept events (one per listen socket)
struct conn_item uring_accept_conns[IOURING_PORT_COUNT] = {0};

// Time statistics
struct timeval uring_tv_begin;

// Bug fix: 为每个监听端口分配独立的 client_addr，避免并发 accept 覆盖
static struct sockaddr_in accept_client_addrs[IOURING_PORT_COUNT];
static socklen_t accept_client_lens[IOURING_PORT_COUNT];

#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// ============================================================================
// Server Socket Initialization
// ============================================================================
int uring_init_server(unsigned short port) {

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in serveraddr;
  memset(&serveraddr, 0, sizeof(struct sockaddr_in));

  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons(port);

  if (-1 ==
      bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(struct sockaddr))) {
    perror("bind");
    close(sockfd);
    return -1;
  }

  listen(sockfd, 512);

  return sockfd;
}

// ============================================================================
// SQE Submission Functions (No malloc - use embedded conn_item)
// ============================================================================

// Submit accept request using pre-allocated accept_conn
int add_accept_request(int sockfd, int accept_idx,
                       struct sockaddr_in *client_addr, socklen_t *client_len) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return -1;
  }

  io_uring_prep_accept(sqe, sockfd, (struct sockaddr *)client_addr, client_len,
                       0);

  // Use pre-allocated accept conn_item
  uring_accept_conns[accept_idx].fd = sockfd;
  uring_accept_conns[accept_idx].uring_event_type = EVENT_TYPE_ACCEPT;

  io_uring_sqe_set_data(sqe, &uring_accept_conns[accept_idx]);

  return 0;
}

// Submit read request using pre-allocated connlist entry
// 若 SQ ring 已满，先 submit 把已有 SQE 推给内核，再重试一次
int add_read_request(int clientfd) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    // SQ ring 满了：flush 已有 SQE，再重试
    io_uring_submit(&ring);
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "[iouring] SQ ring full, drop read for fd=%d\n", clientfd);
      close(clientfd);
      uring_connlist[clientfd].fd = FD_CLOSED_FLAG;
      return -1;
    }
  }

  // Initialize connection item (no malloc!)
  uring_connlist[clientfd].fd = clientfd;
  memset(uring_connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
  uring_connlist[clientfd].rlen = 0;
  uring_connlist[clientfd].uring_event_type = EVENT_TYPE_READ;

  io_uring_prep_recv(sqe, clientfd, uring_connlist[clientfd].rbuffer,
                     BUFFER_LENGTH, 0);

  io_uring_sqe_set_data(sqe, &uring_connlist[clientfd]);

  return 0;
}

// Submit write request using pre-allocated connlist entry
int add_write_request(int clientfd) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    // SQ ring 满了：flush 已有 SQE，再重试
    io_uring_submit(&ring);
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "[iouring] SQ ring full, drop write for fd=%d\n", clientfd);
      close(clientfd);
      uring_connlist[clientfd].fd = FD_CLOSED_FLAG;
      return -1;
    }
  }

  uring_connlist[clientfd].uring_event_type = EVENT_TYPE_WRITE;

  io_uring_prep_send(sqe, clientfd, uring_connlist[clientfd].wbuffer,
                     uring_connlist[clientfd].wlen, 0);

  io_uring_sqe_set_data(sqe, &uring_connlist[clientfd]);

  return 0;
}

// ============================================================================
// CQE Completion Handlers
// ============================================================================

// Handle accept completion
int handle_accept_completion(int accept_idx, int clientfd) {

  if (clientfd < 0) {
    return -1;
  }

  // 重要：为新连接初始化 rlen = 0
  // 防止 fd 复用时读取到旧连接的残留数据
  uring_connlist[clientfd].rlen = 0;

  // Add read request for new client
  add_read_request(clientfd);

  // Bug fix: 使用独立的 client_addr，避免并发覆盖
  int sockfd = uring_accept_conns[accept_idx].fd;
  accept_client_lens[accept_idx] = sizeof(accept_client_addrs[accept_idx]);
  add_accept_request(sockfd, accept_idx,
                     &accept_client_addrs[accept_idx],
                     &accept_client_lens[accept_idx]);



  return 0;
}

// Handle read completion
int handle_read_completion(int clientfd, int bytes_read) {

  // 检查 fd 是否已被标记为关闭（fd 复用保护）
  if (uring_connlist[clientfd].fd == FD_CLOSED_FLAG) {
    return -1;
  }

  if (bytes_read <= 0) {
    if (bytes_read == -ECANCELED) {
      // 连接已关闭后内核取消了挂起的 read，正常现象，静默处理
      return -1;
    }
    close(clientfd);
    uring_connlist[clientfd].fd = FD_CLOSED_FLAG;
    uring_connlist[clientfd].rlen = 0;
    return -1;
  }

  uring_connlist[clientfd].rlen = bytes_read;
  uring_connlist[clientfd].rbuffer[bytes_read] = '\0';

  // Process kvstore request
  kvstore_request(&uring_connlist[clientfd]);

  // Add write request
  add_write_request(clientfd);

  return 0;
}

// Handle write completion
int handle_write_completion(int clientfd, int bytes_written) {

  // 检查 fd 是否已被标记为关闭
  if (uring_connlist[clientfd].fd == FD_CLOSED_FLAG) {
    return -1;
  }

  if (bytes_written < 0) {
    if (bytes_written == -ECANCELED) {
      // 连接关闭后内核取消了挂起的 write，静默处理
      uring_connlist[clientfd].fd = FD_CLOSED_FLAG;
      return -1;
    }
    close(clientfd);
    uring_connlist[clientfd].fd = FD_CLOSED_FLAG;
    return -1;
  }

  // Continue reading after write completes
  add_read_request(clientfd);

  return 0;
}

// ============================================================================
// Main Entry Point with SQPOLL Mode
// ============================================================================
int iouring_entry(void) {

  int i = 0;

  // Initialize io_uring with SQPOLL mode for zero syscall submission
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));

  // Runtime flag to track if SQPOLL is actually active
  int sqpoll_active = 0;

#if ENABLE_SQPOLL
  params.flags = IORING_SETUP_SQPOLL;
  params.sq_thread_idle = SQPOLL_IDLE_MS;
  printf("[io_uring] SQPOLL mode enabled (idle timeout: %dms)\n",
         SQPOLL_IDLE_MS);
#endif

  if (io_uring_queue_init_params(IOURING_ENTRIES, &ring, &params) < 0) {
    perror("io_uring_queue_init_params");
    // Fallback to non-SQPOLL mode
    printf("[io_uring] Fallback to standard mode (SQPOLL requires root)\n");
    if (io_uring_queue_init(IOURING_ENTRIES, &ring, 0) < 0) {
      perror("io_uring_queue_init");
      return -1;
    }
    sqpoll_active = 0; // Fallback to standard mode
  } else {
#if ENABLE_SQPOLL
    sqpoll_active = 1; // SQPOLL initialized successfully
    printf("[io_uring] SQPOLL kernel thread active\n");
#endif
  }

  // Initialize listen sockets on multiple ports
  for (i = 0; i < IOURING_PORT_COUNT; i++) {
    int sockfd = uring_init_server(IOURING_BASE_PORT + i);
    if (sockfd < 0) {
      continue;
    }
    printf("listen port: %d\n", IOURING_BASE_PORT + i);
    // Bug fix: 使用独立的 client_addr
    accept_client_lens[i] = sizeof(accept_client_addrs[i]);
    add_accept_request(sockfd, i, &accept_client_addrs[i], &accept_client_lens[i]);
  }

  // Initial submit (only needed once, SQPOLL takes over after this)
  io_uring_submit(&ring);

  gettimeofday(&uring_tv_begin, NULL);

  printf("[io_uring] Event loop started\n");

  // Main event loop with batch CQE processing
  while (1) {

    struct io_uring_cqe *cqe;
    unsigned head;
    int cqe_count = 0;

    // Wait for at least one completion event
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      perror("io_uring_wait_cqe");
      continue;
    }

    // Batch process all available CQEs (key optimization!)
    io_uring_for_each_cqe(&ring, head, cqe) {

      struct conn_item *item = (struct conn_item *)io_uring_cqe_get_data(cqe);
      if (!item) {
        cqe_count++;
        continue;
      }

      int res = cqe->res;
      int event_type = item->uring_event_type;

      // Dispatch based on event type
      switch (event_type) {

      case EVENT_TYPE_ACCEPT: {
        // Find accept_idx from item pointer
        int accept_idx = (int)(item - uring_accept_conns);
        handle_accept_completion(accept_idx, res);
        break;
      }

      case EVENT_TYPE_READ: {
        handle_read_completion(item->fd, res);
        break;
      }

      case EVENT_TYPE_WRITE: {
        handle_write_completion(item->fd, res);
        break;
      }
      }

      cqe_count++;
    }

    // Advance CQ ring in one batch (more efficient than per-event cqe_seen)
    io_uring_cq_advance(&ring, cqe_count);

    // Submit new SQEs to kernel
    // IMPORTANT: io_uring_submit() must always be called to update SQ tail
    // pointer! In SQPOLL mode, liburing internally avoids syscall unless
    // NEED_WAKEUP is set.
    io_uring_submit(&ring);
  }

  io_uring_queue_exit(&ring);

  return 0;
}

#else

// Non NETWORK_IOURING mode provides empty implementation
int iouring_entry(void) { return 0; }

#endif
