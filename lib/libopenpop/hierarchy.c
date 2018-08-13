/*
 * Provides hierarchy abstractions for threads executing in Popcorn Linux.
 *
 * Copyright Rob Lyerly, SSRG, VT, 2018
 */

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <float.h>
#include "hierarchy.h"

global_info_t ALIGN_PAGE popcorn_global;
node_info_t ALIGN_PAGE popcorn_node[MAX_POPCORN_NODES];

///////////////////////////////////////////////////////////////////////////////
// Global information getters/setters
///////////////////////////////////////////////////////////////////////////////

bool popcorn_distributed() { return popcorn_global.distributed; }
bool popcorn_finished() { return popcorn_global.finished; }
bool popcorn_hybrid_barrier() { return popcorn_global.hybrid_barrier; }
bool popcorn_hybrid_reduce() { return popcorn_global.hybrid_reduce; }
bool popcorn_het_workshare() { return popcorn_global.het_workshare; }

unsigned long omp_popcorn_threads()
{
  int i;
  unsigned long num = 0;
  for(i = 0; i < MAX_POPCORN_NODES; i++)
    num += popcorn_global.threads_per_node[i];
  return num;
}

unsigned long omp_popcorn_threads_per_node(int nid)
{
  if(nid >= 0 && nid < MAX_POPCORN_NODES)
    return popcorn_global.threads_per_node[nid];
  else return UINT64_MAX;
}

unsigned long omp_popcorn_core_speed(int nid)
{
  if(nid >= 0 && nid < MAX_POPCORN_NODES)
    return popcorn_global.core_speed_rating[nid];
  else return UINT64_MAX;
}

void popcorn_set_distributed(bool flag) { popcorn_global.distributed = flag; }
void popcorn_set_finished(bool flag) { popcorn_global.finished = flag; }
void popcorn_set_hybrid_barrier(bool flag) { popcorn_global.hybrid_barrier = flag; }
void popcorn_set_hybrid_reduce(bool flag) { popcorn_global.hybrid_reduce = flag; }
void popcorn_set_het_workshare(bool flag) { popcorn_global.het_workshare = flag; }

///////////////////////////////////////////////////////////////////////////////
// Leader selection
///////////////////////////////////////////////////////////////////////////////

void hierarchy_init_global(int nodes)
{
  popcorn_global.sync.remaining = popcorn_global.sync.num = nodes;
  popcorn_global.opt.remaining = popcorn_global.opt.num = nodes;
  /* Note: *must* use reinit_all, otherwise there's a race condition between
     leaders who have been released are reading generation in the barrier's
     do-while loop and the main thread resetting barrier's generation */
  gomp_barrier_reinit_all(&popcorn_global.bar, nodes);
}

void hierarchy_init_node(int nid)
{
  size_t num;

  assert(popcorn_node[nid].sync.num == popcorn_node[nid].opt.num &&
         "Corrupt node configuration");

  num = popcorn_node[nid].sync.num;
  popcorn_node[nid].sync.remaining = popcorn_node[nid].opt.remaining = num;
  /* See note in hierarchy_init_global() above */
  gomp_barrier_reinit_all(&popcorn_node[nid].bar, num);
}

int hierarchy_assign_node(unsigned tnum)
{
  unsigned cur = 0, thr_total = 0;
  for(cur = 0; cur < MAX_POPCORN_NODES; cur++)
  {
    thr_total += popcorn_global.threads_per_node[cur];
    if(tnum < thr_total)
    {
      popcorn_node[cur].sync.num++;
      popcorn_node[cur].opt.num++;
      return cur;
    }
  }

  /* If we've exhausted the specification default to origin */
  popcorn_node[0].sync.num++;
  popcorn_node[0].opt.num++;
  return 0;
}

/****************************** Internal APIs *******************************/

static bool select_leader_optimistic(leader_select_t *l, size_t *ticket)
{
  size_t rem = __atomic_fetch_add(&l->remaining, -1, MEMMODEL_ACQ_REL);
  if(ticket) *ticket = rem - 1;
  return rem == l->num;
}

static bool select_leader_synchronous(leader_select_t *l,
                                      gomp_barrier_t *bar,
                                      bool final,
                                      size_t *ticket)
{
  unsigned awaited;
  size_t rem = __atomic_add_fetch(&l->remaining, -1, MEMMODEL_ACQ_REL);
  if(ticket) *ticket = rem;
  if(rem) return false;
  else
  {
    /* Wait for non-leader threads to enter barrier */
    if(final)
    {
      do awaited = __atomic_load_n(&bar->awaited_final, MEMMODEL_ACQUIRE);
      while(awaited != 1);
    }
    else
    {
      do awaited = __atomic_load_n(&bar->awaited, MEMMODEL_ACQUIRE);
      while(awaited != 1);
    }
    return true;
  }
}

static void hierarchy_leader_cleanup(leader_select_t *l)
{
  __atomic_store_n(&l->remaining, l->num, MEMMODEL_RELEASE);
}

///////////////////////////////////////////////////////////////////////////////
// Barriers
///////////////////////////////////////////////////////////////////////////////

void hierarchy_hybrid_barrier(int nid, const char *desc)
{
  bool leader;
#ifdef _TIME_BARRIER
  struct timespec start, end;
#endif

  leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                     &popcorn_node[nid].bar,
                                     false, NULL);
  if(leader)
  {
#ifdef _TIME_BARRIER
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
    gomp_team_barrier_wait_nospin(&popcorn_global.bar);
#ifdef _TIME_BARRIER
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("%s barrier wait: %lld ns\n",
           (desc ? desc : "Anonymous"),
           ELAPSED(start, end));
#endif
    hierarchy_leader_cleanup(&popcorn_node[nid].sync);
  }
  gomp_team_barrier_wait(&popcorn_node[nid].bar);
}

bool hierarchy_hybrid_cancel_barrier(int nid, const char *desc)
{
  bool ret = false, leader;
#ifdef _TIME_BARRIER
  struct timespec start, end;
#endif

  leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                     &popcorn_node[nid].bar,
                                     false, NULL);
  if(leader)
  {
#ifdef _TIME_BARRIER
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
    ret = gomp_team_barrier_wait_cancel_nospin(&popcorn_global.bar);
    // TODO if cancelled at global level need to cancel local barrier
#ifdef _TIME_BARRIER
    if(!ret)
    {
      clock_gettime(CLOCK_MONOTONIC, &end);
      printf("%s cancel barrier wait: %lld ns\n",
             (desc ? desc : "Anonymous"),
             ELAPSED(start, end));
    }
#endif
    hierarchy_leader_cleanup(&popcorn_node[nid].sync);
  }
  ret |= gomp_team_barrier_wait_cancel(&popcorn_node[nid].bar);
  return ret;
}

/* End-of-parallel section barriers are a little tricky because upon starting
   the next section the main thread will reset the per-node synchronization
   data.  We need to ensure that all non-leader threads reach the per-node
   barrier *before* performing global synchronization.  Once this is
   accomplished the leader can unconditionally release the threads waiting at
   the per-node barrier.  Note that this function *requires* the main thread
   to call hierarchy_init_node() upon starting the next section, as we leave
   the per-node barrier in an inconsistent state to avoid race conditions. */
void hierarchy_hybrid_barrier_final(int nid, const char *desc)
{
  bool leader;
#ifdef _TIME_BARRIER
  struct timespec start, end;
#endif

  leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                     &popcorn_node[nid].bar,
                                     true, NULL);
  if(leader)
  {
#ifdef _TIME_BARRIER
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
    gomp_team_barrier_wait_final_nospin(&popcorn_global.bar);
#ifdef _TIME_BARRIER
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("%s final team barrier wait: %lld ns\n",
           (desc ? desc : "Anonymous"),
           ELAPSED(start, end));
#endif
    gomp_team_barrier_wait_final_last(&popcorn_node[nid].bar);
  }
  else gomp_team_barrier_wait_final(&popcorn_node[nid].bar);
}

///////////////////////////////////////////////////////////////////////////////
// Reductions
///////////////////////////////////////////////////////////////////////////////

static inline bool
hierarchy_reduce_leader(int nid,
                        void *reduce_data,
                        void (*reduce_func)(void *lhs, void *rhs))
{
  bool global_leader;
  size_t i, reduced = 1, nthreads = popcorn_node[nid].opt.num, max_entry;
  void *thr_data;

  /* First, reduce from all threads locally.  The basic strategy is to loop
     through all reduction entries, waiting for a thread to populate it with
     data.  Keep looping until all local threads have been combined. */
  max_entry = nthreads < REDUCTION_ENTRIES ? nthreads : REDUCTION_ENTRIES;
  while(reduced < nthreads)
  {
    // TODO only execute this a set number of times then donate leadership?
    for(i = 0; i < max_entry; i++)
    {
      thr_data = __atomic_load_n(&popcorn_node[nid].reductions[i].p,
                                 MEMMODEL_ACQUIRE);
      if(!thr_data) continue;

      reduce_func(reduce_data, thr_data);
      __atomic_store_n(&popcorn_node[nid].reductions[i].p, NULL,
                       MEMMODEL_RELEASE);
      reduced++;
      thr_data = NULL;
    }
  }

  /* Now, select a global leader & do the same thing on the global data. */
  global_leader = select_leader_optimistic(&popcorn_global.opt, NULL);
  if(global_leader)
  {
    reduced = 1;
    while(reduced < popcorn_global.opt.num)
    {
      for(i = 0; i < MAX_POPCORN_NODES; i++)
      {
        if(!popcorn_global.threads_per_node[i]) continue;
        thr_data = __atomic_load_n(&popcorn_global.reductions[i].p,
                                   MEMMODEL_ACQUIRE);
        if(!thr_data) continue;

        reduce_func(reduce_data, thr_data);
        __atomic_store_n(&popcorn_global.reductions[i].p, NULL,
                         MEMMODEL_RELEASE);
        reduced++;
        thr_data = NULL;
      }
    }
    hierarchy_leader_cleanup(&popcorn_global.opt);
  }
  else /* Each node should get its own reduction entry, no need to loop */
    __atomic_store_n(&popcorn_global.reductions[nid].p, reduce_data,
                     MEMMODEL_RELEASE);
  return global_leader;
}

static inline void
hierarchy_reduce_local(int nid, size_t ticket, void *reduce_data)
{
  bool set = false;
  void *expected;
  unsigned long long i;

  /* All we need to do is make our reduction data available to the per-node
     leader (it's the leader's job to clean up).  However, we could be sharing
     a reduction entry on manycore machines so spin until it's open. */
  ticket %= REDUCTION_ENTRIES;
  while(true)
  {
    expected = NULL;
    set = __atomic_compare_exchange_n(&popcorn_node[nid].reductions[ticket].p,
                                      &expected,
                                      reduce_data,
                                      false,
                                      MEMMODEL_ACQ_REL,
                                      MEMMODEL_RELAXED);
    if(set) break;

    /* Spin for a bit (adapted from "config/linux/wait.h") */
    for(i = 0; i < gomp_spin_count_var; i++)
    {
      if(__atomic_load_n(&popcorn_node[nid].reductions[ticket].p,
                         MEMMODEL_RELAXED) == NULL) break;
      else __asm volatile("" : : : "memory");
    }
  }
}

bool hierarchy_reduce(int nid,
                      void *reduce_data,
                      void (*reduce_func)(void *lhs, void *rhs))
{
  bool leader;
  size_t ticket;

  leader = select_leader_optimistic(&popcorn_node[nid].opt, &ticket);
  if(leader)
  {
    leader = hierarchy_reduce_leader(nid, reduce_data, reduce_func);
    hierarchy_leader_cleanup(&popcorn_node[nid].opt);
  }
  else hierarchy_reduce_local(nid, ticket, reduce_data);
  return leader;
}

///////////////////////////////////////////////////////////////////////////////
// Work sharing
///////////////////////////////////////////////////////////////////////////////

// TODO for some reason consecutive reads return decreasing values when this
// value should be monotonically increasing...
static unsigned long long get_page_faults()
{
  unsigned long long i, sent = 0, recv = 0;
  FILE *fp;
  if((fp = fopen("/proc/popcorn_stat", "r")))
  {
    /* Skip header statistics & irrelevant message types */
    while(fgetc(fp) != '-');
    for(i = 0; i < 10; )
      if(fgetc(fp) == '\n') i++;

    fscanf(fp, " %llu %llu", &sent, &recv);
    fclose(fp);
    return sent + recv;
  }
  else return 0;
}

static inline void loop_init(struct gomp_work_share *ws,
                             long start,
                             long end,
                             long incr,
                             enum gomp_schedule_type sched,
                             long chunk_size,
                             int nid)
{
  ws->sched = sched;
  ws->chunk_size = chunk_size * incr;
  /* Canonicalize loops that have zero iterations to ->next == ->end.  */
  ws->end = ((incr > 0 && start > end) || (incr < 0 && start < end))
            ? start : end;
  ws->incr = incr;
  ws->next = start;

#ifdef HAVE_SYNC_BUILTINS
  {
    /* For dynamic scheduling prepare things to make each iteration faster. */
    long nthreads = popcorn_node[nid].sync.num;
    if(__builtin_expect (incr > 0, 1))
    {
      /* Cheap overflow protection.  */
      if(__builtin_expect ((nthreads | ws->chunk_size) >=
                           1UL << (sizeof (long) * __CHAR_BIT__ / 2 - 1), 0))
        ws->mode = 0;
      else
        ws->mode = ws->end < (LONG_MAX - (nthreads + 1) * ws->chunk_size);
    }
    /* Cheap overflow protection.  */
    else if(__builtin_expect ((nthreads | -ws->chunk_size)
                              >= 1UL << (sizeof (long)
                                         * __CHAR_BIT__ / 2 - 1), 0))
      ws->mode = 0;
    else
      ws->mode = ws->end > (nthreads + 1) * -ws->chunk_size - LONG_MAX;
  }
#endif
}

static inline void loop_init_ull(struct gomp_work_share *ws,
                                 bool up,
                                 unsigned long long start,
                                 unsigned long long end,
                                 unsigned long long incr,
                                 enum gomp_schedule_type sched,
                                 unsigned long long chunk_size,
                                 int nid)
{
  ws->sched = sched;
  ws->chunk_size_ull = chunk_size * incr;
  /* Canonicalize loops that have zero iterations to ->next == ->end.  */
  ws->end_ull = ((up && start > end) || (!up && start < end))
                ? start : end;
  ws->incr_ull = incr;
  ws->next_ull = start;
  ws->mode = 0;

#if defined HAVE_SYNC_BUILTINS && defined __LP64__
  {
    /* For dynamic scheduling prepare things to make each iteration
       faster.  */
    long nthreads = popcorn_node[nid].sync.num;
    if(__builtin_expect (up, 1))
    {
      /* Cheap overflow protection.  */
      if(__builtin_expect ((nthreads | ws->chunk_size_ull)
                           < 1ULL << (sizeof (unsigned long long)
                                      * __CHAR_BIT__ / 2 - 1), 1))
        ws->mode = ws->end_ull < (__LONG_LONG_MAX__ * 2ULL + 1
                                  - (nthreads + 1) * ws->chunk_size_ull);
    }
    /* Cheap overflow protection.  */
    else if(__builtin_expect ((nthreads | -ws->chunk_size_ull)
                              < 1ULL << (sizeof (unsigned long long)
                                         * __CHAR_BIT__ / 2 - 1), 1))
      ws->mode = ws->end_ull > ((nthreads + 1) * -ws->chunk_size_ull
                                - (__LONG_LONG_MAX__ * 2ULL + 1));
  }
#endif
  if(!up)
    ws->mode |= 2;
}

// TODO these don't support nested work sharing regions

void hierarchy_init_workshare_static(int nid,
                                     long long lb,
                                     long long ub,
                                     long long incr,
                                     long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init(ws, lb, ub, incr, GFS_STATIC, chunk, nid);
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
}

void hierarchy_init_workshare_static_ull(int nid,
                                         unsigned long long lb,
                                         unsigned long long ub,
                                         unsigned long long incr,
                                         unsigned long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init_ull(ws, true, lb, ub, incr, GFS_STATIC, chunk, nid);
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
}

void hierarchy_init_workshare_dynamic(int nid,
                                      long long lb,
                                      long long ub,
                                      long long incr,
                                      long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_team *team = thr->ts.team;
  int nthreads = team ? team->nthreads : 1;
  struct gomp_work_share *ws, *global;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    /* Note: initialize the local work-share to be finished so threads grab
       the next batch from the global pool immediately.  This is because we
       don't know where each node's pool of work starts/ends. */
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init(ws, lb, lb, incr, GFS_HIERARCHY_DYNAMIC, chunk, nid);
    global = gomp_ptrlock_get(&popcorn_global.ws_lock);
    if(global == NULL)
    {
      global = malloc(sizeof(struct gomp_work_share));
      gomp_init_work_share(global, false, nthreads);
      loop_init(global, lb, ub, incr, GFS_HIERARCHY_DYNAMIC, chunk, nid);
      gomp_ptrlock_set(&popcorn_global.ws_lock, global);
    }
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
}

void hierarchy_init_workshare_dynamic_ull(int nid,
                                          unsigned long long lb,
                                          unsigned long long ub,
                                          unsigned long long incr,
                                          unsigned long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_team *team = thr->ts.team;
  int nthreads = team ? team->nthreads : 1;
  struct gomp_work_share *ws, *global;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init_ull(ws, true, lb, lb, incr, GFS_HIERARCHY_DYNAMIC, chunk, nid);
    global = gomp_ptrlock_get(&popcorn_global.ws_lock);
    if(global == NULL)
    {
      global = malloc(sizeof(struct gomp_work_share));
      gomp_init_work_share(global, false, nthreads);
      loop_init_ull(global, true, lb, ub, incr, GFS_HIERARCHY_DYNAMIC,
                    chunk, nid);
      gomp_ptrlock_set(&popcorn_global.ws_lock, global);
    }
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
}

void hierarchy_init_workshare_hetprobe(int nid,
                                       long long lb,
                                       long long ub,
                                       long long incr,
                                       long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_team *team = thr->ts.team;
  int nthreads = team ? team->nthreads : 1;
  struct gomp_work_share *ws, *global;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init(ws, lb, lb, incr, GFS_HETPROBE, chunk, nid);
    popcorn_global.workshare_time[nid] = 0;
    global = gomp_ptrlock_get(&popcorn_global.ws_lock);
    if(global == NULL)
    {
      global = malloc(sizeof(struct gomp_work_share));
      gomp_init_work_share(global, false, nthreads);
      loop_init(global, lb, ub, incr, GFS_HETPROBE, chunk, nid);
      gomp_ptrlock_set(&popcorn_global.ws_lock, global);
    }
    popcorn_node[nid].page_faults = get_page_faults();
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
  thr->ts.static_trip = 0;
  clock_gettime(CLOCK_MONOTONIC, &thr->probe_start);
}

void hierarchy_init_workshare_hetprobe_ull(int nid,
                                           unsigned long long lb,
                                           unsigned long long ub,
                                           unsigned long long incr,
                                           unsigned long long chunk)
{
  struct gomp_thread *thr = gomp_thread();
  struct gomp_team *team = thr->ts.team;
  int nthreads = team ? team->nthreads : 1;
  struct gomp_work_share *ws, *global;

  ws = gomp_ptrlock_get(&popcorn_node[nid].ws_lock);
  if(ws == NULL)
  {
    ws = popcorn_malloc(sizeof(struct gomp_work_share), nid);
    gomp_init_work_share(ws, false, popcorn_node[nid].sync.num);
    loop_init_ull(ws, true, lb, lb, incr, GFS_HETPROBE, chunk, nid);
    popcorn_global.workshare_time[nid] = 0;
    global = gomp_ptrlock_get(&popcorn_global.ws_lock);
    if(global == NULL)
    {
      global = malloc(sizeof(struct gomp_work_share));
      gomp_init_work_share(global, false, nthreads);
      loop_init_ull(global, true, lb, ub, incr, GFS_HETPROBE, chunk, nid);
      gomp_ptrlock_set(&popcorn_global.ws_lock, global);
    }
    popcorn_node[nid].page_faults = get_page_faults();
    gomp_ptrlock_set(&popcorn_node[nid].ws_lock, ws);
  }
  thr->ts.work_share = ws;
  thr->ts.static_trip = 0;
  clock_gettime(CLOCK_MONOTONIC, &thr->probe_start);
}

bool hierarchy_next_dynamic(int nid, long *start, long *end)
{
  bool ret;
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws = thr->ts.work_share;
  long chunk;

  /* *Must* use the locked versions to avoid racing against the leader when
     replenishing work from the global pool */
  gomp_mutex_lock(&ws->lock);
  ret = gomp_iter_dynamic_next_locked_ws(start, end, ws);
  if(!ret && !ws->threads_completed)
  {
    /* Local work share is out of work to distribute; replenish from global */
    chunk = ws->chunk_size * popcorn_node[nid].sync.num;
    ret = gomp_iter_dynamic_next_raw(&ws->next, &ws->end,
                                     popcorn_global.ws, chunk);
    if(ret) ret = gomp_iter_dynamic_next_locked_ws(start, end, ws);
    else ws->threads_completed = true;
  }
  gomp_mutex_unlock(&ws->lock);

  return ret;
}

bool hierarchy_next_dynamic_ull(int nid,
                                unsigned long long *start,
                                unsigned long long *end)
{
  bool ret;
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws = thr->ts.work_share;
  unsigned long long chunk;

  gomp_mutex_lock(&ws->lock);
  ret = gomp_iter_ull_dynamic_next_locked_ws(start, end, ws);
  if(!ret && !ws->threads_completed)
  {
    chunk = ws->chunk_size_ull * popcorn_node[nid].sync.num;
    ret = gomp_iter_ull_dynamic_next_raw(&ws->next_ull, &ws->end_ull,
                                         popcorn_global.ws, chunk);
    if(ret) ret = gomp_iter_ull_dynamic_next_locked_ws(start, end, ws);
    else ws->threads_completed = true;
  }
  gomp_mutex_unlock(&ws->lock);

  return ret;
}

// Note: the logging currently assumes only 1 thread is calling at a time!
// TODO printing floating point operations on aarch64 when starting on x86-64
// is currently not supported (due to software-emulated FP operations)
static char buf[1024];
#define REMAINING_BUF( buf, ptr ) (sizeof(buf) - ((ptr) - (buf)))
static void log_hetprobe_results()
{
  int i, max_node;
  unsigned long long max_time = 0;
  char *cur = buf;

  for(i = 0; i < MAX_POPCORN_NODES; i++)
  {
    if(popcorn_global.workshare_time[i] > max_time)
    {
      max_time = popcorn_global.workshare_time[i];
      max_node = i;
    }
  }

  cur += snprintf(cur, sizeof(buf), "CSR:");
  for(i = 0; i < MAX_POPCORN_NODES; i++)
  {
    if(i && !(i % 8)) cur += snprintf(cur, REMAINING_BUF(buf, cur), "\n");
    cur += snprintf(cur, REMAINING_BUF(buf, cur), "\t%.3f",
                    popcorn_global.core_speed_rating_float[i]);
  }

  float pf_p_us = (float)popcorn_global.page_faults / (float)max_time;
  snprintf(cur, REMAINING_BUF(buf, cur),
           "\nProbe: %llu page faults, %llu us, %.3f faults / us\n",
           popcorn_global.page_faults, max_time, pf_p_us);
  popcorn_log(buf);
}

/* Signal core speed value indicating that a particular node shouldn't receive
   any iterations. */
#define NO_ITER FLT_MIN

/* Shorthands for rounding numbers */
#define ROUND( type, val, incr ) \
  { \
    type remainder = val % incr; \
    val = remainder ? (val + incr - remainder) : val; \
  }
#define ROUND_LONG( val, incr ) ROUND(long, val, incr)
#define ROUND_ULL( val, incr ) ROUND(unsigned long long, val, incr)

static void calc_het_probe_workshare(int nid, bool ull)
{
  bool leader;
  size_t i, max_idx;
  unsigned long long cur_elapsed, min = UINT64_MAX, max = 0;
  float scale;
  struct gomp_work_share *ws;

  /* Calculate this node's average time & page faults */
  popcorn_global.workshare_time[nid] /= popcorn_node[nid].sync.num;
  popcorn_node[nid].page_faults = get_page_faults() -
                                  popcorn_node[nid].page_faults;

  // TODO make this always node 1?
  leader = select_leader_synchronous(&popcorn_global.sync,
                                     &popcorn_global.bar,
                                     false, NULL);
  if(leader)
  {
    /* Find the min & max values for scaling */
    for(i = 0; i < MAX_POPCORN_NODES; i++)
    {
      cur_elapsed = popcorn_global.workshare_time[i];
      if(cur_elapsed)
      {
        if(cur_elapsed < min) min = cur_elapsed;
        if(cur_elapsed > max)
        {
          max = cur_elapsed;
          max_idx = i;
        }
      }
    }

    /* Calculate core speed ratings based on the ratio to the minimum time &
       accumulate page faults from all nodes. */
    popcorn_global.scaled_thread_range_float = 0.0;
    scale = 1.0 / ((float)min / (float)popcorn_global.workshare_time[max_idx]);
    popcorn_global.page_faults = 0;
    for(i = 0; i < MAX_POPCORN_NODES; i++)
    {
      cur_elapsed = popcorn_global.workshare_time[i];
      if(cur_elapsed)
      {
        popcorn_global.core_speed_rating_float[i] =
          (float)min / (float)cur_elapsed * scale;
        popcorn_global.scaled_thread_range_float +=
          popcorn_global.core_speed_rating_float[i] *
          popcorn_node[i].sync.num;
        popcorn_global.page_faults += popcorn_node[nid].page_faults;
      }
    }
    // TODO hardcoded for 2 nodes
    popcorn_global.page_faults /= 2;

    /* Calculate iteration splits between nodes.  This is done by the global
       leader, as having threads accurately calculating boundary conditions
       on loop iteration counts using floating point numbers is hard.
       Additionally, assign work to each node's work share so we can update
       the global work share to be depleted of iterations (there's no need to
       dynamically grab more). */
    ws = popcorn_global.ws;
    float split_range = 0;
    int i, max_node;
    if(ull)
    {
      popcorn_global.ws->next_ull += popcorn_global.ws->chunk_size_ull *
                                     popcorn_global.ws->incr_ull *
                                     gomp_thread()->ts.team->nthreads;
      popcorn_global.remaining_ull = ws->end_ull - ws->next_ull;
      for(i = 0; i < MAX_POPCORN_NODES - 1; i++)
      {
        if(!popcorn_node[i].sync.num) continue;
        max_node = i;
        split_range += popcorn_global.core_speed_rating_float[i] *
                       popcorn_node[i].sync.num;
        popcorn_global.split_ull[i] = ws->next_ull +
          (split_range / popcorn_global.scaled_thread_range_float)
            * (float)popcorn_global.remaining_ull;
        ROUND_ULL(popcorn_global.split_ull[i], ws->incr_ull);
      }

      for(i = 0; i < max_node; i++)
      {
        if(i == 0) popcorn_node[0].ws->next_ull = ws->next_ull;
        else popcorn_node[i].ws->next_ull = popcorn_global.split_ull[i-1];
        popcorn_node[i].ws->end_ull = popcorn_global.split_ull[i];
      }
      popcorn_node[i].ws->next_ull = popcorn_global.split_ull[i-1];
      popcorn_node[i].ws->end_ull = ws->end_ull;
      popcorn_global.ws->next_ull = popcorn_global.ws->end_ull;
    }
    else
    {
      popcorn_global.ws->next += popcorn_global.ws->chunk_size *
                                 popcorn_global.ws->incr *
                                 gomp_thread()->ts.team->nthreads;
      popcorn_global.remaining = ws->end - ws->next;
      for(i = 0; i < MAX_POPCORN_NODES - 1; i++)
      {
        if(!popcorn_node[i].sync.num) continue;
        max_node = i;
        split_range += popcorn_global.core_speed_rating_float[i] *
                       popcorn_node[i].sync.num;
        popcorn_global.split[i] = ws->next +
          (split_range / popcorn_global.scaled_thread_range_float)
            * (float)popcorn_global.remaining;
        ROUND_LONG(popcorn_global.split[i], ws->incr);
      }

      for(i = 0; i < max_node; i++)
      {
        if(i == 0) popcorn_node[0].ws->next = ws->next;
        else popcorn_node[i].ws->next = popcorn_global.split[i-1];
        popcorn_node[i].ws->end = popcorn_global.split[i];
      }
      popcorn_node[i].ws->next = popcorn_global.split[i-1];
      popcorn_node[i].ws->end = ws->end;
      popcorn_global.ws->next = popcorn_global.ws->end;
    }

    hierarchy_leader_cleanup(&popcorn_global.sync);
  }
  gomp_team_barrier_wait_nospin(&popcorn_global.bar);
}

static inline long calc_chunk_from_ratio(int nid, struct gomp_work_share *ws)
{
  long chunk;
  float one_thread_percent = popcorn_global.core_speed_rating_float[nid] /
                             popcorn_global.scaled_thread_range_float;
  /* Note: round up because it's better to slightly overestimate loop count
     distributions (which will get corrected when grabbing work) instead of
     underestimating and forcing another round of global work distribution */
  chunk = ceilf(one_thread_percent * (float)popcorn_global.remaining);
  ROUND_LONG(chunk, ws->incr);
  return chunk;
}

bool hierarchy_next_hetprobe(int nid, long *start, long *end)
{
  bool leader;
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws = thr->ts.work_share;
  struct timespec probe_end;

  switch(thr->ts.static_trip)
  {
  case 0: /* Probe period -- only probe once */
    thr->ts.static_trip = 1;
    *start = ws->next + (thr->ts.team_id * ws->chunk_size * ws->incr);
    *end = *start + (ws->chunk_size * ws->incr);
    return true;
  case 1: /* Finished probe, assign remaining iterations */
    thr->ts.static_trip = 2;

    /* Add this thread's elapsed time to the workshare */
    clock_gettime(CLOCK_MONOTONIC, &probe_end);
    __atomic_add_fetch(&popcorn_global.workshare_time[nid],
                       ELAPSED(thr->probe_start, probe_end) / 1000,
                       MEMMODEL_ACQ_REL);

    leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                       &popcorn_node[nid].bar,
                                       false, NULL);
    if(leader)
    {
      calc_het_probe_workshare(nid, false);
      if(popcorn_global.core_speed_rating_float[nid] == NO_ITER)
      {
        /* The scheduler decided not to give this node any iterations, set work
           share so threads on this node go to ending barrier. */
        ws->chunk_size = LONG_MAX;
        ws->next = ws->end = popcorn_global.ws->end;
      }
      else ws->chunk_size = calc_chunk_from_ratio(nid, popcorn_global.ws);
      ws->sched = GFS_HIERARCHY_DYNAMIC;
      hierarchy_leader_cleanup(&popcorn_node[nid].sync);
    }
    gomp_team_barrier_wait(&popcorn_node[nid].bar);

    // TODO delete
    if(thr->ts.team_id == 0) log_hetprobe_results();

    /* Fall through */
  default: return hierarchy_next_dynamic(nid, start, end);
  }
}

static inline long
calc_chunk_from_ratio_ull(int nid, struct gomp_work_share *ws)
{
  unsigned long long chunk;
  float one_thread_percent = popcorn_global.core_speed_rating_float[nid] /
                             popcorn_global.scaled_thread_range_float;
  chunk = ceilf(one_thread_percent * (float)popcorn_global.remaining_ull);
  ROUND_ULL(chunk, ws->incr);
  return chunk;
}

bool hierarchy_next_hetprobe_ull(int nid,
                                 unsigned long long *start,
                                 unsigned long long *end)
{
  bool leader;
  struct gomp_thread *thr = gomp_thread();
  struct gomp_work_share *ws = thr->ts.work_share;
  struct timespec probe_end;

  switch(thr->ts.static_trip)
  {
  case 0: /* Probe period -- only probe once */
    thr->ts.static_trip = 1;
    *start =
      ws->next_ull + (thr->ts.team_id * ws->chunk_size_ull * ws->incr_ull);
    *end = *start + (ws->chunk_size_ull * ws->incr_ull);
    return true;
  case 1: /* Finished probe, assign remaining iterations */
    thr->ts.static_trip = 2;

    /* Add this thread's elapsed time to the workshare */
    clock_gettime(CLOCK_MONOTONIC, &probe_end);
    __atomic_add_fetch(&popcorn_global.workshare_time[nid],
                       ELAPSED(thr->probe_start, probe_end) / 1000,
                       MEMMODEL_ACQ_REL);

    leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                       &popcorn_node[nid].bar,
                                       false, NULL);
    if(leader)
    {
      calc_het_probe_workshare(nid, true);
      if(popcorn_global.core_speed_rating_float[nid] == NO_ITER)
      {
        /* The scheduler decided not to give this node any iterations, set work
           share so threads on this node go to ending barrier. */
        ws->chunk_size_ull = ULLONG_MAX;
        ws->next_ull = ws->end_ull = popcorn_global.ws->end_ull;
      }
      else
        ws->chunk_size_ull = calc_chunk_from_ratio_ull(nid, popcorn_global.ws);
      ws->sched = GFS_HIERARCHY_DYNAMIC;
      hierarchy_leader_cleanup(&popcorn_node[nid].sync);
    }
    gomp_team_barrier_wait(&popcorn_node[nid].bar);

    // TODO delete
    if(thr->ts.team_id == 0) log_hetprobe_results();

    /* Fall through */
  default: return hierarchy_next_dynamic_ull(nid, start, end);
  }
}

bool hierarchy_last(long end)
{
  return end >= popcorn_global.ws->end;
}

bool hierarchy_last_ull(unsigned long long end)
{
  return end >= popcorn_global.ws->end_ull;
}

void hierarchy_loop_end(int nid, bool global)
{
  struct gomp_thread *thr = gomp_thread();
  bool leader;

  leader = select_leader_synchronous(&popcorn_node[nid].sync,
                                     &popcorn_node[nid].bar,
                                     false, NULL);
  if(leader)
  {
    gomp_fini_work_share(popcorn_node[nid].ws);
    popcorn_free(popcorn_node[nid].ws);
    gomp_ptrlock_destroy(popcorn_node[nid].ws_lock);
    popcorn_node[nid].ws = NULL;
    if(global)
    {
      leader = select_leader_synchronous(&popcorn_global.sync,
                                         &popcorn_global.bar,
                                         false, NULL);
      if(leader)
      {
        gomp_fini_work_share(popcorn_global.ws);
        free(popcorn_global.ws);
        gomp_ptrlock_destroy(popcorn_global.ws_lock);
        popcorn_global.ws = NULL;
        hierarchy_leader_cleanup(&popcorn_global.sync);
      }
      gomp_team_barrier_wait_nospin(&popcorn_global.bar);
    }
    hierarchy_leader_cleanup(&popcorn_node[nid].sync);
  }
  gomp_team_barrier_wait(&popcorn_node[nid].bar);

  /* gomp_team_end() still expects the main thread to have a valid work share
     pointer */
  if(thr->ts.team_id == 0) thr->ts.work_share = &thr->ts.team->work_shares[0];
  else thr->ts.work_share = NULL;
}

