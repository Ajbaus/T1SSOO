CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -O2

all: DCCambios

DCCambios: main.o scheduler.o parse.o
	$(CC) $(CFLAGS) -o $@ $^

main.o: main.c scheduler.h parse.h
	$(CC) $(CFLAGS) -c $<

scheduler.o: scheduler.c scheduler.h
	$(CC) $(CFLAGS) -c $<

parse.o: parse.c parse.h scheduler.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o DCCambios
