# ecmaster top-level Makefile (Phase 9.0)
#
#   make all SOEM_DIR=...     build soft_bus and every app against a patched SOEM
#   make caps [SUDO=]         setcap on every binary that opens a raw socket
#                             (one sudo; rebuilding a binary drops its
#                             capabilities -- Phase 8 operating trap), then getcap
#   make all caps             the usual sequence after a change
#   make test-offline         the CI "offline" job (no SOEM, no network)
#
# SOEM must be v2.0.0 built with -DEC_MAXGROUP=4 and patches/soem-mbx-cnt.patch
# + patches/soem-txtime.patch (see .github/workflows/ci.yml).

SOEM_DIR ?= $(HOME)/projects/SOEM
APPS     := apps/ecm_run apps/ecm_diag apps/l4_test apps/l6_test
CAPS_BIN := tools/soft_bus/soft_bus apps/ecm_run/ecm_run apps/ecm_diag/ecm_diag \
            apps/l4_test/l4_test apps/l6_test/l6_test

.PHONY: all soft_bus apps caps caps-show test-offline clean

all: soft_bus apps

soft_bus:
	$(MAKE) -C tools/soft_bus

apps:
	@for d in $(APPS); do $(MAKE) -C $$d SOEM_DIR=$(SOEM_DIR) || exit 1; done

caps:
	@for d in tools/soft_bus $(APPS); do $(MAKE) -s -C $$d setcap || exit 1; done
	@$(MAKE) -s caps-show

caps-show:
	@for b in $(CAPS_BIN); do \
	    if [ -x $$b ]; then c=$$(getcap $$b); echo "$${c:-$$b  (NO capabilities)}"; \
	    else echo "$$b  (not built)"; fi; done

test-offline:
	tools/esi/check_xsd.sh
	tools/esi/check_ca_variant.sh
	tools/esi/check_xsd.sh config/esi/p1_draft_esi.xml
	tools/eni/check_enicfg.sh
	$(MAKE) -C tools/soft_bus test
	$(MAKE) -C libecmaster/telemetry run
	$(MAKE) -C libecmaster/core test test_asan
	$(MAKE) -C libecmaster/diag test test_asan
	$(MAKE) -C libecmaster/policy test test_asan
	$(MAKE) -C libecmaster/config test
	$(MAKE) -C libecmaster/pdo test test_asan
	tests/tsan/run_tsan.sh 1000000

clean:
	@for d in tools/soft_bus $(APPS); do $(MAKE) -s -C $$d clean; done
