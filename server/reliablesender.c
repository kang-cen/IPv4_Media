#include <asm-generic/errno-base.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "thr_channel.h"
#include "medialib.h"
#include "server_conf.h"
#include "../include/proto.h"
#include "reliablesender.h"

// 计算简单校验和
uint32_t calculate_checksum(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31); // 循环左移
    }
    return checksum;
}


// 获取当前时间戳（毫秒）
uint32_t get_timestamp_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// 发送端初始化
int sender_init(struct channel_sender* sender, const char* multicast_ip, 
                int port, uint16_t channel_id, uint32_t bitrate_kbps) {
    sender->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender->socket_fd < 0) {
        return -1;
    }
    
    // 设置多播TTL
    int ttl = 1;
    if (setsockopt(sender->socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, 
                   &ttl, sizeof(ttl)) < 0) {
        close(sender->socket_fd);
        return -1;
    }
    
    // 设置多播地址
    memset(&sender->multicast_addr, 0, sizeof(sender->multicast_addr));
    sender->multicast_addr.sin_family = AF_INET;
    sender->multicast_addr.sin_port = htons(port);
    inet_pton(AF_INET, multicast_ip, &sender->multicast_addr.sin_addr);
    
    sender->sequence_num = 0;
    sender->channel_id = channel_id;
    
    // 计算发送间隔 - 根据目标比特率
    // 假设每包1000字节数据，计算需要的发送间隔
    uint32_t bytes_per_second = bitrate_kbps * 1024 / 8;
    uint32_t packets_per_second = bytes_per_second / 1000;
    sender->send_interval_us = 1000000 / packets_per_second;
    
    return 0;
}

// 发送数据包
int send_audio_packet(struct channel_sender* sender, const void* data, size_t data_len) {
    if (data_len > MAX_DATA_SIZE) {
        return -1; // 数据太大
    }
    
    // 构造数据包
    uint8_t packet_buffer[sizeof(struct packet_header) + MAX_DATA_SIZE];
    struct packet_header* header = (struct packet_header*)packet_buffer;
    
    header->magic = htonl(PACKET_MAGIC);
    header->sequence = htonl(sender->sequence_num++);
    header->timestamp = htonl(get_timestamp_ms());
    header->channel_id = htons(sender->channel_id);
    header->data_len = htons((uint16_t)data_len);
    
    // 复制数据
    memcpy(packet_buffer + sizeof(struct packet_header), data, data_len);
    
    // 计算校验和（不包括校验和字段本身）
    header->checksum = 0;
    uint32_t checksum = calculate_checksum(packet_buffer, sizeof(struct packet_header) + data_len);
    header->checksum = htonl(checksum);
    
    // 发送数据包
    ssize_t sent = sendto(sender->socket_fd, packet_buffer, 
                         sizeof(struct packet_header) + data_len, 0,
                         (struct sockaddr*)&sender->multicast_addr, 
                         sizeof(sender->multicast_addr));
                         
    if (sent < 0) {
        syslog(LOG_ERR, "send_audio_packet: sendto failed: %s", strerror(errno));
        return -1;
    }
    
    // 速率控制
    usleep(sender->send_interval_us);
    
    return 0;
}