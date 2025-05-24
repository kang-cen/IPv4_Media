#include <stdio.h>
#include <stdlib.h>
#include <string.h>          // 新增：memcpy()
#include <unistd.h>          // 新增：write(), usleep()
#include <errno.h>           // 新增：errno, EINTR, EAGAIN等
#include <pthread.h>         // 新增：pthread相关函数
#include "client.h"          // 先包含client.h
#include "writer_thr.h"      // 再包含writer_thr.h

// 从环形缓冲区读取（消费者）
int ring_buffer_read(struct ring_buffer *rb, void *data, size_t max_len) {
    pthread_mutex_lock(&rb->mutex);
    
    // 等待有数据可读
    while (rb->count == 0) {
        pthread_cond_wait(&rb->not_empty, &rb->mutex);
    }
    
    // 确定实际读取长度
    size_t len = (rb->count < max_len) ? rb->count : max_len;
    
    // 读取数据
    size_t available = RING_BUFFER_SIZE - rb->head;//从当前读取指针（rb->head）到缓冲区末尾，还有多少连续可读的数据字节。
    if (len <= available) {
        memcpy(data, &rb->data[rb->head], len);
        rb->head = (rb->head + len) % RING_BUFFER_SIZE;
    } else {
        memcpy(data, &rb->data[rb->head], available);
        memcpy((char*)data + available, &rb->data[0], len - available);
        rb->head = len - available;
    }
    
    rb->count -= len;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);
    return len;
}

// 管道写入线程
void* writer_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    char buffer[64 * 1024]; // 64KB写入缓冲区
    int bytes_read;
    
    printf("Writer thread started\n");
    
    // 等待接收线程准备就绪
    while (!shared->receiver_ready && !shared->stop_flag) {
        usleep(1000); // 1ms
    }
    
    while (!shared->stop_flag) {
        bytes_read = ring_buffer_read(&shared->rb, buffer, sizeof(buffer));
        
        if (bytes_read > 0) {
            int pos = 0;
            while (pos < bytes_read && !shared->stop_flag) {
                int written = write(shared->pipe_fd, buffer + pos, bytes_read - pos);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        usleep(1000); // 管道满，短暂休眠
                        continue;
                    } else {
                        perror("write to pipe");
                        shared->stop_flag = true;
                        break;
                    }
                } else {
                    pos += written;
                    shared->bytes_written += written;
                }
            }
            shared->packets_written++;
        }
    }
    
    printf("Writer thread exiting\n");
    pthread_exit(NULL);
}