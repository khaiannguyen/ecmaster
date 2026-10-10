# libecmaster/api — Phase 11.1/11.2 remote diagnostics API (docs/api.md).
#   make test        offline unit tests: stats.json (11.1) + ctl.sock (11.2)
#   make test_asan   same, with ASan/UBSan
#   make negctl      negative controls: file written in place MUST be seen torn;
#                    ctl.sock with one protection removed MUST fail its check
#   make tsan        handoff under TSan + negative control
#   make schema      sample documents through tools/gd11/stats_check.py (+ its self-test)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
SRCS    = ecm_api.c ../telemetry/histogram.c ../diag/ecm_diag.c
HDRS    = ecm_api.h ../telemetry/histogram.h ../diag/ecm_diag.h
CTL_SRCS = ecm_ctl.c ecm_json.c ecm_sha256.c
CTL_HDRS = ecm_ctl.h ecm_json.h ecm_sha256.h
# ECM_CTL_BROKEN bit -> the check that MUST fail without that protection
NEG = 1:deny-rxpdo 2:rate 4:deny-comm-area 8:deny-out-of-range 16:deny-initcmd
LIBS    = -lpthread -lm

all: test_api_offline test_ctl_offline

test_ctl_offline: test_ctl_offline.c $(CTL_SRCS) $(CTL_HDRS)
	$(CC) $(CFLAGS) -o $@ test_ctl_offline.c $(CTL_SRCS) -lm

test_api_offline: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_api_offline.c $(SRCS) $(LIBS)

test: test_api_offline test_ctl_offline
	./test_api_offline
	./test_ctl_offline

test_asan: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_api_offline_asan test_api_offline.c $(SRCS) $(LIBS)
	./test_api_offline_asan
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_ctl_offline_asan test_ctl_offline.c $(CTL_SRCS) -lm
	./test_ctl_offline_asan

negctl: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -DECM_API_PUBLISH_INPLACE -o test_api_inplace test_api_offline.c $(SRCS) $(LIBS)
	@if ./test_api_inplace --torn 300; then \
	    echo "negctl FAIL: a file written in place was never seen half-written"; exit 1; \
	else echo "negctl ok: written in place, torn reads seen"; fi
	@for nb in $(NEG); do b=$${nb%%:*}; want=$${nb#*:}; \
	    $(CC) $(CFLAGS) -DECM_CTL_BROKEN=$$b -o test_ctl_neg test_ctl_offline.c $(CTL_SRCS) -lm || exit 1; \
	    out=$$(./test_ctl_neg); rc=$$?; \
	    case "$$want" in deny-rxpdo) pat='deny rxpdo';; rate) pat='rate:';; deny-comm-area) pat='deny comm area';; \
	                     deny-out-of-range) pat='deny out of range';; deny-initcmd) pat='deny initcmd';; esac; \
	    if [ $$rc = 0 ] || ! echo "$$out" | grep -q "FAIL \[$$pat"; then \
	        echo "negctl FAIL: ECM_CTL_BROKEN=$$b ($$want) was not detected"; exit 1; fi; \
	    echo "negctl ok: ECM_CTL_BROKEN=$$b ($$want) detected: $$(echo "$$out" | grep -c 'FAIL \[') failing check(s)"; \
	done; rm -f test_ctl_neg

tsan:
	./run_api_tsan.sh

schema: test_api_offline
	@d=$$(mktemp -d /tmp/api_schema_XXXXXX); ./test_api_offline --dump $$d >/dev/null && \
	python3 ../../tools/gd11/stats_check.py schema $$d/full.json && \
	python3 ../../tools/gd11/stats_check.py schema $$d/empty.json && \
	python3 ../../tools/gd11/stats_check.py selftest $$d/full.json; rc=$$?; rm -rf $$d; exit $$rc

clean:
	rm -f test_api_offline test_api_offline_asan test_api_inplace test_ctl_offline test_ctl_offline_asan test_ctl_neg

.PHONY: all test test_asan negctl tsan schema clean
