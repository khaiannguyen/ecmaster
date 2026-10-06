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

## 6. Sau R-06 (cùng buổi nếu còn giờ)

- R-08 rút gọn: `STEPS=r05 R05_SEC=3600` = 1 h ở OP không enable (bản đầy đủ thêm 10 phút `--link etf`).
- R-07 (rút cáp giữa 2 servo, tắt động lực khi OP, bấm dừng khẩn) và R-09 (chu kỳ 2 ms / 500 µs): làm tay theo kế hoạch, cần ENI tương ứng cho R-09.
- Ghi `claude/giai_doan_10_nhat_ky_10_0.md`: revision, SII khác biệt, mapping thật, 0x6502, 0x1C32/0x1C33, thời gian chuyển trạng thái, statusword ở từng trạng thái nguồn, mã EMCY/AL.
