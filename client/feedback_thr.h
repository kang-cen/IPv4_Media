#ifndef FEEDBACK_THR_H
#define FEEDBACK_THR_H

#include <stdint.h>
#include <stdbool.h>
#include "client.h"



// 函数声明
void* feedback_thread(void* arg);
rate_control_level_t calculate_rate_level(uint32_t buffer_usage_percent);
int send_feedback_message(struct shared_data* shared, rate_control_level_t rate_level, uint32_t buffer_usage_percent);
uint32_t generate_client_id(struct shared_data* shared);
void init_feedback_control(struct shared_data* shared);

#endif // FEEDBACK_THR_H