#include <stdio.h>
#include <stdlib.h>
#include "scheduler.h"
#include "parse.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "Uso: %s <input_file> <output_file>\n", argv[0]);
    return 1;
  }

  SimInput input;
  if (parse_input(argv[1], &input) != 0) {
    fprintf(stderr, "Error al leer el input.\n");
    return 1;
  }

  run_simulation(&input, argv[2]);
  free_input(&input);
  return 0;
}
