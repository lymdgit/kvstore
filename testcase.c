

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <getopt.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>

#define MAX_MAS_LENGTH 512
#define PIPELINE_BATCH_SIZE 10 // 每批发送的命令数

#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 发送消息，通过系统调用send
int send_msg(int connfd, char *msg, int length) {

  int res = send(connfd, msg, length, 0);
  if (res < 0) {
    perror("send");
    exit(1);
  }
  return res;
}

// 接收消息，通过系统调用recv
int recv_msg(int connfd, char *msg, int length) {

  int res = recv(connfd, msg, length, 0);
  if (res < 0) {
    perror("recv");
    exit(1);
  }
  return res;
}

void equals(char *pattern, char *result, char *casename) {

  if (strcmp(pattern, result) == 0) {
    // printf("==> PASS --> %s\n", casename);
  } else {
    printf("==> FAILED --> %s, '%s' != '%s'\n", casename, pattern, result);
  }
}

// "SET Name King\n" "SUCCESS"
void test_case(int connfd, char *msg, char *pattern, char *casename) {

  if (!msg || !pattern || !casename)
    return;

  send_msg(connfd, msg, strlen(msg));

  char result[MAX_MAS_LENGTH] = {0};
  recv_msg(connfd, result, MAX_MAS_LENGTH);

  // 去掉响应末尾的 \n（服务端响应也带 \n）
  int len = strlen(result);
  if (len > 0 && result[len - 1] == '\n') {
    result[len - 1] = '\0';
  }

  equals(pattern, result, casename);
}

// 为了解决粘包问题，加入\n作为消息的边界
void array_testcase(int connfd) {

  test_case(connfd, "SET Name King\n", "SUCCESS", "SETCase");
  test_case(connfd, "GET Name\n", "King", "GETCase");
  test_case(connfd, "MOD Name Darren\n", "SUCCESS", "MODCase");
  test_case(connfd, "GET Name\n", "Darren", "GETCase");
  test_case(connfd, "DEL Name\n", "SUCCESS", "DELCase");
  test_case(connfd, "GET Name\n", "NO EXIST", "GETCase");
}

void array_testcase_10w(int connfd) { // 10w

  int count = 100000;
  int i = 0;

  while (i++ < count) {
    array_testcase(connfd);
  }
}

void rbtree_testcase(int connfd) {

  test_case(connfd, "RSET Name King\n", "SUCCESS", "SETCase");
  test_case(connfd, "RGET Name\n", "King", "GETCase");
  test_case(connfd, "RMOD Name Darren\n", "SUCCESS", "MODCase");
  test_case(connfd, "RGET Name\n", "Darren", "GETCase");
  test_case(connfd, "RDEL Name\n", "SUCCESS", "DELCase");
  test_case(connfd, "RGET Name\n", "NO EXIST", "GETCase");
}

void rbtree_testcase_10w(int connfd) { // 10w

  int count = 100000;
  int i = 0;

  while (i++ < count) {
    rbtree_testcase(connfd);
  }
}

void rbtree_testcase_5w_node(int connfd) {

  int count = 50000;
  int i = 0;

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "RSET Name%d King%d", i, i);
    test_case(connfd, cmd, "SUCCESS", "SETCase");

    char result[128] = {0};
    sprintf(result, "%d", i + 1);
    test_case(connfd, "RCOUNT", result, "RCOUNT");
  }

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "RDEL Name%d", i);
    test_case(connfd, cmd, "SUCCESS", "RDELCase");

    char result[128] = {0};
    sprintf(result, "%d", count - (i + 1));
    test_case(connfd, "RCOUNT", result, "RCOUNT");
  }
}

void hash_testcase(int connfd) {

  test_case(connfd, "HSET Name King\n", "SUCCESS", "HSETCase");
  test_case(connfd, "HGET Name\n", "King", "HGETCase");
  test_case(connfd, "HMOD Name Darren\n", "SUCCESS", "HMODCase");
  test_case(connfd, "HGET Name\n", "Darren", "HGETCase");
  test_case(connfd, "HDEL Name\n", "SUCCESS", "HDELCase");
  test_case(connfd, "HGET Name\n", "NO EXIST", "HGETCase");
}

void hash_testcase_10w(int connfd) { // 10w

  int count = 100000;
  int i = 0;

  while (i++ < count) {
    hash_testcase(connfd);
  }
}

void hash_testcase_5w_node(int connfd) {

  int count = 50000;
  int i = 0;

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "HSET Name%d King%d", i, i);
    test_case(connfd, cmd, "SUCCESS", "SETCase");

    char result[128] = {0};
    sprintf(result, "%d", i + 1);
    test_case(connfd, "HCOUNT", result, "HCOUNT");
  }

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "HDEL Name%d", i);
    test_case(connfd, cmd, "SUCCESS", "HDELCase");

    char result[128] = {0};
    sprintf(result, "%d", count - (i + 1));
    test_case(connfd, "HCOUNT", result, "RCOUNT");
  }
}

void skiptable_testcase(int connfd) {

  test_case(connfd, "SSET Name King\n", "SUCCESS", "SSETCase");
  test_case(connfd, "SGET Name\n", "King", "SGETCase");
  test_case(connfd, "SMOD Name Darren\n", "SUCCESS", "SMODCase");
  test_case(connfd, "SGET Name\n", "Darren", "SGETCase");
  test_case(connfd, "SDEL Name\n", "SUCCESS", "SDELCase");
  test_case(connfd, "SGET Name\n", "NO EXIST", "SGETCase");
}

void skiptable_testcase_5w_node(int connfd) {

  int count = 50000;
  int i = 0;

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "SSET Name%d King%d", i, i);
    test_case(connfd, cmd, "SUCCESS", "SSETCase");

    char result[128] = {0};
    sprintf(result, "%d", i + 1);
    test_case(connfd, "SCOUNT", result, "SCOUNT");
  }

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "SDEL Name%d", i);
    test_case(connfd, cmd, "SUCCESS", "SDELCase");

    char result[128] = {0};
    sprintf(result, "%d", count - (i + 1));
    test_case(connfd, "SCOUNT", result, "SCOUNT");
  }
}

void btree_testcase(int connfd) {

  test_case(connfd, "BSET Name King\n", "SUCCESS", "BSETCase");
  test_case(connfd, "BGET Name\n", "King", "BGETCase");
  test_case(connfd, "BMOD Name Darren\n", "SUCCESS", "BMODCase");
  test_case(connfd, "BGET Name\n", "Darren", "BGETCase");
  test_case(connfd, "BDEL Name\n", "SUCCESS", "BDELCase");
  test_case(connfd, "BGET Name\n", "NO EXIST", "BGETCase");
}

void btree_testcase_5w_node(int connfd) {

  int count = 50000;
  int i = 0;

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "BSET Name%d King%d", i, i);
    test_case(connfd, cmd, "SUCCESS", "BSETCase");

    char result[128] = {0};
    sprintf(result, "%d", i + 1);
    test_case(connfd, "BCOUNT", result, "BCOUNT");
  }

  for (i = 0; i < count; i++) {

    char cmd[128] = {0};

    snprintf(cmd, 128, "BDEL Name%d", i);
    test_case(connfd, cmd, "SUCCESS", "BDELCase");

    char result[128] = {0};
    sprintf(result, "%d", count - (i + 1));
    test_case(connfd, "BCOUNT", result, "BCOUNT");
  }
}

// ===================== Pipeline 测试函数 =====================

// Pipeline 模式：Array 1千节点测试（Array 最大容量 1024）
void array_testcase_pipeline(int connfd) {
  int total = 50000; // Array 最大容量 KVS_ARRAY_SIZE = 512000
  int batch = PIPELINE_BATCH_SIZE;
  char send_buf[MAX_MAS_LENGTH];
  char recv_buf[MAX_MAS_LENGTH];
  int i, j;

  // 插入阶段
  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos,
                      "SET Name%d King%d\n", j, j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }

  // 删除阶段
  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos, "DEL Name%d\n", j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }
}

// Pipeline 模式：RBTree 5万节点测试
// 批量发送命令，一次性接收响应
void rbtree_testcase_pipeline(int connfd) {
  int total = 50000;
  int batch = PIPELINE_BATCH_SIZE;
  char send_buf[MAX_MAS_LENGTH];
  char recv_buf[MAX_MAS_LENGTH];
  int i, j;

  // 插入阶段
  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    // 批量构造命令
    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos,
                      "RSET Name%d King%d\n", j, j);
    }

    // 一次发送
    send(connfd, send_buf, pos, 0);

    // 一次接收
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }

  // 删除阶段
  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos, "RDEL Name%d\n", j);
    }

    send(connfd, send_buf, pos, 0);

    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }
}

// Pipeline 模式：Hash 5万节点测试
void hash_testcase_pipeline(int connfd) {
  int total = 50000;
  int batch = PIPELINE_BATCH_SIZE;
  char send_buf[MAX_MAS_LENGTH];
  char recv_buf[MAX_MAS_LENGTH];
  int i, j;

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos,
                      "HSET Name%d King%d\n", j, j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos, "HDEL Name%d\n", j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }
}

// Pipeline 模式：SkipList 5万节点测试
void skiptable_testcase_pipeline(int connfd) {
  int total = 50000;
  int batch = PIPELINE_BATCH_SIZE;
  char send_buf[MAX_MAS_LENGTH];
  char recv_buf[MAX_MAS_LENGTH];
  int i, j;

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos,
                      "SSET Name%d King%d\n", j, j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos, "SDEL Name%d\n", j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }
}

// Pipeline 模式：BTree 5万节点测试
void btree_testcase_pipeline(int connfd) {
  int total = 50000;
  int batch = PIPELINE_BATCH_SIZE;
  char send_buf[MAX_MAS_LENGTH];
  char recv_buf[MAX_MAS_LENGTH];
  int i, j;

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos,
                      "BSET Name%d King%d\n", j, j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }

  for (i = 0; i < total; i += batch) {
    int end = (i + batch > total) ? total : i + batch;
    int pos = 0;

    for (j = i; j < end; j++) {
      pos += snprintf(send_buf + pos, MAX_MAS_LENGTH - pos, "BDEL Name%d\n", j);
    }

    send(connfd, send_buf, pos, 0);
    int recv_len = recv(connfd, recv_buf, MAX_MAS_LENGTH - 1, 0);
    if (recv_len > 0)
      recv_buf[recv_len] = '\0';
  }
}

int connect_tcpserver(const char *ip, unsigned short port) {

  int connfd = socket(AF_INET, SOCK_STREAM, 0);

  struct sockaddr_in tcpserver_addr;
  memset(&tcpserver_addr, 0, sizeof(struct sockaddr_in));

  tcpserver_addr.sin_family = AF_INET;
  tcpserver_addr.sin_addr.s_addr = inet_addr(ip);
  tcpserver_addr.sin_port = htons(port);

  int ret = connect(connfd, (struct sockaddr *)&tcpserver_addr,
                    sizeof(struct sockaddr_in));
  if (ret) {
    perror("connect");
    return -1;
  }

  return connfd;
}

// array: 0x01, rbtree: 0x02, hash: 0x04, skiptable: 0x08, btree: 0x10

// ./testcase -s 127.0.0.1 -p 9096 -m 1
int main(int argc, char *argv[]) {

  int ret = 0;

  char ip[16] = {0};
  int port = 0;
  int mode = 1;

  int opt;
  while ((opt = getopt(argc, argv, "s:p:m:?")) != -1) {

    switch (opt) {

    case 's':
      strcpy(ip, optarg);
      break;

    case 'p':
      port = atoi(optarg);
      break;

    case 'm':
      mode = atoi(optarg);
      break;

    default:
      return -1;
    }
  }

  int connfd = connect_tcpserver(ip, port);
  // -m 1 (Pipeline 模式)
  if (mode & 0x1) { // array

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    array_testcase_pipeline(connfd); // Pipeline 版本

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    printf("array pipeline--> time_used: %d, qps: %d\n", time_used,
           2000 * 1000 / time_used); // 1000 SET + 1000 DEL
  }
  // -m 2 (Pipeline 模式)
  if (mode & 0x2) { // rbtree

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    rbtree_testcase_pipeline(connfd); // Pipeline 版本

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    printf("rbtree pipeline-->  time_used: %d, qps: %d\n", time_used,
           100000 * 1000 / time_used);
  }
  // -m 4 (Pipeline 模式)
  if (mode & 0x4) { // hash

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    hash_testcase_pipeline(connfd); // Pipeline 版本

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    printf("hash pipeline-->  time_used: %d, qps: %d\n", time_used,
           100000 * 1000 / time_used);
  }
  // -m 8 (Pipeline 模式)
  if (mode & 0x8) { // skiptable

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    skiptable_testcase_pipeline(connfd); // Pipeline 版本

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    printf("skiptable pipeline-->  time_used: %d, qps: %d\n", time_used,
           100000 * 1000 / time_used);
  }
  // -m 16 (Pipeline 模式)
  if (mode & 0x10) { // btree

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    btree_testcase_pipeline(connfd); // Pipeline 版本

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    printf("btree pipeline-->  time_used: %d, qps: %d\n", time_used,
           100000 * 1000 / time_used);
  }

  close(connfd);
  return 0;
}
