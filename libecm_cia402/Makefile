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

# Phase 10.6: negative controls of the safety latches. Build n has latch Sn
# switched off (-DECM_CIA402_BROKEN, test builds only): its S-0n test must
# FAIL. Other tests that rely on the same latch (T-04, T-07: the 10.5
# bases of S2, S5, S7) may fail too: reported as INFO.
negctl: test_cia402_axis.c $(AXSRCS) $(AXHDRS) $(SBSRCS)
	@r=0; for n in 1 2 3 4 5 6 7; do \
	    $(CC) $(CFLAGS) -DECM_CIA402_BROKEN='(1<<'$$n')' -o test_cia402_axis_neg$$n test_cia402_axis.c $(AXSRCS) $(SBSRCS) -lm || exit 1; \
	    if ./test_cia402_axis_neg$$n s0$$n > neg$$n.log 2>&1; then echo "  [FAIL] S$$n negative control: S-0$$n still passes with the latch off"; r=1; \
	    else echo "  [PASS] S$$n negative control: S-0$$n fails with the latch off ($$(grep -c '\[FAIL\]' neg$$n.log) check(s))"; fi; \
	    others=$$(echo t01 t02 t03 t04 t05 t06 t07 t08 s01 s02 s03 s04 s05 s06 s07 | tr ' ' '\n' | grep -v "^s0$$n$$"); \
	    if ! ./test_cia402_axis_neg$$n $$others > neg$$n.others.log 2>&1; then echo "  [INFO] S$$n off also changes: $$(grep '\[FAIL\]' neg$$n.others.log | head -2 | sed 's/ *\[FAIL\] *//; s/ *=.*//' | tr '\n' ';')"; fi; \
	done; rm -f neg?.log neg?.others.log; exit $$r

clean:
	rm -f test_cia402_cfg test_cia402_cfg_asan test_cia402_axis test_cia402_axis_asan test_cia402_axis_neg? neg?.log neg?.others.log

.PHONY: all test test_asan negctl clean
