//void *memcpy(void *dest, const void *src, size_t n);
/*
    * 这个程序演示了如何使用现代柔性数组成员来创建和解析节目单消息。
    * 它定义了频道ID类型、节目单条目结构、节目单主结构和数据频道消息结构。
    * 通过创建和解析这些结构，展示了如何处理可变长度的数据。
    * 注意：这个示例程序使用了现代柔性数组成员的特性，确保编译器支持C99或更高版本。
    * 这个程序是一个简单的示例，实际应用中可能需要更复杂的错误处理和内存管理。  
    * 编译时使用 -std=c99 或更高版本
    * msg_list_st 是节目单的主结构体，包含频道ID和条目数量。
    * msg_listentry_st 是节目单的条目结构体，包含频道ID、描述长度和描述字符串。
    * msg_listentry_st实现是可变数组而msg_list_st也是可变长度但是由于现代编译特性所以两者不能嵌套
    * 所以外层只能用指针，对字节进行赋值
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 定义频道ID类型
typedef uint16_t chnid_t;

// 常量定义
#define LISTCHNID 0
#define MINCHNID 1
#define MAXCHNID 100

// 使用现代柔性数组的频道条目结构
struct msg_listentry_st {
    chnid_t chnid;
    uint16_t len;
    char desc[]; // 现代柔性数组成员
}__attribute__((packed));

// 节目单主结构
struct msg_list_st {
    chnid_t chnid; // 必须是LISTCHNID
    uint16_t entry_count; // 条目数量
    // 不能直接包含柔性数组结构体
    // 实际数据将在此结构体后面分配
}__attribute__((packed));

// 数据频道消息结构
struct msg_channel_st {
    chnid_t chnid; // 必须在MINCHNID和MAXCHNID之间
    uint8_t data[]; // 现代柔性数组成员
}__attribute__((packed));

// 创建一个节目单条目
struct msg_listentry_st* create_listentry(chnid_t id, const char* description) {
    size_t desc_len = strlen(description) + 1; // 包含结尾的'\0'
    struct msg_listentry_st* entry = malloc(sizeof(struct msg_listentry_st) + desc_len);
    
    if (entry == NULL) {
        return NULL;
    }
    
    entry->chnid = id;
    entry->len = desc_len;
    memcpy(entry->desc, description, desc_len);//entry->desc是字符串的地址 entry->desc[0]才是具体字符
    printf("验证复制: %s\n", entry->desc);
    return entry;
}

// 创建一个完整的节目单
void* create_msg_list(chnid_t* channel_ids, const char** descriptions, int count) {
    // 首先计算需要的总内存大小
    size_t total_size = sizeof(struct msg_list_st);
    
    // 临时保存每个条目指针，用于后面复制
    struct msg_listentry_st** temp_entries = malloc(count * sizeof(struct msg_listentry_st*));
    if (!temp_entries) return NULL;
    
    // 创建每个条目并计算总大小
    for (int i = 0; i < count; i++) {
        temp_entries[i] = create_listentry(channel_ids[i], descriptions[i]);
        if (!temp_entries[i]) {//存在则不进入 不存在说明创建失败则释放之前的然后返回NULL
            // 清理已分配的内存
            for (int j = 0; j < i; j++) {
                free(temp_entries[j]);
            }
            free(temp_entries);
            return NULL;
        }
        
        // 每个条目的大小是结构体大小加上描述字符串的长度
        total_size += sizeof(struct msg_listentry_st) + strlen(descriptions[i]) + 1;
    }
    
    // 分配完整的消息内存
    void* buffer = malloc(total_size);
    if (!buffer) {
        for (int i = 0; i < count; i++) {
            free(temp_entries[i]);
        }
        free(temp_entries);
        return NULL;
    }
    
    // 填充主结构体
    struct msg_list_st* msg_list = (struct msg_list_st*)buffer;
    msg_list->chnid = LISTCHNID;
    msg_list->entry_count = count;
    
    // 计算第一个条目的开始位置
    char* current_pos = (char*)buffer + sizeof(struct msg_list_st);
    
    // 复制每个条目到缓冲区
    for (int i = 0; i < count; i++) {
        size_t entry_size = sizeof(struct msg_listentry_st) + temp_entries[i]->len;
        memcpy(current_pos, temp_entries[i], entry_size);
        current_pos += entry_size;
        free(temp_entries[i]); // 释放临时条目
    }
    
    free(temp_entries);
    return buffer;
}

// 解析节目单消息
void parse_msg_list(void* buffer, size_t size) {
    if (size < sizeof(struct msg_list_st)) {
        printf("Buffer too small\n");
        return;
    }
    
    struct msg_list_st* msg_list = (struct msg_list_st*)buffer;
    printf("Channel ID: %u\n", msg_list->chnid);
    printf("Entry Count: %u\n", msg_list->entry_count);
    
    // 指向第一个条目的指针
    char* current_pos = (char*)buffer + sizeof(struct msg_list_st);
    char* end_pos = (char*)buffer + size;
    
    // 遍历并解析每个条目
    for (int i = 0; i < msg_list->entry_count && current_pos < end_pos; i++) {
        struct msg_listentry_st* entry = (struct msg_listentry_st*)current_pos;
        
        // 确保我们不会读取超出缓冲区
        if ((char*)&entry->desc[0] + entry->len > end_pos) {
            printf("Buffer overflow detected\n");
            return;
        }
        
        printf("Entry %d:\n", i + 1);
        printf("  Channel ID: %u\n", entry->chnid);
        printf("  Description Length: %u\n", entry->len);
        printf("  Description: %s\n", entry->desc);
        
        // 移动到下一个条目
        current_pos += sizeof(struct msg_listentry_st) + entry->len;
    }
}

// 创建数据频道消息
struct msg_channel_st* create_channel_msg(chnid_t id, const void* data, size_t data_len) {
    struct msg_channel_st* msg = malloc(sizeof(struct msg_channel_st) + data_len);
    if (!msg) return NULL;
    
    msg->chnid = id;
    memcpy(msg->data, data, data_len);
    
    return msg;
}

// 主函数，展示使用方法
int main() {
    // 演示创建频道列表
    chnid_t channel_ids[] = {1, 2, 3};
    const char* descriptions[] = {
        "Music Channel",
        "News Channel",
        "Sports Channel"
    };
    
    // 创建节目单
    void* msg_list = create_msg_list(channel_ids, descriptions, 3);
    if (!msg_list) {
        printf("Failed to create message list\n");
        return 1;
    }
    
    // 解析并打印节目单
    printf("=== Channel List Message ===\n");
    // 计算总大小（这里为了简化，实际应用中可能需要更精确的计算）
    size_t total_size = sizeof(struct msg_list_st);
    for (int i = 0; i < 3; i++) {
        total_size += sizeof(struct msg_listentry_st) + strlen(descriptions[i]) + 1;
    }
    parse_msg_list(msg_list, total_size);
    
    // 演示创建数据频道消息
    const char* sample_data = "This is sample channel data";
    struct msg_channel_st* channel_msg = create_channel_msg(1, sample_data, strlen(sample_data) + 1);
    
    if (channel_msg) {
        printf("\n=== Channel Data Message ===\n");
        printf("Channel ID: %u\n", channel_msg->chnid);
        printf("Data: %s\n", (char*)channel_msg->data);
        free(channel_msg);
    }
    
    // 释放资源
    free(msg_list);
    
    return 0;
}