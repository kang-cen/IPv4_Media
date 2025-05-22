#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM 3

typedef int chnid_t;

typedef struct {
    chnid_t chnid;
    char *desc;
} mlib_listentry_st;

int main() {
    mlib_listentry_st *mediast = NULL;
    mediast = malloc(NUM * sizeof(mlib_listentry_st));
    if (mediast == NULL) {
        perror("malloc failed");
        return 1;
    }

    for (int i = 0; i < NUM; i++) {
        mediast[i].chnid = i + 1;

        // 创建描述字符串
        char tmp[100];
        /*int snprintf(char *str, size_t size, const char *format, ...);*/
        snprintf(tmp, sizeof(tmp), "CCVT%d", i + 1);  // 比如 CCVT1, CCVT2...

        // 分配内存并复制
        mediast[i].desc = malloc(strlen(tmp) + 1);  // +1 是为了 \0
        if (mediast[i].desc == NULL) {
            perror("malloc for desc failed");
            return 1;
        }
        /*strcpy() 的目标地址（desc）必须指向一个 合法、可写、已分配的内存块，否则会崩溃。malloc 就是给它这块空间的方式。*/
        strcpy(mediast[i].desc, tmp);
    }

    // 打印验证
    for (int i = 0; i < NUM; i++) {
        printf("mediast[%d]   chnid: %d   desc: %s\n", i, mediast[i].chnid, mediast[i].desc);
    }

    // 释放内存
    for (int i = 0; i < NUM; i++) {
        free(mediast[i].desc);//先释放里面的在释放外面的
    }
    free(mediast);
    mediast = NULL;

    return 0;
}
