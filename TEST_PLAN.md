# Test plan — ecmaster `master-v1.0`

Every test of the project: what it proves, how to run it, on which rig, its
negative control (a check that must be seen to FAIL, or the test proves
nothing), and its last result. Phase 12.4, 19/10/2026; the final regression
on the tagged commit is Phase 12.5 (results added there).

**Rigs.** `off` = no network, no root (`make test-offline`, CI). `veth` =
`soft_bus` on `veth_s`, `ecm_run` on `veth_m`, same Jetson (`soft_bus`
SCHED_FIFO 79 on CPU 2). `i226` = the i226 port with a loopback cable or a
real slave. `IS620N` = two Inovance IS620N drives. `x` = another master
(IgH 1.6.13, TwinCAT 3 4024). Timing figures count only from the Jetson.

**One-command runs.**
| Command | Content | Time |
|---|---|---|
| `make test-offline` | every `off` test below (also the CI `offline` job) | ~3 min |
| `make test-license` | LIC-01, LIC-03 (needs `reuse` >= 3.0) | 10 s |
| `sudo -E tools/gd11/run_regression_gd11.sh` | offline + Phase 9, 10, 11 on veth (golden included) | ~45 min |
| `sudo -E tools/gd10/run_regression_gd10.sh` | Phase 9 + 10 on veth | ~25 min |
| `sudo -E tools/gd9/run_regression_gd9.sh` | Phase 9 on veth, golden, L5 | ~15 min |

## 1. Unit and offline (`off`, `make test-offline`)
| ID | Proves | Script / target | Negative control | Last |
|---|---|---|---|---|
| ESI / ENI files | ESIs valid (schema 1.17), CA variant and committed profiles = generator output, `.enicfg` = `eni2cfg.py` output | `tools/esi/check_xsd.sh`, `check_ca_variant.sh`, `tools/eni/check_enicfg.sh`, `make_multiaxis_prof.py \| diff` | ESI without `<Type ProductCode>`; `.enicfg` with a line removed | PASS |
| soft_bus T1–T17, F1–F11, CoE, P-01..06, A-01..07 | the virtual ESC: registers, WKC rules, SII, FMMU, ESM, watchdog, faults, CoE byte for byte as SOEM, profiles, CiA402 drive written from the standard | `make -C tools/soft_bus test` | test tables written independently of the master | PASS |
| telemetry T1–T11 | histogram, SPSC ring, turnaround pairing by index | `make -C libecmaster/telemetry run` | T5 keeps the old by-position pairing (2/9) | PASS |
| core G0–G5, W1–W2 | DC unwrap, reply-age gate, 120 000-cycle closed loop over 28 wraps | `make -C libecmaster/core test test_asan` | G5 gate off accepts the stale sample | PASS |
| tests/dc S1–S4 | DC 1 h in simulated time against the soft_bus ESC, ground truth on the slave side | `make -C tests/dc test` (in `test-offline` since 12.4) | naive `DCtime % cycle` must slip | PASS 19/10 |
| diag D1–D9 | cable / topology localisation, handoff, snapshot file | `make -C libecmaster/diag test test_asan` | forwarded errors only → no CABLE finding | PASS |
| policy P1–P8 | bus FSM (99/100 NOFRAME boundary), recovery planner, freshness, AL code classes | `make -C libecmaster/policy test test_asan` | PARTIAL forever never LOST | PASS |
| config E-01..E-10 offline | ENI parser, identity / layout checks, refused features | `make -C libecmaster/config test` | many invalid files | PASS |
| pdo B-01..B-05 | PDO table, bind, bit-level get/set, N-03 rule | `make -C libecmaster/pdo test test_asan` | B-03 unmapped object, B-05 foreign vendor | PASS |
| cia402 K, T, S-01..S-07, P2, E2 | axis config, drive state machine closed loop with the virtual drive, safety latches | `make -C libecm_cia402 test test_asan negctl` | `negctl`: each latch S1..S7 disabled → its test FAILs | PASS |
| xchg X-01o..X-04o, Q-05 | app ↔ RT exchange, seqlock | `make -C libecmaster/xchg test test_asan tsan` | `ECM_XRING_BROKEN`, `ECM_XST_BROKEN` | PASS |
| api A, C offline | `stats.json` writer, JSON, `ctl.sock` tokenizer, SHA-256, allowlist, rate limit, schema | `make -C libecmaster/api test test_asan negctl tsan schema` | `negctl` builds | PASS |
| K-05 | `libecmaster` holds no CiA402 object index | `tools/gd10/check_k05.py` | `--self-test` (planted literal) | PASS |
| TSan | memory ordering of every lock-free queue | `tests/tsan/run_tsan.sh 1000000` | race.c must be reported; release→relaxed must be reported | PASS |
| IS offline O-01..O-07 | boot-profile tool, IS parsers, dead-time model | `tools/gd11/test_is_offline.sh` | p = 90 % at 250 µs → negative margin | PASS |
| BM offline | benchmark tables refuse non-final / unknown-RT files | `tools/bench/test_bench_offline.sh` | `stats_notfinal.json`, `stats_old.json` | PASS |
| N, C, R, B, I (Phase 12.2) | `ecmaster-net`, `ecmaster-clock`, `ecmaster-run`, `ecmaster-bootprofile` (SYS-05), `install_system.sh` on fakes | `tools/release/test_sys_offline.sh` | NIC ignoring `-C`; timesyncd not restarted when it was off; shell syntax refused | 29/0 |
| D, S, I, P, K (Phase 12.3) | `make dist`, `build_soem.sh` patch sums, `install.sh`/`uninstall.sh`, `ecmaster-port`, `.deb` | `tools/release/test_release_offline.sh` | dirty tree, changed / missing patch, missing binary, package without postinst | 12/0 |
| LIC-01, LIC-03 | REUSE 3.3 compliance, SPDX headers, exec bits, nothing we may not redistribute | `make test-license` | header removed; vendor ESI, PDF, IgH line, ESI profile planted | 17/0 |

## 2. Functional on veth (`veth`)
| ID | Proves | Script | Negative control | Last |
|---|---|---|---|---|
| L1-01..06 | SOEM + soft_bus: slave count, identity, APWR, IOmap, frame split (N = 1/8/32) | `tools/soft_bus/run_l1_tests.sh` | — | 10/0 (GĐ1) |
| L2-01..06 | ESM, AL codes 0x0019/0x0012/0x0011 | `tools/soft_bus/l2_probe` (graded from its `L2:` lines) | — | PASS (GĐ2) |
| L3 | two groups 1 h, cycle sweep | `ecm_run` | — | PASS (GĐ3) |
| L4-01..08 | SDO read/write, SDO in OP without PD disturbance, abort, segmented, CA | `apps/l4_test`, `tools/gd9/run_coe.sh` | `--ca` against a non-CA soft_bus must FAIL L4-08 | PASS |
| L5-01..05, 11b, 13 | bus FSM, recovery, SM watchdog trip | `apps/ecm_run/run_l5_policy.sh` | l502neg (`--rx-timeout-legacy`), l505neg (`--no-recover`), l513neg | PASS (regression) |
| L5-06, 11a/b | cable / broken-chain localisation | `apps/ecm_diag/run_l5_diag.sh` | baseline `findings: none` | PASS |
| L5-07..09, 12 | late / duplicate / reordered replies, stale data, mailbox Cnt | `apps/ecm_run/run_l5_io.sh`, `tools/gd11/run_l507_11_3b.sh` (N=1 × 20) | l507neg (`--no-quarantine --no-reply-check`), l509neg, `--dc-gate-legacy` | PASS |
| L6-01..05 | DC: propagation delay, SYNC0 set-up, 32-bit wrap, drift, reference change; graded on the slave side | `apps/l6_test/run_l6_tests.sh` | `--naive` must FAIL GT-a | PASS |
| Golden | the datagram sequence on the wire: default 4+4 (555), ENI (581), N=1 (90), CA (556), `--io-slaves` = `--motion-slaves` | `tools/golden/check_golden.sh` | `negative_control.sh` (two FPWR swapped), ENI without the Startup SDO, CA golden vs plain bus | PASS 19/10 |
| G-01..08 | process data groups | `tools/gd9/run_groups.sh` | g07 (5 bad options) | 38/0 |
| C-02..05 | CoE CA / segmented end to end | `tools/gd9/run_coe.sh` | c04, CA golden negative | 36/0 |
| E-01..10 | ENI loader with vendor / TwinCAT ENIs | `tools/gd9/run_eni_9_6.sh` | e06 (LRD/LWR: 0 frames sent), e08, e09a | 27/0 |
| DC-01..05 | DC per slave from the ENI | `tools/gd9/run_dc_9_5.sh` | dc02n, dc03n, dc04, dc04b | 24/0 |
| M-01..03, P-01..03 | EMCY path, AL code classes | `tools/gd9/run_emcy_9_7.sh` | — | 27/0 |
| F-01..02 | input freshness | `tools/gd9/run_fresh_9_8.sh` | f02b (`off` does not detect), f02bp | 19/0 |
| B-01..05 | PDO bind on the bus | `tools/gd9/run_pdo_9_10.sh` | b01n, b03 | 27/0 |
| V-01..04, N-01..03 | mixed bus with vendor profiles, refusals with reasons | `tools/gd9/run_mixed_9_9.sh` | n01c, n03a | 33/0, 2 skip (no vendor ESI) |
| D (10.2), A-04 | virtual CiA402 drive through the master | `tools/gd10/run_servo_10_2.sh` | a04 (no `--cia402` → 0x6041 = 0) | 26/0 |
| K-01..08 | axis configuration on the bus | `tools/gd10/run_axes_10_3.sh` | k01n, k02n, k03, k06, k07 | 24/0 |
| Q-01..03, H-01..05 | RT hook + exchange | `tools/gd10/run_hook_10_4.sh` | h02n echo off, h04 lead 0 | 25/0 |
| T-01..08 | drive state machine, CSP/CSV | `tools/gd10/run_cia402_10_5.sh` | — | 31/0 |
| S-01..07 | safety latches on the bus | `tools/gd10/run_safety_10_6.sh` | `--axis-max-step 0`, `ecm_run_neg6` (S6 off) | 22/0 |
| P2-01..04 | PP, PV, homing | `tools/gd10/run_profile_10_7.sh` | — | 14/0 |
| E2-01..03, X-05 tool | axis diagnosis | `tools/gd10/run_diag_10_8.sh` | quick-stop reference trace | 13/0 |
| RC-01..06 | recovery re-runs the ENI InitCmds, refuses OP on failure | `tools/gd10/run_recover_10_0.sh` | rc03, rc06 (`--recover-cyclic-mbx`); RC-02 criterion on a log without the per-slave return | 20/0 19/10 |
| W-01..07 (rehearsal), W-09csv, W-10pv | the IS620N motion runs on calibrated virtual drives | `tools/gd10/run_w_10_9.sh SIM=1`, `wmm` | `PDOSET_OFF=1` must fail at w00 | 55/0, 24/0 |
| A-01..A-11 | `stats.json` (schema, 10 Hz, never half-written, final = report, CPU pinning, golden with `--api`) | `tools/gd11/run_api_11_1.sh` | A-01 `--api` off; A-04 mid-run file must differ; A-11 changed value | PASS 17/10 |
| C-01..C-09 | `ctl.sock` commands, allowlist, refusals | `tools/gd11/run_ctl_11_2.sh` | refusals | PASS 17/10 |
| G-01..G-07 | `ecm_diag --api` | `tools/gd11/run_diag_11_3.sh` | — | PASS 17/10 |
| D-03, D-04, D-12, D-13 | API robustness (client churn, slow reader, permissions, restart) | `tools/gd11/run_d_11_6.sh` | — | PASS 17/10 |
| D-15 | no cyclic mailbox without the SM1 FMMU | `tools/gd11/run_d15_11_6.sh` | `--mbx-cyclic-force` | PASS 17/10 |
| D-01 | API on, RT figures unchanged (6 × 30 min) | `CASES=d01 tools/gd11/run_d_11_6.sh` | d01neg: API on + a FIFO 81 burst must be seen | PASS (with 0041) |

## 3. Hardware (`i226`, `IS620N`, `x`)
| ID | Proves | Script | Negative control | Last |
|---|---|---|---|---|
| L0-03 | i226 TX/RX hardware timestamps delivered | `tools/hw_ts_test` (prints only) | — | read by eye |
| T-01..06, R-02 | PHC follows TAI; ETF launch accuracy (offset ~314 ns, spread ~30 ns); af_packet vs etf | `tools/etf/clock_check`, `txtime_probe`, `soem_txtime_smoke` | `-C mono` refused, `-X` past txtime dropped, `-n` | PASS (GĐ8) |
| H-01..04 | 100 Mbit/s link, ETF at 100 Mbit/s (missed 0) | `tools/gd9/run_100m.sh` | — | PASS (GĐ9.2) |
| X-01a/b/c | IgH reads the same SII; IgH runs soft_bus 600 s with DC bus shift | `tools/xcheck/igh_x01b.c`, `x01c_regs.py` | — | PASS (GĐ8) |
| X-02s | TwinCAT as master drives soft_bus over a cable | manual (`docs/x05_twincat.md`) | — | PASS (GĐ8) |
| X-04 | IgH confirms soft_bus Complete Access | `tools/xcheck/run_x04.sh` | X-04n (abort without CA) | 12/0 |
| X-05 | controlword sequence of TwinCAT NC vs `ecm_run` | `tools/gd10/cw_trace.py` | quick-stop reference | PASS (GĐ10) |
| R-01..R-09 | IS620N bring-up: SII ↔ ESI, PDO, DC, OP 1 h, power-cycle recovery | `tools/gd10/run_bringup_10_0.sh` (`SIM=1` rehearsal) | `SIM_REV` → R-02 must FAIL; `bringup_report.py` FAILs on any Operation-enabled before enable | PASS (GĐ10.0) |
| W-08 | CSP on both IS620N, 1 h, drive limits written and read back | `tools/gd10/run_w_10_9.sh` | — | PASS (GĐ10.9) |
| IS-01..IS-10 | RT profiles P0/P1, ETF sweep, short cycles, receive path, dead time, 250 µs budget | `tools/gd11/run_is_11_5.sh`, `run_is2_11_5.sh`, `run_is7_11_5.sh`, `run_is9_11_5.sh`, `deadtime.py` | O-07; rx-usecs default vs 0 (31 936 vs 87 NOFRAME) | IS-07 250 µs 1 h: 5 NOFRAME |
| D-14 | API on the real drives in motion | `tools/gd11/run_d14_11_6.sh` | API off run | 11/11 |
| BM-02 | 1 ms benchmark, 2 IS620N, af / etf × idle / load, 1 h each | `tools/bench/run_bm_11_7.sh` | — | 4/4 (`docs/benchmark.md`) |
| SYS-01..04 | boot-time set-up survives a reboot; `ecm_run@`, `ecmaster-clock@`, uninstall | `tools/release/run_sys_12_2.sh` | unit disabled → `check_env --boot` FAIL; `--iface` in `ECM_ARGS`; timesyncd off stays off | 13/13 (SYS-04 in INS-01) |
| LIC-02 | the source tarball alone rebuilds SOEM + ecmaster; test-offline + golden pass from it | `tools/release/run_ins_12_3.sh lic02` | tarball without a patch → refused, nothing cloned | 4/4 |
| INS-01 | install.sh / uninstall.sh twice: identical trees, port settings restored | `run_ins_12_3.sh ins01` | — | 3/3 |
| INS-02 | `.deb`: capabilities, nothing enabled, SYS-02 with the packaged binary, purge leaves nothing | `run_ins_12_3.sh ins02` | K-02 offline (no postinst) | 4/4 |

## 4. Long runs
| ID | Proves | Script | Criteria (set before the run) | Last |
|---|---|---|---|---|
| Soak N=1 | 8 h, 1 slave, veth: rare-event rate | `tools/gd10/run_soak_n1.sh` | 0 overrun, DC LOCKED 0 unlock, 0 LOST, WKC 0/0/0, NOFRAME ≤ 2 | 0 NOFRAME / 28.8 M (GĐ10.1) |
| Soak CSP | 8 h, 4 CSP axes on one virtual slave | `tools/gd10/run_soak_cia402.sh` | as above + 0 underrun / late, tracking ≤ 1 inc, hook p99.99 ≤ 20 µs, S6 | PASS (GĐ10.10) |
| rt_tail | kernel latency floor on CPU 3 | `tools/rt_tail/rt_tail.sh` | (reading) | 10 h max 7 µs (GĐ7) |
| Soak 8 h, real drives in motion | the tagged commit, 2 IS620N, both axes CSP (W-08 motion and limits), `--api` | `tools/release/run_soak_12_5.sh` (+ `soak_watch.py`: stops at the first fault with S6) | W-08 checks with NOFRAME ≤ 2 per hour; no fault; CPU 3 never below 95 % of max frequency; no zone above 95 °C; VmRSS growth ≤ 1 MiB. Watcher rules: `soak_watch.py --self-test` (negative controls, in `test-offline`); stop at the first fault seen on the veth rehearsal (overrun on a non-RT host → SIGTERM → S6) | 12.5 |
| Final regression | the tagged commit: clean tree, check_env, full Phase 9–11 regression, licence, L0 10 min, valgrind | `tools/release/run_final_12_5.sh` | valgrind self-check: a planted 4 KiB leak must be reported | **6/6** 19/10 `cf2b241` |
| valgrind | no memory error, no leak in ecm_run through a cable pull (L5-03) | `run_final_12_5.sh` step `vg` | planted leak | 0 B lost, 0 errors |
| L0 | kernel wake-up floor on CPU 3, 10 min | `run_final_12_5.sh` step `l0` (`rt_tail.sh`) | `BREAK_US` stops cyclictest and keeps the trace | max 8 µs |

## 5. `master_plan_v2.md` §12 Definition of Done
| Item | Status | Evidence |
|---|---|---|
| ETG.1500 self-assessment | ✅ | `ETG1500_COMPLIANCE.md` (final, 1103 corrected) |
| soft_bus N ≥ 8, L1-06 | ✅ | L1 10/0 |
| L0–L6 in one script | ✅ | `run_final_12_5.sh` (regression + L0 `rt_tail`) |
| ≥ 2 link backends, A/B with one binary | ✅ | af_packet / etf, R-02, BM-02 |
| Hardware timestamps | ✅ partly | launch accuracy with HW timestamps; turnaround uses software timestamps (README) |
| p50 … p99.99 | ✅ | `libecmaster/telemetry` |
| Theoretical cycle budget next to measurements | ✅ | IS-10 (`docs/rt_profiles.md` §14) |
| DC PI 1 h, 32-bit wrap | ✅ | L6-04, tests/dc S2, soak N=1 (6705 wraps) |
| Explicit SM watchdog | ✅ | `docs/ban_giao_slave.md` §2.2, golden |
| ≥ 2 process data groups | ✅ | G-01..08 |
| TSan on the SPSC queues, arm64 | ✅ | CI + Jetson |
| X-01 / X-02 / X-03 | X-01 ✅; **X-02/X-03 against a LAN9252 → Platform 1** | X-02s with TwinCAT on soft_bus |
| R-01 on a real LAN9252 | **→ Platform 1** | R-01..R-09 on the IS620N |
| Soak ≥ 8 h, valgrind, thermal | ✅ | §4 (8 h with both axes moving) |
| `ecm_diag` | ✅ | L5-06, G-01..07 |
| Golden pcap + compare | ✅ | §2 |
| SII image written to the LAN9252 | **→ Platform 1** | — |
| ≥ 3 `.pcapng` | ✅ | evidence pack `pcap/` (`make_evidence.sh`) |
| `TEST_PLAN.md` | ✅ | this file |
| README "not testable" | ✅ | README |
| No hard-coded PDO / object index in `libecmaster` | ✅ | K-05 (`check_k05.py`); the only object indexes are 0x1C12/0x1C13 (ETG.1000.6 PDO assign, device-independent) |
| Tag + reference figures | ✅ | `master-v1.0` = `cf2b241`, `docs/release_notes_1.0.md` |

Items "→ Platform 1" need the LAN9252 slave; they are not marked as met.
