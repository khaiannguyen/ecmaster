# libecmaster/policy — Phase 7.3 master fault policy (docs/fault_policy.md).
#   make test        offline unit tests (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11

all: test_policy_offline

test_policy_offline: test_policy_offline.c ecm_policy.c ecm_policy.h
	$(CC) $(CFLAGS) -o $@ test_policy_offline.c ecm_policy.c

test: test_policy_offline
	./test_policy_offline

test_asan: test_policy_offline.c ecm_policy.c ecm_policy.h
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_policy_offline_asan test_policy_offline.c ecm_policy.c
	./test_policy_offline_asan

clean:
	rm -f test_policy_offline test_policy_offline_asan

.PHONY: all test test_asan clean
