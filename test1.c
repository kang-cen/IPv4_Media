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

