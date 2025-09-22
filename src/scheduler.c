// scheduler.c — evento persistente y contabilidad estricta tick a tick
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scheduler.h"

#define MAX_TICKS 100000000u  // salvaguarda anti-loop

static inline unsigned q_high_val(unsigned q){ return 2u*q; }
static inline unsigned q_low_val (unsigned q){ return q;    }

static void queue_init(Queue* q, QueueLevel level){ q->level=level; q->arr=NULL; q->size=0; q->cap=0; }
static void queue_free(Queue* q){ free(q->arr); q->arr=NULL; q->size=q->cap=0; }
static void queue_reserve(Queue* q, size_t need){
  if(q->cap>=need) return;
  size_t nc = q->cap? q->cap*2:8; if(nc<need) nc=need;
  q->arr = realloc(q->arr, nc*sizeof(Process*)); if(!q->arr){ perror("realloc"); exit(1); }
  q->cap=nc;
}
static int  queue_index_of(Queue* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void queue_erase_at(Queue* q, size_t idx){ if(idx>=q->size) return; for(size_t i=idx+1;i<q->size;i++) q->arr[i-1]=q->arr[i]; q->size--; }
static void queue_remove(Queue* q, Process* p){ int i=queue_index_of(q,p); if(i>=0) queue_erase_at(q,(size_t)i); }
static void queue_push_back(Queue* q, Process* p){ queue_reserve(q, q->size+1); q->arr[q->size++]=p; }

// prioridad descendente; empate: PID menor
static int cmp_priority(const void* a, const void* b){
  Process* pa = *(Process* const*)a; Process* pb = *(Process* const*)b;
  if (pa->priority_value > pb->priority_value) return -1;
  if (pa->priority_value < pb->priority_value) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}
static void queue_sort_by_priority(Queue* q){ if(q->size>1) qsort(q->arr, q->size, sizeof(Process*), cmp_priority); }

static void recompute_priorities_tick(Queue* q, unsigned t){
  for(size_t i=0;i<q->size;i++){
    Process* p = q->arr[i];
    if(p->force_max_priority){ p->priority_value = INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts>p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    if(p->deadline > t){ double until = (double)(p->deadline - t); p->priority_value = 1.0/until + (double)bursts_left; }
    else               { p->priority_value = 1e30 + (double)bursts_left; }
  }
}
static Process* queue_pick_ready(Queue* q){ for(size_t i=0;i<q->size;i++) if(q->arr[i]->state==READY) return q->arr[i]; return NULL; }

static int cmp_pid_ptr(const void* a, const void* b){
  const Process* const* pa=a; const Process* const* pb=b;
  if((*pa)->pid < (*pb)->pid) return -1; if((*pa)->pid > (*pb)->pid) return 1; return 0;
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
  // clonar para ordenar por PID al final
  Process** procs = malloc(sizeof(Process*) * in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // ordenar eventos por tiempo
  Event* events=NULL; size_t N=in->N;
  if(N>0){ events=malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
           memcpy(events,in->events,sizeof(Event)*N); qsort(events,N,sizeof(Event),cmp_event_time); }

  unsigned qH=q_high_val(in->q_base), qL=q_low_val(in->q_base);
  Queue high,low; queue_init(&high,Q_HIGH); queue_init(&low,Q_LOW);

  // control de evento persistente
  unsigned active_evt_pid = (N>0)? events[0].pid : (unsigned)(-1);
  unsigned active_evt_time= (N>0)? events[0].time: 0;

  unsigned t=0; size_t finished_or_dead=0;
  Process* running=NULL;

  while(finished_or_dead < in->K && t < MAX_TICKS){

    // 1) WAITING -> READY
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->state==WAITING && p->io_remaining>0){
        p->io_remaining--;
        if(p->io_remaining==0){
          p->state=READY;
          if(p->queue_level==Q_HIGH){ if(queue_index_of(&high,p)<0) queue_push_back(&high,p); }
          else                       { if(queue_index_of(&low ,p)<0) queue_push_back(&low ,p); }
        }
      }
    }

    // 2) DEAD en colas por deadline
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        queue_remove(&high,p); queue_remove(&low,p);
        finished_or_dead++;
      }
    }

    // 3) RUNNING (3.1→3.5)
    if(running){
      // 3.1) muere al inicio
      if(t>=running->deadline && running->bursts_done<running->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
      }else{
        // 3.4) evento persistente: si t >= active_evt_time y el que corre NO es ese PID → interrumpe
        if(N>0 && t>=active_evt_time){
          if(running->pid != active_evt_pid){
            running->interruptions++;                    // preempt por evento
            running->state=READY; running->last_left_cpu=t;
            running->queue_level=Q_HIGH; running->force_max_priority=true;
            if(queue_index_of(&high,running)<0) queue_push_back(&high,running);
            running=NULL;
          }
        }

        // 3.5) si sigue, consume 1 tick y luego 3.2/3.3
        if(running){
          if(running->remaining_in_burst>0) running->remaining_in_burst--;
          if(running->remaining_quantum  >0) running->remaining_quantum--;

          // 3.2) terminó ráfaga
          if(running->remaining_in_burst==0){
            running->bursts_done++; running->last_left_cpu=t+1;
            if(running->bursts_done>=running->total_bursts){
              running->state=FINISHED; running->completion_time=t+1; running=NULL; finished_or_dead++;
            }else{
              running->state=WAITING; running->io_remaining=running->io_wait;
              running->remaining_in_burst=running->cpu_burst; running=NULL;
            }
          }
          // 3.3) fin de quantum (si ocurriera)
          else if(running->remaining_quantum==0){
            running->interruptions++;                    // preempt por quantum
            running->last_left_cpu=t+1;
            if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
            running->state=READY;
            running->remaining_quantum=(running->queue_level==Q_HIGH)? qH : qL;
            if(running->queue_level==Q_HIGH){ if(queue_index_of(&high,running)<0) queue_push_back(&high,running); }
            else                             { if(queue_index_of(&low ,running)<0) queue_push_back(&low ,running); }
            running=NULL;
          }
        }
      }
    }

    // 4) ingreso a colas
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.2) primera llegada
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH; p->remaining_quantum=qH;
        if(queue_index_of(&high,p)<0) queue_push_back(&high,p);
      }

      // 4.3) boost Low->High si 2*deadline < t - TLCPU
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        long diff=(long)t-(long)p->last_left_cpu; unsigned waited=(unsigned)((diff<0)?0:diff);
        if((2u*p->deadline) < waited){
          queue_remove(&low,p); p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(queue_index_of(&high,p)<0) queue_push_back(&high,p); }
          if(p->remaining_quantum>qH) p->remaining_quantum=qH;
        }
      }
    }

    // 5) prioridades
    recompute_priorities_tick(&high,t);
    recompute_priorities_tick(&low ,t);
    queue_sort_by_priority(&high); queue_sort_by_priority(&low);

    // 6) meter a CPU
    if(!running){
      // 6.1) evento persistente primero (si t >= T_EVENTO)
      if(N>0 && t>=active_evt_time){
        Process* ev = find_by_pid(procs, in->K, active_evt_pid);
        if(ev && ev->state!=DEAD && ev->state!=FINISHED){
          queue_remove(&high,ev); queue_remove(&low,ev);
          if(ev->remaining_quantum==0) ev->remaining_quantum=(ev->queue_level==Q_HIGH)? qH : qL;
          if(!ev->has_response){ ev->has_response=true; ev->response_time=(t>=ev->start_time)? (t - ev->start_time):0u; }
          ev->state=RUNNING; ev->force_max_priority=false; running=ev;
        }
      }
    }
    if(!running){
      // 6.2 / 6.3 High luego Low
      Process* pick=queue_pick_ready(&high); if(!pick) pick=queue_pick_ready(&low);
      if(pick){
        queue_remove((pick->queue_level==Q_HIGH)? &high : &low , pick);
        if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH)? qH : qL;
        if(!pick->has_response){ pick->has_response=true; pick->response_time=(t>=pick->start_time)? (t - pick->start_time):0u; }
        pick->state=RUNNING; pick->force_max_priority=false; running=pick;
      }
    }

    // waiting time
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(!p->arrived) continue;
      if(p->state==READY || p->state==WAITING) p->waiting_time++;
    }

    t++;
  }

  // salida por PID asc
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = p->state==FINISHED? "FINISHED" : p->state==DEAD? "DEAD" :
                     p->state==RUNNING?  "RUNNING"  : p->state==READY? "READY":"WAITING";
    unsigned turnaround=0u;
    if(p->arrived && p->completion_time>=p->start_time) turnaround=p->completion_time - p->start_time;
    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name,p->pid,st,p->interruptions,turnaround,p->response_time,p->waiting_time);
  }
  fclose(out);
  queue_free(&high); queue_free(&low);
  free(procs); free(events);
}
