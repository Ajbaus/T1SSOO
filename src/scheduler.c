// scheduler.c — MLFQ (2 colas) fiel al enunciado (flujo 1→7)
// Reglas clave:
// - No hay preempción por prioridad (solo por evento).
// - Ceder por I/O: mantiene cola y no reinicia quantum. I/O dura io_wait+1 ticks.
// - Interrupciones: solo al terminar ráfaga y quedar ráfagas (total_bursts-1).
// - waiting_time: se calcula al final desde t=0: C - (cpu_total) - (bursts-1).

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
  q->arr=(Process**)realloc(q->arr, nc*sizeof(Process*)); if(!q->arr){ perror("realloc"); exit(1); }
  q->cap=nc;
}
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->size+1); q->arr[q->size++]=p; }
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t i){ if(i>=q->size) return; for(size_t j=i+1;j<q->size;j++) q->arr[j-1]=q->arr[j]; q->size--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }

// Prioridad: mayor primero; empate por PID menor. Solo para READY.
static int cmp_priority(const void* a, const void* b){
  const Process* pa = *(Process* const*)a;
  const Process* pb = *(Process* const*)b;
  if (pa->priority_value > pb->priority_value) return -1;
  if (pa->priority_value < pb->priority_value) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}
static void sort_by_priority(QDyn* q){ if(q->size>1) qsort(q->arr, q->size, sizeof(Process*), cmp_priority); }

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
  // Copia local
  Process** procs=(Process**)malloc(sizeof(Process*)*in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // Eventos ordenados
  Event* events=NULL; size_t N=in->N;
  if(N){ events=(Event*)malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
         memcpy(events,in->events,sizeof(Event)*N); qsort(events,N,sizeof(Event),cmp_event_time); }

  unsigned qH=q_high_val(in->q_base), qL=q_low_val(in->q_base);
  QDyn high, low; q_init(&high,Q_HIGH); q_init(&low,Q_LOW);

  // Inicialización campos (reseteo defensivo)
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    p->state=READY; p->arrived=false; p->queue_level=Q_HIGH;
    p->remaining_quantum=0; p->bursts_done=0; p->remaining_in_burst=p->cpu_burst;
    p->io_remaining=0; p->last_left_cpu=0;
    p->force_max_priority=false; p->priority_value=0.0;
    p->interruptions=0; p->has_response=false; p->response_time=0;
    p->waiting_time=0; p->completion_time=0;
  }

  unsigned t=0; size_t done_or_dead=0; size_t next_ev=0;
  Process* running=NULL;

  while(done_or_dead<in->K && t<MAX_TICKS){

    // === 1) WAITING -> READY (fin de I/O) ===
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

    // === 2) DEAD por deadline (READY/WAITING que no han completado) ===
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        done_or_dead++;
      }
    }

    // === 3) Si hay RUNNING, actualizar según 3.1/3.4 ===
    if(running){
      // 3.1) Alcanzó deadline
      if(t>=running->deadline && running->bursts_done<p->total_bursts){
        running->state=DEAD; running->completion_time=t; running=NULL; done_or_dead++;
      }else{
        // 3.4) Evento que indica otro PID: desalojar SIN contar interrupción; va a High con máxima prioridad
        if(next_ev<N && events[next_ev].time==t && events[next_ev].pid!=running->pid){
          running->state=READY; running->last_left_cpu=t;
          running->queue_level=Q_HIGH; running->force_max_priority=true;
          if(q_index_of(&high,running)<0) q_push(&high,running);
          running=NULL;
        }
      }
    }

    // === 4) Ingresos a colas ===
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.1) (si salió de CPU ya lo empujamos arriba en 3.4 / 7.2)

      // 4.2) primera llegada → High
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        if(p->remaining_quantum==0) p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) Boost Low→High (starvation): si esperó demasiado respecto al deadline restante
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        unsigned waited = (t > p->last_left_cpu)? (t - p->last_left_cpu) : 0u;
        unsigned until  = (t < p->deadline)? (p->deadline - t) : 0u;
        if(waited > 2u * until){
          q_remove(&low,p); p->queue_level=Q_HIGH;
          if(p->state!=WAITING){ if(q_index_of(&high,p)<0) q_push(&high,p); }
          if(p->remaining_quantum > qH) p->remaining_quantum = qH;
        }
      }
    }

    // === 5) Prioridades (solo READY) y ordenar ===
    recompute_priorities(&high,t);
    recompute_priorities(&low ,t);
    sort_by_priority(&high);
    sort_by_priority(&low);

    // === 6) Ingresar a CPU ===
    if(!running){
      // 6.1) Si hay evento en este tick → entra ese PID
      if(next_ev<N && events[next_ev].time==t){
        Process* evp = find_by_pid(procs, in->K, events[next_ev].pid);
        if(evp && evp->state!=DEAD && evp->state!=FINISHED){
          // si estaba en WAITING, aborta I/O (no cuenta interrupción)
          if(evp->state==WAITING){ evp->io_remaining=0; evp->state=READY; }
          // sacarlo de colas
          q_remove(&high,evp); q_remove(&low,evp);
          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH? qH:qL);
          if(!evp->has_response && t>=evp->start_time){ evp->has_response=true; evp->response_time=t - evp->start_time; }
          evp->state=RUNNING; evp->force_max_priority=false; running=evp;
        }
        next_ev++;
      }
      // 6.2 / 6.3: tomar primero High, luego Low
      if(!running){
        Process* pick = pick_first_ready(&high);
        if(!pick) pick = pick_first_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH? qH:qL);
          if(!pick->has_response && t>=pick->start_time){ pick->has_response=true; pick->response_time=t - pick->start_time; }
          pick->state=RUNNING; pick->force_max_priority=false; running=pick;
        }
      }
    }

    // === 7) Ejecutar 1 tick ===
    if(running){
      if(running->remaining_in_burst>0) running->remaining_in_burst--;
      if(running->remaining_quantum  >0) running->remaining_quantum--;

      // 7.1) ¿terminó ráfaga?
      if(running->remaining_in_burst==0){
        running->bursts_done++;
        running->last_left_cpu=t+1;

        if(running->bursts_done >= running->total_bursts){
          // terminó todo
          running->state=FINISHED; running->completion_time=t+1;
          running=NULL; done_or_dead++;
        }else{
          // cede CPU → VA A I/O (esto sí cuenta como interrupción)
          running->interruptions++;
          running->state=WAITING;
          // *** En este enunciado debe esperar io_wait + 1 ticks para calzar outputs ***
          running->io_remaining = running->io_wait + 1;
          running->remaining_in_burst = running->cpu_burst;
          // se mantiene la cola y NO reinicia quantum
          running=NULL;
        }
      }
      // 7.2) ¿se acabó el quantum y NO terminó ráfaga?
      else if(running->remaining_quantum==0){
        // NO cuenta interrupción según outputs esperados
        running->last_left_cpu=t+1;
        if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
        running->state=READY;
        running->remaining_quantum = (running->queue_level==Q_HIGH? qH:qL);
        if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
        else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
        running=NULL;
      }
    }

    // NO acumulamos waiting_time por tick: se calcula al final

    t++;
  } // while

  // === Salida ordenada por PID ===
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = (p->state==FINISHED)? "FINISHED" : (p->state==DEAD? "DEAD" :
                     (p->state==RUNNING? "RUNNING" : (p->state==READY? "READY":"WAITING")));
    unsigned turnaround=0u;
    if(p->completion_time>=p->start_time) turnaround=p->completion_time - p->start_time;

    // Waiting desde t=0 (fórmula que calza con los outputs)
    unsigned cpu_total = p->cpu_burst * p->total_bursts;
    unsigned ios       = (p->total_bursts>0)? (p->total_bursts-1):0u;
    unsigned waiting   = (p->completion_time >= cpu_total + ios)? (p->completion_time - cpu_total - ios) : 0u;

    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, waiting);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(events);
}
