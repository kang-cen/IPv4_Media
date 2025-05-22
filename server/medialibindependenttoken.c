/*方案一：每频道独立令牌桶（推荐）
为每个频道分配独立的令牌桶，确保公平的带宽分配。 */
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "../include/proto.h"
#include "medialib.h"
#include "mytbf.h"
#include "server_conf.h"

//#define DEBUG

#define PATHSIZE 1024
#define LINEBUFSIZE 1024
#define MP3_BITRATE 320 * 1024 // 比特率（Bitrate）320 kbps 是 MP3 的最高标准比特率

struct channel_context_st {
  chnid_t chnid;
  char *desc;
  glob_t mp3glob; // 目录项  将所有匹配的文件路径存储在 me->mp3glob.gl_pathv 数组
  int pos;        // current song // 当前播放的文件在文件列表中的位置
  int fd;         // current song fd
  off_t offset;
  mytbf_t *tbf; // 每个频道独立的流控器
};

static struct channel_context_st channel[MAXCHNID + 1]; // 全部的频道 保存系统中所有频道的完整上下文信息
static int total_channels = 0; // 记录总频道数

// 将某个目录下的所有文件转为一个频道 
static struct channel_context_st *path2entry(const char *path) {
  syslog(LOG_INFO, "current path: %s\n", path);
  char pathstr[PATHSIZE] = {'\0'};//存储频道描述文件
  char linebuf[LINEBUFSIZE];
  FILE *fp;
  struct channel_context_st *me;

  static chnid_t curr_id =
      MINCHNID; // 由于是一个静态变量所以相当于一直在操作同一块内存 有叠加效果

  // 检测目录合法性
  strcat(pathstr, path);
  strcat(pathstr, "/desc.txt");
  fp = fopen(pathstr, "r"); // 打开频道描述文件
  syslog(LOG_INFO, "channel dir:%s\n", pathstr);
  if (fp == NULL) {
    syslog(LOG_INFO, "%s is not a channel dir (can not find desc.txt)", path);
    return NULL;
  }
  if (fgets(linebuf, LINEBUFSIZE, fp) == NULL) {
    syslog(LOG_INFO, "%s is not a channel dir(cant get the desc.text)", path);
    fclose(fp);
    return NULL;
  }
  fclose(fp); // 关闭频道描述文件

  // 初始化上下文
  me = malloc(sizeof(*me));
  if (me == NULL) {
    syslog(LOG_ERR, "malloc():%s", strerror(errno));
    return NULL;
  }

  // 改进：每个频道分配独立的令牌桶
  // 每个频道的速率设为总带宽除以预期频道数，这里假设最多支持8个频道
  int per_channel_rate = (MP3_BITRATE / 8) / 8; // 5KB/s per channel
  int per_channel_burst = per_channel_rate * 5;  // 25KB burst per channel
  
  me->tbf = mytbf_init(per_channel_rate, per_channel_burst);
  if (me->tbf == NULL) {
    syslog(LOG_ERR, "mytbf_init():%s", strerror(errno));
    free(me);
    return NULL;
  }

  // 初始化频道
  me->desc = strdup(linebuf);
  strncpy(pathstr, path, PATHSIZE);
  strncat(pathstr, "/*.mp3", PATHSIZE-1);
  if (glob(pathstr, 0, NULL, &me->mp3glob) != 0) {
    curr_id++;
    syslog(LOG_ERR, "%s is not a channel dir(can not find mp3 files", path);
    mytbf_destroy(me->tbf);
    free(me);
    return NULL;
  }
  me->pos = 0;//跟踪当前正在播放的文件在文件列表（mp3glob.gl_pathv）中的索引位置
  me->offset = 0;//跟踪当前文件内的读取位置（以字节为单位
  me->fd = open(me->mp3glob.gl_pathv[me->pos], O_RDONLY); // 打开第一个音乐文件
  if (me->fd < 0) {
    syslog(LOG_WARNING, "%s open failed.", me->mp3glob.gl_pathv[me->pos]);
    mytbf_destroy(me->tbf);
    free(me);
    return NULL;
  }
  me->chnid = curr_id;
  curr_id++;
  return me;
}

//扫描媒体目录，获取所有可用频道的列表  回填调用的函数中的参数
int mlib_getchnlist(struct mlib_listentry_st **result, int *resnum) {
  int num = 0;
  int i = 0;
  char path[PATHSIZE];
  glob_t globres;
  struct mlib_listentry_st *ptr;//给其他函数看的
  struct channel_context_st *res;//path2entry函数临时保存处

  for (int i = 0; i <= MAXCHNID; ++i) {
    channel[i].chnid = -1;//当前未启用
  }

  snprintf(path, PATHSIZE, "%s/*", server_conf.media_dir);
#ifdef DEBUG
  printf("current path:%s\n", path);
#endif
  if (glob(path, 0, NULL, &globres)) { // 成功返回0
    return -1;
  }
#ifdef DEBUG
  printf("globres.gl_pathv[0]:%s\n", globres.gl_pathv[0]);
  printf("globres.gl_pathv[1]:%s\n", globres.gl_pathv[1]);
  printf("globres.gl_pathv[2]:%s\n", globres.gl_pathv[2]);
#endif
  ptr = malloc(sizeof(struct mlib_listentry_st) * globres.gl_pathc);//函数最终要返回给调用者的频道列表，包含给客户端使用的频道信息
  if (ptr == NULL) {
    syslog(LOG_ERR, "malloc() error.");
    exit(1);
  }
  for (i = 0; i < globres.gl_pathc; ++i) {
    //globres.gl_path[v]->"var/media/ch1"
    res = path2entry(globres.gl_pathv[i]);//调用path2entry检查其是否为有效频道 res用于临时存储由 path2entry() 函数返回的频道上下文信息
    if (res != NULL) {
      syslog(LOG_INFO, "path2entry() return : %d %s.", res->chnid, res->desc);
      memcpy(channel + res->chnid, res, sizeof(*res)); //channel + res->chnid === &channel[res->chnid]
      ptr[num].chnid = res->chnid;
      ptr[num].desc = strdup(res->desc);//数创建一个字符串的副本
      num++;
    }
  }
  
  total_channels = num; // 记录总频道数
  syslog(LOG_INFO, "Total channels created: %d", total_channels);
  
  /*
  调整分配给 ptr 的内存大小，使其恰好容纳实际找到的有效频道数量
  ptr其实是一个结构体指针，但是指向一块连续的地址空间长度为globres.gl_pathc
  所以可以将指针操作看成数组，就是一个结构体数组
  */
  *result = realloc(ptr, sizeof(struct mlib_listentry_st) * num);
  if (*result == NULL) {
    syslog(LOG_ERR, "realloc() failed.");
  }
  *resnum = num;
  return 0;
}

int mlib_freechnlist(struct mlib_listentry_st *ptr) {
  free(ptr);
  return 0;
}

static int open_next(chnid_t chnid) {
  for (int i = 0; i < channel[chnid].mp3glob.gl_pathc; ++i) {
    channel[chnid].pos++; // 更新偏移
    if (channel[chnid].pos == channel[chnid].mp3glob.gl_pathc) {
      syslog(LOG_DEBUG, "Channel %d: playlist loop", chnid);
      channel[chnid].pos = 0;
    }
    close(channel[chnid].fd);

    // 尝试打开新文件
    channel[chnid].fd =
        open(channel[chnid].mp3glob.gl_pathv[channel[chnid].pos], O_RDONLY);
    if (channel[chnid].fd < 0) {
      syslog(LOG_WARNING, "open(%s):%s", channel[chnid].mp3glob.gl_pathv[chnid],
             strerror(errno));
    } else {
      syslog(LOG_DEBUG, "Channel %d: opened new file", chnid);
      channel[chnid].offset = 0;
      return 0;
    } 
  }
  syslog(LOG_ERR, "None of mp3 in channel %d id available.", chnid);
  return -1;
}

//从指定频道(chnid)读取数据到缓冲区(buf)，最大读取size字节。 返回实际读取的字节数，或发生错误时的负值。
ssize_t mlib_readchn(chnid_t chnid, void *buf, size_t size) {
  int tbfsize;
  int len;
  int next_ret = 0;
  
  // 使用该频道独立的令牌桶获取令牌
  tbfsize = mytbf_fetchtoken(channel[chnid].tbf, size);
  syslog(LOG_DEBUG, "Channel %d: got %d tokens, remaining: %d", 
         chnid, tbfsize, mytbf_checktoken(channel[chnid].tbf));

  while (1) 
  {
    len = pread(channel[chnid].fd, buf, tbfsize, channel[chnid].offset); // 读取tbfsize数据到从offset处开始的buf
    /*current song open failed*/
    if (len < 0) {
      // 当前这首歌可能有问题，错误不至于退出，读取下一首歌
      syslog(LOG_WARNING, "Channel %d: media file %s pread():%s", chnid,
             channel[chnid].mp3glob.gl_pathv[channel[chnid].pos],
             strerror(errno));
      open_next(chnid);
    } 
    else if (len == 0) {//处理文件结束
      syslog(LOG_DEBUG, "Channel %d: media %s file is over", chnid,
             channel[chnid].mp3glob.gl_pathv[channel[chnid].pos]);
      #ifdef DEBUG
            printf("current chnid :%d\n", chnid);
      #endif
      next_ret = open_next(chnid);
      break;
    } 
    else /*len > 0*/ //真正读取到了数据
    {
      channel[chnid].offset += len;
      struct stat buf_stat;
      fstat(channel[chnid].fd, &buf_stat);
      syslog(LOG_DEBUG, "Channel %d: progress: %f%%", chnid,
             (channel[chnid].offset) / (1.0*buf_stat.st_size)*100);//计算并记录当前播放进度百分比
      break;
    }
  }
  
  // 返还未使用的令牌
  if (tbfsize - len > 0)
    mytbf_returntoken(channel[chnid].tbf, tbfsize - len);
    
  syslog(LOG_DEBUG, "Channel %d: sent %d bytes", chnid, len);
  return len; //返回读取到的长度
}

// 新增：获取频道统计信息的函数
void mlib_get_channel_stats(chnid_t chnid) {
  if (chnid < 0 || chnid > MAXCHNID || channel[chnid].chnid == -1) {
    syslog(LOG_WARNING, "Invalid channel ID: %d", chnid);
    return;
  }
  
  int tokens = mytbf_checktoken(channel[chnid].tbf);
  syslog(LOG_INFO, "Channel %d stats: tokens=%d, pos=%d, offset=%ld", 
         chnid, tokens, channel[chnid].pos, channel[chnid].offset);
}

// 新增：清理资源的函数
void mlib_cleanup() {
  for (int i = 0; i <= MAXCHNID; i++) {
    if (channel[i].chnid != -1) {
      if (channel[i].fd >= 0) {
        close(channel[i].fd);
      }
      if (channel[i].tbf) {
        mytbf_destroy(channel[i].tbf);
      }
      if (channel[i].desc) {
        free(channel[i].desc);
      }
      globfree(&channel[i].mp3glob);
    }
  }
  syslog(LOG_INFO, "Media library cleanup completed");
}