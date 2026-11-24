# Tests sur PC de la correction de dérive de nid_clock : make (ou make test)
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

test: test_nid_clock_calc
	./test_nid_clock_calc

test_nid_clock_calc: test_nid_clock_calc.c ../nid_clock_calc.h
	$(CC) $(CFLAGS) -I.. -o $@ test_nid_clock_calc.c

clean:
	rm -f test_nid_clock_calc

.PHONY: test clean
