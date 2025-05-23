#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "../include/proto.h"
#include "medialib.h"
#include "server_conf.h"
#include "thr_list.h"

static pthread_t tid_list; // 线程
static int num_list_entry;//频道总数
static struct mlib_listentry_st *list_entry; // 频道列表

static void *thr_list(void *p) {
  int totalsize;
  struct msg_list_st *entrylistptr; //节目单结构体
  struct msg_listentry_st *entryptr;//频道结构体
  int ret;
  int size;

  totalsize = sizeof(chnid_t); // 之后逐步累计节目单的大小
  for (int i = 0; i < num_list_entry; ++i) {
    totalsize += sizeof(struct msg_listentry_st) + strlen(list_entry[i].desc);
  }
  entrylistptr = malloc(totalsize);
  if (entrylistptr == NULL) {
    syslog(LOG_ERR, "malloc():%s", strerror(errno));
    exit(1);
  }
  entrylistptr->chnid = LISTCHNID; // 这是节目单频道号 0

  entryptr = entrylistptr->entry;//将节目单的频道结构体的地址赋给频道结构体指针

  syslog(LOG_DEBUG, "num_list_entry:%d\n", num_list_entry);
  
  for (int i = 0; i < num_list_entry; ++i) {
    size = sizeof(struct msg_listentry_st) + strlen(list_entry[i].desc);//size是一个频道的大小

    entryptr->chnid = list_entry[i].chnid;
    entryptr->len = htons(size);
    strcpy(entryptr->desc, list_entry[i].desc);
     // 在移动指针之前打印
    syslog(LOG_DEBUG, "entry[%d] len:%hu", i, ntohs(entryptr->len));
    entryptr = (void *)(((char *)entryptr) + size); // 向后移动entptr

  }

  while (1) {
    syslog(LOG_INFO, "thr_list sndaddr :%d\n", sndaddr.sin_addr.s_addr);//#include "server_conf.h"中声明了此处可以直接使用
    ret = sendto(serversd, entrylistptr, totalsize, 0, (void *)&sndaddr,
                 sizeof(sndaddr)); // 频道列表在广播网段每秒发送entrylist
    syslog(LOG_DEBUG, "sent content len:%d\n", entrylistptr->entry->len);
    if (ret < 0) {
      syslog(LOG_WARNING, "sendto(serversd, enlistp...:%s", strerror(errno));
    } else {
      syslog(LOG_DEBUG, "sendto(serversd, enlistp....):success");
    }
    sleep(1);
  }
}

// 创建节目单线程
int thr_list_create(struct mlib_listentry_st *listptr, int num_ent) {
  int err;
  list_entry = listptr;
  num_list_entry = num_ent;
  syslog(LOG_DEBUG, "list content: chnid:%d, desc:%s", listptr->chnid,
         listptr->desc);
  err = pthread_create(&tid_list, NULL, thr_list, NULL);
  if (err) {
    syslog(LOG_ERR, "pthread_create():%s", strerror(errno));
    return -1;
  }
  return 0;
}

// 销毁节目单线程
int thr_list_destroy(void) {
  pthread_cancel(tid_list);
  pthread_join(tid_list, NULL);
  return 0;
}
