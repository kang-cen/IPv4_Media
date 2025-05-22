#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 频道ID类型定义
typedef int chnid_t;

// 频道信息结构体
struct mlib_listentry_st {
    chnid_t chnid;
    char *desc;
};

// 模拟获取频道列表的函数
int mlib_getchnlisttest(struct mlib_listentry_st **mchnarr, int *index) {
    // 模拟频道数据
    const int channel_count = 5;
    const char* channel_names[] = {
        "CCTV1综合",
        "CCTV2财经", 
        "CCTV3综艺",
        "CCTV4中文国际",
        "CCTV5体育"
    };
    
    // 1. 分配结构体数组内存（注意：是结构体数组，不是指针数组）这个指针存储的是一整块连续的内存空间
    struct mlib_listentry_st *temp_array = 
        (struct mlib_listentry_st*)malloc(channel_count * sizeof(struct mlib_listentry_st));
    
    if (temp_array == NULL) {
        *index = 0;
        return -1; // 内存分配失败
    }
    
    // 2. 填充每个结构体的内容
    for (int i = 0; i < channel_count; i++) {
        temp_array[i].chnid = i + 1;
        
        // 为描述信息分配内存并复制字符串
        temp_array[i].desc = (char*)malloc(strlen(channel_names[i]) + 1);
        if (temp_array[i].desc != NULL) {
            strcpy(temp_array[i].desc, channel_names[i]);
        }
    }
    
    // 3. 通过二级指针返回数组地址，修改调用者的指针变量
    *mchnarr = temp_array;  // 让调用者的list指向新分配的数组
    *index = channel_count; // 返回频道数量
    
    printf("函数内部: 分配了 %d 个结构体的数组，地址为 %p\n", 
           channel_count, (void*)temp_array);
    
    return 0; // 成功
}

// 释放频道列表内存的函数
void mlib_free_chnlist(struct mlib_listentry_st *list, int count) {
    if (list == NULL) return;
    
    // 先释放每个描述字符串的内存
    for (int i = 0; i < count; i++) {
        if (list[i].desc != NULL) {
            free(list[i].desc);
        }
    }
    
    // 再释放结构体数组内存
    free(list);
}

// 打印频道列表
void print_channel_list(struct mlib_listentry_st *list, int count) {
    printf("\n=== 频道列表 ===\n");
    for (int i = 0; i < count; i++) {
        printf("频道%d: %s (地址: %p)\n", 
               list[i].chnid, 
               list[i].desc,
               (void*)&list[i]);
    }
    printf("===============\n\n");
}

int main() {
    // 1. 声明指针变量（初始为NULL或未定义）
    static struct mlib_listentry_st *list = NULL;
    int list_size = 0;
    
    printf("调用前: list = %p\n", (void*)list);
    
    // 2. 调用函数获取频道信息
    // 传递list指针变量的地址(&list)和list_size的地址
    int err = mlib_getchnlist(&list, &list_size);
    
    if (err != 0) {
        printf("获取频道列表失败！\n");
        return -1;
    }
    
    printf("调用后: list = %p, 频道数量 = %d\n", (void*)list, list_size);
    
    // 3. 使用返回的频道列表
    print_channel_list(list, list_size);
    
    // 4. 演示数组访问
    printf("=== 数组访问演示 ===\n");
    printf("list[0] 的地址: %p\n", (void*)&list[0]);
    printf("list[1] 的地址: %p\n", (void*)&list[1]);
    printf("结构体大小: %zu 字节\n", sizeof(struct mlib_listentry_st));
    printf("地址差: %ld 字节\n", (char*)&list[1] - (char*)&list[0]);
    
    // 5. 内存分析
    printf("\n=== 内存布局分析 ===\n");
    printf("list指针变量地址: %p\n", (void*)&list);
    printf("list指向的数组地址: %p\n", (void*)list);
    printf("第一个结构体地址: %p\n", (void*)&list[0]);
    printf("这证明list指向的是连续的结构体数组，而不是指针数组\n");
    
    // 6. 清理内存
    mlib_free_chnlist(list, list_size);
    list = NULL; // 避免悬挂指针
    
    printf("\n内存已释放，程序结束。\n");
    
    return 0;
}