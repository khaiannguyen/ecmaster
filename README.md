# EtherCAT Master — Jetson Orin Nano + Intel i226

EtherCAT master on SOEM v2.0.0 (patched) for a Jetson Orin with PREEMPT_RT,
an Intel i226 NIC and DC-synchronised CiA402 drives (tested on two Inovance
IS620N). Entry point: `apps/ecm_run`.

## Documents
| Topic | Document |
|---|---|
| Remote diagnostics API (`stats.json`, `ctl.sock`, `ecm_diag --api`) | `docs/api.md` |
| RT boot profiles P0/P1, ETF, short cycles, dead time, 250 us budget | `docs/rt_profiles.md` |
| Benchmark on 2 IS620N, next to the acontis Jetson Orin figures | `docs/benchmark.md` |
| i226 driver, ETF / LaunchTime, receive path | `docs/i226.md` |
| Fault policy, recovery | `docs/fault_policy.md` |
| CiA402 layer, safety boundary | `docs/cia402.md`, `docs/safety_boundary.md` |

Regression (veth, no drive): `sudo -E tools/gd11/run_regression_gd11.sh`.

## Measured (Phase 11, Jetson, 2 IS620N, details in `docs/benchmark.md`)
- 1 ms, 1 h per case, idle and with `stress-ng` on CPUs 0-2: 0 overrun, 0 lost
  frame; DC error p99.99 1 us with `--link etf` (11-22 us with af_packet).
- 500 us: clean on both backends (10 min). 250 us: profile P1 + `--link etf`
  + `--rx-guard-us 60`, 5 single lost frames in 1 h.
- Remote diagnostics API on (2 readers, 20 Hz SDO, client churn): wake jitter
  and occupancy p99.99 unchanged; API thread 0.2 % of one core.

## Known limitations
- The API is for diagnostics: no axis command, no bus state change, no
  "clear failed" through it. `sdo_write` only for objects in an allowlist
  (empty by default).
- A slave whose ESC has no FMMU left for the SM1 status (the IS620N has 2)
  gets no cyclic mailbox: no SDO and no EMCY in SAFE-OP/OP (the API answers
  `no_mailbox`). Start-up InitCmds work. Drive faults are seen through 0x603F.
- All i226 interrupts are on CPU0 (Tegra); the tail of the receive path is
  what limits 250 us. Keep CPU0 light; API and web clients on CPUs 1-2.
- The JetPack RT kernel has no `NO_HZ_FULL` / `RCU_NOCB`: CPU 3 still gets
  the tick.
- i226 `rx-usecs 0` and GRO/GSO/TSO off must be set again after every boot.
- No poll-mode backend (AF_XDP) yet: Phase 11.8, deferred.
- No LRD/LWR, `tx_launch_err` only through `tools/etf/txtime_probe`.


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

## License
Copyright 2026 khaiannguyen. ecmaster is free software under the **GNU General
Public License v3.0 only** (`LICENSE`, SPDX `GPL-3.0-only`). It links SOEM
v2.0.0, used under its GPLv3 terms; the components used, and how, are listed in
`THIRD_PARTY.md`. Every file states its licence (SPDX header, or `REUSE.toml`
for data files); check with `make test-license` (REUSE 3.3, `reuse lint`).
