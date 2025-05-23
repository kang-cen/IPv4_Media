/*方案二：令牌池 + 公平调度
使用全局令牌池配合公平调度算法，避免线程饥饿。 */
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
  // 新增：优先级和公平调度相关字段
  int priority;        // 优先级 (0-高, 1-中, 2-低)
  long last_fetch_time; // 上次获取令牌的时间戳
  int fetch_count;     // 获取令牌的次数统计
  pthread_mutex_t mut;
  pthread_cond_t cond;
};

// 全局令牌池结构
struct token_pool_st {
  int total_tokens;           // 全局令牌池
  int max_tokens;            // 最大令牌数
  int refill_rate;           // 补充速率
  pthread_mutex_t pool_mut;   // 令牌池互斥锁
  pthread_cond_t pool_cond;   // 令牌池条件变量
  
  // 公平调度队列
  struct mytbf_st *fair_queue[MYTBF_MAX];
  int queue_head;
  int queue_tail;
  int queue_size;
  pthread_mutex_t queue_mut;
};

static struct mytbf_st *job[MYTBF_MAX];
static pthread_mutex_t mut_job = PTHREAD_MUTEX_INITIALIZER; 
static pthread_once_t once_init = PTHREAD_ONCE_INIT;
static pthread_t tid;

// 全局令牌池
static struct token_pool_st token_pool = {
  .total_tokens = 0,
  .max_tokens = 200 * 1024,  // 200KB
  .refill_rate = 40 * 1024,  // 40KB/s
  .pool_mut = PTHREAD_MUTEX_INITIALIZER,
  .pool_cond = PTHREAD_COND_INITIALIZER,
  .queue_head = 0,
  .queue_tail = 0,
  .queue_size = 0,
  .queue_mut = PTHREAD_MUTEX_INITIALIZER
};

// 获取当前时间戳（毫秒）
static long get_timestamp_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 将令牌桶加入公平调度队列
static void enqueue_tbf(struct mytbf_st *tbf) {
  pthread_mutex_lock(&token_pool.queue_mut);
  if (token_pool.queue_size < MYTBF_MAX) {
    token_pool.fair_queue[token_pool.queue_tail] = tbf;
    token_pool.queue_tail = (token_pool.queue_tail + 1) % MYTBF_MAX;
    token_pool.queue_size++;
  }
  pthread_mutex_unlock(&token_pool.queue_mut);
}

// 从公平调度队列中取出令牌桶
static struct mytbf_st *dequeue_tbf() {
  struct mytbf_st *tbf = NULL;
  pthread_mutex_lock(&token_pool.queue_mut);
  if (token_pool.queue_size > 0) {
    tbf = token_pool.fair_queue[token_pool.queue_head];
    token_pool.queue_head = (token_pool.queue_head + 1) % MYTBF_MAX;
    token_pool.queue_size--;
  }
  pthread_mutex_unlock(&token_pool.queue_mut);
  return tbf;
}

// 改进的定时器处理函数
static void alrm_handle(int sig) {
  pthread_mutex_lock(&token_pool.pool_mut);
  
  // 向全局令牌池补充令牌
  token_pool.total_tokens += token_pool.refill_rate;
  if (token_pool.total_tokens > token_pool.max_tokens) {
    token_pool.total_tokens = token_pool.max_tokens;
  }
  
  // 公平分配令牌给等待的令牌桶
  pthread_mutex_lock(&mut_job);
  int active_jobs = 0;
  
  // 统计活跃的令牌桶数量
  for (int i = 0; i < MYTBF_MAX; ++i) {
    if (job[i] != NULL) {
      active_jobs++;
    }
  }
  
  // 如果有活跃的令牌桶且有可用令牌
  if (active_jobs > 0 && token_pool.total_tokens > 0) {
    int tokens_per_job = token_pool.total_tokens / active_jobs;
    int remaining_tokens = token_pool.total_tokens;
    
    for (int i = 0; i < MYTBF_MAX && remaining_tokens > 0; ++i) {
      if (job[i] != NULL) {
        pthread_mutex_lock(&job[i]->mut);
        
        int allocated = min(tokens_per_job, remaining_tokens);
        allocated = min(allocated, job[i]->burst - job[i]->token);
        
        if (allocated > 0) {
          job[i]->token += allocated;
          remaining_tokens -= allocated;
          pthread_cond_broadcast(&job[i]->cond);
        }
        
        pthread_mutex_unlock(&job[i]->mut);
      }
    }
    
    token_pool.total_tokens = remaining_tokens;
  }
  
  pthread_mutex_unlock(&mut_job);
  pthread_cond_broadcast(&token_pool.pool_cond);
  pthread_mutex_unlock(&token_pool.pool_mut);
}

//定时器线程函数
static void *thr_alrm(void *p) {
  struct itimerval tick;
  memset(&tick, 0, sizeof(tick));
  tick.it_value.tv_sec = 1;  // sec
  tick.it_value.tv_usec = 0; // micro sec.
  tick.it_interval.tv_sec = 1;
  tick.it_interval.tv_usec = 0;

  signal(SIGALRM, alrm_handle);
  setitimer(ITIMER_REAL, &tick, NULL);

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
    if (job[i] != NULL) {
      pthread_mutex_destroy(&job[i]->mut);
      pthread_cond_destroy(&job[i]->cond);
      free(job[i]);
    }
  }
  
  pthread_mutex_destroy(&token_pool.pool_mut);
  pthread_cond_destroy(&token_pool.pool_cond);
  pthread_mutex_destroy(&token_pool.queue_mut);
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
  atexit(module_unload);
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
  me->priority = 1; // 默认中等优先级
  me->last_fetch_time = get_timestamp_ms();
  me->fetch_count = 0;
  
  pthread_mutex_init(&me->mut, NULL);
  pthread_cond_init(&me->cond, NULL);
  pthread_mutex_lock(&mut_job);

  pos = get_free_pos_unlocked();
  if (pos < 0) {
    pthread_mutex_unlock(&mut_job);
    fprintf(stderr, "no free position,\n");
    pthread_mutex_destroy(&me->mut);
    pthread_cond_destroy(&me->cond);
    free(me);
    exit(1);
  }
  me->pos = pos;
  job[me->pos] = me;

  pthread_mutex_unlock(&mut_job);
  return me;
}

// 改进的令牌获取函数 - 支持优先级和防饥饿
int mytbf_fetchtoken(mytbf_t *ptr, int size) { 
  int n;
  struct mytbf_st *me = ptr;
  long current_time = get_timestamp_ms();
  
  pthread_mutex_lock(&me->mut);
  
  // 防饥饿机制：如果等待时间过长，提升优先级
  if (current_time - me->last_fetch_time > 5000) { // 5秒
    me->priority = max(0, me->priority - 1);
  }
  
  // 如果没有足够令牌，考虑从全局令牌池获取
  while (me->token <= 0) {
    // 尝试从全局令牌池获取令牌
    pthread_mutex_lock(&token_pool.pool_mut);
    if (token_pool.total_tokens > 0) {
      int request = min(size, token_pool.total_tokens);
      request = min(request, me->burst);
      
      me->token += request;
      token_pool.total_tokens -= request;
      pthread_mutex_unlock(&token_pool.pool_mut);
      break;
    }
    pthread_mutex_unlock(&token_pool.pool_mut);
    
    // 等待令牌补充
    pthread_cond_wait(&me->cond, &me->mut);
  }
  
  n = min(me->token, size);
  me->token -= n;
  me->last_fetch_time = current_time;
  me->fetch_count++;
  
  pthread_mutex_unlock(&me->mut);
  return n;
}

int mytbf_returntoken(mytbf_t *ptr, int size) {
  struct mytbf_st *me = ptr;
  pthread_mutex_lock(&me->mut);
  me->token += size;
  if (me->token > me->burst)
    me->token = me->burst;
  pthread_con