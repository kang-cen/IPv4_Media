#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "stat_thr.h"
#include "client.h"
// 统计线程
void* stats_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    long last_received = 0, last_written = 0;//上次统计的收发包总数，便于计算速率。
    
    while (!shared->stop_flag) {
        sleep(5); // 每5秒输出一次统计
        
        long current_received = shared->packets_received;
        long current_written = shared->packets_written;
        /*
        总共收到多少包；
        每秒接收速度；
        总共写入了多少包（可能是写入文件、网络等）；
        每秒写入速度；
        丢弃了多少包（可能是因为缓冲区满）
        */
        printf("Stats: Received %ld packets (%ld/s), Written %ld packets (%ld/s), Dropped %ld\n",
               current_received, (current_received - last_received) / 5,
               current_written, (current_written - last_written) / 5,
               shared->packets_dropped);
        printf("       Bytes: Received %ld, Written %ld, Buffer usage: %zu/%d\n",
               shared->bytes_received, shared->bytes_written,
               shared->rb.count, RING_BUFFER_SIZE);
        
        last_received = current_received;
        last_written = current_written;
    }
    
    pthread_exit(NULL);
}
