# Process-data groups in `ecm_run` (GD9.1)

`ecm_run` runs two SOEM groups on the RT thread:

| Group | SOEM group | Cycle | Carries |
|---|---|---|---|
| `GROUP_MOTION` | 1 | `--motion-cycle-us` (default 1000) | the tick, the DC FRMW (DC(b) master-shift), SYNC0 slaves, diagnostics |
| `GROUP_IO` | 2 | `--io-cycle-us` (default 8000, multiple of the motion cycle) | slow slaves, free-run |

## Choosing the group of each slave

| Option | Meaning |
|---|---|
| *(none)* | every slave in `GROUP_MOTION`, `GROUP_IO` empty |
| `--io-slaves LIST` | these positions in `GROUP_IO`; `LIST` = `none`, `5-8`, `2,4`, `2,4,6-7` |
| `--motion-slaves k` | GD3..GD8 form: 1..k motion, k+1..n IO, needs 0 < k < n; kept so every script and golden capture runs unchanged |

The two options exclude each other. `GROUP_MOTION` must keep at least one slave.

Typical buses:

| Bus | Command |
|---|---|
| P1/P2/P4 — one LAN9252 (P4: 4 axes behind it, still one slave) | `ecm_run --iface enP1p1s0 --n 1` |
| own slave + commercial servo, both motion | `ecm_run --iface enP1p1s0 --n 2` (or `--eni` with the mixed ENI) |
| soft_bus reference (golden, L-series) | `ecm_run --iface veth_m --n 8 --motion-slaves 4` = `--io-slaves 5-8` |

With `--eni` the group options work the same way; the slave count comes from the ENI.

## Empty `GROUP_IO`

SOEM sends nothing for a group of length 0, but `ecx_receive_processdata_group()` then returns `EC_NOFRAME`, which the fault policy (`docs/fault_policy.md`) would count as a lost IO frame every IO cycle. `ecm_run` therefore never sends or receives an empty group: `io_due` stays 0, the pre-OP / OP-keepalive / RECOVER exchanges skip it (`pd_exchange_all()`), the diagnostics report one WKC group, and the final statistics say `[GROUP_IO] empty (no slave assigned), never sent`.

For an empty group SOEM's `mbxstatuslength` equals `logstartaddr` (0x20000); the GD5 correction (`-= logstartaddr`) brings it to 0, so nothing is sent. The size printed as `N byte IOmap` is the size the group really sends (up to GD8 it included `logstartaddr`, e.g. `65572 byte` for a 36-byte map).

## Checks

| ID | What | How |
|---|---|---|
| G-01 | 4+4 unchanged | `tools/golden/check_golden.sh` (547 lines) |
| G-01b | `--io-slaves 5-8` = `--motion-slaves 4` | `tools/gd9/run_groups.sh` case `g01b` (same golden) |
| G-02 | ENI mode unchanged | `ENI=config/eni/eni_8node_dc_sdo.enicfg tools/golden/check_golden.sh` (573) |
| G-03 | one slave: OP, no IO datagram on the wire, no IO-caused DEGRADED, DC locked | `run_groups.sh g03` |
| G-04 | two motion slaves | `run_groups.sh g04` |
| G-05 | IO slaves between motion slaves | `run_groups.sh g05` |
| G-06 | E-03d: one-node ENI on a one-node bus; refused on two nodes | `run_groups.sh g06` |
| G-07 | argument errors refused | `run_groups.sh g07` |
| G-08 | golden of the one-slave bus | `N=1 GROUP_ARGS= GOLDEN=tools/golden/golden_ecm_run_n1.txt tools/golden/check_golden.sh` (89) |
| L5 on one slave | policy + io cases | `N=1 M=0 L505_SLAVE=1 run_l5_policy.sh`, `N=1 M=0 L509_SLAVE=1 run_l5_io.sh` |
