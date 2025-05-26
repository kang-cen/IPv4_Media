#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <proto.h>
#include "feedback_pro.h"
#include "server_conf.h"

// 全局变量
static pthread_t feedback_tid;
static int feedback_socket = -1;
static struct client_state clients[MAX_CLIENTS];// 客户端状态数组
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;// 客户端数组互斥锁

// 全局速率控制数组
struct channel_rate_control g_channel_rates[MAXCHNID + 1];// 频道速率控制数组

// 初始化速率控制数组
static void init_channel_rates(void) {
    for (int i = 0; i <= MAXCHNID; i++) {
        g_channel_rates[i].channel_id = i;
        g_channel_rates[i].rate_multiplier = 1.0;  
        g_channel_rates[i].avg_rate_level = RATE_NORMAL;
        g_channel_rates[i].last_update = 0;
        pthread_mutex_init(&g_channel_rates[i].mutex, NULL);
    }
}

// 清理速率控制数组
static void cleanup_channel_rates(void) {
    for (int i = 0; i <= MAXCHNID; i++) {
        pthread_mutex_destroy(&g_channel_rates[i].mutex);
    }
}

// 根据速率等级计算速率倍数
static double calculate_rate_multiplier(rate_control_level_t rate_level) {
    switch (rate_level) {
        case RATE_INCREASE:
            return 1.2;  // 提高20%
        case RATE_NORMAL:
            return 1.0;  // 正常速率
        case RATE_MODERATE_DEC:
            return 0.6;  // 降低40% 延迟2s
        case RATE_MAJOR_DEC:
            return 0.3;  // 降低70% 延迟4s
        default:
            return 1.0;
    }
}

// 查找客户端状态
static struct client_state* find_client(uint32_t client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].client_id == client_id) {
            return &clients[i];
        }
    }
    return NULL;
}

// 添加新客户端
static struct client_state* add_client(uint32_t client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) { // // 找到第一个非活跃的槽位
            memset(&clients[i], 0, sizeof(struct client_state));
            clients[i].client_id = client_id;
            clients[i].active = 1;
            clients[i].last_update = time(NULL);
            return &clients[i];
        }
    }
    return NULL;  // 客户端数组已满
}

// 处理反馈消息
static void process_feedback_msg(struct feedback_msg *msg, struct sockaddr_in *client_addr) {
    // 验证魔数
    if (ntohl(msg->magic) != PACKET_MAGIC) {
        syslog(LOG_WARNING, "Invalid feedback magic number from %s", 
               inet_ntoa(client_addr->sin_addr));
        return;
    }

    // 转换网络字节序
    uint32_t client_id = ntohl(msg->client_id);
    int32_t channel_id = ntohl(msg->channel_id);
    uint32_t buffer_usage = ntohl(msg->buffer_usage);
    uint32_t timestamp = ntohl(msg->timestamp);//消息时间戳
    uint32_t seq_num = ntohl(msg->seq_num);//消息序列号

    // 验证频道ID
    if (channel_id < MINCHNID || channel_id > MAXCHNID) {
        syslog(LOG_WARNING, "Invalid channel_id %d from client %u", 
               channel_id, client_id);
        return;
    }

    pthread_mutex_lock(&clients_mutex);

    // 查找或创建客户端状态
    struct client_state *client = find_client(client_id);
    if (client == NULL) {
        client = add_client(client_id);
        if (client == NULL) {
            syslog(LOG_WARNING, "Cannot add new client %u, client array full", client_id);
            pthread_mutex_unlock(&clients_mutex);
            return;
        }
        syslog(LOG_INFO, "New client %u connected from %s", 
               client_id, inet_ntoa(client_addr->sin_addr));
    }

    // 检查重复消息
    if (client->last_seq_num >= seq_num && client->last_timestamp >= timestamp) {
        syslog(LOG_DEBUG, "Duplicate feedback message from client %u, seq %u", 
               client_id, seq_num);
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    // 更新客户端状态
    client->channel_id = channel_id;
    client->last_rate_level = msg->rate_level;
    client->last_seq_num = seq_num;
    client->last_timestamp = timestamp;
    client->last_update = time(NULL);

    pthread_mutex_unlock(&clients_mutex);

    // 更新频道速率控制
    update_channel_rate_control(channel_id, msg->rate_level, client_id);

    syslog(LOG_DEBUG, "Feedback from client %u: channel=%d, rate_level=%d, buffer=%u%%", 
           client_id, channel_id, msg->rate_level, buffer_usage);
}

// 反馈接收线程主函数
static void* feedback_thread(void *arg) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct feedback_msg msg;
    ssize_t recv_len;
    int feedback_port;

    syslog(LOG_INFO, "Feedback thread started");

    // 创建UDP套接字
    feedback_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (feedback_socket < 0) {
        syslog(LOG_ERR, "feedback socket():%s", strerror(errno));
        return NULL;
    }

    // 设置套接字选项 ：允许你的 socket 在关闭后立即重新绑定到相同的地址和端口
    int reuse = 1;
    if (setsockopt(feedback_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        syslog(LOG_WARNING, "feedback setsockopt(SO_REUSEADDR):%s", strerror(errno));
    }

    // 绑定到反馈端口
    feedback_port = atoi(server_conf.rcvport) + FEEDBACK_PORT_OFFSET;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;//绑定所有网络接口：服务器会监听本机所有可用的网络接口
    server_addr.sin_port = htons(feedback_port);

    if (bind(feedback_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "feedback bind():%s", strerror(errno));
        close(feedback_socket);
        return NULL;
    }

    syslog(LOG_INFO, "Feedback receiver listening on port %d", feedback_port);

    // 主接收循环
    while (1) {
        //这个调用会 一直阻塞（阻塞等待）在这里
        recv_len = recvfrom(feedback_socket, &msg, sizeof(msg), 0,
                           (struct sockaddr*)&client_addr, &client_len);
        
        if (recv_len < 0) {
            if (errno == EINTR) continue;  // 被信号中断，继续
            syslog(LOG_ERR, "feedback recvfrom():%s", strerror(errno));
            break;
        }

        if (recv_len != sizeof(msg)) {
            syslog(LOG_WARNING, "Invalid feedback message size %zd from %s", 
                   recv_len, inet_ntoa(client_addr.sin_addr));
            continue;
        }

        // 处理反馈消息
        process_feedback_msg(&msg, &client_addr);
        
        // 定期清理过期客户端
        static time_t last_cleanup = 0;
        time_t now = time(NULL);
        if (now - last_cleanup > 60) {  // 每分钟清理一次
            cleanup_expired_clients();
            last_cleanup = now;
        }
    }

    return NULL;
}

// 创建反馈接收线程
int thr_feedback_create(void) {
    init_channel_rates();
    
    // 初始化客户端数组
    memset(clients, 0, sizeof(clients));
    
    int err = pthread_create(&feedback_tid, NULL, feedback_thread, NULL);
    if (err) {
        syslog(LOG_ERR, "pthread_create(feedback_thread):%s", strerror(err));
        cleanup_channel_rates();
        return -err;
    }
    
    syslog(LOG_INFO, "Feedback thread created successfully");
    return 0;
}

// 销毁反馈接收线程
int thr_feedback_destroy(void) {
    if (feedback_socket > 0) {
        close(feedback_socket);
        feedback_socket = -1;
    }
    
    pthread_cancel(feedback_tid);
    pthread_join(feedback_tid, NULL);
    
    cleanup_channel_rates();
    
    syslog(LOG_INFO, "Feedback thread destroyed");
    return 0;
}

// 获取指定频道的速率倍数
/*
功能：获取当前频道的速率调整倍数
参数：entry->chnid - 频道ID
返回值：速率倍数（如1.0表示正常速率，0.5表示减半）
用途：监控和调试速率调整状态
*/
double get_channel_rate_multiplier(chnid_t channel_id) {
    if (channel_id < MINCHNID || channel_id > MAXCHNID) {
        return 1.0;
    }
    
    double multiplier;
    pthread_mutex_lock(&g_channel_rates[channel_id].mutex);
    multiplier = g_channel_rates[channel_id].rate_multiplier;
    pthread_mutex_unlock(&g_channel_rates[channel_id].mutex);
    
    return multiplier;
}

// 获取指定频道的sleep时间（纳秒）
/*
功能：根据频道状态和基础比特率计算睡眠时间
参数：

entry->chnid：频道ID
BASE_BITRATE：基础比特率


返回值：纳秒为单位的睡眠时间
用途：动态调整发送速率以适应网络条件
*/
long get_channel_sleep_ns(chnid_t channel_id, int base_bitrate) {
    //获取频道的控制速率
    double multiplier = get_channel_rate_multiplier(channel_id);
    
    // 计算每秒应该发送的字节数
    int bytes_per_second = base_bitrate / 8;  // 转换为字节
    int adjusted_bytes_per_second = (int)(bytes_per_second * multiplier);
    
    // 如果速率过低，设置最小值
    if (adjusted_bytes_per_second < bytes_per_second / 4) {
        adjusted_bytes_per_second = bytes_per_second / 4;
    }
    // 如果速率过高，设置最大值
    else if (adjusted_bytes_per_second > bytes_per_second * 2) {
        adjusted_bytes_per_second = bytes_per_second * 2;
    }
    
    // 计算每次发送后的sleep时间（纳秒）
    // 假设每次发送 28KB (224kbps / 8)
    long sleep_ns = (1000000000L * 28000) / adjusted_bytes_per_second;
    
    return sleep_ns;
}

// 更新频道速率控制信息
void update_channel_rate_control(chnid_t channel_id, rate_control_level_t rate_level, uint32_t client_id) {
    if (channel_id < MINCHNID || channel_id > MAXCHNID) {
        return;
    }
    
    pthread_mutex_lock(&g_channel_rates[channel_id].mutex);
    
    struct channel_rate_control *rate_ctrl = &g_channel_rates[channel_id];
    
    // 简单的平均算法：根据新的反馈调整速率倍数
    double new_multiplier = calculate_rate_multiplier(rate_level);
    
    // 使用指数移动平均来平滑速率变化
    double alpha = 0.3;  // 平滑因子
    rate_ctrl->rate_multiplier = alpha * new_multiplier + (1 - alpha) * rate_ctrl->rate_multiplier;
    
    // 限制速率倍数范围
    // if (rate_ctrl->rate_multiplier < 0.5) {
    //     rate_ctrl->rate_multiplier = 0.5;
    // } else if (rate_ctrl->rate_multiplier > 2.0) {
    //     rate_ctrl->rate_multiplier = 2.0;
    // }
    rate_ctrl->avg_rate_level = rate_level;
    rate_ctrl->last_update = time(NULL);
    
    pthread_mutex_unlock(&g_channel_rates[channel_id].mutex);
    
    syslog(LOG_DEBUG, "Channel %d rate multiplier updated to %.2f (level=%d)", 
           channel_id, rate_ctrl->rate_multiplier, rate_level);
}

// 清理过期客户端
void cleanup_expired_clients(void) {
    time_t now = time(NULL);
    int cleaned = 0;
    
    pthread_mutex_lock(&clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && (now - clients[i].last_update) > CLIENT_TIMEOUT) {
            syslog(LOG_INFO, "Client %u timed out, removing", clients[i].client_id);
            clients[i].active = 0;
            cleaned++;
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
    
    if (cleaned > 0) {
        syslog(LOG_DEBUG, "Cleaned up %d expired clients", cleaned);
    }
}