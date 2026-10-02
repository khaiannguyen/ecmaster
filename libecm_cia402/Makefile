# libecm_cia402 -- Phase 10.3: CiA402 layer of the master (axis configuration).
# Kept out of libecmaster: every object index with a CiA402 meaning lives here.
#   make test        offline unit tests (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
SRCS    = ecm_cia402_cfg.c ../libecmaster/pdo/ecm_pdo.c
HDRS    = ecm_cia402_cfg.h ../libecmaster/pdo/ecm_pdo.h

all: test_cia402_cfg

test_cia402_cfg: test_cia402_cfg.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_cia402_cfg.c $(SRCS)

test: test_cia402_cfg
	./test_cia402_cfg

test_asan: test_cia402_cfg.c $(SRCS) $(HDRS)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_cia402_cfg_asan test_cia402_cfg.c $(SRCS)
	./test_cia402_cfg_asan

clean:
	rm -f test_cia402_cfg test_cia402_cfg_asan

.PHONY: all test test_asan clean
