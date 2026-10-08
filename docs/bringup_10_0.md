# Bring-up 10.0 — 2 servo IS620N thật (R-01 … R-06)

Kế hoạch: `claude/giai_doan_10_ke_hoach.md` §2. Script: `tools/gd10/run_bringup_10_0.sh`.
Buổi này **không enable trục nào**: không `--hook cia402`, không `--cia402-script`, không `--pdo-set`;
output giữ 0 (controlword 0 = Disable voltage). Script tự từ chối nếu thấy các cờ đó.

## 1. Chuẩn bị trước buổi tối (làm được khi chưa có servo)

1. Trên Jetson: `git am` bản vá mới, `make all caps`, kiểm `apps/ecm_peek/ecm_peek` đã build.
2. Diễn tập trên servo ảo (veth, 2 IS620N ảo trên `soft_bus`, ~2 phút):
   ```bash
   sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up
   sudo -E SIM=1 IS620N_ESI=<đường dẫn thật tới IS620N-Ecat_v2.6.9.xml> tools/gd10/run_bringup_10_0.sh
   ```
   Kỳ vọng: R-01…R-06b PASS (R-03 so SII sinh từ ESI với chính ESI).
3. Đối chứng âm (revision sai → R-02 FAIL, ecm_run từ chối ENI):
   ```bash
   sudo -E SIM=1 SIM_REV=0x00010002 STEPS="r02 r05" tools/gd10/run_bringup_10_0.sh
   ```
   Kỳ vọng: `RESULT: … FAIL`, R-02 FAIL nêu revision 0x00010002 ≠ ENI 0x00010001, R-05 FAIL (identity check), R-06 SKIP.
4. Không chạy diễn tập khi đang soak.

## 2. An toàn điện (trước khi cắm nguồn)

- [ ] MCB 2 cực trong tầm tay; PE nối tới 2 driver và vỏ motor.
- [ ] P–D giữ nguyên, **không** nối P–C.
- [ ] Nút dừng khẩn (NC) vào DI của từng driver, logic mở mạch = dừng.
- [ ] Motor đặt chắc/bắt chặt, trục để trống, không tải.
- [ ] Buổi R-01…R-06: **chỉ cấp nguồn điều khiển L1C/L2C**, động lực L1/L2 tắt.
- [ ] Laptop TwinCAT rút khỏi bus (hai master trên một bus = lỗi khó hiểu).

## 3. InoDriverShop (trước khi chạy master)

Đọc và **ghi lại** (chép vào nhật ký 10.0), chỉ sửa khi cần, kiểm số tham số trong manual IS620N trước khi đặt:
- chế độ điều khiển EtherCAT; phiên bản firmware (so với 0x100A đọc ở R-04);
- giới hạn torque (nội, hai chiều) và giới hạn tốc độ — đặt thấp (≤ 20 % torque, ≤ 300 rpm) trước 10.9;
- gán chức năng DI cho dừng khẩn và mức logic;
- tham số đồng bộ / chu kỳ nếu manual có (so với 0x1C32 đọc ở R-04).

## 4. Chạy thật (khoảng 13 phút)

Cáp: `enP1p1s0` → IS620N #1 CN3 (IN), #1 CN4 (OUT) → #2 CN3.

```bash
cd ~/projects/ecmaster
tools/jetson/check_env.sh --iface enP1p1s0
sudo -E IS620N_ESI=<đường dẫn ESI> tools/gd10/run_bringup_10_0.sh
```
Script in checklist và đợi gõ `yes`. Lượt ngắn để thử: `LINK_SEC=60 R05_SEC=30`. Chạy riêng một bước: `STEPS=r02` (chỉ đọc).

| Bước | Thời gian | Làm gì | PASS khi |
|---|---|---|---|
| R-01 | 10 phút (`LINK_SEC`) | `ethtool`, carrier_changes, bộ đếm lỗi | 100Mb/s Full, 0 lần mất link, bộ đếm lỗi không tăng |
| R-02 | vài giây | `ecm_peek`: SII + PRE-OP, chỉ đọc | 2 slave, vendor/product/**revision** = ENI = ESI, 0x1018 = SII |
| R-03 | vài giây | `esi_check.py` cho từng SII dump | identity khớp; khác biệt khác ghi DIFF (V-01 trên EEPROM thật) |
| R-04 | ~30 s | ~150 SDO: 0x6502, 0x1C12/13, 0x1600–0x1705, 0x1A00–0x1B04, 0x1C32/33, giới hạn | 0x6502 có CSP; mapping so ESI (khác = DIFF, đó là câu trả lời R-04) |
| R-05 | 60 s (`R05_SEC`) | `ecm_run --eni eni_2servo --pdo-scan`, không hook, có pcap | OP, 14/14 InitCmd, ENI == bus, DC LOCKED, 0 WKC sai, 0 overrun; statusword **không** phải Operation enabled |
| R-06 | — | đọc từ R-05 | ENI == bus (PASS) hoặc `--pdo-scan` từ chối rõ (DIFF) |
| R-06b | ~15 s | `--axis 1:0,2:0 --axis-modes csp`, không hook | 2 trục cấu hình, 0x6502 đọc qua master, "not driven" |

Kết quả: `log_gd10_bringup_*/report.md` (+ `peek.txt`, `sii/`, `esi_check_*.txt`, `er_r05.log`, `r05.pcapng`).

## 5. Đọc kết quả và xử lý

- **R-02 FAIL vì revision**: dừng. Sinh lại ENI trong TwinCAT với đúng revision (kế hoạch GĐ9 §6.1), rồi `eni2cfg.py`.
- **R-04 DIFF** (mapping khác ESI): ghi vào `claude/is620n_esi_review.md`; nếu 0x1C12/0x1C13 khác 0x1701/0x1B01 thì InitCmd của ENI sẽ ghi lại — R-06 cho biết bus cuối cùng có khớp không.
- **R-05 không lên OP**: xem dòng `report_state_failure`/AL code trong `er_r05.log`; thời gian thật nằm ở dòng `state transitions` (ESI: PREOP 3 s, SAFEOP→OP 9 s).
- Statusword khi chỉ có nguồn điều khiển: có thể là Switch on disabled không có bit voltage, hoặc Fault (thấp áp động lực) — ghi lại, đó là số liệu cho servo ảo (10.2).
- `[AXIS]`/EMCY trong log: ghi mã; R-07 sẽ thu có chủ đích.

## 6. Buổi 2: R-01 đầy đủ, R-07, R-09 (sau buổi 8/10)

Kết quả buổi 1: `claude/giai_doan_10_nhat_ky_10_0.md`. Mỗi lần bật Jetson: `sudo tools/jetson/setup_link.sh` (link up — NetworkManager không quản cổng này; GRO/GSO/TSO off; `rx-usecs 0`; EEE off).

### 6.1 R-01 đầy đủ (10 phút, sau khi đổi cáp có bọc chống nhiễu)
```bash
sudo -E YES=1 STEPS=r01 tools/gd10/run_bringup_10_0.sh
```
PASS khi 100/Full, 0 lần mất link, `rx_crc_errors` không tăng. Mọi lượt `ecm_run` của runner giờ cũng chụp `ethtool -S` trước/sau và in "NIC error counters" trong report.

### 6.2 R-09 chu kỳ 2 ms và 500 µs (~2,5 phút)
```bash
sudo -E IS620N_ESI=... YES=1 STEPS=r09 tools/gd10/run_bringup_10_0.sh
```
Mỗi chu kỳ: `tools/eni/eni_cycle.py` tạo bản sao ENI (sync0_ns + InitCmd 0x09A0; bản đo, không phải ENI TwinCAT), `ecm_run --motion-cycle-us` 60 s (`R09_SEC`), chấm như R-05. SM watchdog tự giãn ≥ 3 chu kỳ (2 ms → 6 ms; 1 ms và 500 µs giữ 3 ms). Chu kỳ khác: `R09_CYCLES="4000 250"`. 0x1C32:05 của IS620N = 125 µs.

Kết quả 9/10: 2 ms PASS; 500 µs FAIL 76 NOFRAME / 120 000 (reply về muộn 355–430 µs, ~0,1 %) — nguyên nhân: CPU0 (nơi nhận ngắt i226, không chuyển được) ngủ sâu ở **c7** (kernel khai báo thoát mất 5000 µs). Giữ `/dev/cpu_dma_latency` = 0 → turnaround max 149 µs, 0 NOFRAME. Từ bản vá 0017 `ecm_run` tự giữ (cần root; không được thì in WARNING). Đối chứng âm: `ER_EXTRA=--no-dma-latency STEPS=r09 R09_CYCLES=500` phải FAIL lại.

### 6.3 R-07 sự kiện khi đang OP (~3 phút, cần thao tác tay)
Diễn tập trước trên servo ảo: `sudo -E SIM=1 tools/gd10/run_events_10_0.sh`.
```bash
sudo -E tools/gd10/run_events_10_0.sh                 # estop, mainpower, cable
sudo -E EVENTS="cable" tools/gd10/run_events_10_0.sh  # một sự kiện
```
Script đưa bus lên OP (không enable), rồi với từng sự kiện in việc cần làm; bạn **bấm Enter đúng lúc làm** (thời điểm ghi bằng CLOCK_MONOTONIC, cùng đồng hồ với log ecm_run), giữ `HOLD` 10 s, làm thao tác khôi phục, bấm Enter, đợi `SETTLE` 20 s.

| Sự kiện | Làm | Khôi phục | Muốn ghi lại |
|---|---|---|---|
| estop | bấm dừng khẩn (DI) | nhả | có EMCY không, mã gì; statusword |
| mainpower | gạt MCB động lực L1/L2 xuống (nguồn điều khiển giữ) | gạt lên, đợi đèn CHARGE | EMCY thấp áp (dự kiến 0x3220 hoặc mã hãng), fault có chốt không |
| cable | rút cáp driver 1 OUT → driver 2 IN | cắm lại | bus PARTIAL/LOST, diag "chain broken after slave 1 … cable slave 1 - slave 2" (L5-11 thật), recovery về OP, thời gian |

PASS: `ecm_run` sống qua mọi sự kiện và bus kết thúc ở RUN. Từng sự kiện: OBSERVED (liệt kê dòng `[BUS]`/`[EMCY]`/`[RECOVERY]` theo thời gian tương đối) hoặc NONE. Master **không reset** lỗi của drive: lỗi do sự kiện gây ra vẫn chốt (S7) — xoá trên panel/ tắt-bật nguồn sau khi chạy xong. Log: `log_gd10_events_*/report.md`, `er_r07.log`, `r07.pcapng`, `diag_<sự kiện>_{do,after}.txt`.

Mã EMCY thu được → điền `config/emcy/is620n.emcy` (tra manual IS620N) và servo ảo 10.2.

### 6.4 R-08 đầy đủ
1 h (đã làm rút gọn 8/10) + 10 phút `--link etf`: làm sau R-09.
