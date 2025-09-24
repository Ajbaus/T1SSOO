// scheduler.c — MLFQ (2 colas) pegado al flujo del enunciado.
// Orden por tick:
// 1) WAITING->READY (decrementa I/O)
// 2) DEAD (READY/WAITING que vencieron deadline)
// 3) Si hay RUNNING: 3.1 DEAD, 3.2 fin ráfaga, 3.3 fin quantum, 3.4 evento≠PID, 3.5 ejecuta 1 tick
// 4) Llegadas (High) y boost Low->High
// 5) Prioridades (solo READY) y ordenar colas
// 6) Ingresar a CPU (evento / High / Low) — SIN ejecutar en este tick
//
// Reglas:
// - No hay preempción por prioridad (sí por evento).
// - Fin de ráfaga y quedan ráfagas: WAITING, interruptions++ (se aplica en 3.2).
// - Fin de quantum sin fin de ráfaga: baja a Low, READY, NO suma interrupción (3.3).
// - I/O: io_remaining = io_wait (exacto) en 3.2. El “+1 efectivo” lo introduce el orden 1→3.
// - response_time: se fija en el PRIMER tick real de ejecución (3.5), no en el paso 6.
// - waiting_time: se calcula al final con fórmula absoluta desde t=0:
//     waiting = completion_time - (cpu_burst * total_bursts) - (total_bursts - 1)

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
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->size+1); q->arr[q->size++]=p; }
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t i){ if(i>=q->size) return; for(size_t j=i+1;j<q->size;j++) q->arr[j-1]=q->arr[j]; q->size--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }

// prioridad: mayor primero; empate por PID menor (solo READY)
static int cmp_priority(const void* a, const void* b){
  const Process* pa = *(Process* const*)a;
  const Process* pb = *(Process* const*)b;
  if (pa->priority_value > pb->priority_value) return -1;
  if (pa->priority_value < pb->priority_value) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}

static void recompute_priorities(QDyn* q, unsigned t){
  for(size_t i=0;i<q->size;i++){
    Process* p=q->arr[i];
    if(p->state!=READY){ p->priority_value=-INFINITY; continue; }
    if(p->force_max_priority){ p->priority_value=INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts>p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    unsigned until = (t < p->deadline)? (p->deadline - t) : 1u; // evitar 0
    p->priority_value = (1.0/(double)until) + (double)bursts_left;
  }
}

static Process* pick_first_ready(QDyn* q){
  for(size_t i=0;i<q->size;i++) if(q->arr[i]->state==READY) return q->arr[i];
  return NULL;
}

static int cmp_pid_ptr(const void* a, const void* b){
  Process* const* pa=(Process* const*)a; Process* const* pb=(Process* const*)b;
  if((*pa)->pid < (*pb)->pid) return -1;
  if((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

static int cmp_event_time(const void* a, const void* b){
  const Event* ea=(const Event*)a; const Event* eb=(const Event*)b;
  if(ea->time!=eb->time) return (ea->time<eb->time)?-1:1;
  if(ea->pid !=eb->pid ) return (ea->pid <eb->pid )?-1:1;
  return 0;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid==pid) return procs[i];
  return NULL;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // copia de punteros (salida ordenada por PID al final)
  Process** procs=(Process**)malloc(sizeof(Process*)*in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // eventos ordenados
  Event* events=NULL; size_t N=in->N;
  if(N){
    events=(Event*)malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
    memcpy(events,in->events,sizeof(Event)*N);
    qsort(events,N,sizeof(Event),cmp_event_time);
  }

  unsigned qH=q_high_val(in->q_base), qL=q_low_val(in->q_base);
  QDyn high, low; q_init(&high,Q_HIGH); q_init(&low,Q_LOW);

  // init defensivo
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    p->state=READY; p->arrived=false; p->queue_level=Q_HIGH;
    p->remaining_quantum=0; p->bursts_done=0; p->remaining_in_burst=p->cpu_burst;
    p->io_remaining=0; p->last_left_cpu=0;
    p->force_max_priority=false; p->priority_value=0.0;
    p->interruptions=0; p->has_response=false; p->response_time=0;
    p->waiting_time=0; p->completion_time=0;
  }

  unsigned t=0; size_t finished_or_dead=0; size_t next_ev=0;
  Process* running=NULL;

  while(finished_or_dead<in->K && t<MAX_TICKS){

    // 1) WAITING -> READY (decremento I/O)
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

    // 2) DEAD por deadline (READY/WAITING)
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        finished_or_dead++;
      }
    }

    // 3) RUNNING: 3.1..3.5 (con evento visible en este tick)
    int  has_event = (next_ev<N && events[next_ev].time==t);
    unsigned evpid = has_event? events[next_ev].pid : (unsigned)~0u;

    if(running){
      // 3.1) DEAD por deadline
      if(t>=running->deadline && running->bursts_done<running->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
      }
      // 3.2) fin ráfaga (terminó en tick anterior)
      else if(running->remaining_in_burst==0){
        running->bursts_done++;
        running->last_left_cpu = t; // cerró en tick previo
        if(running->bursts_done >= running->total_bursts){
          running->state=FINISHED; running->completion_time=t; running=NULL; finished_or_dead++;
        }else{
          running->interruptions++;          // solo fin de ráfaga con trabajo pendiente
          running->state=WAITING;
          running->io_remaining = running->io_wait;   // EXACTO (el +1 lo da el orden 1→3)
          running->remaining_in_burst = running->cpu_burst;
          running=NULL;
        }
      }
      // 3.3) fin de quantum (y no terminó ráfaga)
      else if(running->remaining_quantum==0){
        running->last_left_cpu = t;
        if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
        running->state=READY;
        running->remaining_quantum = (running->queue_level==Q_HIGH? qH:qL);
        if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
        else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
        running=NULL;
      }
      // 3.4) evento para otro PID → desalojar sin contar interrupción
      else if(has_event && evpid!=running->pid){
        running->state=READY; running->last_left_cpu=t;
        running->queue_level=Q_HIGH; running->force_max_priority=true;
        if(q_index_of(&high,running)<0) q_push(&high,running);
        running=NULL;
      }
      // 3.5) ejecuta 1 tick (aquí fijamos response_time si es la primera ejecución real)
      else{
        if(!running->has_response && t>=running->start_time){
          running->has_response=true;
          running->response_time = t - running->start_time;   // <<< respuesta en primer tick real
        }
        if(running->remaining_in_burst>0) running->remaining_in_burst--;
        if(running->remaining_quantum  >0) running->remaining_quantum--;
        // Transiciones por llegar a 0 se verán en el 3.2 del siguiente tick
      }
    }

    // 4) Llegadas y boost
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.2) primera llegada → High
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) boost Low->High: 2*deadline < t - TLCPU (tal cual enunciado)
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        long waited = (long)t - (long)p->last_left_cpu;
        if((2u*p->deadline) < (unsigned)((waited<0)?0:waited)){
          q_remove(&low,p); p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          if(p->remaining_quantum > qH) p->remaining_quantum = qH;
        }
      }
    }

    // 5) Prioridades y ordenar
    recompute_priorities(&high,t);
    recompute_priorities(&low ,t);
    if(high.size>1) qsort(high.arr, high.size, sizeof(Process*), cmp_priority);
    if(low .size>1) qsort(low .arr , low .size , sizeof(Process*) , cmp_priority);

    // 6) Ingresar a CPU (para el próximo tick; aquí NO se ejecuta)
    if(!running){
      // 6.1) evento del tick
      if(has_event){
        Process* evp = find_by_pid(procs, in->K, evpid);
        if(evp && evp->state!=DEAD && evp->state!=FINISHED){
          if(evp->state==WAITING){ evp->io_remaining=0; evp->state=READY; } // aborta I/O para correr ahora
          q_remove(&high,evp); q_remove(&low,evp);
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH? qH:qL);
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        next_ev++;
      }
      // 6.2 / 6.3: High, luego Low
      if(!running){
        Process* pick = pick_first_ready(&high);
        if(!pick) pick = pick_first_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH? qH:qL);
          pick->state=RUNNING; pick->force_max_priority=false; running=pick;
        }
      }
    }

    t++;
  } // while

  // Output por PID
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = (p->state==FINISHED)? "FINISHED" : (p->state==DEAD? "DEAD" :
                     (p->state==RUNNING? "RUNNING" : (p->state==READY? "READY":"WAITING")));
    unsigned turnaround=0u;
    if(p->completion_time>=p->start_time) turnaround = p->completion_time - p->start_time;

    // waiting absoluto desde t=0 (coincide con los ejemplos del PDF)
    unsigned cpu_total = p->cpu_burst * p->total_bursts;
    unsigned gaps      = (p->total_bursts>0)? (p->total_bursts-1):0u;
    unsigned waiting   = (p->completion_time >= cpu_total + gaps)? (p->completion_time - cpu_total - gaps) : 0u;

    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, waiting);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(events);
}
