# l6_test — Phase 6 (L6) DC test tool. Needs SOEM v2.0.0 built with CMake.
# Override SOEM_DIR / SOEM_BUILD if your tree lives elsewhere.
SOEM_DIR   ?= $(HOME)/projects/SOEM
SOEM_BUILD ?= $(SOEM_DIR)/build
ECM_CORE   ?= ../../libecmaster/core

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 \
          -I$(SOEM_DIR)/include -I$(SOEM_BUILD)/include \
          -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux \
          -I$(ECM_CORE)
LDLIBS  = $(SOEM_BUILD)/libsoem.a -lpthread -lrt

all: l6_test

l6_test: l6_test.c $(ECM_CORE)/ecm_dc.c $(ECM_CORE)/ecm_dc.h
	$(CC) $(CFLAGS) -o $@ l6_test.c $(ECM_CORE)/ecm_dc.c $(LDLIBS)

setcap: l6_test
	sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice,cap_ipc_lock+ep ./l6_test

clean:
	rm -f l6_test

.PHONY: all setcap clean
