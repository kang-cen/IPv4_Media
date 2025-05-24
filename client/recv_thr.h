#ifndef RECV_H_
#define RECV_H_
#include <sys/types.h>       // 新增：size_t类型
#include "client.h"          // 新增：包含client.h以获取struct ring_buffer定义


// 添加序列号跟踪
extern  uint32_t expected_seq ;
extern  bool first_packet ;
extern  int packet_loss_count ;

void* receiver_thread(void* arg);
int ring_buffer_write(struct ring_buffer *rb, const void *data, size_t len);


#endif