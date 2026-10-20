# ecmaster 1.0.0 (`master-v1.0`)

EtherCAT master on SOEM v2.0.0 (patched) for an NVIDIA Jetson Orin with a
PREEMPT_RT kernel and an Intel i226 NIC, driving DC-synchronised CiA402
servo drives. Tested on two Inovance IS620N and on a virtual slave chain
(soft_bus). Licence GPL-3.0-only.

Tag `master-v1.0` = commit `cf2b241` (19/10/2026), the commit of the final
regression and of the 8 h soak below.

## What it does
- **Cyclic process data** in two groups (motion 1 ms with DC, IO slower),
  LRW, WKC checked per group, one RT thread on an isolated core
  (SCHED_FIFO 80), four non-RT threads (telemetry, application, mailbox,
  monitor).
- **Distributed Clocks, master shift:** the master follows the reference
  clock with a PI loop, 32-bit wrap handled explicitly; SYNC0 per slave from
  the ENI.
- **Two link backends, one binary:** `af_packet`, and `etf` (SO_TXTIME +
  ETF qdisc offload: the i226 launches the motion frame at the cycle target).
- **ENI loader** (TwinCAT ENI → `eni2cfg.py` → `.enicfg`): identity,
  revision and process data layout compared before SAFE-OP, CoE and
  register InitCmds, Complete Access; unsupported ENI features refused with
  the reason.
- **CoE:** SDO normal / segmented / Complete Access, EMCY, SDO in OP without
  disturbing the cycle.
- **Fault policy:** RUN / DEGRADED / LOST / RECOVER, per-slave recovery with
  the ENI InitCmds re-run, configuration AL codes never retried, late /
  duplicate / reordered replies filtered, input freshness check.
- **CiA402 layer:** drive state machine, CSP / CSV / PP / PV / homing,
  safety latches S1–S7 (no automatic re-enable, target = actual, step limit,
  bus loss latch, ordered shutdown, fault reset on command only).
- **Remote diagnostics API** off the RT path: `stats.json` (10 Hz) and
  `ctl.sock` (read-only commands, allowlisted `sdo_write`), `ecm_diag`.
- **Packaging:** `.deb` for arm64, source tarball that rebuilds SOEM and
  ecmaster alone, systemd units (`ecmaster-net@`, `ecmaster-clock@`,
  `ecm_run@NAME`), `ecmaster-port`, `ecmaster-bootprofile` (P0 default, P1
  for 250 µs).

## Measured (Jetson Orin, JetPack r39.2, `6.8.12-1021-rt-tegra`, profile P0)
| Run | Result |
|---|---|
| **8 h soak, 2 IS620N, both axes CSP**, 1 ms, af_packet, `--api` | 28.8 M cycles, 0 overrun, 0 wrong WKC, **1** lost frame, DC LOCKED over 6 706 wraps, \|e\| p99.99 **3 µs**, both axes no error, 0 late / 0 underrun setpoints, S6 at the end, no throttling, ecm_run RSS +0 KiB |
| Benchmark 1 ms, 1 h × 4 (af / etf × idle / `stress-ng` on CPUs 0–2) | 0 overrun, 0 lost frame; DC \|e\| p99.99 **1 µs** with etf, 11–22 µs with af_packet; wake jitter p99.99 5.6–6.3 µs (`docs/benchmark.md`) |
| 250 µs, P1 + etf + `--rx-guard-us 60`, 1 h | 5 single lost frames (`docs/rt_profiles.md`) |
| Final regression of `cf2b241` (veth) | Phase 9–11: 31 steps, 0 FAIL, golden + negative controls; licence 17/0; L0 cyclictest 10 min CPU 3 max **8 µs**; valgrind through a cable pull: 0 bytes lost, 0 errors |
| Packaging | rebuild from the tarball alone (LIC-02 4/4); install / uninstall twice identical (INS-01 3/3); `.deb` install, run, purge clean (INS-02 4/4); boot-time set-up survives a reboot (SYS 13/13) |

Every test, its negative control and its last result: `TEST_PLAN.md`.

## Install
```bash
sudo apt install ./ecmaster_1.0.0-1_arm64.deb          # /opt/ecmaster, nothing enabled
sudo /opt/ecmaster/sbin/ecmaster-port add enP1p1s0      # the EtherCAT port, kept at boot
```
From source: `tar xzf ecmaster-1.0.0.tar.gz && cd ecmaster-1.0.0 &&
tools/release/build_soem.sh && make all SOEM_DIR=$PWD/build/SOEM && sudo
./install.sh --iface enP1p1s0 --enable-net`. The `igc` driver for the i226
is built separately (`docs/i226.md`). Running the master: `docs/systemd.md`.

## Known limitations (README for the full list)
- Self-assessed against ETG.1500, not tested with the Conformance Test
  Tool; no slave-to-slave (1201), no sync window monitoring (1103), no Hot
  Connect, no cable redundancy, no EoE / FoE / SoE.
- A slave whose ESC has only 2 FMMUs (IS620N) gets no cyclic mailbox: no
  SDO / EMCY in SAFE-OP / OP.
- Turnaround is measured with software timestamps; all i226 interrupts are
  on CPU0 (Tegra), the receive path is the limit at 250 µs.
- `jetson_clocks` is a manual step after boot.

**Moved to the Platform 1 slave (LAN9252 + STM32H723), not claimed here:**
cross-checks X-02 / X-03 and R-01 on a LAN9252, its SII written to the chip.
The bring-up tools for it (ESC register reader, 0x092C log, PDO logger, P1
golden) come in `master-v1.1` without changing the v1.0 behaviour on the
wire (`docs/ban_giao_slave.md`).

## Release files
| File | Content |
|---|---|
| `ecmaster_1.0.0-1_arm64.deb` | the package, built from the tag on the Jetson |
| `ecmaster-1.0.0.tar.gz` | the source of the tag (GPLv3 corresponding source, with the SOEM patches and the build script) |
| `ecmaster-1.0.0-evidence.tar.gz` | validation evidence: final regression, 8 h soak, golden, benchmark, captures, machine description (`MANIFEST.md`, `SHA256SUMS` inside) |
| `SHA256SUMS` | of the three |

## Third-party
SOEM v2.0.0 (RT-Labs, GPLv3 / commercial), statically linked, with two
patches (`THIRD_PARTY.md`).
