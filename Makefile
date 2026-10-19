# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ecmaster top-level Makefile (Phase 9.0)
#
#   make all SOEM_DIR=...     build soft_bus and every app against a patched SOEM
#   make caps [SUDO=]         setcap on every binary that opens a raw socket
#                             (one sudo; rebuilding a binary drops its
#                             capabilities -- Phase 8 operating trap), then getcap
#   make all caps             the usual sequence after a change
#   make test-offline         the CI "offline" job (no SOEM, no network)
#   make dist / make deb      Phase 12.3: source tarball / binary package in dist/
#                             (SOEM: tools/release/build_soem.sh, SOEM_DIR=$PWD/build/SOEM)
#   make test-license         Phase 12.1: reuse lint, SPDX headers, LIC-03
#                             (needs reuse >= 3.0, see the script header)
#
# SOEM must be v2.0.0 built with -DEC_MAXGROUP=4 and patches/soem-mbx-cnt.patch
# + patches/soem-txtime.patch (see .github/workflows/ci.yml).

SOEM_DIR ?= $(HOME)/projects/SOEM
APPS     := apps/ecm_run apps/ecm_diag apps/ecm_peek apps/l4_test apps/l6_test
CAPS_BIN := tools/soft_bus/soft_bus apps/ecm_run/ecm_run apps/ecm_diag/ecm_diag apps/ecm_peek/ecm_peek \
            apps/l4_test/l4_test apps/l6_test/l6_test

.PHONY: all soft_bus apps caps caps-show test-offline test-license dist deb clean

all: soft_bus apps

soft_bus:
	$(MAKE) -C tools/soft_bus

apps:
	@for d in $(APPS); do $(MAKE) -C $$d SOEM_DIR=$(SOEM_DIR) || exit 1; done
	$(MAKE) -C apps/ecm_run ecm_run_neg6 SOEM_DIR=$(SOEM_DIR)   # Phase 10.6: S6 negative control (test only)

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
	python3 tools/esi/make_multiaxis_prof.py --axes 4 | diff -q - config/profiles/cia402_4ax.prof   # Phase 10.2: committed profile = generator
	$(MAKE) -C tools/soft_bus test
	$(MAKE) -C libecmaster/telemetry run
	$(MAKE) -C libecmaster/core test test_asan
	$(MAKE) -C libecmaster/diag test test_asan
	$(MAKE) -C libecmaster/policy test test_asan
	$(MAKE) -C libecmaster/config test
	$(MAKE) -C libecmaster/pdo test test_asan
	$(MAKE) -C libecm_cia402 test test_asan negctl   # Phase 10.6: S1..S7 negative controls
	$(MAKE) -C libecmaster/xchg test test_asan tsan
	$(MAKE) -C libecmaster/api test test_asan negctl tsan schema   # Phase 11.1, 11.2 (ctl.sock offline + negative controls)
	python3 tools/gd10/check_k05.py --self-test && python3 tools/gd10/check_k05.py
	tests/tsan/run_tsan.sh 1000000
	$(MAKE) -C tests/dc test   # Phase 12.4: DC unit + closed loop with the soft_bus ESC (was in no target)
	tools/release/test_sys_offline.sh   # Phase 12.2: units, helpers, boot profiles, install_system.sh (fakes, no root)
	tools/release/test_release_offline.sh   # Phase 12.3: make dist, build_soem patch check, install/uninstall, ecmaster-port, deb
	python3 tools/release/soak_watch.py --self-test   # Phase 12.5: soak fault rules + negative controls

dist:
	tools/release/make_dist.sh            # Phase 12.3: dist/ecmaster-VERSION.tar.gz (git archive of HEAD)

deb:
	tools/release/build_deb.sh            # Phase 12.3: dist/ecmaster_VERSION-1_ARCH.deb from the built binaries

test-license:
	tools/release/test_license_offline.sh

clean:
	@for d in tools/soft_bus libecm_cia402 libecmaster/api $(APPS); do $(MAKE) -s -C $$d clean; done
