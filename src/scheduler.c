#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "scheduler.h"

static inline unsigned q_high_val(unsigned q){ return 2u*q; }
static inline unsigned q_low_val (unsigned q){ return q;    }

static void queue_init(Queue* q, QueueLevel level){
  q->level = level; q->arr=NULL; q->size=0; q->cap=0;
}
static void queue_free(Queue* q){
  free(q->arr); q->arr=NULL; q->size=q->cap=0;
}
static void queue_reserve(Queue* q, size_t need){
  if(q->cap>=need) return;
  size_t nc = q->cap? q->cap*2:8;
  if(nc<need) nc=need;
  q->arr = realloc(q->arr, nc*sizeof(Process*));
  if(!q->arr){ perror("realloc"); exit(1); }
  q->cap=nc;
}
static int queue_index_of(Queue* q, Process* p){
  for(size_t i=0;i<q->size;i++) if(q->arr[i]==p) return (int)i;
  return -1;
}
static void queue_erase_at(Queue* q, size_t idx){
  if(idx>=q->size) return;
  for(size_t i=idx+1;i<q->size;i++) q->arr[i-1]=q->arr[i];
  q->size--;
}
static void queue_remove(Queue* q, Process* p){
  int i = queue_index_of(q, p);
  if(i>=0) queue_erase_at(q, (size_t)i);
}
static void queue_push_back(Queue* q, Process* p){
  queue_reserve(q, q->size+1);
  q->arr[q->size++] = p;
}

// ordenar por prioridad descendente (mayor prioridad primero)
// prioridad = 1/(deadline - t) + (bursts restantes)
// en empate: menor PID primero (mayor prioridad)
static int cmp_priority(const void* a, const void* b){
  Process* pa = *(Process* const*)a;
  Process* pb = *(Process* const*)b;

  double A = pa->priority_value;
  double B = pb->priority_value;

  if (A > B) return -1;
  if (A < B) return  1;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return  1;
  return 0;
}
static void queue_sort_by_priority(Queue* q){
  if(q->size>1) qsort(q->arr, q->size, sizeof(Process*), cmp_priority);
}

// recomputa prioridad para procesos en READY/WAITING dentro de una cola
static void recompute_priorities_tick(Queue* q, unsigned t){
  for(size_t i=0;i<q->size;i++){
    Process* p = q->arr[i];

    if(p->force_max_priority){
      p->priority_value = INFINITY; // ignora fórmula hasta reingresar a CPU
      continue;
    }
    // bursts restantes = total - completadas
    unsigned bursts_left = (p->total_bursts > p->bursts_done) ? (p->total_bursts - p->bursts_done) : 0u;

    // Tuntil = deadline - t. Si ya no hay margen, el paso 2/3.1 se encarga del DEAD
    if(p->deadline > t){
      double until = (double)(p->deadline - t);
      p->priority_value = 1.0/until + (double)bursts_left;
    }else{
      // no debería usarse (se marcará DEAD en 2/3.1), pero evitamos NaN
      p->priority_value = 1e30 + (double)bursts_left;
    }
  }
}

// busca el primer READY en la cola (está ordenada por prioridad)
static Process* queue_pick_ready(Queue* q){
  for(size_t i=0;i<q->size;i++){
    if(q->arr[i]->state == READY) return q->arr[i];
  }
  return NULL;
}

static int cmp_pid_ptr(const void* a, const void* b){
  const Process* const* pa = a;
  const Process* const* pb = b;
  if ((*pa)->pid < (*pb)->pid) return -1;
  if ((*pa)->pid > (*pb)->pid) return  1;
  return 0;
}

static int cmp_event_time(const void* a, const void* b){
  const Event* ea = (const Event*)a;
  const Event* eb = (const Event*)b;
  if (ea->time < eb->time) return -1;
  if (ea->time > eb->time) return  1;
  // si empatan por tiempo, desempata por PID menor
  if (ea->pid  < eb->pid ) return -1;
  if (ea->pid  > eb->pid ) return  1;
  return 0;
}

static Process* find_by_pid(Process** procs, size_t K, unsigned pid){
  for(size_t i=0;i<K;i++) if(procs[i]->pid == pid) return procs[i];
  return NULL;
}

void run_simulation(const SimInput* in, const char* output_csv){
  // Clonar punteros para ordenar por PID al final
  Process** procs = malloc(sizeof(Process*) * in->K);
  if(!procs){ perror("malloc"); exit(1); }
  for(size_t i=0;i<in->K;i++) procs[i] = in->processes[i];

  // Ordenamos eventos por tiempo (el input no garantiza orden)
  Event* events = NULL;
  size_t N = in->N;
  if(N>0){
    events = malloc(sizeof(Event)*N);
    if(!events){ perror("malloc events"); exit(1); }
    memcpy(events, in->events, sizeof(Event)*N);
    qsort(events, N, sizeof(Event), cmp_event_time);
  }

  unsigned qH = q_high_val(in->q_base);
  unsigned qL = q_low_val (in->q_base);

  Queue high, low; queue_init(&high, Q_HIGH); queue_init(&low, Q_LOW);

  // Estado global
  unsigned t = 0;
  Process* running = NULL;
  size_t   finished_or_dead = 0;
  size_t   next_event_idx = 0;

 

  // Bucle principal (termina cuando todos FINISHED o DEAD)
  while (finished_or_dead < in->K) {

    // ---- Paso 1: actualizar WAITING -> READY si termina IO ----
    for(size_t i=0;i<in->K;i++){
      Process* p = procs[i];
      if(p->state == WAITING && p->io_remaining > 0){
        p->io_remaining--;
        if(p->io_remaining == 0){
          p->state = READY;
          // vuelve a la MISMA cola en la que estaba, conservando remaining_quantum (cede CPU)
          // Si estaba en ninguna, por seguridad reencolar en su queue_level
          if(p->queue_level == Q_HIGH){
            if(queue_index_of(&high, p) < 0) queue_push_back(&high, p);
          }else{
            if(queue_index_of(&low, p)  < 0) queue_push_back(&low, p);
          }
        }
      }
    }

    // ---- Paso 2: marcar DEAD en colas si llegó deadline y no completó ----
    for(size_t i=0;i<in->K;i++){
      Process* p = procs[i];
      if((p->state == READY || p->state == WAITING) && p->bursts_done < p->total_bursts){
        if(t >= p->deadline){
          p->state = DEAD;
          p->completion_time = t;
          // retirar de colas
          queue_remove(&high, p); queue_remove(&low, p);
          finished_or_dead++;
        }
      }
    }

    // ---- Paso 3: si hay RUNNING, actualizar en orden 3.1→3.5 ----
    if (running) {
      // 3.1) Si alcanzó su deadline, muere de inmediato (no consume el tick)
      if (t >= running->deadline && running->bursts_done < running->total_bursts) {
        running->state = DEAD;
        running->completion_time = t;   // muere al comienzo del tick
        running = NULL;
      } else {
        // 3.4) Si hay evento en este tick y NO es su PID, interrumpir antes de correr
        if (next_event_idx < N && events[next_event_idx].time == t) {
          unsigned ev_pid = events[next_event_idx].pid;
          if (running->pid != ev_pid) {
            running->interruptions++;          // cuenta interrupción por evento
            running->state = READY;
            running->last_left_cpu = t;        // abandona ya
            running->queue_level = Q_HIGH;     // sube a High
            running->force_max_priority = true;
            if (queue_index_of(&high, running) < 0) queue_push_back(&high, running);
            running = NULL;
          }
          // avanzamos el índice de eventos (si hay múltiples en el mismo tick, avanza de a uno por tick)
          next_event_idx++;
        }

        // si sigue en CPU, ahora sí ejecuta (3.5) y luego aplicamos 3.2 / 3.3
        if (running) {
          running->remaining_in_burst--;
          running->remaining_quantum--;

          // 3.2) terminó ráfaga al cerrar el tick
          if (running->remaining_in_burst == 0) {
            running->bursts_done++;
            running->last_left_cpu = t + 1;      // salió al final del tick
            if (running->bursts_done >= running->total_bursts) {
              running->state = FINISHED;
              running->completion_time = t + 1;  // terminó al final del tick
            } else {
              running->state = WAITING;          // cede CPU → NO reinicia quantum
              running->io_remaining = running->io_wait;
              running->remaining_in_burst = running->cpu_burst;
            }
            running = NULL;
          }
          // 3.3) se acabó el quantum al cerrar el tick (y NO terminó ráfaga)
          else if (running->remaining_quantum == 0) {
            running->interruptions++;             // cuenta interrupción por fin de quantum
            running->last_left_cpu = t + 1;       // salió al final del tick
            if (running->queue_level == Q_HIGH) running->queue_level = Q_LOW;
            running->state = READY;
            running->remaining_quantum = (running->queue_level==Q_HIGH) ? qH : qL;
            if (running->queue_level==Q_HIGH) {
              if (queue_index_of(&high, running) < 0) queue_push_back(&high, running);
            } else {
              if (queue_index_of(&low, running) < 0) queue_push_back(&low, running);
            }
            running = NULL;
          }
          // 3.5) sigue RUNNING; no hacemos nada más
        }
      }
    }

    // ---- Paso 4: ingresar a colas según corresponda ----
    for(size_t i=0;i<in->K;i++){
      Process* p = procs[i];

      // 4.2: si ya llegó su tiempo de inicio, ingresa a High una sola vez
      if(!p->arrived && t >= p->start_time && p->state!=DEAD && p->state!=FINISHED){
        p->arrived = true;
        p->state = READY;
        p->queue_level = Q_HIGH;
        // al entrar por primera vez, se asigna quantum de High
        p->remaining_quantum = qH;
        if(queue_index_of(&high, p) < 0) queue_push_back(&high, p);
      }

      // 4.3: Para cada proceso en Low, revisar condición de subir a High:
      // if (2*deadline < t - TLCPU) => sube a High
      if(p->state!=DEAD && p->state!=FINISHED && p->queue_level==Q_LOW){
        // Ojo: TLCPU es tick en que salió por última vez de CPU
        // Si nunca entró a CPU, last_left_cpu estará 0; la condición se cumple rara vez ahí, está bien.
        long diff = (long)t - (long)p->last_left_cpu;
        if( (2u*p->deadline) < (unsigned)((diff<0)?0:diff) ){
          // subir a High, conservar estado (READY/WAITING)
          queue_remove(&low, p);
          p->queue_level = Q_HIGH;
          if(p->state != WAITING){ // si READY, se encola en High
            if(queue_index_of(&high, p)<0) queue_push_back(&high, p);
          }
          // el quantum en High cuando vuelva a ejecutar será 2q,
          // pero si cedió antes, mantenemos remaining_quantum si corresponde.
          if(p->remaining_quantum > qH) p->remaining_quantum = qH; // cota
        }
      }
    }

    // 4.1: si un proceso salió recién de CPU (ya manejado en 3.x), acá no hay acción adicional.

    // ---- Paso 5: actualizar prioridades (en colas) ----
    recompute_priorities_tick(&high, t);
    recompute_priorities_tick(&low,  t);

    // ordenar por prioridad dentro de cada cola (READY/WAITING pueden convivir; sólo se elige READY)
    queue_sort_by_priority(&high);
    queue_sort_by_priority(&low);

    // ---- Paso 6: ingresar proceso a la CPU ----
    Process* pick = NULL;

    // 6.1) proceso del evento (si queda alguno con time == t que aún no fue atendido)
    Process* evt_pick = NULL;
    if (next_event_idx > 0) {
      // el evento del tick t ya se avanzó; recuperamos el PID aplicado en t (si no fue el mismo running)
      unsigned prev_idx = next_event_idx - 1;
      if (events[prev_idx].time == t) {
        evt_pick = find_by_pid(procs, in->K, events[prev_idx].pid);
      }
    }
    if (!running && evt_pick && evt_pick->state != DEAD && evt_pick->state != FINISHED) {
      // lo removemos de su cola si estaba
      queue_remove(&high, evt_pick);
      queue_remove(&low,  evt_pick);
      if (evt_pick->remaining_quantum == 0) {
        evt_pick->remaining_quantum = (evt_pick->queue_level==Q_HIGH)? qH : qL;
      }
      if (!evt_pick->has_response) {
        evt_pick->has_response = true;
        evt_pick->response_time = (t >= evt_pick->start_time) ? (t - evt_pick->start_time) : 0u;
      }
      evt_pick->state = RUNNING;
      evt_pick->force_max_priority = false;   // ya reingresó
      running = evt_pick;
    }

    // 6.2 / 6.3 si aún no hay running
    if (!running) {
      pick = queue_pick_ready(&high);
      if(!pick) pick = queue_pick_ready(&low);
      if (pick) {
        queue_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);
        if (pick->remaining_quantum == 0)
          pick->remaining_quantum = (pick->queue_level==Q_HIGH) ? qH : qL;
        if (!pick->has_response) {
          pick->has_response = true;
          pick->response_time = (t >= pick->start_time) ? (t - pick->start_time) : 0u;
        }
        pick->state = RUNNING;
        pick->force_max_priority = false;
        running = pick;
      }
    }


    // ---- Acumular waiting time para quienes están READY o WAITING (desde que “arrived”) ----
    for(size_t i=0;i<in->K;i++){
      Process* p = procs[i];
      if(!p->arrived) continue;
      if(p->state == READY || p->state == WAITING){
        p->waiting_time++;
      }
    }

    // avanzar tick
    t++;
  } // while !all_done

  // ---- Escribir salida ordenada por PID ----
  qsort(procs, in->K, sizeof(Process*), cmp_pid_ptr);
  FILE* out = fopen(output_csv, "w");
  if(!out){ perror("fopen output"); exit(1); }

  for(size_t i=0;i<in->K;i++){
    Process* p = procs[i];
    const char* st =
      p->state==FINISHED? "FINISHED":
      p->state==DEAD?     "DEAD":
      p->state==RUNNING?  "RUNNING":
      p->state==READY?    "READY":"WAITING";

    unsigned turnaround = 0u;
    if(p->arrived){
      // turnaround = completion_time - start_time
      // (si quedó DEAD, completion_time guarda el tick en que murió)
      if(p->completion_time >= p->start_time)
        turnaround = p->completion_time - p->start_time;
    }

    fprintf(out, "%s,%u,%s,%u,%u,%u,%u\n",
      p->name, p->pid, st,
      p->interruptions,
      turnaround,
      p->response_time,
      p->waiting_time
    );
  }

  fclose(out);
  queue_free(&high); queue_free(&low);
  free(procs);
  free(events);
}
