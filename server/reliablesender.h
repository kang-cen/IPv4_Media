#ifndef RELIABLESENDER_H_
#define RELIABLESENDER_H_

#include "../include/proto.h"
#include <stdio.h>

// 发送端实现
struct channel_sender {
    int socket_fd;
    struct sockaddr_in multicast_addr;
    uint32_t sequence_num;
    uint16_t channel_id;
    uint32_t send_interval_us; // 发送间隔（微秒）
};

uint32_t calculate_checksum(const void* data, size_t len) ;
uint32_t get_timestamp_ms();
int sender_init(struct channel_sender* sender, const char* multicast_ip, \
    int port, uint16_t channel_id, uint32_t bitrate_kbps) ;
int send_audio_packet(struct channel_sender* sender, const void* data, size_t data_len) {



#endif // RELIABLESENDER_H_
