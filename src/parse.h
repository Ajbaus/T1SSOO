#ifndef PARSE_H
#define PARSE_H
#include "scheduler.h"

int parse_input(const char* path, SimInput* out);
void free_input(SimInput* in);

#endif
