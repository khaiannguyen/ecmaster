# libecm_cia402 -- Phase 10.3: CiA402 layer of the master (axis configuration).
# Kept out of libecmaster: every object index with a CiA402 meaning lives here.
#   make test        offline unit tests (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
SRCS    = ecm_cia402_cfg.c ../libecmaster/pdo/ecm_pdo.c
HDRS    = ecm_cia402_cfg.h ../libecmaster/pdo/ecm_pdo.h
# Phase 10.5: the axis state machine, tested in a closed loop against the
# virtual drive of soft_bus (test only: the library itself does not use it)
AXSRCS  = ecm_cia402_axis.c ecm_cia402_cfg.c ../libecmaster/pdo/ecm_pdo.c ../libecmaster/xchg/ecm_xchg.c
SBDIR   = ../tools/soft_bus
SBSRCS  = $(SBDIR)/esc_core.c $(SBDIR)/esc_coe.c $(SBDIR)/esc_dc.c $(SBDIR)/esc_fault.c $(SBDIR)/esc_profile.c $(SBDIR)/esc_cia402.c
AXHDRS  = ecm_cia402_axis.h ecm_cia402_cfg.h ../libecmaster/xchg/ecm_xchg.h $(SBDIR)/esc_cia402.h

all: test_cia402_cfg test_cia402_axis

test_cia402_axis: test_cia402_axis.c $(AXSRCS) $(AXHDRS) $(SBSRCS)
	$(CC) $(CFLAGS) -o $@ test_cia402_axis.c $(AXSRCS) $(SBSRCS) -lm

test_cia402_cfg: test_cia402_cfg.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_cia402_cfg.c $(SRCS)

test: test_cia402_cfg test_cia402_axis
	./test_cia402_cfg && ./test_cia402_axis

test_asan: test_cia402_cfg.c $(SRCS) $(HDRS)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_cia402_cfg_asan test_cia402_cfg.c $(SRCS)
	./test_cia402_cfg_asan
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_cia402_axis_asan test_cia402_axis.c $(AXSRCS) $(SBSRCS) -lm
	./test_cia402_axis_asan

clean:
	rm -f test_cia402_cfg test_cia402_cfg_asan test_cia402_axis test_cia402_axis_asan

.PHONY: all test test_asan clean
