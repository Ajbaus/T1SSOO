#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "scheduler.h"
#include "parse.h"

static Process* alloc_process(void) {
  Process* p = calloc(1, sizeof(Process));
  if (!p) { perror("calloc"); exit(1); }
  p->state = READY;          // se encola en High al cumplir start_time
  p->queue_level = Q_HIGH;
  return p;
}

int parse_input(const char* path, SimInput* out) {
  memset(out, 0, sizeof(*out));
  FILE* f = fopen(path, "r");
  if (!f) { perror("fopen"); return -1; }

  if (fscanf(f, "%u", &out->q_base) != 1) { fclose(f); return -1; }
  if (fscanf(f, "%zu", &out->K) != 1)      { fclose(f); return -1; }
  if (fscanf(f, "%zu", &out->N) != 1)      { fclose(f); return -1; }

  out->processes = calloc(out->K, sizeof(Process*));
  out->events    = calloc(out->N, sizeof(Event));
  if (!out->processes || (!out->events && out->N>0)) { perror("calloc"); fclose(f); return -1; }

  // K líneas de procesos
  for (size_t i = 0; i < out->K; i++) {
    char name[MAX_NAME_LEN];
    unsigned pid, t_start, t_cpu, n_bursts, io_wait, t_deadline;
    if (fscanf(f, "%63s %u %u %u %u %u %u",
               name, &pid, &t_start, &t_cpu, &n_bursts, &io_wait, &t_deadline) != 7) {
      fprintf(stderr, "Error leyendo proceso %zu\n", i);
      fclose(f); return -1;
    }
    Process* p = alloc_process();
    strncpy(p->name, name, MAX_NAME_LEN-1);
    p->pid          = pid;
    p->start_time   = t_start;
    p->cpu_burst    = t_cpu;
    p->total_bursts = n_bursts;
    p->io_wait      = io_wait;
    p->deadline     = t_deadline;
    p->remaining_in_burst = p->cpu_burst;
    out->processes[i] = p;
  }

  // N líneas de eventos
  for (size_t j = 0; j < out->N; j++) {
    unsigned pid, t_event;
    if (fscanf(f, "%u %u", &pid, &t_event) != 2) {
      fprintf(stderr, "Error leyendo evento %zu\n", j);
      fclose(f); return -1;
    }
    out->events[j].pid  = pid;
    out->events[j].time = t_event;
  }

  fclose(f);
  return 0;
}

static int cmp_pid(const void* a, const void* b) {
  Process* const* pa = a;
  Process* const* pb = b;
  if ((*pa)->pid < (*pb)->pid) return -1;
  if ((*pa)->pid > (*pb)->pid) return 1;
  return 0;
}

void free_input(SimInput* in) {
  if (!in) return;
  if (in->processes) {
    for (size_t i = 0; i < in->K; i++) free(in->processes[i]);
    free(in->processes);
  }
  free(in->events);
  memset(in, 0, sizeof(*in));
}
