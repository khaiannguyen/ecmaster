# libecmaster/xchg — Phase 10.4 app <-> RT exchange (docs/app_rt_exchange.md).
#   make test        offline unit tests (one thread)
#   make test_asan   same, with ASan/UBSan
#   make tsan        Q-05: three threads under TSan + 2 negative controls
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
SRCS    = ecm_xchg.c ../pdo/ecm_pdo.c
HDRS    = ecm_xchg.h ../pdo/ecm_pdo.h

all: test_xchg_offline

test_xchg_offline: test_xchg_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_xchg_offline.c $(SRCS)

test: test_xchg_offline
	./test_xchg_offline

test_asan: test_xchg_offline.c $(SRCS) $(HDRS)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_xchg_offline_asan test_xchg_offline.c $(SRCS)
	./test_xchg_offline_asan

tsan:
	./run_xchg_tsan.sh

clean:
	rm -f test_xchg_offline test_xchg_offline_asan

.PHONY: all test test_asan tsan clean
