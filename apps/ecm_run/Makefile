CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
TARGET  = ecm_run

SOEM_DIR ?= $(HOME)/projects/SOEM
SOEM_INC  = -I$(SOEM_DIR)/build/include -I$(SOEM_DIR)/include -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux
SOEM_LIB  = $(shell find $(SOEM_DIR)/build -name 'libsoem*.a' -o -name 'libsoem*.so' 2>/dev/null | head -1)

all: $(TARGET)

$(TARGET): ecm_run.c
	@test -n "$(SOEM_LIB)" || { echo "libsoem not found under $(SOEM_DIR)/build — build SOEM first, or set SOEM_DIR=..."; exit 1; }
	@echo "Using SOEM lib: $(SOEM_LIB)"
	$(CC) $(CFLAGS) -o $@ $< $(SOEM_INC) $(SOEM_LIB) -lpthread -lrt

setcap: $(TARGET)
	sudo setcap cap_net_raw,cap_net_admin,cap_ipc_lock,cap_sys_nice+ep ./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all setcap clean