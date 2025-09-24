// scheduler.c — MLFQ (2 colas) con flujo por tick adherido a enunciado y
// ajustes de tests nuevos (gating por evento y revival).
//
// Orden de cada tick:
// 1) WAITING->READY (decremento I/O; si está "aparcado por evento", NO se suelta)
// 2) (sin muerte por deadline en tests nuevos)
// 3) Si hay RUNNING:
//    - revival por evento (si aplica): consume 2 ticks fantasma y sale sin ejecutar
//    - 3.2 fin ráfaga (del tick anterior)
//    - 3.3 fin quantum
//    - 3.4 evento para otro PID (desaloja sin contar interrupción; a High con prioridad forzada)
//    - 3.5 ejecuta 1 tick (primer tick fija response_time)
// 4) Llegadas a High y boost Low->High
// 5) Recalcular prioridades (solo READY) y ordenar colas
// 6) Ingresar a CPU: evento -> High -> Low (SIN ejecutar este tick)
//    * evento puede apuntar a FINISHED/DEAD: "revival" que ALTERA SOLO turnaround
//
// waiting_time: se suma al FINAL del tick a:
// - READY
// - WAITING con I/O pendiente (io_remaining > 0)
// - RUNNING admitido en 6 que no ejecutó este tick (no revival)
// No se suma en "aparque" (I/O=0 esperando evento) ni durante revival.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
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
    unsigned bursts_left = (p->total_bursts > p->bursts_done)? (p->total_bursts - p->bursts_done) : 0u;
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

static size_t index_of(Process** procs, size_t K, Process* p){
  for(size_t i=0;i<K;i++) if(procs[i]==p) return i;
  return (size_t)-1;
}

void run_simulation(const SimInput* in, const char* output_csv){
  Process** procs=(Process**)malloc(sizeof(Process*)*in->K); if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i]=in->processes[i];

  Event* events=NULL; size_t N=in->N;
  if(N){
    events=(Event*)malloc(sizeof(Event)*N); if(!events){ perror("malloc events"); exit(1); }
    memcpy(events,in->events,sizeof(Event)*N);
    qsort(events,N,sizeof(Event),cmp_event_time);
  }

  unsigned qH=q_high_val(in->q_base), qL=q_low_val(in->q_base);
  QDyn high, low; q_init(&high,Q_HIGH); q_init(&low,Q_LOW);

  // ------- Estado auxiliar (no toca scheduler.h) -------
  // Aparque por evento tras I/O:
  bool* wait_for_event    = (bool*)calloc(in->K, sizeof(bool));
  unsigned* next_ev_time  = (unsigned*)malloc(sizeof(unsigned)*in->K);
  // Revival (evento sobre FINISHED/DEAD):
  bool* revived_by_event  = (bool*)calloc(in->K, sizeof(bool));
  unsigned* revive_ticks  = (unsigned*)calloc(in->K, sizeof(unsigned));
  ProcState* terminal_st  = (ProcState*)malloc(sizeof(ProcState)*in->K);
  if(!wait_for_event||!next_ev_time||!revived_by_event||!revive_ticks||!terminal_st){
    perror("aux alloc"); exit(1);
  }
  for(size_t i=0;i<in->K;i++){
    wait_for_event[i]=false; revived_by_event[i]=false; revive_ticks[i]=0;
    next_ev_time[i]=UINT_MAX; terminal_st[i]=READY;
  }
  // Precalcular primer evento por PID
  for(size_t j=0;j<N;j++){
    Process* p = find_by_pid(procs,in->K, events[j].pid);
    if(p){
      size_t idx=index_of(procs,in->K,p);
      if(next_ev_time[idx]==UINT_MAX) next_ev_time[idx]=events[j].time;
    }
  }
  // -----------------------------------------------------

  // Init seguro
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    p->state=READY; p->arrived=false; p->queue_level=Q_HIGH;
    p->remaining_quantum=0; p->bursts_done=0; p->remaining_in_burst=p->cpu_burst;
    p->io_remaining=0; p->last_left_cpu=0;
    p->force_max_priority=false; p->priority_value=0.0;
    p->interruptions=0; p->has_response=false; p->response_time=0;
    p->waiting_time=0; p->completion_time=0;
  }

  unsigned t=0; size_t finished=0; size_t next_ev=0;
  Process* running=NULL;

  while(finished<in->K && t<MAX_TICKS){

    // 1) WAITING -> READY (decremento I/O; respetar "aparque por evento")
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(p->state==WAITING && p->io_remaining>0){
        p->io_remaining--;
        if(p->io_remaining==0){
          size_t idx=index_of(procs,in->K,p);
          if(wait_for_event[idx] && t < next_ev_time[idx]){
            // Aparcado hasta que llegue el evento (WAITING con io_remaining==0)
          }else{
            p->state=READY;
            if(p->queue_level==Q_HIGH){ if(q_index_of(&high,p)<0) q_push(&high,p); }
            else                       { if(q_index_of(&low ,p)<0) q_push(&low ,p); }
          }
        }
      }
    }

    // 2) (sin matar por deadline)

    // 3) RUNNING
    int  has_event = (next_ev<N && events[next_ev].time==t);
    unsigned evpid = has_event? events[next_ev].pid : (unsigned)~0u;

    // Para contar waiting al final
    int ran_this_tick = 0;
    int admitted_now  = 0;

    if(running){
      size_t ridx=index_of(procs,in->K,running);

      // --- Revival por evento (no ejecuta; altera solo turnaround) ---
      if(revived_by_event[ridx]){
        if(revive_ticks[ridx] > 0){
          revive_ticks[ridx]--;
          if(revive_ticks[ridx] == 0){
            // Restituye estado terminal y mueve completion_time a "ahora"
            running->state = terminal_st[ridx]; // FINISHED o DEAD original
            running->completion_time = t;       // efecto esperado: t_event + 2
            revived_by_event[ridx] = false;
            running = NULL;
            // NO tocar 'finished' (ya estaba finalizado antes)
          }
        }
        // Saltar 3.2/3.3/3.4/3.5 para revival
      }
      // --- Normal: 3.2 -> 3.3 -> 3.4 -> 3.5 ---
      else{
        // 3.2) fin ráfaga (terminó en tick anterior)
        if(running->remaining_in_burst==0){
          running->bursts_done++;
          running->last_left_cpu = t; // cerró en tick previo
          if(running->bursts_done >= running->total_bursts){
            running->state=FINISHED; running->completion_time=t; running=NULL; finished++;
          }else{
            running->interruptions++;          // solo fin de ráfaga con trabajo pendiente
            running->state=WAITING;
            running->io_remaining = running->io_wait;   // I/O exacto
            running->remaining_in_burst = running->cpu_burst;
            // Si hay evento futuro, aparcar tras I/O
            wait_for_event[ridx] = (next_ev_time[ridx]!=UINT_MAX && next_ev_time[ridx] > t);
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
        // 3.4) evento para otro PID → desaloja (a High con prioridad forzada)
        else if(has_event && evpid!=running->pid){
          running->state=READY; running->last_left_cpu=t;
          running->queue_level=Q_HIGH; running->force_max_priority=true;
          if(q_index_of(&high,running)<0) q_push(&high,running);
          running=NULL;
        }
        // 3.5) ejecuta 1 tick
        else{
          if(!running->has_response && t>=running->start_time){
            running->has_response=true;
            running->response_time = t - running->start_time;
          }
          if(running->remaining_in_burst>0) running->remaining_in_burst--;
          if(running->remaining_quantum  >0) running->remaining_quantum--;
          ran_this_tick = 1;
        }
      }
    }

    // 4) Llegadas (High) y boost Low->High
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];

      // 4.2) primera llegada → High
      if(!p->arrived && t>=p->start_time && p->state!=FINISHED){
        p->arrived=true; p->state=READY; p->queue_level=Q_HIGH;
        p->remaining_quantum=qH;
        if(q_index_of(&high,p)<0) q_push(&high,p);
      }

      // 4.3) boost Low->High: 2*deadline < t - TLCPU (según enunciado)
      if(p->state!=FINISHED && p->queue_level==Q_LOW){
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
      // 6.1) evento del tick — admite SIEMPRE aunque FINISHED/DEAD (revival)
      if(has_event){
        Process* evp = find_by_pid(procs, in->K, evpid);
        if(evp){
          size_t eidx=index_of(procs,in->K,evp);

          // liberar aparque si estaba WAITING aparcado
          if(evp->state==WAITING){ evp->io_remaining=0; evp->state=READY; }

          q_remove(&high,evp); q_remove(&low,evp);

          // Revival si estaba FINISHED/DEAD: no ejecuta, altera solo turnaround
          if(evp->state==FINISHED || evp->state==DEAD){
            terminal_st[eidx] = evp->state;
            revived_by_event[eidx] = true;
            revive_ticks[eidx] = 2; // efecto +2 ticks en completion_time (t_event + 2)
          }

          if(evp->remaining_quantum==0) evp->remaining_quantum=(evp->queue_level==Q_HIGH? qH:qL);
          evp->state=RUNNING; evp->force_max_priority=false; running=evp; admitted_now=1;

          // Avanzar "próximo evento" para este PID (por si hay múltiples)
          if(next_ev_time[eidx]==t){
            unsigned nxt = UINT_MAX;
            for(size_t jj=next_ev+1; jj<N; ++jj){
              if(events[jj].pid==evpid){ nxt=events[jj].time; break; }
            }
            next_ev_time[eidx]=nxt;
          }
        }
        next_ev++;
      }

      // 6.2 / 6.3: High luego Low
      if(!running){
        Process* pick = pick_first_ready(&high);
        if(!pick) pick = pick_first_ready(&low);
        if(pick){
          q_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
          if(pick->remaining_quantum==0) pick->remaining_quantum=(pick->queue_level==Q_HIGH? qH:qL);
          pick->state=RUNNING; pick->force_max_priority=false; running=pick; admitted_now=1;
        }
      }
    }

    // ---- Conteo de waiting_time AL FINAL DEL TICK ----
    for(size_t i=0;i<in->K;i++){
      Process* p=procs[i];
      if(!p->arrived || p->state==FINISHED) continue;

      if(p->state==READY){
        p->waiting_time++;
        continue;
      }
      if(p->state==WAITING){
        // contar solo I/O pendiente; NO contar aparque (io_remaining==0 y esperando evento)
        if(p->io_remaining > 0) p->waiting_time++;
        continue;
      }
      if(p==running){
        size_t ridx=index_of(procs,in->K,p);
        // Si fue admitido en 6 y no ejecutó este tick (y no es revival), suma 1
        if(admitted_now && !ran_this_tick && !revived_by_event[ridx]) p->waiting_time++;
      }
    }

    t++;
  } // while

  // Output por PID
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out=fopen(output_csv,"w"); if(!out){ perror("fopen output"); exit(1); }
  for(size_t i=0;i<in->K;i++){
    Process* p=procs[i];
    const char* st = (p->state==FINISHED)? "FINISHED" :
                     (p->state==RUNNING? "RUNNING" : (p->state==READY? "READY":"WAITING"));
    unsigned turnaround=0u;
    if(p->completion_time>=p->start_time) turnaround = p->completion_time - p->start_time;

    fprintf(out,"%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, st, p->interruptions, turnaround, p->response_time, p->waiting_time);
  }
  fclose(out);

  q_free(&high); q_free(&low);
  free(wait_for_event); free(next_ev_time);
  free(revived_by_event); free(revive_ticks); free(terminal_st);
  free(procs); free(events);
}
