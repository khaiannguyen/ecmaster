# libecmaster/core — ecm_dc (Giai doan 6) + reply-age gate test (Giai doan 7.4)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11

all: test_dc_offline

test_dc_offline: test_dc_offline.c ecm_dc.c ecm_dc.h
	$(CC) $(CFLAGS) -o $@ test_dc_offline.c ecm_dc.c

test: test_dc_offline
	./test_dc_offline

test_asan: test_dc_offline.c ecm_dc.c ecm_dc.h
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -o test_dc_offline_asan test_dc_offline.c ecm_dc.c
	./test_dc_offline_asan

clean:
	rm -f test_dc_offline test_dc_offline_asan

.PHONY: all test test_asan clean
