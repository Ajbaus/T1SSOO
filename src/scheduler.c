// scheduler.c — MLFQ (2 colas) pegado al enunciado.
// Reglas:
// - No hay preempción por prioridad (solo por evento).
// - Fin de ráfaga con ráfagas pendientes: va a WAITING, cuenta 1 interrupción.
// - Fin de quantum: baja de cola, NO cuenta interrupción.
// - I/O: se fija io_remaining = io_wait al cerrar la ráfaga, y el avance de I/O
//   se hace al FINAL del tick; cuando llega a 0 pasa a READY para el tick SIGUIENTE.
// - waiting_time: se calcula al final con la fórmula absoluta desde t=0.

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
    Process* p = q->arr[i];
    if(p->state != READY){ p->priority_value = -INFINITY; continue; }
    if(p->force_max_priority){ p->priority_value = INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts>p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    unsigned until = (t < p->deadline)? (p->deadline - t) : 1u; // evitar 0
    p->priority_value = (1.0/(double)until) + (double)bursts_left;
  }
}

static Process* pick_ready(QDyn* q){
  // colas ya ordenadas por prioridad; devolver el primer READY
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
  if(ea->time != eb->time) return (ea->time < eb->time)? -1:1;
  if(ea->pid  != eb->pid ) return (ea->pid  < eb->pid )? -1:1;
  return 0;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid==pid) return procs[i];
  return NULL;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // copia de punteros para salida ordenada
  Process** procs = (Process**)malloc(sizeof(Process*) * in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // eventos ordenados
  Event* events=NULL; size_t N=in->N;
  if(N){
    events=(Event*)malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
    memcpy(events,in->events,sizeof(Event)*N);
    qsort(events,N,sizeof(Event),cmp_event_time);
  }

  unsigned qH = q_high_val(in->q_base), qL = q_low_val(in->q_base);
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

  while(finished_or_dead < in->K && t < MAX_TICKS){

    // ===== A) EVENTO de este tick (preempt, pero sin contar interrupción) =====
    int has_event = (next_ev < N && events[next_ev].time == t);
    unsigned evpid = has_event ? events[next_ev].pid : (unsigned)~0u;
    if(has_event){
      if(running && running->pid != evpid){
        // desalojar actual: va a READY (High) y con prioridad forzada (siguiente pick)
        running->state=READY; running->last_left_cpu=t;
        running->queue_level=Q_HIGH; running->force_max_priority=true;
        if(q_index_of(&high,running)<0) q_push(&high,running);
        running=NULL;
      }
    }

    // ===== B) Llegadas iniciales (entran a High) =====
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }
    }

    // ===== C) Deadlines (mueren READY/WAITING que no completaron) =====
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY || p->state==WAITING) && p->bursts_done < p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        finished_or_dead++;
      }
    }
    // deadline para RUNNING se chequea antes de ejecutar el tick
    if(running && t>=running->deadline && running->bursts_done<running->total_bursts){
      running->state=DEAD; running->completion_time=t; running=NULL; finished_or_dead++;
    }

    // ===== D) Recompute prioridades y ordenar colas =====
    recompute_priorities(&high,t);
    recompute_priorities(&low ,t);
    if(high.size>1) qsort(high.arr, high.size, sizeof(Process*), cmp_priority);
    if(low .size>1) qsort(low .arr, low .size, sizeof(Process*), cmp_priority);

    // ===== E) Ingresar a CPU (sin preempción por prioridad) =====
    if(!running){
      if(has_event){
        Process* evp = find_by_pid(procs, in->K, evpid);
        if(evp && evp->state!=FINISHED && evp->state!=DEAD){
          // si estaba esperando I/O, el evento lo saca de I/O sin contar interrupción
          if(evp->state==WAITING){ evp->io_remaining=0; evp->state=READY; }
          q_remove(&high,evp); q_remove(&low,evp);
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH? qH:qL);
          if(!evp->has_response && t>=evp->start_time){ evp->has_response=true; evp->response_time=t - evp->start_time; }
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        next_ev++;
      }
      if(!running){
        Process* pick = pick_ready(&high);
        if(!pick) pick = pick_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH? qH:qL);
          if(!pick->has_response && t>=pick->start_time){ pick->has_response=true; pick->response_time=t - pick->start_time; }
          pick->state=RUNNING; pick->force_max_priority=false; running=pick;
        }
      }
    }

    // ===== F) Ejecutar 1 tick de CPU =====
    if(running){
      if(running->remaining_in_burst>0) running->remaining_in_burst--;
      if(running->remaining_quantum  >0) running->remaining_quantum--;

      // fin de ráfaga
      if(running->remaining_in_burst==0){
        running->bursts_done++;
        running->last_left_cpu = t+1;

        if(running->bursts_done >= running->total_bursts){
          running->state=FINISHED; running->completion_time=t+1; running=NULL; finished_or_dead++;
        }else{
          // pasa a I/O: cuenta interrupción
          running->interruptions++;
          running->state=WAITING;
          running->io_remaining = running->io_wait;   // I/O exacto
          running->remaining_in_burst = running->cpu_burst;
          // cede CPU y mantiene cola; quantum se refrescará cuando vuelva a RUNNING si está en 0
          running=NULL;
        }
      }
      // fin de quantum (no cuenta interrupción)
      else if(running->remaining_quantum==0){
        running->last_left_cpu = t+1;
        if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
        running->state=READY;
        running->remaining_quantum = (running->queue_level==Q_HIGH? qH:qL);
        if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
        else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
        running=NULL;
      }
    }

    // ===== G) Avance de I/O al FINAL del tick =====
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->state==WAITING && p->io_remaining>0){
        p->io_remaining--;
        if(p->io_remaining==0){
          // pasa a READY para el **próximo** ciclo de selección
          p->state=READY;
          if(p->queue_level==Q_HIGH){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          else                       { if(q_index_of(&low ,p)<0) q_push(&low ,p); }
        }
      }
    }

    t++;
  } // while

  // ===== Salida ordenada por PID =====
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = (p->state==FINISHED)? "FINISHED" : (p->state==DEAD? "DEAD" :
                     (p->state==RUNNING? "RUNNING" : (p->state==READY? "READY":"WAITING")));

    unsigned turnaround = 0u;
    if(p->completion_time >= p->start_time) turnaround = p->completion_time - p->start_time;

    // waiting absoluto desde t=0 (como en ejemplos del enunciado)
    unsigned cpu_total = p->cpu_burst * p->total_bursts;
    unsigned gaps = (p->total_bursts>0)? (p->total_bursts-1):0u;
    unsigned waiting = (p->completion_time >= cpu_total + gaps)? (p->completion_time - cpu_total - gaps) : 0u;

    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, waiting);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(events);
}
