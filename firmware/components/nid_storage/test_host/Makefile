# Tests sur PC des calculs d'espace de nid_storage : make (ou make test)
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

test: test_nid_storage_calc
	./test_nid_storage_calc

test_nid_storage_calc: test_nid_storage_calc.c ../nid_storage_calc.h
	$(CC) $(CFLAGS) -I.. -o $@ test_nid_storage_calc.c

clean:
	rm -f test_nid_storage_calc

.PHONY: test clean
