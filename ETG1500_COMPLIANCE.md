# ETG.1500 Master Classes — Self-Assessment

> **This is a self-assessment, NOT a certification.** Not verified against the ETG EtherCAT
> Conformance Test Tool. Source: ETG.1500 D(R) 1.0.2, ethercat.org.

> This project does **not** implement item 1201 (Slave-to-Slave Communication), so it does not fully
> qualify for either class in the strict sense of the spec; it reaches "most of Class B plus part of
> Class A", consistent with the self-assessment approach. Since GD8 8.4 (ENI import) item 503 Complete
> Access also becomes a Class B `shall`; implemented in GD9.3 (see 503).
>
> Last update: GD9.3 (30/9/2026) -- Complete Access (loader + soft_bus), normal/segmented SDO download tests.

## Basic Features

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 101 | Service Commands | shall (if ENI import) | shall (if ENI import) | ✅ | ✅ | Via `ecx_*` API. Mandatory since GD8 8.4 (ENI import). All DL commands are available through SOEM; the golden capture (CI) pins the sequence the master actually uses. ESI/SII flag `UseLrdLwr` (slaves without LRW) not evaluated yet |
| 102 | IRQ field in datagram | should | should | ✅ | ⏳ | Not prioritized yet, easy to add later |
| 103 | Slaves with Device Emulation | shall | shall | ✅ | ✅ | Handled via AL Status per slave |
| 104 | EtherCAT State Machine (ESM) | shall | shall | ✅ | ✅ | Phase 3 |
| 105 | Error Handling | shall | shall | ✅ | ✅ | WKC checking, core to the whole project |
| 106 | VLAN | may | may | ❌ | ❌ | Not needed for this project |
| 107 | EtherCAT Frame Types | shall | shall | ✅ | ✅ | Native 0x88A4 |
| 108 | UDP Frame Types | may | may | ❌ | ❌ | No UDP tunneling used |

## Process Data Exchange

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 201 | Cyclic PDO | shall | shall | ✅ | ✅ | Core |
| 202 | Multiple Tasks | may | may | ✅ | ✅ | Implemented as `GROUP_MOTION`/`GROUP_IO` (master_plan_v2.md §2.8) |
| 203 | Frame repetition | may | may | ⏳ | ❌ | Not needed, stable network over veth/short cable |

## Network Configuration

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 301 | Online scanning | at least 1 of 2 | at least 1 of 2 | ✅ | ✅ | Verified via `slaveinfo` |
| — | Reading ENI | (the other option for 301) | (the other option for 301) | ❌ (SOEM has no ENI reader) | ✅ | GD8 8.4: TwinCAT 4024 ENI → `eni2cfg.py` → `.enicfg` → `libecmaster/config` (`ecm_run --eni`). E-01…E-05: OP + CoE InitCmd on the wire, A/B identical with/without ENI, wrong identity/revision rejected, SDO abort → SAFE-OP refused, golden ENI in CI. Online scan still runs to cross-check |
| 302 | Compare network config at boot-up | shall | shall | ⏳ | ✅ | ENI mode (GD8 8.4): slave count, VendorID, ProductCode, RevisionNo (per ENI `check_rev`) and process data layout compared before SAFE-OP, mismatch = start refused (E-03a/b/c). Online mode: Phase 7 diagnostics. SerialNo, IdentificationAdo and topology are not compared |
| 303 | Explicit Device Identification | should | should | ⏳ | ❌ | Not needed, no Hot Connect |
| 304 | Station Alias Addressing | may | may | ✅ | ❌ | Not needed |
| 305 | Access to EEPROM | Read shall / Write may | Read shall / Write may | ✅ | ✅ | Already used in practice (PDI_SELECT on LAN9252). GD8 8.1: SII read by this master and by IgH is byte-identical (X-01a, N=1/8/32) |

## Mailbox Support

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 401 | Support Mailbox | shall | shall | ✅ | ✅ | Phase 5 |
| 402 | Mailbox Resilient Layer | shall | shall | ✅ | ✅ | Handled by SOEM |
| 403 | Multiple Mailbox channels | may | may | ❌ | ❌ | Not needed |
| 404 | Mailbox polling | shall | shall | ✅ | ✅ | SOEM 2.0's new mailbox cyclic API |

## CAN application layer over EtherCAT (CoE)

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 501 | SDO Up/Download | shall | shall | ✅ | ✅ | L4 test series |
| 502 | Segmented Transfer | shall | should | ✅ | ✅ | Handled by SOEM automatically for >4 bytes. Upload: L4-05 (200 byte). Download (GD9.3): L4-07 (16 byte normal, 250 byte = init + 2 segments), offline C-01 (400 byte = init + 3 segments, short last segment, toggle error -> abort 0x05030000) |
| 503 | Complete Access | shall | should (**shall if ENI import**) | ✅ (`ecx_SDOread/write` CA flag) | ✅ | GD9.3: the ENI loader runs `CompleteAccess` InitCmds with `ecx_SDOwrite/ecx_SDOread(..., CA=TRUE)`, data verbatim from the ENI; a failure stops before SAFE-OP and names slave, object and abort code (C-03/C-04). `soft_bus --coe-ca` serves CA (SI0 as U8 + pad, CA bit echoed, PDO assign writable in PREOP only, atomic) and advertises it in an SII General category, so SOEM maps process data with `ecx_readPDOmapCA` (C-02: IOmap identical to the SII path). Still open: IgH cross-check X-04 on the Jetson, and a TwinCAT-generated CA ENI (step 9.4) |
| 504 | SDO Info service | shall | should | ✅ | ✅ | **Exact code path read in `ec_coe.c` GET_OD_REQ** |
| 505 | Emergency Message | shall | shall | ✅ | ✅ | |
| 506 | PDO transmission with CoE | may | may | ✅ | ❌ | Spec itself states "no relevant use case known" |

## Ethernet over EtherCAT (EoE) / FoE / SoE / AoE / VoE

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 601 | EoE Protocol | shall | shall if EoE supported | ✅ (`ec_eoe.c`) | ❌ | Not needed for motor control |
| 602 | Virtual Switch | shall | shall if EoE supported | ✅ | ❌ | Bundled with 601 |
| 603 | EoE Endpoint to OS | should | should if EoE supported | ✅ | ❌ | Bundled with 601 |
| 701 | FoE Protocol | shall | shall if FoE supported | ✅ (`ec_foe.c`) | 🤔 | **Under consideration**: firmware update for the H723 over EtherCAT — a nice portfolio talking point |
| 702 | Firmware Up/Download | shall | should | ✅ | 🤔 | Bundled with 701 |
| 703 | Boot State | shall | shall if FW UP/DL | ✅ | 🤔 | Bundled with 701 |
| 801 | SoE Services | shall | should if SoE supported | ✅ (`ec_soe.c`) | ❌ | SoE drive profile not used |
| 901 | AoE Protocol | should | should | ❌ | ❌ | Not needed, no CANopen gateway |
| 1001 | VoE Protocol | may | may | ❌ | ❌ | Not needed |

## Synchronization with Distributed Clocks (DC)

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 1101 | DC support | shall | shall if DC supported | ✅ (`ec_dc.c`, DC(a)) | ✅ | **Core of this project** — Phase 6, DC(a)+DC(b) |
| 1102 | Continuous Propagation Delay compensation | should | should | ⏳ | ✅ | Spec's suggested technique (5.13.2): replace the NOP with a periodic BWR at 0x0900 — applied inside the PI loop |
| 1103 | Sync window monitoring | should | should | ⏳ | ✅ | Read 0x092C (System Time Difference) via BRD — added to `ecm_diag` in Phase 7 |

## Slave-to-Slave Communication & Master information

| ID | Feature | Class A | Class B | SOEM 2.0 available | Project implements | Notes |
|---|---|---|---|---|---|---|
| 1201 | Slave-to-Slave via Master | **shall** | **shall** | ❌ | ❌ | **OUT OF SCOPE** — see warning at top of file. Requires LRW across multiple logical address ranges plus inter-cycle data copying; not part of this project's simple motor-control use case |
| 1301 | Master Object Dictionary | should | may | ❌ | ❌ | Not mandatory for Class B (`may`), skipped |

## Feature Packs (optional)

| Feature Pack | Category | Project implements | Notes |
|---|---|---|---|
| FP Cable Redundancy — Basic Functions | M | 🤔 | GD8 8.6 (optional) not done — **not verified**. SOEM supports it (`ecx_init_redundant`); the SO_TXTIME patch leaves the secondary port on plain `send()` |
| FP Cable Redundancy — Diagnosis Functions | M | 🤔 | Bundled with the above |
| FP Cable Redundancy — Redundancy with Hot Connect | O | ❌ | Not implemented, depends on 1201 which is also skipped |
| FP Cable Redundancy — Redundancy with DC | O | ❌ | Optional, not prioritized |
| FP Motion Control — Drive Profile CiA402 | M | ✅ | **Directly relevant** — CiA402 lives at the slave/application layer (Platform 1-4); the master needs correct DC sync support (already covered by 1101) |
| FP Motion Control — Drive Profile SERCOS | O | ❌ | SERCOS not used |
| FP Motion Control — Synchronization with DC | M | ✅ | = 1101 |
| FP Hot Connect | — | ❌ | Spec states "to be defined" (not yet finalized in v1.0.2) |
| FP External Synchronization | — | ❌ | Spec states "to be defined" |
| FP EtherCAT Automation Protocol | — | ❌ | Spec states "to be defined" |
| FP Device Replacement | — | ❌ | Spec states "to be defined" |
| FP Mailbox Gateway | — | ❌ | Spec states "to be defined" |

## Related evidence (not ETG.1500 feature IDs)

| Topic | Result | Where |
|---|---|---|
| ESI for the soft_bus simulator | Validates against the ESI XML schema 1.17 (`tools/esi/check_xsd.sh`, CI, with negative control); accepted by TwinCAT 4024.78; `esi_check.py` SII ↔ ESI cross-check | GD8 8.2, 8.3 |
| Cross-check with IgH EtherCAT Master 1.6.13 | IgH brings the same bus (N=8) to OP for 600 s with 0 WKC errors, DC bus shift + SYNC0 | GD8 8.1 (X-01a/b/c) |
| Cross-check with TwinCAT 3 as master | TwinCAT drives soft_bus over a real cable: OP, OP→PREOP, CoE online read/write, device scan | GD8 8.3 (X-02s) |
| Launch time (TSN, SO_TXTIME + ETF offload on i226) | `ecm_run --link etf`: motion frame launched by the NIC at the cycle target, offset ~314 ns, spread ~30 ns; R-02 A/B vs af_packet: no WKC loss at N=8/32, DC error p99 halved at N=8. Not an ETG requirement; a master-side implementation choice for 201/1101 | GD8 8.5, `docs/i226.md` |

## Self-assessment conclusion

The project meets most of **Class B** (every Class B `shall` item is implemented, except 1201 and — since ENI import made it mandatory — 503 Complete Access), plus a substantial part of **Class A** (including DC, which Class A requires at a higher bar than Class B in some respects). No claim of fully meeting either class is made, due to the deliberate omission of 1201 (Slave-to-Slave) — a scoping decision, not a technical shortcoming.
