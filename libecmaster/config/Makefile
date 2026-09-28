# libecmaster/config -- ENI loader (no SOEM dependency in ecm_eni.c)
CFLAGS ?= -O2 -Wall -Wextra
SAN     = -fsanitize=address,undefined -g

all: test_eni_offline

test_eni_offline: test_eni_offline.c ecm_eni.c ecm_eni.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_eni_offline.c ecm_eni.c

# run from the repo root: it reads config/eni/*.enicfg
test: test_eni_offline
	cd ../.. && libecmaster/config/test_eni_offline

clean:
	rm -f test_eni_offline

.PHONY: all test clean
