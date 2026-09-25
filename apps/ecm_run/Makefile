CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
TARGET  = ecm_run

SOEM_DIR ?= $(HOME)/projects/SOEM
SOEM_INC  = -I$(SOEM_DIR)/build/include -I$(SOEM_DIR)/include -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux
SOEM_LIB  = $(shell find $(SOEM_DIR)/build -name 'libsoem*.a' -o -name 'libsoem*.so' 2>/dev/null | head -1)

TELEMETRY_DIR = ../../libecmaster/telemetry
TELEMETRY_SRC = $(TELEMETRY_DIR)/histogram.c $(TELEMETRY_DIR)/ring_spsc.c $(TELEMETRY_DIR)/turnaround.c
TELEMETRY_INC = -I$(TELEMETRY_DIR) -I../..

MAILBOX_DIR = ../../libecmaster/mailbox
MAILBOX_SRC = $(MAILBOX_DIR)/ecm_mailbox.c
DC_SRC = ../../libecmaster/core/ecm_dc.c
DIAG_SRC = ../../libecmaster/diag/ecm_diag.c ../../libecmaster/diag/ecm_diag_soem.c
POLICY_SRC = ../../libecmaster/policy/ecm_policy.c

all: $(TARGET)

$(TARGET): ecm_run.c $(TELEMETRY_SRC) $(MAILBOX_SRC) $(DC_SRC) $(DIAG_SRC) $(POLICY_SRC)
	@test -n "$(SOEM_LIB)" || { echo "libsoem not found under $(SOEM_DIR)/build — build SOEM first, or set SOEM_DIR=..."; exit 1; }
	@echo "Using SOEM lib: $(SOEM_LIB)"
	$(CC) $(CFLAGS) -o $@ $^ $(SOEM_INC) $(TELEMETRY_INC) -I../../libecmaster/diag $(SOEM_LIB) -lpthread -lrt -lm

setcap: $(TARGET)
	sudo setcap cap_net_raw,cap_net_admin,cap_ipc_lock,cap_sys_nice+ep ./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all setcap clean