# Third-party components

ecmaster is licensed under **GPL-3.0-only** (`LICENSE`, copyright 2026 khaiannguyen).
Every file carries its licence either in an SPDX header or in `REUSE.toml`;
`reuse lint` checks it (`make test-license`, CI).

This file lists what ecmaster uses from others, and in which way.

## 1. Linked into the ecmaster binaries

| Component | Version | Licence | How it is used |
|---|---|---|---|
| **SOEM** — Simple Open EtherCAT Master | v2.0.0 (git tag) | GPLv3, or a commercial licence from RT-Labs (dual-licensed) | Statically linked into `ecm_run`, `ecm_diag`, `ecm_peek`, `l4_test`, `l6_test`. Not vendored: it is fetched at its tag at build time (CI, release build script) |

SOEM copyright (from its `LICENSE.md`):
- Copyright (C) 2005-2025 Speciaal Machinefabriek Ketels v.o.f.
- Copyright (C) 2005-2025 Arthur Ketels
- Copyright (C) 2009-2025 RT-Labs AB, Sweden

ecmaster uses SOEM under its **GPLv3** terms. A product that cannot ship under GPLv3 needs the commercial SOEM licence from RT-Labs, and a separate licence for ecmaster.

**Changes to SOEM** (GPLv3, same terms as SOEM; applied before the build):

| Patch | Purpose |
|---|---|
| `patches/soem-mbx-cnt.patch` | Drops duplicated mailbox responses by their counter, ETG.1000.4 (Phase 7.4, L5-12, `docs/fault_policy.md` §11) |
| `patches/soem-txtime.patch` | `SO_TXTIME` launch time for `--link etf` (Phase 8.5); off unless `--link etf` |

SOEM build options: `-DEC_MAXGROUP=4`.

## 2. Not part of ecmaster, needed on the target

| Component | Licence | Note |
|---|---|---|
| Linux kernel with PREEMPT_RT (NVIDIA JetPack r39.2) | GPL-2.0 | The system ecmaster runs on |
| `igc` driver for the Intel i226 | GPL-2.0 | **Not in this repository and not in the ecmaster package.** Built by the user from the kernel sources (`docs/i226.md`) |
| ethtool, iproute2 (`ip`, `tc`), linuxptp (`phc2sys`), libcap (`setcap`) | GPL-2.0 / GPL-2.0+ / LGPL | System tools called by the scripts; installed from the distribution |
| Python 3 | PSF-2.0 | Tools only use the standard library |

## 3. Used for testing only, not distributed

| Component | Licence | Used by |
|---|---|---|
| IgH EtherCAT Master 1.6 (userspace library `libethercat`) | LGPL-2.1 (library), GPL-2.0 (kernel modules) | `tools/xcheck/igh_x01b.c` (X-01 cross-check). Built only against a local IgH install; no IgH code is in this repository |
| Beckhoff TwinCAT 3 | proprietary | Generated the ENIs of Phase 9.4 / 10 (below). The TwinCAT project files under `config/eni/twincat/` are the project's own |
| EtherCAT ESI XML schema (`EtherCATInfo.xsd`, `EtherCATBase.xsd`) | ETG | Downloaded into a local cache by `tools/esi/check_xsd.sh`; never committed |
| Wireshark / tshark, stress-ng, valgrind, xmllint, reuse | GPL-2.0+ / GPL-2.0 / GPL-2.0 / MIT / Apache-2.0, GPL-3.0 | Test and capture tools, installed from the distribution or pip |

## 4. Vendor data

| Item | Status |
|---|---|
| Inovance IS620N ESI (`IS620N-Ecat_v2.6.9.xml`) | **Not in this repository.** Profiles generated from it (`config/profiles/is620n.prof`) stay local (`.gitignore`) |
| `config/eni/eni_2servo*.xml`, `config/eni/eni_mixed*.xml` (+ `.enicfg`) | ENIs exported by TwinCAT 3 for this project's buses. They are configuration files of the project (topology, PDO assignment, init commands) and hold the IS620N identity and PDO entries, not the vendor ESI. Declared in `REUSE.toml` |
| ETG specifications (ETG.1000, ETG.1020, ETG.1500, ETG.2000…) | Referenced by number in the code and docs; no copy in this repository |

`tools/release/check_third_party.sh` (LIC-03) checks that no vendor ESI, no specification document and no copied third-party copyright line is in the tree.
