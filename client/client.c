#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "../include/proto.h"
#include "client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <net/if.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/select.h>  // 添加select头文件
#include <signal.h>      // 添加信号处理头文件
#include "recv_thr.h"
#include "writer_thr.h"
#include "stat_thr.h"
#include "feedback_thr.h"  

/**
 * 全局变量：程序退出标志
 * 使用volatile sig_atomic_t确保在信号处理和主线程间的可见性
 * volatile: 告诉编译器这个变量可能被异步修改，不要优化对它的访问
 * sig_atomic_t: 保证原子性访问的整数类型，在信号处理中安全使用
 */
volatile sig_atomic_t should_exit = 0;

/**
 * 信号处理函数
 * 当接收到SIGINT(Ctrl+C)或SIGTERM信号时被调用
 * @param sig: 接收到的信号编号
 */
void signal_handler(int sig) {
    printf("\nReceived signal %d. Shutting down gracefully...\n", sig);
    should_exit = 1;  // 设置退出标志
}

/*
命令行参数说明:
-M --mgroup specify multicast group  (指定多播组)
-P --port specify receive port       (指定接收端口)
-p --player specify player          (指定播放器)
-H --help show help                 (显示帮助)
*/
struct client_conf_st client_conf = {.rcvport = DEFAULT_RCVPORT,
                                     .mgroup = DEFAULT_MGROUP,
                                     .player_cmd = DEFAULT_PLAYERCMD};

/**
 * 打印帮助信息
 */
static void print_help() {
    printf("-P --port   specify receive port\n");
    printf("-M --mgroup specify multicast group\n");
    printf("-p --player specify player \n");
    printf("-H --help   show help\n");
}

/**
 * 初始化环形缓冲区
 * @param rb: 指向环形缓冲区结构的指针
 */
void ring_buffer_init(struct ring_buffer *rb) {
    memset(rb->data, 0, RING_BUFFER_SIZE);
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
}

/**
 * 使用select方式接收节目单
 * 支持优雅退出，不会无限阻塞在recvfrom上
 * @param sd: socket文件描述符
 * @param msg_list: 存储节目单的缓冲区
 * @param server_addr: 存储服务器地址的结构体指针
 * @return: 成功返回接收到的数据长度，失败返回-1，被信号中断返回0
 */
int receive_channel_list_with_select(int sd, struct msg_list_st *msg_list, 
                                   struct sockaddr_in *server_addr) {
    socklen_t serveraddr_len = sizeof(*server_addr);
    int len;
    
    printf("Waiting for channel list from server...\n");
    printf("Press Ctrl+C to exit if no server is available.\n");
    
    while (!should_exit) {
        fd_set readfds;           // 文件描述符集合，用于select监控
        struct timeval timeout;   // 超时时间结构体
        
        // 1. 初始化文件描述符集合
        FD_ZERO(&readfds);        // 清空集合
        FD_SET(sd, &readfds);     // 将socket描述符添加到集合中
        
        // 2. 设置超时时间为1秒
        // 这样每1秒会检查一次should_exit标志，确保能及时响应退出信号
        timeout.tv_sec = 1;       // 秒
        timeout.tv_usec = 0;      // 微秒
        
        // 3. 使用select监控socket是否有数据可读
        // select参数说明:
        // - sd + 1: 监控的最大文件描述符值+1
        // - &readfds: 监控读事件的文件描述符集合
        // - NULL: 不监控写事件
        // - NULL: 不监控异常事件  
        // - &timeout: 超时时间
        int result = select(sd + 1, &readfds, NULL, NULL, &timeout);
        
        if (result < 0) {
            // select调用出错
            if (errno == EINTR) {
                // 被信号中断（比如Ctrl+C），这是正常情况
                printf("Select interrupted by signal, checking exit condition...\n");
                continue;  // 继续循环，检查should_exit
            }
            perror("select");
            return -1;  // 其他错误
        } else if (result == 0) {
            // 超时，没有数据可读
            // 每秒打印一次等待消息，让用户知道程序还在运行
            printf("Still waiting for server... (Press Ctrl+C to exit)\n");
            continue;  // 继续循环
        } else if (FD_ISSET(sd, &readfds)) {
            // 4. socket有数据可读，调用recvfrom接收数据
            len = recvfrom(sd, msg_list, MSG_LIST_MAX, 0, 
                          (void *)server_addr, &serveraddr_len);
            
            if (len < 0) {
                perror("recvfrom");
                return -1;
            }
            
            printf("Received data from server: %s\n", inet_ntoa(server_addr->sin_addr));
            
            // 5. 验证接收到的数据
            if (len < sizeof(struct msg_list_st)) {
                fprintf(stderr, "Message too short: %d bytes\n", len);
                continue;  // 继续等待有效数据
            }
            
            if (msg_list->chnid != LISTCHNID) {
                fprintf(stderr, "Wrong channel ID: expected %d, got %d\n", 
                       LISTCHNID, msg_list->chnid);
                continue;  // 继续等待正确的节目单
            } else {
                printf("Valid channel list received (channel ID: %d)\n", msg_list->chnid);
                return len;  // 成功接收到节目单
            }
        }
    }
    
    // 如果到达这里，说明收到了退出信号
    printf("Exiting channel list reception due to signal.\n");
    return 0;  // 返回0表示被信号中断
}

int main(int argc, char *argv[]) {
    // 1. 注册信号处理函数
    // 这必须在程序开始时就设置，以便能够捕获用户的Ctrl+C
    struct sigaction sa;
    sa.sa_handler = signal_handler;  // 设置信号处理函数
    sigemptyset(&sa.sa_mask);        // 清空信号掩码
    sa.sa_flags = 0;                 // 不设置特殊标志
    
    // 注册SIGINT(Ctrl+C)和SIGTERM(终止)信号的处理函数
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        exit(1);
    }
    
    // 2. 变量声明
    int index = 0;
    int sd = 0;                    // socket描述符
    struct ip_mreqn mreq;          // 多播组成员结构体
    struct sockaddr_in laddr;      // 本地地址结构体
    int pd[2];                     // 管道描述符对
    pid_t pid;                     // 进程ID
    struct sockaddr_in server_addr; // 服务器地址
    int len;                       // 接收数据长度
    int chosenid;                  // 用户选择的频道ID
    struct msg_list_st *msg_list;  // 节目单数据
    
    // 线程相关变量
    pthread_t receiver_tid, writer_tid, stats_tid, feedback_tid;
    struct shared_data shared_data = {0};
    
    // 3. 命令行参数解析
    struct option argarr[] = {{"port", 1, NULL, 'P'},
                              {"mgroup", 1, NULL, 'M'},
                              {"player", 1, NULL, 'p'},
                              {"help", 0, NULL, 'H'},
                              {NULL, 0, NULL, 0}};
    int c;
    while (1) {
        c = getopt_long(argc, argv, "P:M:p:H", argarr, &index);
        if (c < 0) break;
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
    
    // 4. 创建UDP socket
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0) {
        perror("socket()");
        exit(1);
    }
    
    // 5. 配置多播组
    // 将多播组地址从字符串转换为网络字节序
    inet_pton(AF_INET, client_conf.mgroup, &mreq.imr_multiaddr);
    inet_pton(AF_INET, "0.0.0.0", &mreq.imr_address);  // 任意本地接口
    mreq.imr_ifindex = if_nametoindex("ens33");         // 指定网络接口
    
    // 加入多播组
    if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        exit(1);
    }
    
    // 启用多播回环（允许接收自己发送的多播包）
    int loop = 1;
    if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        perror("setsockopt IP_MULTICAST_LOOP");
        exit(1);
    }
    
    // 6. 设置更大的UDP接收缓冲区（提高网络性能）
    int rcvbuf_size = 2 * 1024 * 1024; // 2MB
    if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        perror("setsockopt SO_RCVBUF");
        // 非致命错误，继续执行
    }
    
    // 7. 绑定本地地址
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(atoi(client_conf.rcvport));  // 转换端口为网络字节序
    inet_pton(AF_INET, "0.0.0.0", &laddr.sin_addr);    // 绑定所有本地地址
    
    if (bind(sd, (void *)&laddr, sizeof(laddr)) < 0) {
        perror("bind()");
        exit(1);
    }
    
    // 8. 创建管道用于向音频播放器传输数据
    if (pipe(pd) < 0) {
        perror("pipe()");
        exit(1);
    }
    
    // 9. 优化管道性能
    // 设置写端为非阻塞模式，防止写入时阻塞
    int flags = fcntl(pd[1], F_GETFL);
    if (flags != -1) {
        fcntl(pd[1], F_SETFL, flags | O_NONBLOCK);
    }
    
    // 增加管道缓冲区大小（如果系统支持）
    #ifdef F_SETPIPE_SZ
        fcntl(pd[1], F_SETPIPE_SZ, 1024 * 1024); // 1MB管道缓冲区
    #endif
    
    // 10. 接收节目单（使用改进的select方法）
    msg_list = malloc(MSG_LIST_MAX);
    if (msg_list == NULL) {
        perror("malloc");
        exit(1);
    }
    
    // 使用select方法接收节目单，支持优雅退出
    len = receive_channel_list_with_select(sd, msg_list, &server_addr);
    
    if (len == 0) {
        // 被信号中断退出
        printf("Program terminated by user signal.\n");
        free(msg_list);
        close(sd);
        exit(0);
    } else if (len < 0) {
        // 接收出错
        fprintf(stderr, "Failed to receive channel list.\n");
        free(msg_list);
        close(sd);
        exit(1);
    }
    
    // 11. 显示频道列表
    printf("\n=== Available Channels ===\n");
    struct msg_listentry_st *pos;
    for (pos = msg_list->entry; (char *)pos < ((char *)msg_list + len);
         pos = (void *)((char *)pos) + ntohs(pos->len)) {
        printf("Channel %d: %s\n", pos->chnid, pos->desc);
    }
    printf("========================\n");
    
    // 12. 用户选择频道
    printf("Please enter the channel number you want: ");
    while (scanf("%d", &chosenid) != 1) {
        // 清理输入缓冲区中的无效字符
        while (getchar() != '\n');
        printf("Invalid input! Please enter a valid channel number: ");
    }
    // 清理输入缓冲区中剩余的换行符
    while (getchar() != '\n');
    
    printf("You selected channel: %d\n", chosenid);
    
    // 13. 验证频道号有效性
    if (chosenid < MINCHNID || chosenid > MAXCHNID) {
        fprintf(stderr, "Invalid channel ID. Must be between %d and %d.\n", 
                MINCHNID, MAXCHNID);
        free(msg_list);
        close(sd);
        exit(1);
    }
    
    free(msg_list);  // 释放节目单内存
    
    // 14. 创建子进程处理音频播放
    pid = fork();
    if (pid < 0) {
        perror("fork()");
        exit(1);
    }
    
    if (pid == 0) {
        // 子进程：负责音频播放
        close(sd);      // 子进程不需要socket
        close(pd[1]);   // 子进程不需要管道写端
        
        // 将管道读端重定向到标准输入
        dup2(pd[0], 0);
        if (pd[0] > 0) close(pd[0]);
        
        // 执行音频播放器
        // --quiet: 静默模式，减少输出
        // --buffer 2048: 设置音频缓冲区
        // -: 从标准输入读取数据
        execl("/bin/sh", "sh", "-c", 
              "mpg123 --quiet --buffer 2048 -", NULL);
        perror("execl");
        exit(1);
    } else {
        // 父进程：负责网络数据接收和处理
        close(pd[0]);   // 父进程不需要管道读端
        
        printf("Audio player started (PID: %d)\n", pid);
        
        // 15. 初始化共享数据结构
        ring_buffer_init(&shared_data.rb);
        shared_data.socket_fd = sd;
        shared_data.pipe_fd = pd[1];
        shared_data.chosen_channel = chosenid;
        shared_data.server_addr = server_addr;
        shared_data.stop_flag = false;
        shared_data.receiver_ready = false;
        
        printf("Starting threads...\n");
        
        // 16. 创建各个工作线程
        
        // UDP数据接收线程：从网络接收音频数据包
        if (pthread_create(&receiver_tid, NULL, receiver_thread, &shared_data) != 0) {
            perror("pthread_create receiver");
            exit(1);
        }
        printf("✓ Receiver thread started\n");
        
        // 管道写入线程：将音频数据写入管道供播放器使用
        if (pthread_create(&writer_tid, NULL, writer_thread, &shared_data) != 0) {
            perror("pthread_create writer");
            exit(1);
        }
        printf("✓ Writer thread started\n");
        
        // 统计监控线程：监控网络性能和丢包情况
        if (pthread_create(&stats_tid, NULL, stats_thread, &shared_data) != 0) {
            perror("pthread_create stats");
            exit(1);
        }
        printf("✓ Statistics thread started\n");
        
        // 反馈控制线程：根据网络状况调整接收策略
        if (pthread_create(&feedback_tid, NULL, feedback_thread, &shared_data) != 0) {
            perror("pthread_create feedback");
            exit(1);
        }
        printf("✓ Feedback thread started\n");
        
        printf("\n=== Streaming Started ===\n");
        printf("Channel: %d\n", chosenid);
        printf("Server: %s\n", inet_ntoa(server_addr.sin_addr));
        printf("Press Ctrl+C to stop streaming\n");
        printf("========================\n");
        
        // 17. 等待线程结束
        // 通常由信号处理函数设置退出标志后，各线程检测到并退出
        pthread_join(receiver_tid, NULL);
        printf("Receiver thread ended\n");
        
        pthread_join(writer_tid, NULL);
        printf("Writer thread ended\n");
        
        pthread_join(stats_tid, NULL);
        printf("Statistics thread ended\n");
        
        pthread_join(feedback_tid, NULL);
        printf("Feedback thread ended\n");
        
        // 18. 清理资源
        close(sd);
        close(pd[1]);
        
        printf("All threads terminated. Program exiting.\n");
    }
    
    return 0;
}