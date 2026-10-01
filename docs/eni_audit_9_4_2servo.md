## `eni_2servo.xml`

2 slave(s). Master InitCmds: 12.

**Cyclic datagrams** (process data by LRW)

| cmd | address | length | role |
|---|---|---|---|
| NOP | ado 0x0900 | 4 | DC receive times |
| ARMW | ado 0x0910 | 4 | DC system time |
| LRW | 0x01000000 | 56 | process data |
| BRD | ado 0x0130 | 2 | AL status |

### Slave 1: Drive 1 (IS620N)

- identity: vendor 1048576 product 786696 rev 65537 serial 0; PhysAddr 1001 AutoIncAddr 0
- process data: send 96 bit, recv 224 bit; RxPdo ['#x1701'] TxPdo ['#x1b01']
- mailbox: CoE
- DC: ReferenceClock true CycleTime0 1000000 CycleTime1 0 ShiftTime 0

| # | trans | cmd | adp | ado | register | data | comment | known |
|---|---|---|---|---|---|---|---|---|
| 1 | PI,BI,SI,OI | APWR | 0 | 0x0120 | AL control | `1100` | set device state to INIT | yes |
| 2 | SI,OI | APWR | 0 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 3 | PI,SI,OI | APRD | 0 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 4 | BI | APRD | 0 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 5 | IP,IB | APWR | 0 | 0x0120 | AL control | `1100` | set device state to INIT | yes |
| 6 | IP,IB | APRD | 0 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 7 | IP | APWR | 0 | 0x0500 | SII access | `00` | assign EEPROM to ECAT | yes |
| 8 | IP | APWR | 0 | 0x0502 | SII control/status | `000108000000` | check vendor id | yes |
| 9 | IP | APRD | 0 | 0x0508 | SII data | `00000000` | check vendor id | yes |
| 10 | IP | APWR | 0 | 0x0502 | SII control/status | `00010a000000` | check product code | yes |
| 11 | IP | APRD | 0 | 0x0508 | SII data | `00000000` | check product code | yes |
| 12 | IP | APWR | 0 | 0x0502 | SII control/status | `00010c000000` | check revision number | yes |
| 13 | IP | APRD | 0 | 0x0508 | SII data | `00000000` | check revision number | yes |
| 14 | IP,IB | APWR | 0 | 0x0010 | station address | `e903` | set physical address | yes |
| 15 | IP,IB,PI,SI,OI | FPWR | 1001 | 0x0800 | SM 0 | `000000000000000000000000...(16 B)` | clear sm 0/1 (mailbox out/in) | yes |
| 16 | BI | APWR | 0 | 0x0800 | SM 0 | `000000000000000000000000...(16 B)` | clear sm 0/1 (mailbox out/in) | yes |
| 17 | IP,IB | FPWR | 1001 | 0x0800 | SM 0 | `0010800026000100` | set sm 0 (mailbox out) | yes |
| 18 | IP,IB | FPWR | 1001 | 0x0808 | SM 1 | `0014800022000100` | set sm 1 (mailbox in) | yes |
| 19 | PS | FPWR | 1001 | 0x09A0 | SYNC0 cycle | `40420f0000000000` | set DC cycle time | yes |
| 20 | PS | FPWR | 1001 | 0x0990 | DC start time | `0000000000000000` | set DC start time | yes |
| 21 | PS | FPWR | 1001 | 0x0980 | DC activation (AssignActivate) | `0003` | set DC activation | yes |
| 22 | PS | FPWR | 1001 | 0x09A8 | Latch0/1 control | `0000` | set DC latch cfg | yes |
| 23 | IP,PP | FPWR | 1001 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 24 | SP,OP | FPWR | 1001 | 0x0120 | AL control | `1200` | set device state to PREOP | yes |
| 25 | IP,SP,SI,OP,OI | FPWR | 1001 | 0x0810 | SM 2 | `000000000000000000000000...(16 B)` | clear sms | yes |
| 26 | PS | FPWR | 1001 | 0x0810 | SM 2 | `00180c0064000100` | set sm 2 (outputs) | yes |
| 27 | PS | FPWR | 1001 | 0x0818 | SM 3 | `001c1c0020000100` | set sm 3 (inputs) | yes |
| 28 | PS | FPWR | 1001 | 0x0600 | FMMU 0 | `000000010c00000700180002...(16 B)` | set fmmu 0 (outputs) | yes |
| 29 | PS | FPWR | 1001 | 0x0610 | FMMU 1 | `000000011c000007001c0001...(16 B)` | set fmmu 1 (inputs) | yes |
| 30 | OS | FPWR | 1001 | 0x0120 | AL control | `0400` | set device state to SAFEOP | yes |
| 31 | SP,SI,OP,OI | FPWR | 1001 | 0x0600 | FMMU 0 | `000000000000000000000000...(16 B)` | clear fmmu 0 | yes |
| 32 | SP,SI,OP,OI | FPWR | 1001 | 0x0610 | FMMU 1 | `000000000000000000000000...(16 B)` | clear fmmu 1 | yes |
| 33 | SP,OP | FPWR | 1001 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 34 | SP,OP | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for PREOP | yes |
| 35 | IP,IB | FPWR | 1001 | 0x0500 | SII access | `01` | assign EEPROM to PDI | yes |
| 36 | II | APWR | 0 | 0x0500 | SII access | `00` | assign EEPROM back to ECAT | yes |
| 37 | IP | FPWR | 1001 | 0x0120 | AL control | `1200` | set device state to PREOP | yes |
| 38 | IP | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for PREOP | yes |
| 39 | IP,BI | APWR | 0 | 0x0500 | SII access | `00` | assign EEPROM back to ECAT | yes |
| 40 | IB | FPWR | 1001 | 0x0120 | AL control | `1300` | set device state to BOOT | yes |
| 41 | IB | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for BOOT | yes |
| 42 | PS | FPWR | 1001 | 0x0120 | AL control | `0400` | set device state to SAFEOP | yes |
| 43 | PS | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for SAFEOP | yes |
| 44 | OS | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for SAFEOP | yes |
| 45 | SO | FPWR | 1001 | 0x0120 | AL control | `0800` | set device state to OP | yes |
| 46 | SO | FPRD | 1001 | 0x0130 | AL status | `000000000000` | check device state for OP | yes |

| # | trans | ccs | object | CA | timeout ms | data | comment | disabled |
|---|---|---|---|---|---|---|---|---|
| 1 | PS | 1 | 0x1C12:00 | no | 0 | `00` | clear sm pdos (0x1C12) |  |
| 2 | PS | 1 | 0x1C13:00 | no | 0 | `00` | clear sm pdos (0x1C13) |  |
| 3 | PS | 1 | 0x1C12:01 | no | 0 | `0117` | download pdo 0x1C12:01 index |  |
| 4 | PS | 1 | 0x1C12:00 | no | 0 | `01` | download pdo 0x1C12 count |  |
| 5 | PS | 1 | 0x1C13:01 | no | 0 | `011b` | download pdo 0x1C13:01 index |  |
| 6 | PS | 1 | 0x1C13:00 | no | 0 | `01` | download pdo 0x1C13 count |  |
| 7 | PS | 1 | 0x6060:00 | no | 0 | `08` | Modes of operation |  |

### Slave 2: Drive 2 (IS620N)

- identity: vendor 1048576 product 786696 rev 65537 serial 0; PhysAddr 1002 AutoIncAddr 65535
- process data: send 96 bit, recv 224 bit; RxPdo ['#x1701'] TxPdo ['#x1b01']
- mailbox: CoE
- DC: ReferenceClock false CycleTime0 1000000 CycleTime1 0 ShiftTime 0

| # | trans | cmd | adp | ado | register | data | comment | known |
|---|---|---|---|---|---|---|---|---|
| 1 | PI,BI,SI,OI | APWR | 65535 | 0x0120 | AL control | `1100` | set device state to INIT | yes |
| 2 | SI,OI | APWR | 65535 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 3 | PI,SI,OI | APRD | 65535 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 4 | BI | APRD | 65535 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 5 | IP,IB | APWR | 65535 | 0x0120 | AL control | `1100` | set device state to INIT | yes |
| 6 | IP,IB | APRD | 65535 | 0x0130 | AL status | `0000` | check device state for INIT | yes |
| 7 | IP | APWR | 65535 | 0x0500 | SII access | `00` | assign EEPROM to ECAT | yes |
| 8 | IP | APWR | 65535 | 0x0502 | SII control/status | `000108000000` | check vendor id | yes |
| 9 | IP | APRD | 65535 | 0x0508 | SII data | `00000000` | check vendor id | yes |
| 10 | IP | APWR | 65535 | 0x0502 | SII control/status | `00010a000000` | check product code | yes |
| 11 | IP | APRD | 65535 | 0x0508 | SII data | `00000000` | check product code | yes |
| 12 | IP | APWR | 65535 | 0x0502 | SII control/status | `00010c000000` | check revision number | yes |
| 13 | IP | APRD | 65535 | 0x0508 | SII data | `00000000` | check revision number | yes |
| 14 | IP,IB | APWR | 65535 | 0x0010 | station address | `ea03` | set physical address | yes |
| 15 | IP,IB,PI,SI,OI | FPWR | 1002 | 0x0800 | SM 0 | `000000000000000000000000...(16 B)` | clear sm 0/1 (mailbox out/in) | yes |
| 16 | BI | APWR | 65535 | 0x0800 | SM 0 | `000000000000000000000000...(16 B)` | clear sm 0/1 (mailbox out/in) | yes |
| 17 | IP,IB | FPWR | 1002 | 0x0800 | SM 0 | `0010800026000100` | set sm 0 (mailbox out) | yes |
| 18 | IP,IB | FPWR | 1002 | 0x0808 | SM 1 | `0014800022000100` | set sm 1 (mailbox in) | yes |
| 19 | PS | FPWR | 1002 | 0x09A0 | SYNC0 cycle | `40420f0000000000` | set DC cycle time | yes |
| 20 | PS | FPWR | 1002 | 0x0990 | DC start time | `0000000000000000` | set DC start time | yes |
| 21 | PS | FPWR | 1002 | 0x0980 | DC activation (AssignActivate) | `0003` | set DC activation | yes |
| 22 | PS | FPWR | 1002 | 0x09A8 | Latch0/1 control | `0000` | set DC latch cfg | yes |
| 23 | IP,PP | FPWR | 1002 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 24 | SP,OP | FPWR | 1002 | 0x0120 | AL control | `1200` | set device state to PREOP | yes |
| 25 | IP,SP,SI,OP,OI | FPWR | 1002 | 0x0810 | SM 2 | `000000000000000000000000...(16 B)` | clear sms | yes |
| 26 | PS | FPWR | 1002 | 0x0810 | SM 2 | `00180c0064000100` | set sm 2 (outputs) | yes |
| 27 | PS | FPWR | 1002 | 0x0818 | SM 3 | `001c1c0020000100` | set sm 3 (inputs) | yes |
| 28 | PS | FPWR | 1002 | 0x0600 | FMMU 0 | `1c0000010c00000700180002...(16 B)` | set fmmu 0 (outputs) | yes |
| 29 | PS | FPWR | 1002 | 0x0610 | FMMU 1 | `1c0000011c000007001c0001...(16 B)` | set fmmu 1 (inputs) | yes |
| 30 | OS | FPWR | 1002 | 0x0120 | AL control | `0400` | set device state to SAFEOP | yes |
| 31 | SP,SI,OP,OI | FPWR | 1002 | 0x0600 | FMMU 0 | `000000000000000000000000...(16 B)` | clear fmmu 0 | yes |
| 32 | SP,SI,OP,OI | FPWR | 1002 | 0x0610 | FMMU 1 | `000000000000000000000000...(16 B)` | clear fmmu 1 | yes |
| 33 | SP,OP | FPWR | 1002 | 0x0980 | DC activation (AssignActivate) | `0000` | clear DC activation | yes |
| 34 | SP,OP | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for PREOP | yes |
| 35 | IP,IB | FPWR | 1002 | 0x0500 | SII access | `01` | assign EEPROM to PDI | yes |
| 36 | II | APWR | 65535 | 0x0500 | SII access | `00` | assign EEPROM back to ECAT | yes |
| 37 | IP | FPWR | 1002 | 0x0120 | AL control | `1200` | set device state to PREOP | yes |
| 38 | IP | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for PREOP | yes |
| 39 | IP,BI | APWR | 65535 | 0x0500 | SII access | `00` | assign EEPROM back to ECAT | yes |
| 40 | IB | FPWR | 1002 | 0x0120 | AL control | `1300` | set device state to BOOT | yes |
| 41 | IB | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for BOOT | yes |
| 42 | PS | FPWR | 1002 | 0x0120 | AL control | `0400` | set device state to SAFEOP | yes |
| 43 | PS | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for SAFEOP | yes |
| 44 | OS | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for SAFEOP | yes |
| 45 | SO | FPWR | 1002 | 0x0120 | AL control | `0800` | set device state to OP | yes |
| 46 | SO | FPRD | 1002 | 0x0130 | AL status | `000000000000` | check device state for OP | yes |

| # | trans | ccs | object | CA | timeout ms | data | comment | disabled |
|---|---|---|---|---|---|---|---|---|
| 1 | PS | 1 | 0x1C12:00 | no | 0 | `00` | clear sm pdos (0x1C12) |  |
| 2 | PS | 1 | 0x1C13:00 | no | 0 | `00` | clear sm pdos (0x1C13) |  |
| 3 | PS | 1 | 0x1C12:01 | no | 0 | `0117` | download pdo 0x1C12:01 index |  |
| 4 | PS | 1 | 0x1C12:00 | no | 0 | `01` | download pdo 0x1C12 count |  |
| 5 | PS | 1 | 0x1C13:01 | no | 0 | `011b` | download pdo 0x1C13:01 index |  |
| 6 | PS | 1 | 0x1C13:00 | no | 0 | `01` | download pdo 0x1C13 count |  |
| 7 | PS | 1 | 0x6060:00 | no | 0 | `08` | Modes of operation |  |

**Register InitCmds not in the known list: 0**

