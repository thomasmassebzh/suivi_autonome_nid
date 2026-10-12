# Tests sur PC du calcul jour/nuit de nid_ir (nid_ir_sun.c) — ne fait PAS partie de la
# compilation ESP-IDF.
#
#   make            compile et exécute les tests
#   make sanitize   idem avec AddressSanitizer + UndefinedBehaviorSanitizer
#   make refs       régénère refs_data.h avec astral (pip install astral), à ne faire que
#                   pour changer les lieux ou les dates de gen_refs.py
#   make clean
#
# Seul nid_ir_sun.c est compilé : il ne dépend d'ESP-IDF que par les codes d'erreur,
# fournis par stubs/esp_err.h, et par nid_config.h (en-tête pur).

CC      ?= cc
CFLAGS  ?= -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wshadow -Wdouble-promotion
INCLUDES = -Istubs -I../include -I../../nid_config/include
SOURCES  = ../nid_ir_sun.c test_nid_ir.c
PYTHON  ?= python3

.PHONY: test sanitize refs clean

test: test_nid_ir
	./test_nid_ir

test_nid_ir: $(SOURCES) refs_data.h ../include/nid_ir.h stubs/esp_err.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SOURCES) -lm

sanitize: CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=undefined -O1
sanitize: clean test

refs:
	$(PYTHON) gen_refs.py > refs_data.h.new && mv refs_data.h.new refs_data.h

clean:
	rm -f test_nid_ir refs_data.h.new
