# Tests sur PC des calculs purs de nid_capture : make (ou make test)
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

test: test_nid_capture_calc
	./test_nid_capture_calc

test_nid_capture_calc: test_nid_capture_calc.c ../nid_capture_calc.h
	$(CC) $(CFLAGS) -I.. -o $@ test_nid_capture_calc.c

clean:
	rm -f test_nid_capture_calc

.PHONY: test clean
