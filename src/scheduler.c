// scheduler.c — MLFQ (2 colas) siguiendo el flujo del enunciado al pie de la letra
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scheduler.h"

#define MAX_TICKS 100000000u

static inline unsigned q_high_val(unsigned q){ return 2u*q; }
static inline unsigned q_low_val (unsigned q){ return q;    }

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
  q->arr = (Process**)realloc(q->arr, nc*sizeof(Process*)); if(!q->arr){ perror("realloc"); exit(1); }
  q->cap = nc;
}
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t idx){ if(idx>=q->size) return; for(size_t i=idx+1;i<q->size;i++) q->arr[i-1]=q->arr[i]; q->size--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->size+1); q->arr[q->size++]=p; }

// prioridad: mayor primero; empate por PID menor
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
    if(p->state != READY) continue;
    if(p->force_max_priority){ p->priority_value = INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts > p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    if(p->deadline > t){
      double until = (double)(p->deadline - t);
      p->priority_value = 1.0 / until + (double)bursts_left;
    }else{
      // Será marcado DEAD en pasos 2/3.1, pero mantenemos valor alto para ordenar
      p->priority_value = 1e30 + (double)bursts_left;
    }
  }
}

static Process* q_pick_first_ready(QDyn* q){
  for(size_t i=0;i<q->size;i++) if(q->arr[i]->state==READY) return q->arr[i];
  return NULL;
}

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
  // copia local (para ordenar por PID al final)
  Process** procs = (Process**)malloc(sizeof(Process*) * in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // ordenar eventos por (time, pid)
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

    // -------- 1) WAITING -> READY (fin de I/O) --------
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->state==WAITING && p->io_remaining>0){
        p->io_remaining--;
        if(p->io_remaining==0){
          p->state=READY;
          // NO nos metemos con la cola aquí; ya estaba con un level asignado
          if(p->queue_level==Q_HIGH){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          else                       { if(q_index_of(&low ,p)<0) q_push(&low ,p); }
        }
      }
    }

    // -------- 2) DEAD en READY/WAITING por deadline --------
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        finished_or_dead++;
      }
    }

    // -------- 3) RUNNING: 3.1 .. 3.5 --------
    if(running){
      // 3.1) deadline al inicio del tick
      if(t>=running->deadline && running->bursts_done<running->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
      }else{
        // 3.2/3.3: aquí no hay cambios (contadores se actualizan al final del tick)
        // 3.4) si hay evento en este tick y NO es su PID: debe abandonar CPU
        if(next_event_idx<N && events[next_event_idx].time==t){
          unsigned evpid = events[next_event_idx].pid;
          if(running->pid != evpid){
            running->interruptions++;                  // cuenta interrupción por evento
            running->state=READY; running->last_left_cpu=t;
            running->queue_level=Q_HIGH; running->force_max_priority=true;
            if(q_index_of(&high,running)<0) q_push(&high,running);
            running=NULL;
          }
        }
        // 3.5) si sigue RUNNING, ejecutará el tick más abajo (después de admisiones)
      }
    }

    // -------- 4) Ingresos a colas --------
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.1) Si un proceso salió de CPU, ya lo reinsertamos en 3.4/3.3/3.2 según corresponda

      // 4.2) llegada inicial → High
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        if(p->remaining_in_burst==0) p->remaining_in_burst=p->cpu_burst;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) boost Low→High según enunciado literal: 2*deadline < t - TLCPU
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        long diff=(long)t - (long)p->last_left_cpu;
        unsigned waited = (unsigned)((diff<0)?0:diff);
        if( (2u * p->deadline) < waited ){
          q_remove(&low,p);
          p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          if(p->remaining_quantum > qH) p->remaining_quantum = qH;
        }
      }
    }

    // -------- 5) Prioridades --------
    recompute_priorities(&high, t);
    recompute_priorities(&low , t);
    q_sort_by_priority(&high);
    q_sort_by_priority(&low);

    // -------- 6) Ingreso a CPU --------
    if(!running){
      // 6.1) si hay evento en este tick, entra el PID del evento
      if(next_event_idx<N && events[next_event_idx].time==t){
        Process* evp = find_by_pid(procs, in->K, events[next_event_idx].pid);
        if(evp && evp->state!=DEAD && evp->state!=FINISHED){
          // si estaba en WAITING, aborta I/O (no suma interrupción adicional)
          if(evp->state==WAITING){ evp->io_remaining=0; evp->state=READY; }
          // remover de colas si estaba en READY
          q_remove(&high,evp); q_remove(&low,evp);
          // preparar quantum si es necesario
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH)? qH : qL;
          if(!evp->has_response){
            evp->has_response=true;
            evp->response_time = (t>=evp->start_time)? (t - evp->start_time) : 0u;
          }
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        // consumimos el evento aquí
        next_event_idx++;
      }
      // 6.2 / 6.3: si CPU sigue libre, High primero, luego Low (por prioridad dentro de cada cola)
      if(!running){
        Process* pick = q_pick_first_ready(&high);
        if(!pick) pick = q_pick_first_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH)? qH : qL;
          if(!pick->has_response){
            pick->has_response=true;
            pick->response_time=(t>=pick->start_time)? (t - pick->start_time) : 0u;
          }
          pick->state=RUNNING; pick->force_max_priority=false; running=pick;
        }
      }
    }

    // -------- Acumular waiting en READY o WAITING (si ya llegó) --------
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->arrived && (p->state==READY || p->state==WAITING)){
        p->waiting_time++;
      }
    }

    // -------- Ejecutar el tick: decrementar contadores del RUNNING --------
    if(running){
      Process* p=running;
      if(p->remaining_in_burst==0) p->remaining_in_burst = p->cpu_burst;
      if(p->remaining_quantum==0)  p->remaining_quantum  = (p->queue_level==Q_HIGH)? qH : qL;

      p->remaining_in_burst--;
      p->remaining_quantum--;

      int end_burst = (p->remaining_in_burst==0);
      int end_quant = (p->remaining_quantum==0);

      if(end_burst && end_quant){
        // Consideraciones especiales: cede la CPU (no baja de cola).
        p->bursts_done++;
        p->last_left_cpu = t+1;
        if(p->bursts_done >= p->total_bursts){
          p->state=FINISHED; p->completion_time=t+1; running=NULL; finished_or_dead++;
        }else{
          p->interruptions++; // cuenta yield a I/O como interrupción
          p->state=WAITING;
          p->io_remaining = p->io_wait;         // espera EXACTAMENTE io_wait ticks
          p->remaining_in_burst = p->cpu_burst; // próxima ráfaga
          // NO democión de cola
          running=NULL;
        }
      }
      else if(end_burst){
        p->bursts_done++;
        p->last_left_cpu = t+1;
        if(p->bursts_done >= p->total_bursts){
          p->state=FINISHED; p->completion_time=t+1; running=NULL; finished_or_dead++;
        }else{
          p->interruptions++;  // yield a I/O cuenta como interrupción
          p->state=WAITING;
          p->io_remaining = p->io_wait;         // EXACTAMENTE io_wait
          p->remaining_in_burst = p->cpu_burst;
          // no se reinicia quantum
          running=NULL;
        }
      }
      else if(end_quant){
        p->interruptions++;               // interrupción por quantum
        p->last_left_cpu = t+1;
        if(p->queue_level==Q_HIGH) p->queue_level=Q_LOW;
        p->state=READY;
        p->remaining_quantum = (p->queue_level==Q_HIGH)? qH : qL;
        if(p->queue_level==Q_HIGH){ if(q_index_of(&high,p)<0) q_push(&high,p); }
        else                       { if(q_index_of(&low ,p)<0) q_push(&low ,p); }
        running=NULL;
      }
      // si no terminó ráfaga ni quantum, sigue RUNNING al próximo tick
    }

    t++;
  } // while

  // -------- Output ordenado por PID --------
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
