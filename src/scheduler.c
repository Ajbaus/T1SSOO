#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scheduler.h"

// Nota: En el Paso 2 implementamos el flujo por tick (1..6) y toda la MLFQ.
// Por ahora dejamos un stub que confirma que el pipeline funciona.

static int cmp_pid_ptr(const void* a, const void* b) {
  const Process* const* pa = a;
  const Process* const* pb = b;
  if ((*pa)->pid < (*pb)->pid) return -1;
  if ((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

void run_simulation(const SimInput* in, const char* output_csv) {
  unsigned q_high = 2 * in->q_base;
  unsigned q_low  = in->q_base;
  (void)q_high; (void)q_low; // se usarán en el Paso 2

  // ordenar procesos por PID para respetar el orden de salida
  Process** ordered = malloc(sizeof(Process*) * in->K);
  if (!ordered) { perror("malloc"); exit(1); }
  for (size_t i = 0; i < in->K; i++) ordered[i] = in->processes[i];
  qsort(ordered, in->K, sizeof(Process*), cmp_pid_ptr);

  FILE* out = fopen(output_csv, "w");
  if (!out) { perror("fopen output"); free(ordered); exit(1); }

  // Placeholder: estado READY y métricas en 0. (Se reemplaza en Paso 2)
  for (size_t i = 0; i < in->K; i++) {
    const Process* p = ordered[i];
    fprintf(out, "%s,%u,%s,%u,%u,%u,%u\n",
            p->name, p->pid, "READY", 0u, 0u, 0u, 0u);
  }

  fclose(out);
  free(ordered);
}
