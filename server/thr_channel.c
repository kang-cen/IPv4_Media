#include <asm-generic/errno-base.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "thr_channel.h"
#include "medialib.h"
#include "server_conf.h"
#include "../include/proto.h"
#include "feedback_pro.h"
#include <time.h>

static int tid_nextpos = 0;

// 每一个线程负责一个频道 频道号 处理该频道的线程
struct thr_channel_entry_st {
  chnid_t chnid;
  pthread_t tid;
};

struct thr_channel_entry_st thr_channel[CHANNUM];


static void *thr_channel_snder(void *ptr)
{
  uint32_t sequence_number = 0;
  struct msg_channel_st *sbufp;
  int len;
  struct mlib_listentry_st *entry = ptr;
  struct timespec sleep_time;
  long sleep_ns;
  const int BASE_BITRATE = 224 * 1000; // 
  
  sbufp = malloc(MSG_CHANNEL_MAX);
  if (sbufp == NULL) 
  {
    syslog(LOG_ERR, "malloc():%s", strerror(errno));
    exit(1);
  }
  sbufp->chnid = entry->chnid;
  
  syslog(LOG_INFO, "Channel %d sender thread started", entry->chnid);
  
  while(1) 
  {
    sbufp->seq = sequence_number;
    
    // 读取数据 - 使用令牌桶控制
    len = mlib_readchn(entry->chnid, sbufp->data, BASE_BITRATE/8); // 28KB
    if (len < 0) 
    {
      syslog(LOG_ERR, "Channel %d read failed", entry->chnid);
      break;
    }
    if (len == 0) {
      // 文件结束，继续循环等待下一个文件
      continue;
    }
    
    // 发送数据包
    if (sendto(serversd, sbufp, len + sizeof(chnid_t) + sizeof(uint32_t), 0, 
               (void*)&sndaddr, sizeof(sndaddr)) < 0) {
      syslog(LOG_ERR, "Channel %d sendto():%s", entry->chnid, strerror(errno));
      break;
    }
    
    // 记录发送日志
    syslog(LOG_DEBUG, "Channel %d: Sent packet seq=%u, len=%d", 
           entry->chnid, sequence_number, len);
    
    sequence_number++;
    
    // 根据反馈动态调整发送速率
    sleep_ns = get_channel_sleep_ns(entry->chnid, BASE_BITRATE);
    
    // 转换为 timespec 结构
    sleep_time.tv_sec = sleep_ns / 1000000000L;
    sleep_time.tv_nsec = sleep_ns % 1000000000L;
    
    // 精确睡眠控制发送速率
    if (nanosleep(&sleep_time, NULL) < 0) {
      if (errno != EINTR) {
        syslog(LOG_WARNING, "Channel %d nanosleep():%s", entry->chnid, strerror(errno));
      }
    }
    
    // 记录当前速率倍数（调试用）
    double rate_multiplier = get_channel_rate_multiplier(entry->chnid);
    if (sequence_number % 100 == 0) {  // 每100个包记录一次
      syslog(LOG_DEBUG, "Channel %d: rate_multiplier=%.2f, sleep_ns=%ld", 
             entry->chnid, rate_multiplier, sleep_ns);
    }
  }
  
  free(sbufp);
  pthread_exit(NULL);
}

// 创建对应的频道线程
int thr_channel_create(struct mlib_listentry_st *ptr) {
  int err;
  err = pthread_create(&thr_channel[tid_nextpos].tid, NULL, thr_channel_snder, ptr);
  if (err) {
    syslog(LOG_WARNING, "pthread_create():%s", strerror(err));
    return -err;
  }
  thr_channel[tid_nextpos].chnid = ptr->chnid; //填写频道信息
  tid_nextpos++;
  return 0;
}

// 销毁对应的频道线程
int thr_channel_destroy(struct mlib_listentry_st *ptr) {
  for (int i = 0;i < CHANNUM;++i) {
    if (thr_channel[i].chnid == ptr->chnid) {
      if (pthread_cancel(thr_channel[i].tid) < 0) {
        syslog(LOG_ERR, "pthread_cancel():thr thread of channel%d", ptr->chnid);
        return -ESRCH;
      }
      pthread_join(thr_channel[i].tid, NULL);
      thr_channel[i].chnid = -1;
      break;
    }
  }
  return 0;
}

// 销毁所有的频道线程
int thr_channel_destroyall(void) {
  for (int i = 0; i < CHANNUM; i++) {
    if (thr_channel[i].chnid > 0) {
      if (pthread_cancel(thr_channel[i].tid) < 0) {
        syslog(LOG_ERR, "pthread_cancel():thr thread of channel:%ld",
               thr_channel[i].tid);
        return -ESRCH;
      }
      pthread_join(thr_channel[i].tid, NULL);
      thr_channel[i].chnid = -1;
    }
  }
  return 0;
}
