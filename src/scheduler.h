#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_NAME_LEN 64

typedef enum {
  RUNNING, READY, WAITING, FINISHED, DEAD
} ProcState;

typedef enum {
  Q_HIGH = 0, Q_LOW = 1
} QueueLevel;

typedef struct {
  unsigned pid;
  unsigned time;   // tick del evento
} Event;

typedef struct Process {
  char     name[MAX_NAME_LEN];
  unsigned pid;
  ProcState state;

  // Datos del input
  unsigned start_time;    // T_INICIO
  unsigned cpu_burst;     // T_CPU_BURST (duración de cada ráfaga)
  unsigned total_bursts;  // N_BURSTS
  unsigned io_wait;       // IO_WAIT
  unsigned deadline;      // T_DEADLINE

  // Estado de simulación
  unsigned bursts_done;         // ráfagas completadas
  unsigned remaining_in_burst;  // tiempo restante de la ráfaga actual
  unsigned remaining_quantum;   // tiempo restante del quantum actual
  unsigned last_left_cpu;       // TLCPU (último tick en que salió de CPU)
  QueueLevel queue_level;       // High o Low
  bool force_max_priority;      // por interrupción de evento (ignora fórmula hasta reingreso)
  double priority_value;        // prioridad calculada

  // Métricas
  unsigned interruptions;  // número de interrupciones
  bool     has_response;   // si ya midió response time
  unsigned response_time;  // primer ingreso a CPU - start_time
  unsigned waiting_time;   // tiempo en READY o WAITING
  unsigned completion_time;// último tick en que terminó
} Process;

typedef struct {
  unsigned q_base;  // q
  size_t   K;       // procesos
  size_t   N;       // eventos
  Process** processes; // arreglo de punteros a Process (tamaño K)
  Event*    events;    // arreglo de eventos (tamaño N)
} SimInput;

void run_simulation(const SimInput* in, const char* output_csv);

#endif
