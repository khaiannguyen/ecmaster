CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
TARGET  = soft_bus
TEST    = test_offline

all: $(TARGET)

$(TARGET): soft_bus_main.c esc_core.c esc_types.h esc_sii.h esc_core.h
	$(CC) $(CFLAGS) -o $@ soft_bus_main.c esc_core.c

$(TEST): test_offline.c esc_core.c esc_types.h esc_sii.h esc_core.h
	$(CC) $(CFLAGS) -o $@ test_offline.c esc_core.c

test: $(TEST)
	./$(TEST)

# Grants permission to run without sudo. Needs BOTH capabilities: NET_RAW to
# open a raw socket, NET_ADMIN for promiscuous mode — lesson learned earlier
# from slaveinfo/hw_ts_test (NET_RAW alone silently fails with a misleading
# "socket" error).
setcap: $(TARGET)
	sudo setcap cap_net_raw,cap_net_admin+ep ./$(TARGET)

clean:
	rm -f l1_probe
	rm -f $(TARGET) $(TEST)

.PHONY: all test setcap clean

# ---- l1_probe: needs SOEM. Override SOEM_DIR if your repo lives elsewhere ----
SOEM_DIR ?= $(HOME)/projects/SOEM
# build/include is needed first: cmake generates the real ec_options.h there,
# not into the source tree's include/soem/ (which only has the .h.in template).
SOEM_INC  = -I$(SOEM_DIR)/build/include -I$(SOEM_DIR)/include -I$(SOEM_DIR)/osal -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux
# Library name/location depends on how cmake was configured; auto-detect it.
SOEM_LIB  = $(shell find $(SOEM_DIR)/build -name 'libsoem*.a' -o -name 'libsoem*.so' 2>/dev/null | head -1)

l1_probe: l1_probe.c
	@test -n "$(SOEM_LIB)" || { echo "libsoem not found under $(SOEM_DIR)/build — build SOEM first, or set SOEM_DIR=..."; exit 1; }
	@echo "Using SOEM lib: $(SOEM_LIB)"
	$(CC) $(CFLAGS) -o $@ $< $(SOEM_INC) $(SOEM_LIB) -lpthread

setcap-probe: l1_probe
	sudo setcap cap_net_raw,cap_net_admin+ep ./l1_probe

.PHONY: setcap-probe
