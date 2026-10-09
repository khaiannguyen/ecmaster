# libecmaster/api — Phase 11.1 remote diagnostics API (docs/api.md).
#   make test        offline unit tests + publish race (no SOEM, no network)
#   make test_asan   same, with ASan/UBSan
#   make negctl      negative control: file written in place MUST be seen torn
#   make tsan        handoff under TSan + negative control
#   make schema      sample documents through tools/gd11/stats_check.py (+ its self-test)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
SRCS    = ecm_api.c ../telemetry/histogram.c ../diag/ecm_diag.c
HDRS    = ecm_api.h ../telemetry/histogram.h ../diag/ecm_diag.h
LIBS    = -lpthread -lm

all: test_api_offline

test_api_offline: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_api_offline.c $(SRCS) $(LIBS)

test: test_api_offline
	./test_api_offline

test_asan: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o test_api_offline_asan test_api_offline.c $(SRCS) $(LIBS)
	./test_api_offline_asan

negctl: test_api_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -DECM_API_PUBLISH_INPLACE -o test_api_inplace test_api_offline.c $(SRCS) $(LIBS)
	@if ./test_api_inplace --torn 300; then \
	    echo "negctl FAIL: a file written in place was never seen half-written"; exit 1; \
	else echo "negctl ok: written in place, torn reads seen"; fi

tsan:
	./run_api_tsan.sh

schema: test_api_offline
	@d=$$(mktemp -d /tmp/api_schema_XXXXXX); ./test_api_offline --dump $$d >/dev/null && \
	python3 ../../tools/gd11/stats_check.py schema $$d/full.json && \
	python3 ../../tools/gd11/stats_check.py schema $$d/empty.json && \
	python3 ../../tools/gd11/stats_check.py selftest $$d/full.json; rc=$$?; rm -rf $$d; exit $$rc

clean:
	rm -f test_api_offline test_api_offline_asan test_api_inplace

.PHONY: all test test_asan negctl tsan schema clean
