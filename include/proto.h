#ifndef PROTO_H_
#define PROTO_H_

#include <stdint.h>

#define DEFAULT_MGROUP "224.2.2.2" // default multicast group 多播组
#define DEFAULT_RCVPORT "1989"  //端口号  
#define CHANNUM 200 // channel number  频道数量---观看的节目种类

#define LISTCHNID 0 // list channel	默认频道0是节目列表
#define MINCHNID 1 // minimum channel id    
#define MAXCHNID (MINCHNID + CHANNUM - 1) // maximum channel id

#define MSG_CHANNEL_MAX ((1<<16)-20-8) // 20:IP package head, 8:udp package head  udp包的最大长度    
#define MAX_DATA (MSG_CHANNEL_MAX - sizeof(chnid_t))   //最大data包的大小

#define MSG_LIST_MAX ((1<<16)-20-8)
#define MAX_ENTRY (MSG_CHANNEL_MAX - sizeof(chnid_t)) //节目单包的最大大小

#include "site_type.h"
// 每一个频道内容结构体： 频道号，data(指针)
struct msg_channel_st
{
  chnid_t chnid; // must between MINCHNID MAXCHNID  频道号
  uint8_t data[1];
}__attribute__((packed)); // do not align

// 每一条节目项包含的信息：chnid len desc
struct msg_listentry_st
{
  chnid_t chnid;
  uint16_t len;
  char desc[1]; // 频道的描述信息
  //uint8_t desc[1]; // 频道的描述信息
}__attribute__((packed)); // do not align

// 节目单频道内容 chnid len desc
struct msg_list_st
{
  chnid_t chnid; // must be LISTCHNID 0
  struct msg_listentry_st entry[1];
}__attribute__((packed)); // do not align

#endif // PROTO_H_
