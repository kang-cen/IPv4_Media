#ifndef THR_FEEDBACK_H_
#define THR_FEEDBACK_H_

#include <stdint.h>
#include <pthread.h>
#include "../include/proto.h"

#define MAX_CLIENTS 1024 //系统支持的最大客户端数量限制
#define CLIENT_TIMEOUT 30  // 客户端超时时间（秒）

// 客户端状态信息  维护单个客户端的状态信息和反馈数据
struct client_state {
    uint32_t client_id;//客户端id hash出来的
    int32_t channel_id;
    rate_control_level_t last_rate_level; //客户端上次报告的速率控制等级（网络质量指标）
    uint32_t last_seq_num; //最后接收的数据包序列号，用于丢包检测
    uint32_t last_timestamp; //最后活动时间戳，用于同步和延迟计算
    time_t last_update; //最后更新时间，用于超时检测
    int active; //客户端活跃状态标志
};

// 频道速率控制信息 管理每个频道的动态速率控制策略
struct channel_rate_control {
    chnid_t channel_id;
    double rate_multiplier;  // 速率倍数 (0.5 - 2.0)
    rate_control_level_t avg_rate_level;  // 平均速率控制等级
    pthread_mutex_t mutex; //线程同步锁，保护并发访问
    time_t last_update; //
};

// 全局速率控制数组，按频道ID索引
extern struct channel_rate_control g_channel_rates[MAXCHNID + 1];

// 函数声明
int thr_feedback_create(void);
int thr_feedback_destroy(void);
static int create_log_file(void) ;
static void log_feedback_data(uint32_t client_id, int32_t channel_id, uint32_t buffer_usage, 
                             uint32_t timestamp, uint32_t seq_num, double rate_multiplier) ;
// 获取指定频道的速率倍数
double get_channel_rate_multiplier(chnid_t channel_id);

// 获取指定频道的sleep时间（纳秒）
long get_channel_sleep_ns(chnid_t channel_id, int base_bitrate);

// 更新频道速率控制信息
void update_channel_rate_control(chnid_t channel_id, rate_control_level_t rate_level, uint32_t client_id);

// 清理过期客户端
void cleanup_expired_clients(void);

#endif // THR_FEEDBACK_H_