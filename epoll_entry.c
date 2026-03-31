

#include "kvstore.h"

#if (ENABLE_NETWORK_SELECT == NETWORK_EPOLL)

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <sys/time.h>

// listenfd
// EPOLLIN -->
int accept_cb(int fd);
// clientfd
//
int recv_cb(int fd);
int send_cb(int fd);

// conn, fd, buffer, callback

int epfd = 0;
struct conn_item connlist[1048576] = {0}; // 只在 NETWORK_EPOLL 时编译
// list
struct timeval zvoice_king;
//
// 1000000

#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int set_event(int fd, int event, int flag) {

  if (flag) { // 1 add, 0 mod
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
  } else {

    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
  }

  return 0;
}

int accept_cb(int fd) {

  struct sockaddr_in clientaddr;
  socklen_t len = sizeof(clientaddr);

  int clientfd = accept(fd, (struct sockaddr *)&clientaddr, &len);
  if (clientfd < 0) {
    return -1;
  }
  set_event(clientfd, EPOLLIN, 1);

  connlist[clientfd].fd = clientfd;
  memset(connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
  connlist[clientfd].rlen = 0;
  memset(connlist[clientfd].wbuffer, 0, BUFFER_LENGTH);
  connlist[clientfd].wlen = 0;

  connlist[clientfd].recv_t.recv_callback = recv_cb;
  connlist[clientfd].send_callback = send_cb;



  return clientfd;
}

int recv_cb(int fd) { // fd --> EPOLLIN

  char *buffer = connlist[fd].rbuffer;

  int count = recv(fd, buffer, BUFFER_LENGTH - 1, 0); // 留一个字节给 \0
  if (count == 0) {

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    return -1;
  }

  // 确保字符串以 \0 结尾
  buffer[count] = '\0';
  connlist[fd].rlen = count;

#if 0 // echo: need to send
	memcpy(connlist[fd].wbuffer, connlist[fd].rbuffer, connlist[fd].rlen);
	connlist[fd].wlen = connlist[fd].rlen;
	connlist[fd].rlen -= connlist[fd].rlen;
#else

  // 当fd可读时，进行协议解析
  kvstore_request(&connlist[fd]);
  connlist[fd].wlen = strlen(connlist[fd].wbuffer);
#endif
  // 设置fd可写
  set_event(fd, EPOLLOUT, 0);

  return count;
}

// 当fd可写时，进行协议发送
int send_cb(int fd) {

  char *buffer = connlist[fd].wbuffer;
  int idx = connlist[fd].wlen;
  // 发送数据
  int count = send(fd, buffer, idx, 0);

  // 设置fd可读
  set_event(fd, EPOLLIN, 0);

  return count;
}

int init_server(unsigned short port) {

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);

  struct sockaddr_in serveraddr;
  memset(&serveraddr, 0, sizeof(struct sockaddr_in));

  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons(port);

  if (-1 ==
      bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(struct sockaddr))) {
    perror("bind");
    return -1;
  }

  listen(sockfd, 512);

  return sockfd;
}

// port:2048-2067
int epoll_entry(void) {

  int port_count = 20;
  unsigned short port = 2048;
  int i = 0;

  epfd = epoll_create(1); // int size
  // 创建监听socket，并设置为检测 可读状态（accept_cb）
  for (i = 0; i < port_count; i++) {
    int sockfd = init_server(port + i); // 2048, 2049, 2050, 2051 ... 2057
    connlist[sockfd].fd = sockfd;
    connlist[sockfd].recv_t.accept_callback = accept_cb;
    set_event(sockfd, EPOLLIN, 1);
  }

  gettimeofday(&zvoice_king, NULL);

  struct epoll_event events[1024] = {0};

  while (1) { // mainloop();

    int nready = epoll_wait(epfd, events, 1024, -1); //

    int i = 0;
    for (i = 0; i < nready; i++) {

      int connfd = events[i].data.fd;
      if (events[i].events & EPOLLIN) { //

        int count = connlist[connfd].recv_t.recv_callback(connfd);
        // printf("recv count: %d <-- buffer: %s\n", count,
        // connlist[connfd].rbuffer);

      } else if (events[i].events & EPOLLOUT) {
        // printf("send --> buffer: %s\n",  connlist[connfd].wbuffer);

        int count = connlist[connfd].send_callback(connfd);
      }
    }
  }

  return 0;
}

#else

// 非 NETWORK_EPOLL 模式时提供空实现
int epoll_entry(void) { return 0; }

#endif
