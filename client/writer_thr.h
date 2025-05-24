#ifndef WRITERTH_H
#define WRITERTH_H
#include <sys/types.h>       // 新增：size_t类型
#include "client.h"          // 新增：包含client.h以获取struct ring_buffer定义


void* writer_thread(void* arg);
int ring_buffer_read(struct ring_buffer *rb, void *data, size_t max_len);

#endif