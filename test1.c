#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

// 假设chnid_t是一个已定义的类型，替换为实际类型
typedef uint32_t chnid_t;

// 发送端函数，添加序号并记录日志
int send_with_sequence(int serversd, const char *sbufp, size_t len, chnid_t chnid, struct sockaddr_in *sndaddr) {
    // 定义序号
    static uint32_t sequence_number = 0; // 静态变量，保持递增
    size_t total_len = sizeof(uint32_t) + sizeof(chnid_t) + len; // 总长度：序号 + chnid + 音频数据
    char *new_buffer = malloc(total_len); // 分配新缓冲区
    if (!new_buffer) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }

    // 构造数据包：[序号][chnid][音频数据]
    size_t offset = 0;
    // 写入序号
    memcpy(new_buffer, &sequence_number, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    // 写入chnid
    memcpy(new_buffer + offset, &chnid, sizeof(chnid_t));
    offset += sizeof(chnid_t);
    // 写入音频数据
    memcpy(new_buffer + offset, sbufp, len);

    // 发送数据
    int ret = sendto(serversd, new_buffer, total_len, 0, (void*)sndaddr, sizeof(struct sockaddr_in));
    if (ret < 0) {
        perror("sendto failed");
        free(new_buffer);
        return -1;
    }

    // 记录日志
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] Sent packet with sequence: %u, length: %zu\n", time_str, sequence_number, total_len);

    // 递增序号
    sequence_number++;

    // 释放缓冲区
    free(new_buffer);
    return ret;
}

// 示例主函数
int main() {
    // 假设的socket和地址设置
    int serversd = socket(AF_INET, SOCK_DGRAM, 0);
    if (serversd < 0) {
        perror("socket creation failed");
        return 1;
    }

    struct sockaddr_in sndaddr;
    memset(&sndaddr, 0, sizeof(sndaddr));
    sndaddr.sin_family = AF_INET;
    sndaddr.sin_port = htons(12345); // 目标端口
    sndaddr.sin_addr.s_addr = inet_addr("192.168.1.100"); // 目标IP

    // 示例音频数据
    char sbufp[] = "Sample audio data";
    size_t len = strlen(sbufp) + 1;
    chnid_t chnid = 123; // 假设的通道ID

    // 发送多次以模拟音频流
    for (int i = 0; i < 5; i++) {
        send_with_sequence(serversd, sbufp, len, chnid, &sndaddr);
        usleep(100000); // 模拟100ms间隔
    }

    close(serversd);
    return 0;
}


#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>    // 包含 fcntl 函数和 F_GETFL、F_SETFL、O_NONBLOCK 等宏
#include <unistd.h>
//#include <proto.h>
#include "../include/proto.h"
#include "client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <net/if.h>
#include <string.h>
#include <stdbool.h>
/*
-M --mgroup specify multicast group
-P --port specify receive port
-p --player specify player
-H --help show help
*/


// 添加序列号跟踪
static uint32_t expected_seq = 0;
static bool first_packet = true;
static int packet_loss_count = 0;


struct client_conf_st client_conf = {.rcvport = DEFAULT_RCVPORT,
                                     .mgroup = DEFAULT_MGROUP,
                                     .player_cmd = DEFAULT_PLAYERCMD};

static void print_help() {
  printf("-P --port   specify receive port\n");
  printf("-M --mgroup specify multicast group\n");
  printf("-p --player specify player \n");
  printf("-H --help   show help\n");
}

#define BUFSIZE 320*1024/8*8 // 定义缓冲区大小为 320KB

/*write to fd len bytes data*/
static int writen(int fd, const void *buf, size_t len) {
    int count = 0;
    int pos = 0;    
    while (len > 0) {
        count = write(fd, buf + pos, len);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {//说明当前写缓冲区满了（在非阻塞模式下会返回）。
                // 非阻塞模式下的正常情况
                usleep(1000); // 短暂休眠
                continue;
            }
            perror("write()");
            return -1;
        }
        len -= count;
        pos += count;
    }
    
    return 0;
}

int main(int argc, char *argv[]) 
{

  /*
  initializing
  level:default < configuration file < environment < arg
  */
  int index = 0;
  int sd = 0;
  struct ip_mreqn mreq;     // group setting
  struct sockaddr_in laddr; // local address
  // uint64_t receive_buf_size = BUFSIZE;
  int pd[2];
  pid_t pid;
  struct sockaddr_in server_addr;
  socklen_t serveraddr_len;
  int len;
  int chosenid;
  struct msg_channel_st *msg_channel;
  struct sockaddr_in raddr;
  socklen_t raddr_len;

  struct option argarr[] = {{"port", 1, NULL, 'P'},
                            {"mgroup", 1, NULL, 'M'},
                            {"player", 1, NULL, 'p'},
                            {"help", 0, NULL, 'H'},
                            {NULL, 0, NULL, 0}};
  int c;
  while (1) {
    /*long format argument parse*/
    c = getopt_long(argc, argv, "P:M:p:H", argarr, &index);
    if (c < 0)
      break;
    switch (c) {
    case 'P':
      client_conf.rcvport = optarg;
      break;
    case 'M':
      client_conf.mgroup = optarg;
      break;
    case 'p':
      client_conf.player_cmd = optarg;
      break;
    case 'H':
      print_help();
      exit(0);
      break;
    default:
      abort();
      break;
    }
  }

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0) {
    perror("socket()");
    exit(0);
  }
  // multicast group
  inet_pton(AF_INET, client_conf.mgroup,
            &mreq.imr_multiaddr); // 255.255.255.255-->0xFF..
  // local address(self)
  inet_pton(AF_INET, "0.0.0.0", &mreq.imr_address);
  // local net card
  mreq.imr_ifindex = if_nametoindex("ens33");
  if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    perror("setsockopt()");
    exit(1);
  }
  // improve efficiency
  int loop = 1;  // 启用回环  
  // if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &(receive_buf_size), sizeof(receive_buf_size)) < 0) {
  if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop))) {
    perror("setsockopt()");
    exit(1);
  }
  int rcvbuf_size = 1024 * 1024; // 增加UDP接收缓冲区大小设置为1MB
  if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
      perror("setsockopt SO_RCVBUF");
      exit(1);
  }


  laddr.sin_family = AF_INET;
  laddr.sin_port = htons(atoi(client_conf.rcvport));
  inet_pton(AF_INET, "0.0.0.0", &laddr.sin_addr);
  if (bind(sd, (void *)&laddr, sizeof(laddr)) < 0) {
    perror("bind()");
    exit(1);
  }
  if (pipe(pd) < 0) {
    perror("pipe()");
    exit(1);
  }
    // 设置管道属性  设置文件描述符为非阻塞模式 一旦内核缓冲区满了，write() 不会阻塞，而是立即返回错误 EAGAIN 或 EWOULDBLOCK。
    int flags = fcntl(pd[1], F_GETFL);//使用 fcntl 获取 pd[1] 的文件状态标志（flags
    if (flags != -1) {
        fcntl(pd[1], F_SETFL, flags | O_NONBLOCK);
    }
    
    #ifdef F_SETPIPE_SZ
    fcntl(pd[1], F_SETPIPE_SZ, 1024 * 1024); // 1MB管道缓冲区
    #endif
    
  pid = fork();
  if (pid < 0) {
    perror("fork()");
    exit(1);
  }
  if (pid == 0) // child, read, close write
  {
    /*decode*/
    /*mpg123 read from stdin*/
    close(sd);      // socket
    close(pd[1]);   // 0:read, 1:write
    dup2(pd[0], 0); // set pd[0] as stdin / 将管道读端重定向到标准输入
    if (pd[0] > 0)  // close pd[0]
      close(pd[0]);
     // 启动音频播放器，添加更多容错参数
    execl("/bin/sh", "sh", "-c", 
          "mpg123 --quiet --buffer 1024 -", NULL);
    // execl("/bin/sh", "sh", "-c", 
    //       "mpg123  -", NULL);
    perror("execl");
    exit(1);
  } 
  
  else // parent
  {
    close(pd[0]);
    /*receive data from network, write it to pipe*/
    // receive programme
    struct msg_list_st *msg_list;
    msg_list = malloc(MSG_LIST_MAX);
    if (msg_list == NULL) {
      perror("malloc");
      exit(1);
    }
    //必须从节目单开始
    serveraddr_len=sizeof(server_addr);//为什么一定要这个？
    while (1) {
      len = recvfrom(sd, msg_list, MSG_LIST_MAX, 0, (void *)&server_addr,//此处是收节目单
                     &serveraddr_len);
      fprintf(stderr, "server_addr: %s\n", inet_ntoa(server_addr.sin_addr));
      if (len < sizeof(struct msg_list_st)) {
        fprintf(stderr, "massage is too short.\n");
        continue;
      }
      if (msg_list->chnid != LISTCHNID) {
        fprintf(stderr, "current chnid:%d.\n", msg_list->chnid);
        fprintf(stderr, "chnid is not match.\n");
        continue;
      }
      else
        fprintf(stderr, "chnid is mathced current chnid:%d.\n", msg_list->chnid);

      break;
    }

    // printf programme, select channel
    /*
    1.music xxx
    2.radio xxx
    3.....
    */
    // receive channel package, send it to child process
    struct msg_listentry_st *pos;
    for (pos = msg_list->entry; (char *)pos < ((char *)msg_list + len);
         pos = (void *)((char *)pos) + ntohs(pos->len)) {
      printf("channel:%d:%s\n", pos->chnid, pos->desc);
    }


    printf("请输入您想要的频道号: ");
    while (scanf("%d", &chosenid) != 1) {
        // Clear input buffer
        while (getchar() != '\n');
        printf("输入无效！请重新输入一个整数频道号: ");
    }
    // Clear any remaining newline
    while (getchar() != '\n');
    
    printf("您选择了频道号: %d\n", chosenid);
    if (chosenid < MINCHNID || chosenid > MAXCHNID) {
      fprintf(stderr, "channel id is not match.\n");
      exit(1);
    }
    
    /*free list*/
    free(msg_list);
    msg_channel = malloc(MSG_CHANNEL_MAX);
    if (msg_channel == NULL) {
      perror("malloc");
      exit(1);
    }
    raddr_len = sizeof(raddr);
    char ipstr_raddr[30];
    char ipstr_server_addr[30];
    char rcvbuf[BUFSIZE];// 接收缓冲区数组
    uint32_t offset = 0;// 偏移量，追踪缓冲区中的当前位置
    memset(rcvbuf, 0, BUFSIZE);//清零接收缓冲区
    while (1) 
    {
      len = recvfrom(sd, msg_channel, MSG_CHANNEL_MAX, 0, (void *)&raddr, &raddr_len);//此处是收音频数据
      //防止有人恶意发送不相关的包
      if (raddr.sin_addr.s_addr != server_addr.sin_addr.s_addr) {
        inet_ntop(AF_INET, &raddr.sin_addr.s_addr, ipstr_raddr, 30);
        inet_ntop(AF_INET, &server_addr.sin_addr.s_addr, ipstr_server_addr, 30);
        fprintf(stderr, "Ignore:addr not match. raddr:%s server_addr:%s.\n",
                ipstr_raddr, ipstr_server_addr);
        continue;
      }
      if (raddr.sin_port != server_addr.sin_port) {
        fprintf(stderr, "Ignore:port not match.\n");
        continue;
      }
      if (len < sizeof(struct msg_channel_st)) {
        fprintf(stderr, "Ignore:massage too short.\n");
        continue;
      }

      if (msg_channel->chnid == chosenid) 
      {

        // 检查序列号连续性
          if (first_packet) {
              expected_seq = msg_channel->seq + 1;
              first_packet = false;
          } 
          else 
          {
              if (msg_channel->seq != expected_seq) {
                  fprintf(stderr, "Warning: Sequence gap! Expected %u, got %u (lost %d packets)\n", 
                          expected_seq, msg_channel->seq, 
                          msg_channel->seq - expected_seq);
                  packet_loss_count += (msg_channel->seq - expected_seq);
                  
                  // 对于音频流，可能需要插入静音数据来维持时序
                  // 这里可以选择跳过这个包或者继续处理
              }
              expected_seq = msg_channel->seq + 1;
          }
        memcpy(rcvbuf + offset, msg_channel->data, len - sizeof(chnid_t)-sizeof(uint32_t));
        offset += len - sizeof(chnid_t)-sizeof(uint32_t);

        if (offset >= BUFSIZE / 2) { // 缓冲区达到一半时写入
            fd_set wfds;//是 select 系统调用的文件描述符集合。
            struct timeval tv = {0, 0};//设置 select 的超时时间为 0 秒 0 微秒，即非阻塞（立即返回）。
            FD_ZERO(&wfds);
            FD_SET(pd[1], &wfds);//FD_SET(fd, &set)：把文件描述符 fd 加入到集合 set 中。
            if (select(pd[1] + 1, NULL, &wfds, NULL, &tv) > 0) {//select 是一个 I/O 多路复用函数，检查指定的文件描述符是否“就绪”。
                if (writen(pd[1], rcvbuf, offset) < 0) {
                    exit(1);
                }
                offset = 0;
            } else {
                fprintf(stderr, "Pipe not writable, skipping write\n");
            }
        }
      }
  
    }

    free(msg_channel);
    close(sd);
    exit(0);

  }
}

