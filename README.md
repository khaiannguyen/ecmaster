# EtherCAT Master — Jetson Orin Nano + Intel i226

EtherCAT master on SOEM v2.0.0 (patched) for a Jetson Orin with PREEMPT_RT,
an Intel i226 NIC and DC-synchronised CiA402 drives (tested on two Inovance
IS620N). Entry point: `apps/ecm_run`.

## Documents
| Topic | Document |
|---|---|
| Remote diagnostics API (`stats.json`, `ctl.sock`, `ecm_diag --api`) | `docs/api.md` |
| RT boot profiles P0/P1, ETF, short cycles, dead time, 250 us budget | `docs/rt_profiles.md` |
| Boot-time set-up (systemd units, `/etc/ecmaster/NAME.conf`, `ecmaster-bootprofile`) | `docs/systemd.md` |
| Benchmark on 2 IS620N, next to the acontis Jetson Orin figures | `docs/benchmark.md` |
| i226 driver, ETF / LaunchTime, receive path | `docs/i226.md` |
| Fault policy, recovery | `docs/fault_policy.md` |
| CiA402 layer, safety boundary | `docs/cia402.md`, `docs/safety_boundary.md` |
| Every test: script, rig, negative control, last result; DoD | `TEST_PLAN.md` |
| ETG.1500 master class self-assessment | `ETG1500_COMPLIANCE.md` |
| What the master does on the wire, requirements for a new slave | `docs/ban_giao_slave.md` |
| Third-party components and licences | `THIRD_PARTY.md` |

Regression (veth, no drive): `sudo -E tools/gd11/run_regression_gd11.sh`.

## Measured (Phase 11, Jetson, 2 IS620N, details in `docs/benchmark.md`)
- 1 ms, 1 h per case, idle and with `stress-ng` on CPUs 0-2: 0 overrun, 0 lost
  frame; DC error p99.99 1 us with `--link etf` (11-22 us with af_packet).
- 500 us: clean on both backends (10 min). 250 us: profile P1 + `--link etf`
  + `--rx-guard-us 60`, 5 single lost frames in 1 h.
- Remote diagnostics API on (2 readers, 20 Hz SDO, client churn): wake jitter
  and occupancy p99.99 unchanged; API thread 0.2 % of one core.

## Architecture
```
apps/ecm_run      the master (one process, 5 threads)
  RT thread       main() itself, SCHED_FIFO 80 on the isolated CPU 3: one tick =
                  send/receive of each process data group (LRW), WKC check,
                  DC master-shift (PI on the reference clock), bus state
                  machine, the application hook, mailbox pump (1 frame/cycle),
                  diagnostic frame once a second; never blocks, never prints
  telemetry       histograms (wake jitter, prep_send, turnaround, occupancy)
  application     CiA402 / xchg producers (hook ring, seqlock)
  mailbox         SDO queue (SOEM cyclic mailbox, pumped by the RT thread)
  monitor         bus events, per-slave recovery, diagnostics, API (--api)
libecm_cia402     axis = (slave, n): drive state machine, CSP/CSV/PP/PV/HM,
                  safety latches S1..S7 (docs/cia402.md, docs/safety_boundary.md)
libecmaster       no CiA402 object knowledge (check_k05.py): config (ENI
                  loader), core (DC), diag, mailbox, pdo (bind by index/sub),
                  policy (bus FSM, recovery), telemetry, xchg, api
SOEM v2.0.0       + patches/ (mailbox Cnt, SO_TXTIME), EC_MAXGROUP=4
i226 / igc        af_packet, or etf: launch time by the NIC (--link etf)
tools/soft_bus    a chain of N virtual ESCs on veth: the slave of every test
```
Two process data groups (`GROUP_MOTION` 1 ms with DC, `GROUP_IO` slower);
fault policy RUN / DEGRADED / LOST / RECOVER with per-slave recovery and
configuration errors never retried (`docs/fault_policy.md`); remote
diagnostics `stats.json` + `ctl.sock` off the RT path (`docs/api.md`).

## Known limitations
- The API is for diagnostics: no axis command, no bus state change, no
  "clear failed" through it. `sdo_write` only for objects in an allowlist
  (empty by default).
- A slave whose ESC has no FMMU left for the SM1 status (the IS620N has 2)
  gets no cyclic mailbox: no SDO and no EMCY in SAFE-OP/OP (the API answers
  `no_mailbox`). Start-up InitCmds work. Drive faults are seen through 0x603F.
  New slaves for this master should have >= 3 FMMUs.
- All i226 interrupts are on CPU0 (Tegra); the tail of the receive path is
  what limits 250 us. Keep CPU0 light; API and web clients on CPUs 1-2.
- The JetPack RT kernel has no `NO_HZ_FULL` / `RCU_NOCB`: CPU 3 still gets
  the tick.
- i226 `rx-usecs 0` and GRO/GSO/TSO off are kept across reboots only with
  `ecmaster-net@IFACE` enabled (`docs/systemd.md`); `jetson_clocks` is
  still a manual step after boot.
- No poll-mode backend (AF_XDP, Phase 11.8, deferred); `libecmaster/link`
  is an interface without implementation.
- No LRD/LWR process data, `tx_launch_err` only through `tools/etf/txtime_probe`.
- Turnaround is measured with software timestamps (both directions, network
  stack included); hardware timestamps are used only for the ETF launch
  accuracy (`tools/etf/txtime_probe`).
- No sync window monitoring: 0x092C is not read (ETG.1500 1103); the DC
  quality reported is the master-shift error |e| and the slaves' SYNC0
  ground truth on soft_bus.
- Not implemented: Hot Connect, slave-to-slave (ETG.1500 1201), cable
  redundancy, EoE / FoE / SoE / AoE / VoE, SDO Info use, a master object
  dictionary (`ETG1500_COMPLIANCE.md`).
- Known code issues kept for after v1.0 (none seen in a test or on the
  rig): an SDO that times out while queued behind another leaves its job
  on the caller's stack (`libecmaster/mailbox`); soft_bus answers no SDO
  Info request although two test profiles advertise it; soft_bus starts the
  ESM only on a write that starts at 0x0120; `xchg` counts a SET dropped
  during a bus loss as applied; the histogram sum is unsigned (mean only).

## Not testable on this rig, and why
| Item | Why | Covered instead by |
|---|---|---|
| A LAN9252 slave (X-02/X-03 against it, R-01 on it, its SII written to the chip) | the LAN9252 / STM32H723 slave is a separate project (Platform 1), not built yet | 2 Inovance IS620N (Phase 10: bring-up R-01..R-09, CSP W-08, D-14, BM-02); soft_bus with the P1 draft profile; IgH and TwinCAT cross-checks (X-01, X-02s, X-04, X-05) |
| Cyclic mailbox in OP on a real slave | the IS620N has 2 FMMUs (above) | soft_bus (3 FMMUs) L4-03, D-15 negative control on the IS620N |
| ETG Conformance Test Tool | not available to the project | `ETG1500_COMPLIANCE.md` (self-assessment) |
| More than 2 real slaves, 4 axes on one slave, 250 us on a real 4-axis slave | hardware | soft_bus N = 8 / 32, `cia402_4ax.prof` (8 h CSP soak), IS-05/IS-10 budget |
| Cable redundancy, Hot Connect | not implemented | -- |

## Input freshness (`ecm_run --fresh`, GD9.8)

A correct working counter only says that every slave *processed* the frame.
It does not say that the inputs are new: a slave whose application stalls
keeps answering with the same data. `ecm_run` can catch that when the slave's
PDO contract has an application counter in its inputs, which the slave
increments every cycle:

    --fresh all=0              16-bit counter at input byte 0 of every slave
    --fresh 1=0,2=off,5=4:8    slave 1: byte 0; slave 2: no counter; slave 5: 8-bit at byte 4
    --fresh-offset 0           same as --fresh all=0 (GD7.4)

Entries apply left to right; a later one overrides. A counter outside a
slave's inputs is refused at startup.

**A slave with `off` (or not listed) is not checked: for that slave the master
cannot detect "WKC correct but data old".** `ecm_run` prints this for every
such slave at startup. Commercial drives usually have no such counter, so
they are `off`; use the drive's own status (statusword toggle bits, EMCY)
in the application layer instead.

## Install (Phase 12.3)
Jetson Orin, JetPack r39.2 with the PREEMPT_RT kernel, i226 with the `igc`
driver (`docs/i226.md`; the driver is not part of ecmaster).

**Package** (arm64, built on the target):
```bash
tools/release/build_soem.sh                 # SOEM v2.0.0 + patches/ (sha256 checked) in build/SOEM
make all SOEM_DIR=$PWD/build/SOEM
make deb                                    # dist/ecmaster_<version>-1_arm64.deb
sudo apt install ./dist/ecmaster_*.deb      # /opt/ecmaster; nothing enabled
sudo /opt/ecmaster/sbin/ecmaster-port add enP1p1s0     # the EtherCAT port, kept at every boot
sudo apt purge ecmaster                     # removes everything, port settings restored
```
**From source** (the release tarball `ecmaster-<version>.tar.gz` holds all
that is needed, GPLv3): the same first two lines, then `sudo ./install.sh
--iface enP1p1s0 --enable-net`, removed with `sudo ./uninstall.sh`. Both
install the same layout; use one or the other. Running it: `docs/systemd.md`.

## License
Copyright 2026 khaiannguyen. ecmaster is free software under the **GNU General
Public License v3.0 only** (`LICENSE`, SPDX `GPL-3.0-only`). It links SOEM
v2.0.0, used under its GPLv3 terms; the components used, and how, are listed in
`THIRD_PARTY.md`. Every file states its licence (SPDX header, or `REUSE.toml`
for data files); check with `make test-license` (REUSE 3.3, `reuse lint`).
