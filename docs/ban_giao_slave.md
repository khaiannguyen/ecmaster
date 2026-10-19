# Hợp đồng master → slave (`master-v1.0`)

**Dành cho:** người làm slave EtherCAT chạy với master này (dự án slave LAN9252 + STM32H723 Platform 1/2, slave 4 trục Platform 4, hoặc slave khác).
**Trạng thái:** bản cuối cho `master-v1.0` (GĐ12.4, 19/10/2026). Hành vi trên dây ghi ở đây được khoá bằng golden (`tools/golden/`); đổi hành vi → tag `master-v1.0.x` và chạy lại hồi quy đầy đủ.
**Nguyên tắc:** master là hằng số. Lỗi khi đưa một slave mới lên mặc định là lỗi phía slave, cho tới khi §5 chứng minh ngược lại.

## 1. Tầng vật lý
| Mục | Giá trị |
|---|---|
| NIC | Intel i226, driver `igc` (`docs/i226.md`); cổng được cấu hình lúc boot bởi `ecmaster-net@IFACE` (`docs/systemd.md`) |
| Tốc độ | Theo slave (100BASE-TX full duplex với LAN9252, IS620N). ETF offload đã kiểm ở 100 Mbit/s: missed 0, launch_err max 177 ns |
| Backend | `af_packet` mặc định; `--link etf` (launch time phần cứng) là tùy chọn, cần `ecmaster-clock@IFACE`. **Một chiến dịch đo dùng một backend cố định**, ghi vào mỗi lần chạy (`stats.json` → `bus.link`, `rt.*`) |

## 2. Master làm gì trên dây
### 2.1 Khởi tạo và ESM
- SOEM `v2.0.0` + `patches/` (mailbox Cnt, SO_TXTIME), `EC_MAXGROUP=4`.
- Timeout chuyển trạng thái: không `--eni` thì của SOEM (2 s); có `--eni` thì theo `Timeout` của ENI, không nhỏ hơn 2 s.
- Kiểm **từng slave** lên đúng trạng thái (không tin giá trị OR của BRD).
- SAFE-OP → OP: master gửi process data liên tục trong lúc chờ OP (slave chậm như IS620N cần tới 9 s), nên SM watchdog không hết hạn trong lúc chuyển.
- AL status code lớp cấu hình (0x001D, 0x001E, 0x0011…, danh sách ở `docs/fault_policy.md` §13): bus vào `FAILED(config)`, **không thử lại**, in mã và slave. Mã khác hoặc slave rơi khỏi OP lúc chạy: phục hồi theo slave, không tự enable lại trục.
- Slave mất nguồn rồi có lại: master cấu hình lại (chạy lại InitCmd của ENI) và chỉ đưa lên OP khi mọi InitCmd thành công.
- Lệnh ENI master từ chối: LRD/LWR cho process data, register InitCmd ngoài `config/eni/known_regs.txt` (`docs/eni.md`).

### 2.2 SM watchdog
| Thanh ghi | Giá trị | Ý nghĩa |
|---|---|---|
| 0x0400 | 2498 | (2498 + 2) × 40 ns = tick 100 µs |
| 0x0420, slave nhóm motion | 30 | **3 ms** |
| 0x0420, slave nhóm IO | 240 | 24 ms |

0x0410 (PDI watchdog) không được ghi: đó là việc của firmware slave. Giá trị 3 ms **không đổi theo chu kỳ** (6 chu kỳ ở 500 µs, 12 chu kỳ ở 250 µs). Dừng vật lý khi mất bus là việc của slave qua SM watchdog: master không phải chức năng an toàn (`docs/safety_boundary.md`).

### 2.3 Process data
- Hai nhóm: motion (mặc định 1 ms, mang DC) và IO (mặc định 8 ms); `--motion-slaves` / `--io-slaves` (`docs/process_data_groups.md`). Lệnh cyclic: **LRW**, WKC kỳ vọng riêng từng nhóm.
- Bảng PDO từ `--eni` hoặc `--pdo-scan` (đọc 0x1C12/0x1C13 và mapping qua SDO ở PRE-OP, SII nếu không có CoE). Có cả hai thì phải khớp từng entry. Slave khác vendor `0x00000499` thì bind theo (index, sub) **bắt buộc** `--pdo-scan` (`docs/pdo_bind.md`).
- **SOEM dùng lại mapping đã biết** của slave cùng Vendor/Product/Revision: đổi PDO ở slave thì **phải tăng Revision** trong SII.
- Reply muộn / trùng / đảo: cách ly index, kiểm header reply, cổng tuổi DC (`docs/fault_policy.md` §11).
- Độ tươi input (`--fresh`) chỉ khi slave có bộ đếm ứng dụng trong TxPDO; không có thì master **không phát hiện được** "WKC đúng nhưng dữ liệu cũ" của slave đó (README).

### 2.4 Mailbox / CoE
- SDO upload/download normal, segmented, Complete Access (download); EMCY nhận và gán cho slave/trục; SDO lúc OP không làm gián đoạn PDO (một frame mailbox mỗi chu kỳ).
- Master **không dùng** SDO Info. Không có FoE, EoE, SoE, VoE.
- **Mailbox tuần hoàn cần một FMMU cho trạng thái SM1.** ESC chỉ có 2 FMMU (IS620N) → không có SDO/EMCY ở SAFE-OP/OP (GĐ11.6 D-15).

### 2.5 Distributed Clock
| Mục | Giá trị |
|---|---|
| Kiểu | **Master-shift**: master chỉnh nhịp của chính nó bám reference clock; không ghi 0x0910 của ref |
| Reference clock | Slave có DC **đầu tiên** trên bus (ENI chỉ ref khác → từ chối) |
| SYNC0 | Chu kỳ = chu kỳ motion. Không `--eni`: shift 0. Có `--eni`: AssignActivate (0x0300/0x0700), SYNC1, shift theo ENI |
| Setpoint pha | Frame tới ref **30 % chu kỳ sau SYNC0** (300 µs ở 1 ms, 75 µs ở 250 µs), `--dc-setpoint-pct`. Mặc định không đổi trong `v1.0` |
| Wrap 32 bit | Xử lý tường minh: 8 h soak, 6705 wrap, 0 unlock |
| 0x0930 (reset bộ lọc) | Không ghi (giống SOEM) |
| 0x092C | **Không đọc** (ETG.1500 1103 chưa có). Số chất lượng DC master báo: \|e\| của vòng master-shift |
| 0x1C32/0x1C33 | Chỉ ghi khi ENI có InitCmd tương ứng |

Thời gian chết từ lúc slave chốt input tới lúc output được áp: **2 chu kỳ**, không phụ thuộc setpoint (`docs/rt_profiles.md` §13, `tools/gd11/deadtime.py`).

### 2.6 CiA402 (`--axis` + `--hook cia402`)
- Ở PRE-OP: bind đủ object theo mode, **đọc 0x6502** và từ chối mode slave không hỗ trợ (`--axis-no-6502` để bỏ qua). Trục n của một slave = index + 0x800·n.
- Chuỗi enable 0x06 → 0x07 → 0x0F, timeout mỗi bước; đổi mode xác nhận qua 0x6061 trước khi gửi setpoint.
- Chốt S1–S7: không tự enable lại, target = actual trước enable, giữ vị trí khi cạn setpoint, chặn bước nhảy, mất bus → disabled, tắt đúng trình tự khi thoát (S6), reset lỗi chỉ khi có lệnh (`docs/safety_boundary.md`).
- Mã lỗi gán cho trục: 0x603F của trục trước; EMCY (không mang số trục) chỉ gán được khi slave có 1 trục hoặc đúng 1 trục đang lỗi.

## 3. Yêu cầu với slave mới (rút ra từ GĐ9–11)
| # | Yêu cầu | Vì sao |
|---|---|---|
| Y-1 | **ESC có ≥ 3 FMMU** (process data out, in, trạng thái SM1) | Không có FMMU thứ 3 thì không có SDO/EMCY lúc OP (§2.4). LAN9252 có 3 FMMU |
| Y-2 | **0x603F (Error code) của từng trục nằm trong TxPDO** | Chẩn đoán lỗi trục không phụ thuộc mailbox; bắt buộc khi > 1 trục trên một slave |
| Y-3 | Đổi PDO → tăng Revision trong SII và ESI | §2.3 (cache mapping của SOEM) |
| Y-4 | 0x6502 có và đúng | Master từ chối mode không có trong 0x6502 |
| Y-5 | Cửa sổ thời gian ở chu kỳ ngắn: với setpoint 30 %, slave có **≈ 0,3 × chu kỳ từ SYNC0** để chốt input và ghi xong TxPDO (SM3) trước khi frame đi qua (75 µs ở 250 µs), và **≈ 0,7 × chu kỳ trừ thời gian frame tới slave** để đọc RxPDO (SM2) trước SYNC0 kế (khoảng 175 µs ở 250 µs) | Muốn input của chính chu kỳ đó tới master và output kịp SYNC0 kế |
| Y-6 | SM watchdog tắt PWM/đưa trục về an toàn trong ≤ 3 ms khi mất process data | Master không phải chức năng an toàn (§2.2) |
| Y-7 | Bộ đếm ứng dụng trong TxPDO nếu muốn master phát hiện dữ liệu cũ (`--fresh`) | §2.3 |
| Y-8 | ESI hợp lệ theo schema EtherCATInfo (`tools/esi/check_xsd.sh`), SII khớp ESI (`tools/esi/esi_check.py`) | R-03 của bring-up |

## 4. Đưa một slave mới lên (thứ tự đã dùng với IS620N, `docs/bringup_10_0.md`)
1. `ecm_peek`: đếm slave, dump SII, identity, SDO cơ bản; không lên quá PRE-OP, không ghi gì.
2. `esi_check.py` SII ↔ ESI; `esi2profile.py` sinh profile soft_bus từ ESI và diễn tập mọi bước dưới đây trên soft_bus trước (`SIM=1`).
3. `ecm_run --pdo-scan --pdo-dump` lên OP không trục, 1 h: 0 WKC sai, DC LOCKED.
4. TwinCAT sinh ENI từ ESI (`docs/twincat_9_4.md`), `eni2cfg.py`, chạy lại bước 3 với `--eni`.
5. Trục: `--axis … --hook cia402` với giới hạn của drive đã ghi và đọc lại, motor kẹp, aptomat là dừng khẩn (`docs/w_10_9.md`).
6. Theo dõi: `ecm_diag` (lỗi cáp, AL status, mất link), `stats.json` (`docs/api.md`).

## 5. Chứng minh lỗi không nằm ở master
1. **Đúng tag:** `git describe --tags` (hoặc `/opt/ecmaster/VERSION`) đúng tag ghi trong nhật ký đo của slave; `tools/jetson/check_env.sh` pass.
2. **Master vẫn như lúc tag:** `tools/golden/check_golden.sh` (golden 555 dòng, ENI 581, N=1 90, CA 556) khớp.
3. **Cùng dòng lệnh trên soft_bus** với profile sinh từ ESI của slave: chạy được trên soft_bus mà lỗi trên slave thật → nghi slave trước (SII, firmware, PDI).
4. **Master độc lập:** cùng slave với TwinCAT hoặc IgH; hai master cùng lỗi → gần như chắc là slave.
5. **Số timing của master** (wake jitter, prep_send, turnaround, occupancy) nằm trong dải chuẩn (§6); lệch thì kiểm tải Jetson trước.

Bước 2 hoặc 3 fail → lỗi ở master: sửa thành `master-v1.0.x`, chạy lại hồi quy đầy đủ.

## 6. Số chuẩn của master
| Đại lượng (µs) | soft_bus 1 slave, 4 trục CSP, veth, 8 h (GĐ10.10) | 2 × IS620N, 1 ms, i226, 1 h, không tải (BM-02, af_packet / etf) |
|---|---|---|
| wake jitter p99.99 / max | 3,5 / 16 | 5,6 / 14,4 · 5,6 / 64,6 |
| prep_send p99.99 | — | 17,8 · 11,2 |
| turnaround p99.99 | — | 70,8 · 100,0 |
| occupancy p99.99 / max | 71 / 112 | 79,4 / 159,4 · 112,2 / 174,7 |
| DC \|e\| p99.99 | 27 | 11 · **1** |

Có tải CPU 0–2 và chu kỳ ngắn: `docs/benchmark.md`, `docs/rt_profiles.md`. Số đo trên slave mới phải ghi kèm `stats.json` (`rt.*`: kernel, CPU cô lập, cpuidle, backend).

## 7. Môi trường đóng băng của `master-v1.0`
| Mục | Giá trị |
|---|---|
| JetPack / kernel | r39.2, `6.8.12-1021-rt-tegra` (PREEMPT_RT) |
| `igc.ko` | build tay từ nguồn kernel (`docs/i226.md`), không nằm trong gói |
| Boot profile | **P0** mặc định: `isolcpus=3 irqaffinity=0-2 transparent_hugepage=never`, cpuidle bật, `ecm_run` giữ `/dev/cpu_dma_latency`. P1 (`cpuidle.off=1`) cho 250 µs (`docs/rt_profiles.md` §2.0) |
| Cổng EtherCAT | `ecmaster-net@enP1p1s0`: `rx-usecs 0`, GRO/GSO/TSO off, EEE off, NetworkManager unmanaged |
| Power | MAXN_SUPER + `jetson_clocks` (chạy tay sau boot) |
| SOEM | `v2.0.0` + `patches/` (sha256 trong `patches/SHA256SUMS`), `tools/release/build_soem.sh` |
| `ecmaster` | tag `master-v1.0`, commit và số đo của lần hồi quy cuối: release notes và evidence pack (GĐ12.6) |
