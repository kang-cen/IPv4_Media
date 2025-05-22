#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>

#include "mytbf.h"

static int min(int a, int b) { return a < b ? a : b; }

struct mytbf_st {
  int cps; // c per second
  int burst;
  int token;
  int pos;
  /*
  为什么需要：每个令牌桶的token值可能被多个线程访问：
  用户线程(获取/返还令牌)和定时器线程(添加令牌)。不加锁会导致令牌计数不准确。
  */
  pthread_mutex_t mut;
  pthread_cond_t cond;
};

static struct mytbf_st *job[MYTBF_MAX];
/*为什么需要：因为job[]是全局共享资源，可能被多线程同时访问，
如主线程和定时器线程。不加锁保护会导致race condition，可能出现数据竞争。 */
static pthread_mutex_t mut_job = PTHREAD_MUTEX_INITIALIZER; 

static pthread_once_t once_init = PTHREAD_ONCE_INIT;//指定的初始化函数只被执行一次，即使有多个线程同时调用
static pthread_t tid;

static void alrm_handle(int sig) {
  pthread_mutex_lock(&mut_job);
    for (int i = 0; i < MYTBF_MAX; ++i) {
      if (job[i] != NULL) {
        pthread_mutex_lock(&job[i]->mut);
        job[i]->token += job[i]->cps;
        if (job[i]->token > job[i]->burst) {
          job[i]->token = job[i]->burst;
        }
        pthread_cond_broadcast(&job[i]->cond); // 惊群
        pthread_mutex_unlock(&job[i]->mut);
      }
    }
    pthread_mutex_unlock(&mut_job);
}

//这是后台定时器线程的主函数 启动一个每秒触发一次 SIGALRM 信号的定时器，首次触发在 1 秒后。
static void *thr_alrm(void *p) {
  struct itimerval tick;
  memset(&tick, 0, sizeof(tick));
  tick.it_value.tv_sec = 1;  // sec
  tick.it_value.tv_usec = 0; // micro sec.
  tick.it_interval.tv_sec = 1;
  tick.it_interval.tv_usec = 0;

  signal(SIGALRM, alrm_handle);
  setitimer(ITIMER_REAL, &tick, NULL);//启动实时定时器。 ITIMER_REAL：基于真实时间（系统时钟），到期时发送 SIGALRM 信号。

  while (1) {
    pause();
  }
}

// 模块卸载函数
static void module_unload() {
  int i;
  pthread_cancel(tid);
  pthread_join(tid, NULL);

  for (int i = 0; i < MYTBF_MAX; i++) {
    free(job[i]);
  }
  return;
}

// 模块加载函数
static void module_load() {
  int err;
  err = pthread_create(&tid, NULL, thr_alrm, NULL);
  if (err) {
    fprintf(stderr, "pthread_create():%s", strerror(errno));
    exit(1);
  }
  atexit(module_unload);//是C标准库函数，用于注册一个函数，该函数会在程序正常终止时被调用
}

static int get_free_pos_unlocked() {
  for (int i = 0; i < MYTBF_MAX; ++i) {
    if (job[i] == NULL) {
      return i;
    }
  }
  return -1;
}

// 初始化一个令牌桶
mytbf_t *mytbf_init(int cps, int burst) {
  struct mytbf_st *me;

  module_load();                         // 开启定时token派发
  pthread_once(&once_init, module_load); // 限定只开启一次

  int pos;
  // 初始化mytbf
  me = malloc(sizeof(*me));
  if (me == NULL) {
    return NULL;
  }
  me->cps = cps;
  me->burst = burst;
  me->token = 0;
  pthread_mutex_init(&me->mut, NULL); // 初始化该令牌桶的mutex
  pthread_cond_init(&me->cond, NULL); // 初始化该令牌桶的conditional variable
  pthread_mutex_lock(&mut_job);

  pos = get_free_pos_unlocked();
  if (pos < 0) {
    pthread_mutex_unlock(&mut_job);
    fprintf(stderr, "no free position,\n");
    free(me);
    exit(1);
  }
  me->pos = pos;
  job[me->pos] = me; // 分配槽位

  pthread_mutex_unlock(&mut_job);
  return me;
}

int mytbf_fetchtoken(mytbf_t *ptr, int size) { 
  int n;
  struct mytbf_st *me = ptr;
  pthread_mutex_lock(&me->mut); //什么时候别人会和你一样在访问token
  while (me->token <= 0)
    pthread_cond_wait(&me->cond, &me->mut); // 没有令牌的时候 等待信号量通知
  n = min(me->token, size);
  me->token -= n; 
  pthread_cond_broadcast(&me->cond);
  pthread_mutex_unlock(&me->mut);
  return n;
}

int mytbf_returntoken(mytbf_t *ptr, int size) {
  struct mytbf_st *me = ptr;
  pthread_mutex_lock(&me->mut);
  me->token += size;
  if (me->token > me->burst)
    me->token = me->burst;
  pthread_mutex_unlock(&me->mut);
  return 0;
}

int mytbf_destroy(mytbf_t *ptr) {
  struct mytbf_st *me = ptr;
  pthread_mutex_lock(&mut_job);
  job[me->pos] = NULL;
  pthread_mutex_unlock(&mut_job);

  pthread_mutex_destroy(&me->mut);
  pthread_cond_destroy(&me->cond);
  free(ptr);
  return 0;
}

int mytbf_checktoken(mytbf_t *ptr) {
  int token_left = 0;
  struct mytbf_st *me = ptr;
  pthread_mutex_lock(&me->mut);
  token_left = me->token;
  pthread_mutex_unlock(&me->mut);
  return token_left;
}
