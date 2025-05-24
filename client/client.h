#ifndef CLIENT_H_
#define CLIENT_H_
#define DEFAULT_PLAYERCMD " /usr/bin/mpg123 -   > /dev/null"
// #define DEFAULT_PLAYERCMD " /usr/bin/mplayer -   > /dev/null"
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
struct client_conf_st
{
  char *rcvport; // for local using
  char *mgroup;
  char *player_cmd;
};


// 环形缓冲区结构
#define RING_BUFFER_SIZE (2 * 1024 * 1024) // 2MB环形缓冲区大小

struct ring_buffer {
    char data[RING_BUFFER_SIZE];      // 实际存储音频数据的缓冲区数组
                                      // 使用char数组便于按字节操作
    
    volatile size_t head;             // 读指针：指向下一个要读取的位置
                                      // volatile确保多线程间可见性
                                      // 消费者线程(writer_thread)使用
    
    volatile size_t tail;             // 写指针：指向下一个要写入的位置  
                                      // volatile确保多线程间可见性
                                      // 生产者线程(receiver_thread)使用
    
    volatile size_t count;            // 当前缓冲区中的数据字节数
                                      // 用于判断缓冲区空/满状态
                                      // volatile确保统计数据实时更新
    
    pthread_mutex_t mutex;            // 互斥锁：保护缓冲区的并发访问
                                      // 确保读写操作的原子性
    
    pthread_cond_t not_empty;         // 条件变量：通知缓冲区非空
                                      // 当生产者写入数据后，唤醒等待的消费者
    
    pthread_cond_t not_full;          // 条件变量：通知缓冲区非满
                                      // 当消费者读取数据后，唤醒等待的生产者
};

// 共享数据结构 - 多线程间的通信桥梁
struct shared_data {
    struct ring_buffer rb;            // 环形缓冲区实例
                                      // 接收线程和写入线程共享的数据存储
    
    int socket_fd;                    // UDP socket文件描述符
                                      // 接收线程用于从网络接收数据
    
    int pipe_fd;                      // 管道写端文件描述符
                                      // 写入线程用于向子进程发送数据
    
    int chosen_channel;               // 用户选择的音频频道号
                                      // 接收线程用于过滤数据包
    
    struct sockaddr_in server_addr;   // 服务器地址信息
                                      // 用于验证数据包来源，防止恶意包
    
    volatile bool stop_flag;          // 全局停止标志
                                      // 用于优雅地终止所有线程
                                      // volatile确保线程间立即可见
    
    volatile bool receiver_ready;     // 接收线程就绪标志
                                      // 确保写入线程在接收线程启动后再开始工作
                                      // 避免写入线程空等待
    
    // 统计信息 - 用于监控程序性能和调试
    volatile long packets_received;   // 已接收的数据包总数
                                      // 接收线程更新，统计线程读取
    
    volatile long packets_written;    // 已写入管道的数据包总数  
                                      // 写入线程更新，统计线程读取
    
    volatile long packets_dropped;    // 丢弃的数据包总数
                                      // 当缓冲区满时递增
    
    volatile long bytes_received;     // 已接收的字节总数
                                      // 用于计算网络吞吐量
    
    volatile long bytes_written;      // 已写入的字节总数
                                      // 用于计算处理吞吐量
};


extern  struct shared_data shared_data;

#endif
