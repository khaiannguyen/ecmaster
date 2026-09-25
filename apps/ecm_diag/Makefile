# ecm_diag — Phase 7.2 diagnostics CLI. Needs SOEM v2.0.0 built with CMake
# (only for --standalone; the snapshot reader itself does not touch the bus).
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
TARGET  = ecm_diag

SOEM_DIR ?= $(HOME)/projects/SOEM
SOEM_INC  = -I$(SOEM_DIR)/build/include -I$(SOEM_DIR)/include -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux
SOEM_LIB  = $(shell find $(SOEM_DIR)/build -name 'libsoem*.a' -o -name 'libsoem*.so' 2>/dev/null | head -1)

DIAG_DIR = ../../libecmaster/diag
DIAG_SRC = $(DIAG_DIR)/ecm_diag.c $(DIAG_DIR)/ecm_diag_soem.c

all: $(TARGET)

$(TARGET): ecm_diag.c $(DIAG_SRC) $(DIAG_DIR)/ecm_diag.h $(DIAG_DIR)/ecm_diag_soem.h
	@test -n "$(SOEM_LIB)" || { echo "libsoem not found under $(SOEM_DIR)/build — build SOEM first, or set SOEM_DIR=..."; exit 1; }
	$(CC) $(CFLAGS) -o $@ ecm_diag.c $(DIAG_SRC) $(SOEM_INC) -I../.. -I$(DIAG_DIR) $(SOEM_LIB) -lpthread -lrt

# --standalone opens a raw socket: same capabilities as the other tools.
setcap: $(TARGET)
	sudo setcap cap_net_raw,cap_net_admin+ep ./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all setcap clean
