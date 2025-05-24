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
// #include "reliablesender.h"
static int tid_nextpos = 0;

// 每一个线程负责一个频道 频道号 处理该频道的线程
struct thr_channel_entry_st {
  chnid_t chnid;
  pthread_t tid;
};

struct thr_channel_entry_st thr_channel[CHANNUM];

static void *thr_channel_snder(void *ptr)
{
  uint32_t sequence_number = 0; // 静态变量，保持递增
  struct msg_channel_st *sbufp;
  int len;
  struct mlib_listentry_st *entry = ptr;//void *-> struct mlib_listentry_st *
  sbufp = malloc(MSG_CHANNEL_MAX);
  if (sbufp == NULL) 
  {
    syslog(LOG_ERR, "malloc():%s", strerror(errno));
    exit(1);
  }
  sbufp->chnid = entry->chnid; // 频道号处理
  // 频道内容读取
  while(1) 
  {
    sbufp->seq=sequence_number;
    syslog(LOG_INFO, "开始");
    len = mlib_readchn(entry->chnid, sbufp->data, 320*1024/8); // 320kbit/s
    syslog(LOG_DEBUG, "读取的字节数: %d bytes", len);
    if (len < 0) 
    {
      break;
    }
    if (sendto(serversd, sbufp, len + sizeof(chnid_t)+sizeof(uint32_t), 0, (void*)&sndaddr, sizeof(sndaddr)) < 0) {
      syslog(LOG_ERR, "thr_channel(%d):sendto():%s", entry->chnid,
             strerror(errno));
      break;
    }
    // 记录日志
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    syslog(LOG_INFO, "current channel :%d:[%s] Sent packet with sequence: %u, length: %zu\n",entry->chnid,time_str, sequence_number, 
     sizeof(uint32_t) + sizeof(chnid_t) + len);

    sequence_number++;
    sched_yield();//出让调度器
    
    // 替换你的发送部分
    // struct channel_sender sender;
    // sender_init(&sender, DEFAULT_MGROUP, DEFAULT_RCVPORT, entry->chnid, 320); // 320kbps
    // // 在发送循环中
    // if (send_audio_packet(&sender, sbufp, len) < 0) 
    // {
    //     syslog(LOG_ERR, "Failed to send packet for channel %d", entry->chnid);
    //     break;
    // }
    // 移除 sched_yield() - 速率控制已内置
  }
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
