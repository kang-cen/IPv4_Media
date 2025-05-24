#include <stdio.h>
#include <stdlib.h>
#include <string.h>          // 新增：memcpy()
#include <errno.h>           // 新增：errno, EINTR等
#include <time.h>            // 新增：clock_gettime(), struct timespec
#include <pthread.h>         // 新增：pthread相关函数
#include <sys/socket.h>      // 新增：recvfrom()
#include <netinet/in.h>      // 新增：struct sockaddr_in
#include <arpa/inet.h>       // 新增：inet_ntop()
#include "../include/proto.h" // 新增：MSG_CHANNEL_MAX, struct msg_channel_st等
#include "recv_thr.h"        // 修正：应该是 recv_thr.h 而不是 recv.h
#include "client.h"


// 添加序列号跟踪
uint32_t expected_seq = 0;
bool first_packet = true;
int packet_loss_count = 0;


// 写入环形缓冲区（生产者）
int ring_buffer_write(struct ring_buffer *rb, const void *data, size_t len) {
    pthread_mutex_lock(&rb->mutex);
    
    // 等待缓冲区有空间，但设置超时避免永久阻塞
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += 10000000; // 10ms超时
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec += 1;
        timeout.tv_nsec -= 1000000000;
    }
    
    while (rb->count + len > RING_BUFFER_SIZE) {  //rb->count 当前缓冲区中的数据字节数
      /*pthread_cond_timedwait 是 POSIX 线程库（pthreads）中用于条件变量的函数，带有超时功能。
      它的作用是：
      等待一个条件变量（rb->not_full）满足，同时自动解锁互斥锁（rb->mutex）；
      如果条件变量在超时时间（timeout）内被唤醒，就继续执行；
      如果超时了还没被唤醒，则返回超时错误。
      */
        if (pthread_cond_timedwait(&rb->not_full, &rb->mutex, &timeout) != 0) {
            pthread_mutex_unlock(&rb->mutex);
            return -1; // 超时，丢弃数据
        }
    }
    
    // 写入数据
    size_t available = RING_BUFFER_SIZE - rb->tail; //写指针：指向下一个要写入的位置
    if (len <= available) {
        memcpy(&rb->data[rb->tail], data, len);
        rb->tail = (rb->tail + len) % RING_BUFFER_SIZE;
    } else { //空间不够，需要“绕回到头部”写：
        memcpy(&rb->data[rb->tail], data, available);//// 先写到缓冲区尾
        memcpy(&rb->data[0], (const char*)data + available, len - available);//// 再从头写剩下的部分
        rb->tail = len - available;//tail 更新为写入结束后的新位置（从 0 开始偏移）
    }
    
    rb->count += len;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

// UDP接收线程 这三个线程的参数都是共享数据
void* receiver_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    struct msg_channel_st *msg_channel;
    struct sockaddr_in raddr;
    socklen_t raddr_len = sizeof(raddr);
    int len;
    char ipstr_raddr[30];
    char ipstr_server_addr[30];
    
    msg_channel = malloc(MSG_CHANNEL_MAX);
    if (msg_channel == NULL) {
        perror("malloc in receiver_thread");
        pthread_exit(NULL);
    }
    
    printf("Receiver thread started\n");
    shared->receiver_ready = true;
    
    while (!shared->stop_flag) {
        len = recvfrom(shared->socket_fd, msg_channel, MSG_CHANNEL_MAX, 0, 
                      (void *)&raddr, &raddr_len);
        
        if (len < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom in receiver_thread");
            break;
        }
        
        // 验证发送者地址
        if (raddr.sin_addr.s_addr != shared->server_addr.sin_addr.s_addr) {
            inet_ntop(AF_INET, &raddr.sin_addr.s_addr, ipstr_raddr, sizeof(ipstr_raddr));
            inet_ntop(AF_INET, &shared->server_addr.sin_addr.s_addr, ipstr_server_addr, sizeof(ipstr_server_addr));
            fprintf(stderr, "Ignore: addr not match. raddr:%s server_addr:%s\n",
                    ipstr_raddr, ipstr_server_addr);
            continue;
        }
        
        if (raddr.sin_port != shared->server_addr.sin_port) {
            fprintf(stderr, "Ignore: port not match\n");
            continue;
        }
        
        if (len < sizeof(struct msg_channel_st)) {
            fprintf(stderr, "Ignore: message too short\n");
            continue;
        }
        
        if (msg_channel->chnid == shared->chosen_channel) {
            shared->packets_received++;
            
            // 检查序列号连续性
            if (first_packet) {
                expected_seq = msg_channel->seq + 1;
                first_packet = false;
            } else {
                if (msg_channel->seq != expected_seq) {
                    fprintf(stderr, "Warning: Sequence gap! Expected %u, got %u (lost %d packets)\n", 
                            expected_seq, msg_channel->seq, 
                            msg_channel->seq - expected_seq);
                    packet_loss_count += (msg_channel->seq - expected_seq);
                }
                expected_seq = msg_channel->seq + 1;
            }
            
            // 写入环形缓冲区
            size_t data_len = len - sizeof(chnid_t) - sizeof(uint32_t);
            if (ring_buffer_write(&shared->rb, msg_channel->data, data_len) == 0) {
                shared->bytes_received += data_len;
            } else {
                shared->packets_dropped++;
                fprintf(stderr, "Buffer full, dropped packet (seq: %u)\n", msg_channel->seq);
            }
        }
    }
    
    free(msg_channel);
    printf("Receiver thread exiting\n");
    pthread_exit(NULL);
}
