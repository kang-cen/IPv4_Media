#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "feedback_thr.h"
#include "client.h"
#include <proto.h>

// 初始化反馈控制结构
void init_feedback_control(struct shared_data* shared) {
    shared->feedback_ctrl.enabled = true;
    shared->feedback_ctrl.seq_counter = 1;
    shared->feedback_ctrl.last_sent_seq = 0;
    shared->feedback_ctrl.last_rate_level = RATE_NORMAL;
    shared->feedback_ctrl.retry_count = 0;
    shared->feedback_ctrl.ack_received = true; // 初始设为true，避免首次发送被阻塞
}

// 生成客户端ID（基于本地地址和端口）
uint32_t generate_client_id(struct sockaddr_in* addr) {
    // 简单的hash方法：IP地址XOR端口号
    uint32_t ip = ntohl(addr->sin_addr.s_addr);
    uint16_t port = ntohs(addr->sin_port);
    return ip ^ port ;

}

// 根据缓冲区使用率计算速率控制等级
rate_control_level_t calculate_rate_level(uint32_t buffer_usage_percent) {
    if (buffer_usage_percent > 75) {
        return RATE_MAJOR_DEC;      // 大幅降低速率
    } else if (buffer_usage_percent > 50) {
        return RATE_MODERATE_DEC;   // 适度降低速率
    } else if (buffer_usage_percent < 25) {
        return RATE_INCREASE;       // 提高速率
    } else {
        return RATE_NORMAL;         // 正常速率
    }
}

// 发送反馈消息给服务器
int send_feedback_message(struct shared_data* shared, rate_control_level_t rate_level, uint32_t buffer_usage_percent) {
    struct feedback_msg msg;
    int ret;
    
    // 构造反馈消息
    memset(&msg, 0, sizeof(msg));
    msg.magic = htonl(PACKET_MAGIC);
    msg.client_id = htonl(generate_client_id(&shared->server_addr));
    msg.channel_id = htonl(shared->chosen_channel);
    msg.rate_level = htonl(rate_level);
    msg.buffer_usage = htonl(buffer_usage_percent);
    msg.timestamp = htonl((uint32_t)time(NULL));
    msg.seq_num = htonl(shared->feedback_ctrl.seq_counter++);
    
    // 发送UDP消息到服务器
    ret = sendto(shared->socket_fd, &msg, sizeof(msg), 0,
                 (struct sockaddr*)&shared->server_addr, sizeof(shared->server_addr));
    
    if (ret < 0) {
        perror("sendto feedback message");
        return -1;
    }
    
    // 更新发送记录
    shared->feedback_ctrl.last_sent_seq = msg.seq_num;
    shared->feedback_ctrl.last_rate_level = rate_level;
    shared->feedback_ctrl.ack_received = false; // 等待确认
    
    return 0;
}

// 反馈控制线程主函数
void* feedback_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    uint32_t buffer_usage_percent;
    rate_control_level_t current_rate_level;//速率控制等级
    rate_control_level_t last_reported_level = RATE_NORMAL;
    time_t last_send_time = 0;     // 上次发送时间
    int current_send_interval = 1; // 当前发送间隔（秒）
    // 休眠指定间隔
    struct timespec req;
    req.tv_sec = 1;          // 1 秒
    req.tv_nsec = 0;         // 0 纳秒    
    printf("Feedback control thread started\n");
    
    // 等待接收线程准备就绪
    while (!shared->receiver_ready && !shared->stop_flag) {
        usleep(10000); // 10ms
    }
    
    // 初始化反馈控制
    init_feedback_control(shared);
    
    while (!shared->stop_flag) {
        nanosleep(&req, NULL);     //休眠1s   
        if (shared->stop_flag) break;
        
        // 读取当前缓冲区状态（使用原有互斥量保护）
        pthread_mutex_lock(&shared->rb.mutex);
        size_t current_count = shared->rb.count;
        pthread_mutex_unlock(&shared->rb.mutex);
        
        // 计算缓冲区使用率百分比
        buffer_usage_percent = (uint32_t)((current_count * 100) / RING_BUFFER_SIZE);
        current_rate_level = calculate_rate_level(buffer_usage_percent);
                
        // 根据条件调整发送间隔
        if (current_rate_level != last_reported_level) {
            current_send_interval = 1; // 变化时立即发送
        } else if (current_rate_level == RATE_MAJOR_DEC) {
            current_send_interval = 1; // 每秒发送
        } else if (current_rate_level == RATE_MODERATE_DEC) {
            current_send_interval = 2; // 每2秒发送
        } else {
            current_send_interval = 3; // 每3秒发送
        }
        
        // 检查是否到达发送时间
        time_t now = time(NULL);
        if (now - last_send_time >= current_send_interval && shared->feedback_ctrl.enabled) {
            if (send_feedback_message(shared, current_rate_level, buffer_usage_percent) == 0) {
                printf(" last_send_time=%lu, now=%lu ,diff=%lu\n",last_send_time,now,now-last_send_time);
                last_send_time = now;  // 发送成功后更新 last_send_time
                printf("Feedback sent: Rate=%d, Buffer=%u%%, Seq=%u\n",
                       current_rate_level, buffer_usage_percent, 
                       shared->feedback_ctrl.last_sent_seq);
                
                       last_reported_level = current_rate_level;
            } else {
                fprintf(stderr, "Failed to send feedback message\n");
                shared->feedback_ctrl.retry_count++;
                
                // 重试机制：如果连续失败3次，暂停反馈5秒
                if (shared->feedback_ctrl.retry_count >= 3) {
                    printf("Feedback sending failed 3 times, pausing for 5 seconds\n");
                     struct timespec req1;

                    req1.tv_sec = 1;          // 5 秒
                    req1.tv_nsec = 0;         // 0 纳秒
                    nanosleep(&req1,NULL);
                    shared->feedback_ctrl.retry_count = 0;
                }
            }
        }
    }
        
    printf("Feedback control thread exiting\n");
    pthread_exit(NULL);
}