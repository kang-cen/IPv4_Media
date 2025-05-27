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
#include <signal.h>
#include <ifaddrs.h>
#include <sys/wait.h>
#include "recv_thr.h"
#include "writer_thr.h"
#include "stat_thr.h"
#include "feedback_thr.h"  


/*
-M --mgroup specify multicast group
-P --port specify receive port
-p --player specify player
-H --help show help
*/
struct client_conf_st client_conf = {.rcvport = DEFAULT_RCVPORT,
                                     .mgroup = DEFAULT_MGROUP,
                                     .player_cmd = DEFAULT_PLAYERCMD};

// 全局变量用于线程间通信
static struct shared_data *g_shared_data = NULL;
static pthread_t receiver_tid, writer_tid, stats_tid, feedback_tid;
static pid_t player_pid = 0;

static void print_help() {
    printf("-P --port   specify receive port\n");
    printf("-M --mgroup specify multicast group\n");
    printf("-p --player specify player \n");
    printf("-H --help   show help\n");
}

// 信号处理函数
void signal_handler(int sig) {
    printf("\nReceived signal %d. Shutting down gracefully...\n", sig);
    
    if (g_shared_data) {
        g_shared_data->stop_flag = true;
        
        // 唤醒可能阻塞的线程
        pthread_cond_broadcast(&g_shared_data->rb.not_empty);
        pthread_cond_broadcast(&g_shared_data->rb.not_full);
    }
    
    // 如果有播放器子进程，也要终止它
    if (player_pid > 0) {
        kill(player_pid, SIGTERM);
    }
}

// 获取第一个非回环网络接口名称
char* get_default_interface() {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    static char interface_name[IF_NAMESIZE];
    
    if (getifaddrs(&ifaddrs_ptr) == -1) {
        perror("getifaddrs");
        return NULL;
    }
    
    // 遍历网络接口
    for (ifa = ifaddrs_ptr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        // 检查是否为IPv4接口且不是回环接口
        if (ifa->ifa_addr->sa_family == AF_INET &&
            !(ifa->ifa_flags & IFF_LOOPBACK) &&
            (ifa->ifa_flags & IFF_UP) &&
            (ifa->ifa_flags & IFF_RUNNING)) {
            
            strncpy(interface_name, ifa->ifa_name, IF_NAMESIZE - 1);
            interface_name[IF_NAMESIZE - 1] = '\0';
            freeifaddrs(ifaddrs_ptr);
            printf("Using network interface: %s\n", interface_name);
            return interface_name;
        }
    }
    
    freeifaddrs(ifaddrs_ptr);
    printf("Warning: No suitable network interface found, using default 'eth0'\n");
    strcpy(interface_name, "eth0");
    return interface_name;
}

// 初始化环形缓冲区
void ring_buffer_init(struct ring_buffer *rb) {
    memset(rb->data, 0, RING_BUFFER_SIZE);
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
}

// 清理资源
void cleanup_resources() {
    printf("Cleaning up resources...\n");
    
    if (g_shared_data) {
        // 等待所有线程结束
        if (pthread_join(receiver_tid, NULL) == 0) {
            printf("Receiver thread joined\n");
        }
        if (pthread_join(writer_tid, NULL) == 0) {
            printf("Writer thread joined\n");
        }
        if (pthread_join(stats_tid, NULL) == 0) {
            printf("Stats thread joined\n");
        }
        if (pthread_join(feedback_tid, NULL) == 0) {
            printf("Feedback thread joined\n");
        }
        
        // 销毁互斥量和条件变量
        pthread_mutex_destroy(&g_shared_data->rb.mutex);
        pthread_cond_destroy(&g_shared_data->rb.not_empty);
        pthread_cond_destroy(&g_shared_data->rb.not_full);
        
        // 关闭文件描述符
        if (g_shared_data->socket_fd > 0) {
            close(g_shared_data->socket_fd);
        }
        if (g_shared_data->pipe_fd > 0) {
            close(g_shared_data->pipe_fd);
        }
    }
    
    printf("Cleanup completed\n");
}

int main(int argc, char *argv[]) {
    int index = 0;
    int sd = 0;
    struct ip_mreqn mreq;
    struct sockaddr_in laddr;
    int pd[2];
    pid_t pid;
    struct sockaddr_in server_addr;
    socklen_t serveraddr_len;
    int len;
    int chosenid;
    struct msg_list_st *msg_list;
    char *interface_name;
    
    // 线程变量
    struct shared_data shared_data = {0};
    g_shared_data = &shared_data;  // 设置全局指针
    
    // 注册信号处理函数
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // 终止信号
    signal(SIGPIPE, SIG_IGN);         // 忽略管道破裂信号
    
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
    
    // 创建socket
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0) {
        perror("socket()");
        exit(1);
    }
    
    // 获取默认网络接口
    interface_name = get_default_interface();
    if (!interface_name) {
        fprintf(stderr, "Failed to get network interface\n");
        exit(1);
    }
    
    // 设置组播
    inet_pton(AF_INET, client_conf.mgroup, &mreq.imr_multiaddr);
    inet_pton(AF_INET, "0.0.0.0", &mreq.imr_address);
    mreq.imr_ifindex = if_nametoindex(interface_name);
    if (mreq.imr_ifindex == 0) {
        perror("if_nametoindex");
        fprintf(stderr, "Failed to get interface index for %s\n", interface_name);
        exit(1);
    }
    
    if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        exit(1);
    }
    
    int loop = 1;
    if (setsockopt(sd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        perror("setsockopt IP_MULTICAST_LOOP");
        exit(1);
    }
    
    // 设置更大的UDP接收缓冲区
    int rcvbuf_size = 2 * 1024 * 1024; // 2MB
    if (setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        perror("setsockopt SO_RCVBUF");
        // 非致命错误，继续执行
    }
    
    // 绑定地址
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(atoi(client_conf.rcvport));
    inet_pton(AF_INET, "0.0.0.0", &laddr.sin_addr);
    if (bind(sd, (void *)&laddr, sizeof(laddr)) < 0) {
        perror("bind()");
        exit(1);
    }
    
    // 创建管道
    if (pipe(pd) < 0) {
        perror("pipe()");
        exit(1);
    }
    
    // 设置管道为非阻塞模式并增加缓冲区
    int flags = fcntl(pd[1], F_GETFL);
    if (flags != -1) {
        fcntl(pd[1], F_SETFL, flags | O_NONBLOCK);
    }
    
    #ifdef F_SETPIPE_SZ
        fcntl(pd[1], F_SETPIPE_SZ, 1024 * 1024); // 1MB管道缓冲区
    #endif
    
    // 接收节目单
    msg_list = malloc(MSG_LIST_MAX);
    if (msg_list == NULL) {
        perror("malloc");
        exit(1);
    }
    
    serveraddr_len = sizeof(server_addr);
    while (1) {
        len = recvfrom(sd, msg_list, MSG_LIST_MAX, 0, (void *)&server_addr, &serveraddr_len);
        fprintf(stderr, "server_addr: %s\n", inet_ntoa(server_addr.sin_addr));
        
        if (shared_data.stop_flag) {
            printf("Received stop signal during channel list reception\n");
            free(msg_list);
            close(sd);
            exit(0);
        }
        
        if (len < sizeof(struct msg_list_st)) {
            fprintf(stderr, "message is too short.\n");
            continue;
        }
        
        if (msg_list->chnid != LISTCHNID) {
            fprintf(stderr, "current chnid:%d.\n", msg_list->chnid);
            fprintf(stderr, "chnid is not match.\n");
            continue;
        } else {
            fprintf(stderr, "chnid is matched current chnid:%d.\n", msg_list->chnid);
            break;
        }
    }
    
    // 显示频道列表
    struct msg_listentry_st *pos;
    for (pos = msg_list->entry; (char *)pos < ((char *)msg_list + len);
         pos = (void *)((char *)pos) + ntohs(pos->len)) {
        printf("channel:%d:%s\n", pos->chnid, pos->desc);
    }
    
    // 选择频道
    printf("请输入您想要的频道号: ");
    while (scanf("%d", &chosenid) != 1) {
        if (shared_data.stop_flag) {
            printf("Received stop signal during input\n");
            free(msg_list);
            close(sd);
            exit(0);
        }
        while (getchar() != '\n');
        printf("输入无效！请重新输入一个整数频道号: ");
    }
    while (getchar() != '\n');
    
    printf("您选择了频道号: %d\n", chosenid);
    if (chosenid < MINCHNID || chosenid > MAXCHNID) {
        fprintf(stderr, "channel id is not match.\n");
        exit(1);
    }
    
    free(msg_list);
    
    // fork子进程处理音频播放
    pid = fork();
    if (pid < 0) {
        perror("fork()");
        exit(1);
    }
    
    if (pid == 0) { // 子进程
        close(sd);
        close(pd[1]);
        dup2(pd[0], 0);
        if (pd[0] > 0) close(pd[0]);
        
        execl("/bin/sh", "sh", "-c", 
              "mpg123 --quiet --buffer 2048 -", NULL);
        perror("execl");
        exit(1);
    } else { // 父进程
        player_pid = pid;  // 保存播放器进程ID
        close(pd[0]);
        
        // 初始化共享数据
        ring_buffer_init(&shared_data.rb);
        shared_data.socket_fd = sd;
        shared_data.pipe_fd = pd[1];
        shared_data.chosen_channel = chosenid;
        shared_data.server_addr = server_addr;
        shared_data.stop_flag = false;
        shared_data.receiver_ready = false;
        
        // 创建线程 UDP接受数据线程
        if (pthread_create(&receiver_tid, NULL, receiver_thread, &shared_data) != 0) {
            perror("pthread_create receiver");
            cleanup_resources();
            exit(1);
        }
        //管道写入线程
        if (pthread_create(&writer_tid, NULL, writer_thread, &shared_data) != 0) {
            perror("pthread_create writer");
            cleanup_resources();
            exit(1);
        }
        
        // (stats_thread): 监控性能和丢包情况
        if (pthread_create(&stats_tid, NULL, stats_thread, &shared_data) != 0) {
            perror("pthread_create stats");
            cleanup_resources();
            exit(1);
        }

        // 创建反馈控制线程
        if (pthread_create(&feedback_tid, NULL, feedback_thread, &shared_data) != 0) {
            perror("pthread_create feedback");
            cleanup_resources();
            exit(1);
        }
        printf("All threads started. Press Ctrl+C to exit.\n");
        
        // 等待线程结束（通常通过信号处理）
        pthread_join(receiver_tid, NULL);
        pthread_join(writer_tid, NULL);
        pthread_join(stats_tid, NULL);
        pthread_join(feedback_tid, NULL);
        
        // 清理资源
        cleanup_resources();
        
        // 等待子进程结束
        if (player_pid > 0) {
            int status;
            waitpid(player_pid, &status, 0);
        }
    }
    
    return 0;
}