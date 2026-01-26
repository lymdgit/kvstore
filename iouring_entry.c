

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

// port:2048-2067
#define IOURING_ENTRIES 1024
#define IOURING_PORT_COUNT 20
#define IOURING_BASE_PORT 2048

// io_uring 事件类型
enum {
  EVENT_TYPE_ACCEPT = 0,
  EVENT_TYPE_READ,
  EVENT_TYPE_WRITE,
};

// 连接请求信息
struct conn_info {
  int fd;
  int type;
};

// io_uring 上下文
struct io_uring ring;

// 连接管理 - 只在 NETWORK_IOURING 时编译
struct conn_item uring_connlist[1048576] = {0};

// 时间统计
struct timeval uring_tv_begin;

#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 初始化服务器 socket
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

// 提交 accept 请求
int add_accept_request(int sockfd, struct sockaddr_in *client_addr,
                       socklen_t *client_len) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return -1;
  }

  io_uring_prep_accept(sqe, sockfd, (struct sockaddr *)client_addr, client_len,
                       0);

  struct conn_info *conn = (struct conn_info *)malloc(sizeof(struct conn_info));
  if (!conn) {
    return -1;
  }
  conn->fd = sockfd;
  conn->type = EVENT_TYPE_ACCEPT;

  io_uring_sqe_set_data(sqe, conn);

  return 0;
}

// 提交 read 请求
int add_read_request(int clientfd) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return -1;
  }

  // 初始化连接项
  uring_connlist[clientfd].fd = clientfd;
  memset(uring_connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
  uring_connlist[clientfd].rlen = 0;

  io_uring_prep_recv(sqe, clientfd, uring_connlist[clientfd].rbuffer,
                     BUFFER_LENGTH, 0);

  struct conn_info *conn = (struct conn_info *)malloc(sizeof(struct conn_info));
  if (!conn) {
    return -1;
  }
  conn->fd = clientfd;
  conn->type = EVENT_TYPE_READ;

  io_uring_sqe_set_data(sqe, conn);

  return 0;
}

// 提交 write 请求
int add_write_request(int clientfd) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return -1;
  }

  io_uring_prep_send(sqe, clientfd, uring_connlist[clientfd].wbuffer,
                     uring_connlist[clientfd].wlen, 0);

  struct conn_info *conn = (struct conn_info *)malloc(sizeof(struct conn_info));
  if (!conn) {
    return -1;
  }
  conn->fd = clientfd;
  conn->type = EVENT_TYPE_WRITE;

  io_uring_sqe_set_data(sqe, conn);

  return 0;
}

// 处理 accept 完成事件
int handle_accept_completion(int sockfd, int clientfd) {

  if (clientfd < 0) {
    printf("accept failed: %d\n", clientfd);
    return -1;
  }

  // 添加 read 请求
  add_read_request(clientfd);

  // 继续监听新连接
  static struct sockaddr_in client_addr;
  static socklen_t client_len = sizeof(client_addr);
  add_accept_request(sockfd, &client_addr, &client_len);

  // 统计连接数
  if ((clientfd % 1000) == 999) {
    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, uring_tv_begin);

    memcpy(&uring_tv_begin, &tv_cur, sizeof(struct timeval));

    printf("clientfd : %d, time_used: %d\n", clientfd, time_used);
  }

  return 0;
}

// 处理 read 完成事件
int handle_read_completion(int clientfd, int bytes_read) {

  if (bytes_read <= 0) {
    if (bytes_read == 0) {
      printf("disconnect: fd=%d\n", clientfd);
    }
    close(clientfd);
    return -1;
  }

  uring_connlist[clientfd].rlen = bytes_read;

  // 处理 kvstore 请求
  kvstore_request(&uring_connlist[clientfd]);
  // wlen 已在 kvstore_request 中设置

  // 添加 write 请求
  add_write_request(clientfd);

  return 0;
}

// 处理 write 完成事件
int handle_write_completion(int clientfd, int bytes_written) {

  if (bytes_written < 0) {
    close(clientfd);
    return -1;
  }

  // 写完成后，继续读取
  add_read_request(clientfd);

  return 0;
}

// io_uring 入口函数
int iouring_entry(void) {

  int i = 0;

  // 初始化 io_uring
  if (io_uring_queue_init(IOURING_ENTRIES, &ring, 0) < 0) {
    perror("io_uring_queue_init");
    return -1;
  }

  // 初始化多个端口
  static struct sockaddr_in client_addr;
  static socklen_t client_len = sizeof(client_addr);

  for (i = 0; i < IOURING_PORT_COUNT; i++) {
    int sockfd = uring_init_server(IOURING_BASE_PORT + i);
    if (sockfd < 0) {
      continue;
    }
    printf("listen port: %d\n", IOURING_BASE_PORT + i);
    add_accept_request(sockfd, &client_addr, &client_len);
  }

  // 提交所有请求
  io_uring_submit(&ring);

  gettimeofday(&uring_tv_begin, NULL);

  // 事件循环
  while (1) {

    struct io_uring_cqe *cqe;

    // 等待完成事件
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      perror("io_uring_wait_cqe");
      continue;
    }

    // 获取用户数据
    struct conn_info *conn = (struct conn_info *)io_uring_cqe_get_data(cqe);
    if (!conn) {
      io_uring_cqe_seen(&ring, cqe);
      continue;
    }

    int res = cqe->res;

    // 根据事件类型处理
    switch (conn->type) {

    case EVENT_TYPE_ACCEPT: {
      handle_accept_completion(conn->fd, res);
      break;
    }

    case EVENT_TYPE_READ: {
      handle_read_completion(conn->fd, res);
      break;
    }

    case EVENT_TYPE_WRITE: {
      handle_write_completion(conn->fd, res);
      break;
    }
    }

    free(conn);
    io_uring_cqe_seen(&ring, cqe);
    io_uring_submit(&ring);
  }

  io_uring_queue_exit(&ring);

  return 0;
}

#else

// 非 NETWORK_IOURING 模式时提供空实现
int iouring_entry(void) { return 0; }

#endif
