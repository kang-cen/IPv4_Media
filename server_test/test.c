// 自适应音频接收器 - 根据播放器处理能力动态调节
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

// 自适应控制结构
struct adaptive_control {
    // 速度监控
    double receive_rate;    // 接收速率 (packets/s)
    double consume_rate;    // 消费速率 (packets/s)
    double target_latency;  // 目标延迟 (秒)
    
    // 动态调节参数
    int skip_factor;        // 跳帧因子 (1=不跳, 2=跳一半, 3=跳2/3)
    int quality_level;      // 质量等级 (1-5)
    bool adaptive_enabled;  // 是否启用自适应
    
    // 统计信息
    struct timeval last_rate_calc;
    long packets_in_window;
    long consumed_in_window;
    
    pthread_mutex_t mutex;
};

// 扩展的共享数据结构
struct shared_data {
    // 原有成员...
    struct ring_buffer rb;
    pthread_mutex_t rb_mutex;
    pthread_cond_t rb_cond;
    
    // 新增自适应控制
    struct adaptive_control adaptive;
    
    // 统计信息
    volatile long packets_received;
    volatile long packets_written;
    volatile long packets_dropped;
    volatile long packets_skipped;  // 新增：主动跳过的包
    volatile long bytes_written;
    
    // 控制标志
    volatile bool stop_flag;
    volatile bool receiver_ready;
    
    // 播放器相关
    int fifo_fd;
    pid_t player_pid;
    char fifo_path[256];
};

// 初始化自适应控制
void init_adaptive_control(struct adaptive_control *ctrl) {
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->target_latency = 0.5;  // 500ms目标延迟
    ctrl->skip_factor = 1;       // 初始不跳帧 
    ctrl->quality_level = 5;     // 最高质量
    ctrl->adaptive_enabled = true;
    gettimeofday(&ctrl->last_rate_calc, NULL);
    pthread_mutex_init(&ctrl->mutex, NULL);
}

// 更新速率统计
void update_rates(struct shared_data *shared) {
    struct adaptive_control *ctrl = &shared->adaptive;
    struct timeval now;
    gettimeofday(&now, NULL);
    
    double elapsed = (now.tv_sec - ctrl->last_rate_calc.tv_sec) + 
                    (now.tv_usec - ctrl->last_rate_calc.tv_usec) / 1000000.0;
    
    if (elapsed >= 1.0) {  // 每秒更新一次
        pthread_mutex_lock(&ctrl->mutex);
        
        ctrl->receive_rate = ctrl->packets_in_window / elapsed;
        ctrl->consume_rate = ctrl->consumed_in_window / elapsed;
        
        // 重置计数器
        ctrl->packets_in_window = 0;
        ctrl->consumed_in_window = 0;
        ctrl->last_rate_calc = now;
        
        pthread_mutex_unlock(&ctrl->mutex);
    }
}

// 自适应算法 - 根据缓冲区状态和速率差调整策略
void adaptive_adjust(struct shared_data *shared) {
    struct adaptive_control *ctrl = &shared->adaptive;
    
    if (!ctrl->adaptive_enabled) return;
    
    pthread_mutex_lock(&ctrl->mutex);
    
    // 计算缓冲区使用率和速率比
    double buffer_usage = (double)ring_buffer_used(&shared->rb) / ring_buffer_size(&shared->rb);
    double rate_ratio = (ctrl->consume_rate > 0) ? ctrl->receive_rate / ctrl->consume_rate : 10.0;
    
    // 自适应策略
    if (buffer_usage > 0.8 || rate_ratio > 1.5) {
        // 缓冲区快满或接收速度明显快于消费速度
        if (ctrl->skip_factor < 4) {
            ctrl->skip_factor++;
            printf("Adaptive: Increasing skip factor to %d (buffer: %.1f%%, ratio: %.2f)\n", 
                   ctrl->skip_factor, buffer_usage * 100, rate_ratio);
        }
    } else if (buffer_usage < 0.3 && rate_ratio < 1.2) {
        // 缓冲区使用率低且速度匹配良好
        if (ctrl->skip_factor > 1) {
            ctrl->skip_factor--;
            printf("Adaptive: Decreasing skip factor to %d (buffer: %.1f%%, ratio: %.2f)\n", 
                   ctrl->skip_factor, buffer_usage * 100, rate_ratio);
        }
    }
    
    pthread_mutex_unlock(&ctrl->mutex);
}

// 智能包过滤 - 决定是否跳过当前包
bool should_skip_packet(struct shared_data *shared, int seq_num) {
    struct adaptive_control *ctrl = &shared->adaptive;
    
    if (ctrl->skip_factor <= 1) return false;
    
    // 简单的跳帧策略：保留关键帧，跳过部分非关键帧
    // 对于音频，我们可以基于序列号进行周期性跳帧
    return (seq_num % ctrl->skip_factor) != 0;
}

// 优化的接收线程
void* adaptive_receiver_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    char buffer[2048];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int bytes_received;
    int seq_num = 0;
    
    printf("Adaptive receiver thread started\n");
    shared->receiver_ready = true;
    
    while (!shared->stop_flag) {
        bytes_received = recvfrom(shared->socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&client_addr, &addr_len);
        
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            perror("recvfrom");
            break;
        }
        
        seq_num++;
        shared->adaptive.packets_in_window++;
        shared->packets_received++;
        
        // 自适应跳包决策
        if (should_skip_packet(shared, seq_num)) {
            shared->packets_skipped++;
            continue;
        }
        
        // 尝试写入环形缓冲区
        if (ring_buffer_write(&shared->rb, buffer, bytes_received) < 0) {
            shared->packets_dropped++;
            printf("Buffer full, dropped packet (seq: %d)\n", seq_num);
        }
        
        // 定期更新速率和调整策略
        if (seq_num % 100 == 0) {
            update_rates(shared);
            adaptive_adjust(shared);
        }
    }
    
    printf("Adaptive receiver thread exiting\n");
    return NULL;
}

// 改进的FIFO写入线程
void* adaptive_fifo_writer_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    char buffer[64 * 1024];  // 适中的缓冲区大小
    int bytes_read;
    
    printf("Adaptive FIFO writer thread started\n");
    
    while (!shared->receiver_ready && !shared->stop_flag) {
        usleep(1000);
    }
    
    while (!shared->stop_flag) {
        bytes_read = ring_buffer_read_timeout(&shared->rb, buffer, sizeof(buffer), 100);
        
        if (bytes_read <= 0) {
            usleep(10000);  // 10ms
            continue;
        }
        
        // 非阻塞写入，如果FIFO满就暂时跳过
        int written = write(shared->fifo_fd, buffer, bytes_read);
        
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // FIFO满，说明播放器处理不过来，跳过这个buffer
                shared->packets_dropped++;
                printf("FIFO full, dropping data to prevent blocking\n");
                continue;
            } else if (errno != EINTR) {
                perror("write to fifo");
                shared->stop_flag = true;
                break;
            }
        } else if (written > 0) {
            shared->bytes_written += written;
            shared->packets_written++;
            shared->adaptive.consumed_in_window++;
        }
    }
    
    printf("Adaptive FIFO writer thread exiting\n");
    return NULL;
}

// 增强的统计线程
void* enhanced_stats_thread(void* arg) {
    struct shared_data *shared = (struct shared_data*)arg;
    long prev_received = 0, prev_written = 0, prev_dropped = 0, prev_skipped = 0;
    
    while (!shared->stop_flag) {
        sleep(5);
        
        long curr_received = shared->packets_received;
        long curr_written = shared->packets_written;  
        long curr_dropped = shared->packets_dropped;
        long curr_skipped = shared->packets_skipped;
        
        struct adaptive_control *ctrl = &shared->adaptive;
        double buffer_usage = (double)ring_buffer_used(&shared->rb) / ring_buffer_size(&shared->rb);
        
        printf("\n=== Enhanced Stats ===\n");
        printf("Packets: Recv %ld (+%ld/5s), Written %ld (+%ld/5s)\n",
               curr_received, curr_received - prev_received,
               curr_written, curr_written - prev_written);
        printf("Dropped: %ld (+%ld/5s), Skipped: %ld (+%ld/5s)\n", 
               curr_dropped, curr_dropped - prev_dropped,
               curr_skipped, curr_skipped - prev_skipped);
        printf("Rates: Receive %.1f pps, Consume %.1f pps\n", 
               ctrl->receive_rate, ctrl->consume_rate);
        printf("Buffer usage: %.1f%%, Skip factor: %d\n", 
               buffer_usage * 100, ctrl->skip_factor);
        printf("Bytes written: %ld\n", shared->bytes_written);
        printf("=====================\n\n");
        
        prev_received = curr_received;
        prev_written = curr_written;
        prev_dropped = curr_dropped;
        prev_skipped = curr_skipped;
    }
    
    return NULL;
}

// 使用示例main函数
int main(int argc, char *argv[]) {
    struct shared_data shared_data = {0};
    pthread_t receiver_tid, writer_tid, stats_tid;
    
    // 初始化
    init_ring_buffer(&shared_data.rb, 2 * 1024 * 1024);  // 2MB环形缓冲区
    init_adaptive_control(&shared_data.adaptive);
    
    // 设置信号处理
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);
    
    // 初始化网络socket (省略具体实现)
    // shared_data.socket = setup_socket();
    
    // 设置命名管道和播放器
    if (setup_named_fifo_player(&shared_data) < 0) {
        exit(1);
    }
    
    // 创建线程
    pthread_create(&receiver_tid, NULL, adaptive_receiver_thread, &shared_data);
    pthread_create(&writer_tid, NULL, adaptive_fifo_writer_thread, &shared_data);
    pthread_create(&stats_tid, NULL, enhanced_stats_thread, &shared_data);
    
    printf("Adaptive audio receiver started. Press Ctrl+C to exit.\n");
    
    // 等待线程结束
    pthread_join(receiver_tid, NULL);
    pthread_join(writer_tid, NULL);
    pthread_join(stats_tid, NULL);
    
    // 清理资源
    cleanup_resources(&shared_data);
    
    return 0;
}