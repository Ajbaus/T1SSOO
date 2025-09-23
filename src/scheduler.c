// scheduler.c — MLFQ (2 queues) with 1→6 flow and strict tick-by-tick accounting
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scheduler.h"

#define MAX_TICKS 100000000u  // safety against infinite loops

static inline unsigned q_high_val(unsigned q){ return 2u * q; }
static inline unsigned q_low_val (unsigned q){ return q; }

typedef struct {
  QueueLevel level;
  Process**  arr;
  size_t     size, cap;
} QDyn;

static void q_init(QDyn* q, QueueLevel level){ q->level = level; q->arr = NULL; q->size = 0; q->cap = 0; }
static void q_free(QDyn* q){ free(q->arr); q->arr = NULL; q->size = q->cap = 0; }
static void q_reserve(QDyn* q, size_t need){
  if(q->cap >= need) return;
  size_t nc = q->cap ? q->cap * 2 : 8; if(nc < need) nc = need;
  q->arr = (Process**)realloc(q->arr, nc * sizeof(Process*)); if(!q->arr){ perror("realloc"); exit(1); }
  q->cap = nc;
}
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t idx){ if(idx>=q->size) return; for(size_t i=idx+1;i<q->size;i++) q->arr[i-1]=q->arr[i]; q->size--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->size+1); q->arr[q->size++]=p; }

// priority: higher first; tie-break by smaller PID
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

// Priority formula (from statement):
// Priority = 1 / T_until_deadline + (#bursts remaining)
static void recompute_priorities(QDyn* q, unsigned t){
  for(size_t i=0;i<q->size;i++){
    Process* p = q->arr[i];
    if(p->force_max_priority){ p->priority_value = INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts > p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    if(p->deadline > t){
      double until = (double)(p->deadline - t);
      p->priority_value = 1.0 / until + (double)bursts_left;
    }else{
      // Will be marked DEAD in steps 2 / 3.1
      p->priority_value = 1e30 + (double)bursts_left;
    }
  }
}

static Process* q_pick_ready(QDyn* q){ for(size_t i=0;i<q->size;i++) if(q->arr[i]->state==READY) return q->arr[i]; return NULL; }

static int cmp_pid_ptr(const void* a, const void* b){
  const Process* const* pa = (const Process* const*)a;
  const Process* const* pb = (const Process* const*)b;
  if((*pa)->pid < (*pb)->pid) return -1;
  if((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

static int cmp_event_time(const void* a, const void* b){
  const Event* ea = (const Event*)a;
  const Event* eb = (const Event*)b;
  if(ea->time != eb->time) return (ea->time < eb->time)? -1:1;
  if(ea->pid  != eb->pid ) return (ea->pid  < eb->pid )? -1:1;
  return 0;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid == pid) return procs[i];
  return NULL;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // Copy process pointers so we can sort by PID for output
  Process** procs = (Process**)malloc(sizeof(Process*) * in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // Sort events by time (and PID as tie-breaker)
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

    // Determine if there is an event at this tick (do NOT consume yet)
    bool has_event = (next_event_idx < N && events[next_event_idx].time == t);
    unsigned event_pid = has_event ? events[next_event_idx].pid : (unsigned)~0u;

    // 1) WAITING -> READY (I/O completion)
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

    // 2) Mark DEAD for missed deadlines (READY/WAITING not finished)
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        finished_or_dead++;
      }
    }

    // 3) RUNNING handling: 3.1 → 3.5
    if(running){
      // 3.1) deadline hit at the start of tick
      if(t>=running->deadline && running->bursts_done<running->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
      }else{
        // 3.4) event at this tick for a different PID → preempt BEFORE executing
        if(has_event && running->pid != event_pid){
          running->interruptions++;                 // interruption due to event
          running->state=READY; running->last_left_cpu=t;
          running->queue_level=Q_HIGH; running->force_max_priority=true;
          if(q_index_of(&high,running)<0) q_push(&high,running);
          running=NULL;
          // do NOT consume the event here; step 6.1 will schedule it
        }

        // 3.5) if still RUNNING, execute 1 tick, then 3.2/3.3
        if(running){
          if(running->remaining_in_burst>0) running->remaining_in_burst--;
          if(running->remaining_quantum  >0) running->remaining_quantum--;

          // 3.2) burst finished (at end of tick)
          if (running->remaining_in_burst == 0) {
            running->bursts_done++;
            running->last_left_cpu = t + 1;

            if (running->bursts_done >= running->total_bursts) {
              // finished whole execution
              running->state = FINISHED;
              running->completion_time = t + 1;
              running = NULL;
              finished_or_dead++;
            } else {
              // goes to WAITING (I/O) — counts as an interruption (left CPU before finishing all its bursts)
              running->interruptions++;

              running->state = WAITING;

              // I/O waits IO_WAIT + 1 ticks after leaving CPU (so with IO_WAIT=1 and leaving at t+1, it becomes READY at t+2)
              running->io_remaining = running->io_wait + 1;

              // next CPU burst length reload
              running->remaining_in_burst = running->cpu_burst;

              // yields CPU → quantum does NOT reset
              running = NULL;
            }
          }

          // 3.3) quantum exhausted (and did NOT finish the burst)
          else if(running->remaining_quantum==0){
            running->interruptions++;                 // interruption due to quantum
            running->last_left_cpu = t+1;
            if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
            running->state=READY;
            running->remaining_quantum = (running->queue_level==Q_HIGH)? qH : qL;
            if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
            else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
            running=NULL;
          }
          // 3.5) keep RUNNING: nothing else
        }
      }
    }

    // 4) Queue admissions
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.2) first arrival → High
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) boost Low→High if waited long vs time until deadline:
      // condition: (t - T_LCPU) > 2 * (T_deadline - t)
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        unsigned waited = (t > p->last_left_cpu)? (t - p->last_left_cpu) : 0u;
        unsigned until_deadline = (t < p->deadline)? (p->deadline - t) : 0u;
        if( waited > 2u * until_deadline ){
          q_remove(&low,p);
          p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          if(p->remaining_quantum > qH) p->remaining_quantum = qH;
        }
      }
    }

    // 5) Priorities and ordering in each queue
    recompute_priorities(&high, t);
    recompute_priorities(&low , t);
    q_sort_by_priority(&high);
    q_sort_by_priority(&low);

    // 6) Put a process on CPU (no preemption by priority here)
    if(!running){
      // 6.1) if an event occurs THIS tick, the event PID must enter CPU (and consume the event)
      if(has_event){
        Process* evp = find_by_pid(procs, in->K, event_pid);
        if(evp && evp->state!=DEAD && evp->state!=FINISHED){
          // remove from queues if present
          q_remove(&high,evp); q_remove(&low,evp);

          // If it was WAITING, the event interrupts its I/O immediately
          if(evp->state==WAITING){
            evp->io_remaining = 0;
          }
          // Mark as arrived if it hadn't been
          if(!evp->arrived){
            evp->arrived = true;
          }
          // Ensure it has a quantum to run
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH)? qH : qL;

          if(!evp->has_response){
            evp->has_response=true;
            evp->response_time=(t>=evp->start_time)? (t-evp->start_time):0u;
          }
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        next_event_idx++; // consume the event
      }

      // 6.2 / 6.3: if CPU still free, take from High then Low by priority
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

    // Waiting time: count all ticks from t=0 until completion where the process is
    // NOT RUNNING and NOT in I/O WAIT (WAITING). This matches the expected outputs.
    for (size_t i = 0; i < in->K; i++) {
      Process* p = procs[i];
      if (p->state==FINISHED || p->state==DEAD) continue;
      if (p->state != RUNNING && p->state != WAITING) {
        p->waiting_time++;
      }
    }

    t++;
  } // while

  // Output ordered by PID
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = p->state==FINISHED? "FINISHED" : p->state==DEAD? "DEAD" :
                     p->state==RUNNING?  "RUNNING"  : p->state==READY? "READY":"WAITING";
    unsigned turnaround=0u;
    if(p->completion_time>=p->start_time) turnaround=p->completion_time - p->start_time;
    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, p->waiting_time);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(events);
}
