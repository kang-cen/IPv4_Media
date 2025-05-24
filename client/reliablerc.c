#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
//#include <proto.h>
#include "../include/proto.h"
#include "client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <net/if.h>
#include <string.h>
#include "reliablerc.h"


// 接收端初始化
int receiver_init(struct receive_buffer* rcv_buf) {
    memset(rcv_buf, 0, sizeof(struct receive_buffer));
    rcv_buf->expected_seq = 0;
    rcv_buf->last_received_seq = 0;
    return 0;
}

// 检查序列号是否在接收窗口内
int is_sequence_in_window(uint32_t seq, uint32_t expected_seq) {
    // 处理序列号回绕
    int32_t diff = (int32_t)(seq - expected_seq);
    return (diff >= 0 && diff < SEQUENCE_WINDOW) || 
           (diff < 0 && diff > -SEQUENCE_WINDOW);
}

// 处理接收到的数据包
int process_received_packet(struct receive_buffer* rcv_buf, 
                           const uint8_t* packet_data, size_t packet_len,
                           uint16_t target_channel_id,
                           uint8_t* output_buffer, size_t* output_len) {
    if (packet_len < sizeof(struct packet_header)) {
        return -1; // 包太小
    }
    
    const struct packet_header* header = (const struct packet_header*)packet_data;
    
    // 验证魔数
    if (ntohl(header->magic) != PACKET_MAGIC) {
        return -1; // 无效包
    }
    
    // 检查频道ID
    uint16_t channel_id = ntohs(header->channel_id);
    if (channel_id != target_channel_id) {
        return 0; // 不是目标频道，但不是错误
    }
    
    // 验证校验和
    struct packet_header temp_header = *header;
    temp_header.checksum = 0;
    uint32_t expected_checksum = calculate_checksum(packet_data, packet_len);
    if (ntohl(header->checksum) != expected_checksum) {
        syslog(LOG_WARNING, "Packet checksum mismatch, dropping");
        return -1; // 校验和不匹配
    }
    
    uint32_t sequence = ntohl(header->sequence);
    uint16_t data_len = ntohs(header->data_len);
    uint32_t timestamp = ntohl(header->timestamp);
    
    // 检查数据长度
    if (sizeof(struct packet_header) + data_len != packet_len) {
        return -1; // 长度不匹配
    }
    
    // 序列号处理
    if (sequence == rcv_buf->expected_seq) {
        // 期望的包，直接处理
        memcpy(output_buffer, packet_data + sizeof(struct packet_header), data_len);
        *output_len = data_len;
        
        rcv_buf->expected_seq++;
        rcv_buf->last_received_seq = sequence;
        
        // 检查缓冲区中是否有连续的包
        while (1) {
            uint32_t next_seq = rcv_buf->expected_seq;
            uint32_t buffer_idx = next_seq % SEQUENCE_WINDOW;
            
            if (rcv_buf->packets[buffer_idx].valid && 
                next_seq <= rcv_buf->last_received_seq + SEQUENCE_WINDOW) {
                
                // 找到连续的包，添加到输出
                size_t current_len = *output_len;
                memcpy(output_buffer + current_len, 
                       rcv_buf->packets[buffer_idx].data,
                       rcv_buf->packets[buffer_idx].len);
                *output_len += rcv_buf->packets[buffer_idx].len;
                
                // 清除缓冲区
                rcv_buf->packets[buffer_idx].valid = 0;
                rcv_buf->expected_seq++;
            } else {
                break;
            }
        }
        
        return 1; // 有数据输出
        
    } else if (is_sequence_in_window(sequence, rcv_buf->expected_seq)) {
        // 乱序包或重复包
        if (sequence > rcv_buf->expected_seq) {
            // 未来的包，缓存起来
            uint32_t buffer_idx = sequence % SEQUENCE_WINDOW;
            
            if (!rcv_buf->packets[buffer_idx].valid) {
                rcv_buf->packets[buffer_idx].valid = 1;
                memcpy(rcv_buf->packets[buffer_idx].data, 
                       packet_data + sizeof(struct packet_header), data_len);
                rcv_buf->packets[buffer_idx].len = data_len;
                rcv_buf->packets[buffer_idx].timestamp = timestamp;
                
                rcv_buf->reorder_count++;
                
                // 更新最后接收的序列号
                if (sequence > rcv_buf->last_received_seq) {
                    rcv_buf->last_received_seq = sequence;
                }
            }
        }
        // 过去的包（重复包）直接丢弃
        
    } else {
        // 序列号超出窗口，可能是网络严重延迟或者重传的旧包
        syslog(LOG_WARNING, "Packet sequence %u out of window (expected %u)", 
               sequence, rcv_buf->expected_seq);
    }
    
    return 0; // 没有数据输出
}

// 获取接收统计信息
void get_receive_stats(const struct receive_buffer* rcv_buf, 
                      uint32_t* missing_count, uint32_t* reorder_count) {
    *missing_count = rcv_buf->missing_count;
    *reorder_count = rcv_buf->reorder_count;
}

// 改进的发送线程函数示例
void* improved_sender_thread(void* arg) {
    struct channel_sender sender;
    // ... 初始化发送器 ...
    
    uint8_t audio_data[1000]; // 音频数据缓冲区
    
    while (1) {
        // 从音频源获取数据
        // ... 填充 audio_data ...
        
        if (send_audio_packet(&sender, audio_data, sizeof(audio_data)) < 0) {
            syslog(LOG_ERR, "Failed to send audio packet");
            break;
        }
    }
    
    return NULL;
}

// 改进的接收循环示例
void improved_receive_loop(int socket_fd, uint16_t target_channel_id, int pipe_fd) {
    struct receive_buffer rcv_buf;
    receiver_init(&rcv_buf);
    
    uint8_t packet_buffer[sizeof(struct packet_header) + MAX_DATA_SIZE];
    uint8_t output_buffer[MAX_DATA_SIZE * 10]; // 输出缓冲区
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    
    while (1) {
        ssize_t len = recvfrom(socket_fd, packet_buffer, sizeof(packet_buffer), 0,
                              (struct sockaddr*)&sender_addr, &addr_len);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            syslog(LOG_ERR, "recvfrom failed: %s", strerror(errno));
            break;
        }
        
        size_t output_len = 0;
        int result = process_received_packet(&rcv_buf, packet_buffer, len, 
                                           target_channel_id, output_buffer, &output_len);
        
        if (result > 0 && output_len > 0) {
            // 有数据需要写入管道
            if (write(pipe_fd, output_buffer, output_len) < 0) {
                syslog(LOG_ERR, "write to pipe failed: %s", strerror(errno));
                break;
            }
        }
        
        // 定期输出统计信息
        static uint32_t packet_count = 0;
        if (++packet_count % 1000 == 0) {
            uint32_t missing, reorder;
            get_receive_stats(&rcv_buf, &missing, &reorder);
            syslog(LOG_INFO, "Stats: missing=%u, reorder=%u", missing, reorder);
        }
    }
}