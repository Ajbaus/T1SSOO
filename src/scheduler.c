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
    // Modelo de "consume 1 tick" antes de chequear 3.1..3.5.
    // (coherente con quantum y bursts como tiempos restantes discretos)
    if(running){
      // Consume trabajo del tick
      if(running->remaining_in_burst > 0) running->remaining_in_burst--;
      if(running->remaining_quantum   > 0) running->remaining_quantum--;

      bool reached_deadline = (t >= running->deadline && running->bursts_done < running->total_bursts);
      bool burst_finished   = (running->remaining_in_burst == 0);
      bool quantum_over     = (running->remaining_quantum   == 0);

      // 3.1: Alcanzó su deadline (mata el proceso)
      if(reached_deadline){
        running->state = DEAD;
        running->completion_time = t;
        running->last_left_cpu = t;
        // se baja de CPU
        running = NULL;
        finished_or_dead++;
      }
      else if(burst_finished){
        // 3.2: Terminó su CPU burst
        running->bursts_done++;
        running->last_left_cpu = t;

        if(running->bursts_done >= running->total_bursts){
          // terminó todo → FINISHED (consideración especial: si coincide con fin de quantum, igual FINISHED)
          running->state = FINISHED;
          running->completion_time = t;
          running = NULL;
          finished_or_dead++;
        }else{
          // pasa a WAITING por io_wait (cede CPU → NO reinicia quantum)
          running->state = WAITING;
          running->io_remaining = running->io_wait;
          // preparar próxima ráfaga
          running->remaining_in_burst = running->cpu_burst;
          // reencolar al volver del WAITING en su misma cola (lo hará el Paso 1)
          running = NULL;
        }
      }
      else if(quantum_over){
        // 3.3: Se acabó su quantum (si justo terminó ráfaga, 3.2 ya lo tomó como "cede")
        // Baja a Low si estaba en High; si ya estaba en Low, se mantiene
        if(running->queue_level == Q_HIGH){
          running->queue_level = Q_LOW;
        }
        // sale de CPU y vuelve READY en su cola correspondiente
        running->state = READY;
        running->last_left_cpu = t;

        // NO reinicia quantum si "cedió"; acá NO cedió, así que lo preparamos con quantum de su cola nueva
        running->remaining_quantum = (running->queue_level==Q_HIGH)? qH : qL;

        // encolar
        if(running->queue_level==Q_HIGH){
          if(queue_index_of(&high, running)<0) queue_push_back(&high, running);
        }else{
          if(queue_index_of(&low, running) <0) queue_push_back(&low, running);
        }
        running = NULL;
      }
      else{
        // 3.4: si ocurre evento con PID distinto -> se interrumpe en 6.1 (aquí sólo anotamos el estado)
        // 3.5: continúa normal (nada que hacer acá)
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

    // ---- Paso 6: ingresar proceso a CPU ----
    // ¿hay evento en este tick?
    Process* event_proc = NULL;
    if(next_event_idx < N && events[next_event_idx].time == t){
      // puede haber múltiples eventos en el mismo tick; tomamos el primero en orden (ya ordenados)
      unsigned ev_pid = events[next_event_idx].pid;
      event_proc = find_by_pid(procs, in->K, ev_pid);
      // avanzar todos los eventos de mismo tiempo (pero mantenemos el primero encontrado como “activo”)
      // Regla: si hay varios, el orden por PID ya es determinista.
      // Sólo aplicamos uno por tick (6.1). Los demás suceden en ticks siguientes iguales → se aplican secuencialmente.
      // Si quieres aplicar TODOS los del mismo tick, descomenta el while y maneja colisiones.
      next_event_idx++;
    }

    if(event_proc){
      // Si hay alguien en CPU y NO es el del evento, interrumpirlo (3.4):
      if(running && running->pid != event_proc->pid){
        running->interruptions++;
        running->state = READY;
        running->last_left_cpu = t;
        // Al ser interrumpido: va a High con máxima prioridad temporal, ignorando fórmula
        running->queue_level = Q_HIGH;
        running->force_max_priority = true;
        // NO reiniciamos su quantum (no “cedió”), conserva remaining_quantum
        if(queue_index_of(&high, running)<0) queue_push_back(&high, running);
        running = NULL;
      }

      // El proceso del evento debe entrar a CPU o continuar, sin importar su estado
      if(event_proc->state == DEAD || event_proc->state == FINISHED){
        // Si está muerto o terminado, no hacemos nada (caso borde)
      } else {
        // Si está en WAITING, lo sacamos de WAITING (no cancelamos su io_remaining; WAITING sólo aplica al terminar burst)
        // Pero la regla dice "ingresa a CPU sin importar si está WAITING"; lo ponemos RUNNING.
        // Lo removemos de la cola donde esté.
        queue_remove(&high, event_proc);
        queue_remove(&low,  event_proc);
        // Asignar quantum acorde a su cola actual:
        if(event_proc->queue_level==Q_HIGH && event_proc->remaining_quantum==0) event_proc->remaining_quantum=qH;
        if(event_proc->queue_level==Q_LOW  && event_proc->remaining_quantum==0) event_proc->remaining_quantum=qL;

        // Primera respuesta
        if(!event_proc->has_response){
          event_proc->has_response = true;
          event_proc->response_time = (t >= event_proc->start_time)? (t - event_proc->start_time) : 0u;
        }

        event_proc->state = RUNNING;
        running = event_proc;
        // Si tenía flag de “máxima prioridad”, se limpia al entrar a CPU (ya “logró ingresar nuevamente a ejecutar”)
        if(running->force_max_priority) running->force_max_priority = false;
      }
    }

    // 6.2 y 6.3: si no hay proceso por evento en CPU, tomar de High o Low
    if(!running){
      Process* pick = queue_pick_ready(&high);
      if(!pick) pick = queue_pick_ready(&low);

      if(pick){
        // sacarlo de su cola
        queue_remove((pick->queue_level==Q_HIGH)? &high : &low, pick);

        // set RUNNING y quantum si corresponde (si venía de "ceder", mantiene remaining_quantum)
        if(pick->remaining_quantum==0){
          pick->remaining_quantum = (pick->queue_level==Q_HIGH)? qH : qL;
        }
        // primera respuesta
        if(!pick->has_response){
          pick->has_response = true;
          pick->response_time = (t >= pick->start_time)? (t - pick->start_time) : 0u;
        }
        pick->state = RUNNING;
        // si tenía máxima prioridad temporal, la apagamos al reingresar
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
