# libecmaster/diag — Phase 7.2 bus diagnostics.
#   make test        offline unit tests (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11

all: test_diag_offline

test_diag_offline: test_diag_offline.c ecm_diag.c ecm_diag.h
	$(CC) $(CFLAGS) -o $@ test_diag_offline.c ecm_diag.c

test: test_diag_offline
	./test_diag_offline

test_asan: test_diag_offline.c ecm_diag.c ecm_diag.h
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_diag_offline_asan test_diag_offline.c ecm_diag.c
	./test_diag_offline_asan

clean:
	rm -f test_diag_offline test_diag_offline_asan

.PHONY: all test test_asan clean
