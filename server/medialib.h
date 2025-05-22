#ifndef MEDIALIB_H_
#define MEDIALIB_H_

#include "../include/proto.h"
#include <stdio.h>

// 记录每一条节目单信息 频道号 描述信息
struct mlib_listentry_st{
  chnid_t chnid;
  char *desc;//当前模块的实现是本机下的，直到server端获取完频道信息才会发送因此这个还是本地在用
};

int mlib_getchnlist(struct mlib_listentry_st **mchnarr, int *index);
int mlib_freechnlist(struct mlib_listentry_st *mchn);
ssize_t mlib_readchn(chnid_t, void *, size_t);

#endif // MEDIALIB_H_
