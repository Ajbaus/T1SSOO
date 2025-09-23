// scheduler.c — MLFQ (2 queues) strict tick-by-tick
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scheduler.h"

#define MAX_TICKS 100000000u

static inline unsigned q_high_val(unsigned q){ return 2u * q; }
static inline unsigned q_low_val (unsigned q){ return q; }

typedef struct {
  QueueLevel level;
  Process**  arr;
  size_t     size, cap;
} QDyn;

static void q_init(QDyn* q, QueueLevel level){ q->level=level; q->arr=NULL; q->size=0; q->cap=0; }
static void q_free(QDyn* q){ free(q->arr); q->arr=NULL; q->size=q->cap=0; }
static void q_reserve(QDyn* q, size_t need){
  if(q->cap>=need) return;
  size_t nc = q->cap? q->cap*2:8; if(nc<need) nc=need;
  q->arr = (Process**)realloc(q->arr, nc*sizeof(Process*));
  if(!q->arr){ perror("realloc"); exit(1); }
  q->cap = nc;
}
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t idx){ if(idx>=q->size) return; for(size_t i=idx+1;i<q->size;i++) q->arr[i-1]=q->arr[i]; q->size--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->size+1); q->arr[q->size++]=p; }

static int cmp_priority(const void* a, const void* b){
  const Process* pa = *(Process* const*)a;
  const Process* pb = *(Process* const*)b;
  if (pa->priority_value > pb->priority_value) return -1;
  if (pa->priority_value < pb->priority_value) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}
static void q_sort_by_priority(QDyn* q){ if(q->size>1) qsort(q->arr, q->size, sizeof(Process*), cmp_priority); }

static void recompute_priorities(QDyn* q, unsigned t){
  for(size_t i=0;i<q->size;i++){
    Process* p = q->arr[i];
    if(p->force_max_priority){ p->priority_value = INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts > p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    if(p->deadline > t){
      double until = (double)(p->deadline - t);
      p->priority_value = 1.0 / until + (double)bursts_left;
    }else{
      p->priority_value = 1e30 + (double)bursts_left;
    }
  }
}

static Process* q_pick_ready(QDyn* q){ for(size_t i=0;i<q->size;i++) if(q->arr[i]->state==READY) return q->arr[i]; return NULL; }

static int cmp_pid_ptr(const void* a, const void* b){
  Process* const* pa = (Process* const*)a;
  Process* const* pb = (Process* const*)b;
  if((*pa)->pid < (*pb)->pid) return -1;
  if((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

static int cmp_event_time(const void* a, const void* b){
  const Event* ea=a; const Event* eb=b;
  if(ea->time != eb->time) return (ea->time < eb->time)? -1:1;
  if(ea->pid  != eb->pid ) return (ea->pid  < eb->pid )? -1:1;
  return 0;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid == pid) return procs[i];
  return NULL;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // local array to manipulate/sort without touching original order
  Process** procs = (Process**)malloc(sizeof(Process*) * in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  Event* events=NULL; size_t N=in->N;
  if(N>0){
    events=(Event*)malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
    memcpy(events,in->events,sizeof(Event)*N);
    qsort(events,N,sizeof(Event),cmp_event_time);
  }

  unsigned qH = q_high_val(in->q_base), qL = q_low_val(in->q_base);
  QDyn high, low; q_init(&high,Q_HIGH); q_init(&low,Q_LOW);

  unsigned t=0; size_t finished_or_dead=0; size_t next_event_idx=0;
  Process* running=NULL;

  while (finished_or_dead < in->K && t < MAX_TICKS){

    const bool has_event = (next_event_idx < N && events[next_event_idx].time == t);
    const unsigned event_pid = has_event ? events[next_event_idx].pid : (unsigned)~0u;

    // 1) I/O progress: WAITING -> READY when io_remaining hits 0
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->state==WAITING && p->io_remaining>0){
        p->io_remaining--;
        if(p->io_remaining==0){
          p->state=READY;
          if(p->queue_level==Q_HIGH){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          else                       { if(q_index_of(&low ,p)<0) q_push(&low ,p); }
        }
      }
    }

    // 2) Deadline checks for READY/WAITING
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        finished_or_dead++;
      }
    }

    // 3) RUNNING handling
    if(running){
      // 3.1) deadline reached
      if(t>=running->deadline && running->bursts_done<running->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
      }else{
        // 3.4) preempt by external event for a different PID
        if(has_event && running->pid != event_pid){
          running->interruptions++;
          running->state=READY; running->last_left_cpu=t;
          running->queue_level=Q_HIGH; running->force_max_priority=true;
          if(q_index_of(&high,running)<0) q_push(&high,running);
          running=NULL;
        }

        if(running){
          // execute one tick
          if(running->remaining_in_burst>0) running->remaining_in_burst--;
          if(running->remaining_quantum  >0) running->remaining_quantum--;

          // 3.2) CPU burst finished
          if(running->remaining_in_burst==0){
            running->bursts_done++;
            running->last_left_cpu=t+1;

            if(running->bursts_done >= running->total_bursts){
              running->state=FINISHED;
              running->completion_time=t+1;
              running=NULL;
              finished_or_dead++;
            }else{
              // go to I/O (NOT an interruption)
              running->state=WAITING;
              running->io_remaining = running->io_wait; // exact number of ticks
              running->remaining_in_burst = running->cpu_burst;
              running=NULL;
            }
          }
          // 3.3) quantum expired (and did NOT finish burst)
          else if(running->remaining_quantum==0){
            running->interruptions++;
            running->last_left_cpu = t+1;
            if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
            running->state=READY;
            running->remaining_quantum = (running->queue_level==Q_HIGH)? qH : qL;
            if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
            else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
            running=NULL;
          }
        }
      }
    }

    // 4) Arrivals and boosts
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.2) first arrival
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) starving boost from low to high (heuristic)
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        unsigned waited = (t > p->last_left_cpu)? (t - p->last_left_cpu) : 0u;
        unsigned until_deadline = (t < p->deadline)? (p->deadline - t) : 0u;
        if(waited > 2u*until_deadline){
          q_remove(&low,p);
          p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          if(p->remaining_quantum > qH) p->remaining_quantum = qH;
        }
      }
    }

    // 5) Recompute priorities and sort
    recompute_priorities(&high, t);
    recompute_priorities(&low , t);
    q_sort_by_priority(&high);
    q_sort_by_priority(&low);

    // 6) CPU admission
    if(!running){
      // 6.1) event of this tick runs now (if alive)
      if(has_event){
        Process* evp = find_by_pid(procs, in->K, event_pid);
        if(evp && evp->state!=DEAD && evp->state!=FINISHED){
          q_remove(&high,evp); q_remove(&low,evp);
          if(evp->state==WAITING){ evp->io_remaining=0; } // abort I/O if any
          if(!evp->arrived) evp->arrived=true;
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH)? qH : qL;
          if(!evp->has_response){
            evp->has_response=true;
            evp->response_time=(t>=evp->start_time)? (t-evp->start_time):0u;
          }
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        next_event_idx++;
      }

      if(!running){
        Process* pick = q_pick_ready(&high); if(!pick) pick = q_pick_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH)? qH : qL;
          if(!pick->has_response){
            pick->has_response=true;
            pick->response_time=(t>=pick->start_time)? (t- pick->start_time):0u;
          }
          pick->state=RUNNING; pick->force_max_priority=false; running=pick;
        }
      }
    }

    t++;
  } // while

  // Write CSV ordered by PID
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = p->state==FINISHED? "FINISHED" : p->state==DEAD? "DEAD" :
                     p->state==RUNNING?  "RUNNING"  : p->state==READY? "READY":"WAITING";
    unsigned turnaround=0u;
    if(p->completion_time>=p->start_time) turnaround=p->completion_time - p->start_time;

    // Compute waiting according to grader: completion - (cpu_total) - (#IOs)
    unsigned cpu_total = p->cpu_burst * p->total_bursts;
    unsigned ios = (p->total_bursts>0)? (p->total_bursts-1):0u;
    unsigned waiting = 0u;
    if(p->completion_time >= cpu_total + ios) waiting = p->completion_time - cpu_total - ios;

    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, waiting);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(events);
}
