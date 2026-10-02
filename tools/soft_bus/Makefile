SUDO ?= sudo   # Phase 9.0: make caps SUDO= when already root
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
TARGET  = soft_bus
TEST    = test_offline
TESTF   = test_fault
TESTC   = test_coe
TESTP   = test_profile
TESTD   = test_cia402
SRCS    = esc_core.c esc_coe.c esc_dc.c esc_fault.c esc_profile.c esc_cia402.c
HDRS    = esc_types.h esc_sii.h esc_core.h esc_coe.h esc_dc.h esc_dc_state.h esc_fault.h esc_profile.h esc_cia402.h
LDLIBS  = -lm

all: $(TARGET)

$(TARGET): soft_bus_main.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ soft_bus_main.c $(SRCS) $(LDLIBS)

$(TEST): test_offline.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_offline.c $(SRCS) $(LDLIBS)
sii_dump: sii_dump.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ sii_dump.c $(SRCS) $(LDLIBS)

$(TESTF): test_fault.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_fault.c $(SRCS) $(LDLIBS)

$(TESTC): test_coe.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_coe.c $(SRCS) $(LDLIBS)

$(TESTP): test_profile.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_profile.c $(SRCS) $(LDLIBS)

$(TESTD): test_cia402.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_cia402.c $(SRCS) $(LDLIBS)

test: $(TEST) $(TESTF) $(TESTC) $(TESTP) $(TESTD)
	./$(TEST) && ./$(TESTF) && ./$(TESTC) && ./$(TESTP) && ./$(TESTD)

# Grants permission to run without sudo. Needs BOTH capabilities: NET_RAW to
# open a raw socket, NET_ADMIN for promiscuous mode — lesson learned earlier
# from slaveinfo/hw_ts_test (NET_RAW alone silently fails with a misleading
# "socket" error).
setcap: $(TARGET)
	$(SUDO) setcap cap_net_raw,cap_net_admin+ep ./$(TARGET)

clean:
	rm -f l1_probe
	rm -f $(TARGET) $(TEST) $(TESTF) $(TESTC) $(TESTP) $(TESTD) sii_dump

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
	$(SUDO) setcap cap_net_raw,cap_net_admin+ep ./l1_probe

l2_probe: l2_probe.c
	@test -n "$(SOEM_LIB)" || { echo "libsoem not found under $(SOEM_DIR)/build — build SOEM first, or set SOEM_DIR=..."; exit 1; }
	@echo "Using SOEM lib: $(SOEM_LIB)"
	$(CC) $(CFLAGS) -o $@ $< $(SOEM_INC) $(SOEM_LIB) -lpthread

setcap-probe2: l2_probe
	$(SUDO) setcap cap_net_raw,cap_net_admin+ep ./l2_probe

.PHONY: setcap-probe