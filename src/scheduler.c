// scheduler.c — MLFQ (2 colas) adherido al enunciado
// - Interrupciones: solo por fin de ráfaga con ráfagas pendientes (va a I/O)
// - Waiting: se calcula al final por fórmula desde t=0
// - No hay preempción por prioridad; solo por evento
// - I/O: dura exactamente io_wait ticks contados desde el fin de la ráfaga

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
  Process**  a;
  size_t     n, cap;
} QDyn;

static void q_init(QDyn* q, QueueLevel level){ q->level=level; q->a=NULL; q->n=0; q->cap=0; }
static void q_free(QDyn* q){ free(q->a); q->a=NULL; q->n=q->cap=0; }
static void q_reserve(QDyn* q, size_t need){
  if(q->cap>=need) return;
  size_t nc = q->cap? q->cap*2:8; if(nc<need) nc=need;
  q->a = (Process**)realloc(q->a, nc*sizeof(Process*));
  if(!q->a){ perror("realloc"); exit(1); }
  q->cap = nc;
}
static void q_push(QDyn* q, Process* p){ q_reserve(q, q->n+1); q->a[q->n++]=p; }
static int  q_index_of(QDyn* q, Process* p){ for(size_t i=0;i<q->n;i++) if(q->a[i]==p) return (int)i; return -1; }
static void q_erase_at(QDyn* q, size_t i){ if(i>=q->n) return; for(size_t j=i+1;j<q->n;j++) q->a[j-1]=q->a[j]; q->n--; }
static void q_remove(QDyn* q, Process* p){ int i=q_index_of(q,p); if(i>=0) q_erase_at(q,(size_t)i); }

// prioridad descendente, empate por PID ascendente (se usa solo al elegir)
static int cmp_priority(const void* A, const void* B){
  const Process* pa = *(Process* const*)A;
  const Process* pb = *(Process* const*)B;
  if (pa->priority_value > pb->priority_value) return -1;
  if (pa->priority_value < pb->priority_value) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}

static void recompute_priorities(QDyn* q, unsigned t){
  for(size_t i=0;i<q->n;i++){
    Process* p = q->a[i];
    if(p->state != READY) { p->priority_value = -INFINITY; continue; }
    unsigned bursts_left = (p->total_bursts > p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
    unsigned until = (t < p->deadline)? (p->deadline - t) : 1u; // evitar 0
    // Enunciado: 1/(T_deadline - t) + (#bursts restantes)
    p->priority_value = (1.0 / (double)until) + (double)bursts_left;
  }
}

static Process* pick_best(QDyn* high, QDyn* low){
  // ordenar cada cola por prioridad; tomar el primero READY de High, si no hay, de Low
  if(high->n>1) qsort(high->a, high->n, sizeof(Process*), cmp_priority);
  if(low ->n>1) qsort(low ->a, low ->n, sizeof(Process*), cmp_priority);
  for(size_t i=0;i<high->n;i++) if(high->a[i]->state==READY) return high->a[i];
  for(size_t i=0;i<low ->n;i++) if(low ->a[i]->state==READY) return low ->a[i];
  return NULL;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid==pid) return procs[i];
  return NULL;
}

static int cmp_pid_ptr(const void* A, const void* B){
  Process* const* pa = (Process* const*)A;
  Process* const* pb = (Process* const*)B;
  if((*pa)->pid < (*pb)->pid) return -1;
  if((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

static int cmp_event(const void* A, const void* B){
  const Event* a = (const Event*)A, *b = (const Event*)B;
  if(a->time != b->time) return (a->time < b->time)? -1:1;
  if(a->pid  != b->pid ) return (a->pid  < b->pid )? -1:1;
  return 0;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // Copia local (punteros) para poder ordenar por PID al final
  Process** procs = (Process**)malloc(in->K * sizeof(Process*));
  if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  // Eventos ordenados
  Event* ev = NULL; size_t N=in->N;
  if(N){
    ev = (Event*)malloc(N*sizeof(Event)); if(!ev){ perror("malloc events"); exit(1); }
    memcpy(ev, in->events, N*sizeof(Event));
    qsort(ev, N, sizeof(Event), cmp_event);
  }

  unsigned qH = q_high_val(in->q_base), qL = q_low_val(in->q_base);
  QDyn high, low; q_init(&high,Q_HIGH); q_init(&low,Q_LOW);

  // Inicializar estado de procesos
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    p->state = READY;          // llegará cuando t>=start_time
    p->arrived=false;
    p->queue_level=Q_HIGH;
    p->remaining_quantum=0;
    p->bursts_done=0;
    p->remaining_in_burst=p->cpu_burst;
    p->io_remaining=0;
    p->last_left_cpu=0;
    p->force_max_priority=false;
    p->priority_value=0.0;
    p->interruptions=0;
    p->has_response=false;
    p->response_time=0;
    p->waiting_time=0;         // NO la usaremos tick a tick (se calculará al final)
    p->completion_time=0;
  }

  unsigned t=0; size_t done_or_dead=0; size_t iev=0;
  Process* running=NULL;

  while(done_or_dead<in->K && t < MAX_TICKS){

    // 0) EVENTO: si hay evento en este tick, corre ese PID (preempción sin "interrupción")
    if(iev<N && ev[iev].time==t){
      unsigned pid = ev[iev].pid;
      if(running && running->pid!=pid){
        // desalojar al actual (no cuenta como interrupción)
        running->state=READY;
        if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
        else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
        running=NULL;
      }
      Process* tar = find_by_pid(procs, in->K, pid);
      if(tar && tar->state!=FINISHED && tar->state!=DEAD){
        // si estaba esperando I/O, lo aborta
        if(tar->state==WAITING){ tar->io_remaining=0; tar->state=READY; }
        // sacarlo de colas si estuviera
        q_remove(&high,tar); q_remove(&low,tar);
        // forzar a CPU
        running=tar; tar->state=RUNNING;
        if(!tar->arrived && t>=tar->start_time) tar->arrived=true;
        if(!tar->has_response && t>=tar->start_time){ tar->has_response=true; tar->response_time=t - tar->start_time; }
      }
      iev++;
    }

    // 1) FIN DE I/O de este tick (se vuelven READY antes de elegir CPU)
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

    // 2) LLEGADAS iniciales (entran a High en READY con quantum de High)
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(!p->arrived && t>=p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }
    }

    // 3) DEADLINE: matar READY/WAITING atrasados
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if((p->state==READY||p->state==WAITING) && p->bursts_done<p->total_bursts && t>=p->deadline){
        p->state=DEAD; p->completion_time=t;
        q_remove(&high,p); q_remove(&low,p);
        done_or_dead++;
      }
    }

    // 4) Si CPU libre, elegir por prioridad (no se desaloja por prioridad)
    if(!running){
      recompute_priorities(&high, t);
      recompute_priorities(&low , t);
      Process* pick = pick_best(&high,&low);
      if(pick){
        q_remove((pick->queue_level==Q_HIGH)? &high:&low, pick);
        if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH)? q_high_val(in->q_base): q_low_val(in->q_base);
        if(!pick->has_response && t>=pick->start_time){ pick->has_response=true; pick->response_time=t - pick->start_time; }
        pick->state=RUNNING; running=pick;
      }
    }

    // 5) Ejecutar 1 tick (si hay RUNNING)
    if(running){
      if(running->remaining_in_burst>0) running->remaining_in_burst--;
      if(running->remaining_quantum  >0) running->remaining_quantum--;

      // fin de ráfaga
      if(running->remaining_in_burst==0){
        running->bursts_done++;
        running->last_left_cpu = t+1;

        if(running->bursts_done >= running->total_bursts){
          running->state=FINISHED;
          running->completion_time = t+1;
          running=NULL;
          done_or_dead++;
        }else{
          // va a I/O — SEGÚN ENUNCIADO: esto sí cuenta como "interrupción" en el output esperado
          running->interruptions++;

          running->state=WAITING;
          running->io_remaining = running->io_wait;     // EXACTAMENTE io_wait ticks
          running->remaining_in_burst = running->cpu_burst;
          // cede CPU (no reiniciamos quantum aquí; se recalcula al re-entrar si es necesario)
          running=NULL;
        }
      }
      // fin de quantum (raro con q grande, pero lo dejamos correcto)
      else if(running->remaining_quantum==0){
        // NO se cuenta como interrupción según los outputs esperados de tu test
        running->last_left_cpu = t+1;
        if(running->queue_level==Q_HIGH) running->queue_level=Q_LOW;
        running->state=READY;
        running->remaining_quantum = (running->queue_level==Q_HIGH)? qH:qL;
        if(running->queue_level==Q_HIGH){ if(q_index_of(&high,running)<0) q_push(&high,running); }
        else                             { if(q_index_of(&low ,running)<0) q_push(&low ,running); }
        running=NULL;
      }
    }

    // *** NO acumulamos waiting tick-a-tick; se calcula al final por fórmula ***

    t++;
  } // while

  // Salida ordenada por PID
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = (p->state==FINISHED)? "FINISHED" : (p->state==DEAD? "DEAD" :
                     (p->state==RUNNING? "RUNNING" : (p->state==READY? "READY":"WAITING")));

    unsigned turnaround = 0u;
    if(p->completion_time >= p->start_time) turnaround = p->completion_time - p->start_time;

    // Waiting según enunciado (desde t=0)
    unsigned cpu_total = p->cpu_burst * p->total_bursts;
    unsigned ios       = (p->total_bursts>0)? (p->total_bursts-1):0u;
    unsigned waiting   = (p->completion_time >= cpu_total + ios)? (p->completion_time - cpu_total - ios) : 0u;

    // Interrupciones: ya quedaron como #veces que terminó ráfaga y no terminó el programa
    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, waiting);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(procs); free(ev);
}
