# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# Phase 6 offline DC tests — no veth, no root, no Jetson needed.
#   test_ecm_dc_unit : ecm_dc unwrap / phase math / stale / 10.5 s gap
#   test_dc_e2e      : closed loop, simulated time: SOEM-equivalent
#                      config_init+configdc+dcsync0 datagrams -> real
#                      esc_core.c + esc_dc.c -> ecm_dc PI. S2 = 1 h @ 32-bit.
SB   = ../../tools/soft_bus
CORE = ../../libecmaster/core
CC   = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -I$(SB) -I$(CORE)
SAN    = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

all: test_ecm_dc_unit test_dc_e2e

test_ecm_dc_unit: test_ecm_dc_unit.c $(CORE)/ecm_dc.c $(CORE)/ecm_dc.h
	$(CC) $(CFLAGS) -o $@ test_ecm_dc_unit.c $(CORE)/ecm_dc.c

# Phase 12.4: esc_core.c calls into the CoE, fault, profile and CiA402 parts
# since Phase 5..10; the test had stopped linking (it was in no make target)
SB_SRCS = $(addprefix $(SB)/,esc_core.c esc_coe.c esc_dc.c esc_fault.c esc_profile.c esc_cia402.c)
test_dc_e2e: test_dc_e2e.c $(SB_SRCS) $(CORE)/ecm_dc.c
	$(CC) $(CFLAGS) -o $@ test_dc_e2e.c $(SB_SRCS) $(CORE)/ecm_dc.c -lm

test: all
	./test_ecm_dc_unit && ./test_dc_e2e

asan:
	$(CC) $(CFLAGS) $(SAN) -o test_ecm_dc_unit_asan test_ecm_dc_unit.c $(CORE)/ecm_dc.c
	./test_ecm_dc_unit_asan

clean:
	rm -f test_ecm_dc_unit test_dc_e2e test_ecm_dc_unit_asan

.PHONY: all test asan clean
