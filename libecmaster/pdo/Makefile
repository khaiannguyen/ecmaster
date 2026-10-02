# libecmaster/pdo — Phase 9.10 bind process data by (slave, index, sub).
#   make test        offline unit tests (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11

all: test_pdo_offline

test_pdo_offline: test_pdo_offline.c ecm_pdo.c ecm_pdo.h
	$(CC) $(CFLAGS) -o $@ test_pdo_offline.c ecm_pdo.c

test: test_pdo_offline
	./test_pdo_offline

test_asan: test_pdo_offline.c ecm_pdo.c ecm_pdo.h
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_pdo_offline_asan test_pdo_offline.c ecm_pdo.c
	./test_pdo_offline_asan

clean:
	rm -f test_pdo_offline test_pdo_offline_asan

.PHONY: all test test_asan clean
