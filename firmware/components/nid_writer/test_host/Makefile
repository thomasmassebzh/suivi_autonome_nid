# Tests sur PC des copies du tampon circulaire de nid_writer : make (ou make test)
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

test: test_nid_writer_ring
	./test_nid_writer_ring

test_nid_writer_ring: test_nid_writer_ring.c ../nid_writer_ring.h
	$(CC) $(CFLAGS) -I.. -o $@ test_nid_writer_ring.c

clean:
	rm -f test_nid_writer_ring

.PHONY: test clean
