# Giai đoạn 10 — Tổng hợp: lớp CiA402 phía master và 2 servo IS620N thật

Thời gian: 2/10 → 9/10/2026. Nhánh `gd10-en`, bản vá 0001 … 0026 (sandbox → `git am` trên Jetson). Kế hoạch: `claude/giai_doan_10_ke_hoach.md`. Nhật ký từng bước: `claude/giai_doan_10_nhat_ky_10_*.md`. Tài liệu kỹ thuật: `docs/cia402.md`, `docs/safety_boundary.md`, `docs/app_rt_exchange.md`, `docs/soft_bus_cia402.md`, `docs/bringup_10_0.md`, `docs/w_10_9.md`, `docs/x05_twincat.md`, `docs/fault_policy.md`.

## 1. Mục tiêu
Master điều khiển được drive CiA402 (CSP/CSV/PP/PV/HM) mà không đưa index CiA402 vào `libecmaster`; hook RT không làm xấu timing; chốt an toàn phía master có đối chứng âm; kiểm trên servo ảo **và** trên 2 servo Inovance IS620N thật; so với TwinCAT NC.

## 2. Kết quả từng bước

| Bước | Nội dung | Kết quả |
|---|---|---|
| 10.1 | dọn đầu giai đoạn: `--pdo-scan` bắt buộc cho slave khác hãng, thiết kế trao đổi app ↔ RT, soak N=1 8 h | xong (2/10) |
| 10.2 | servo CiA402 ảo trong `soft_bus` (A-01…A-07); **hiệu chỉnh theo IS620N** (`is620n.cal`, A-10) | xong; W-02 ảo 144 856 vs thật 139 640 inc |
| 10.3 | cấu hình trục: object theo mode, 0x6502, 0x800·n, `axes.cfg`, K-05 | xong |
| 10.4 | hook RT + ring lệnh/setpoint + seqlock trạng thái | hook p99.99 2,2 µs (soak), golden khớp khi tắt |
| 10.5 | state machine drive, CSP/CSV | xong |
| 10.6 | chốt S1–S7 + 10 đối chứng âm (`make negctl`, `ecm_run_neg6`) | xong |
| 10.7 | PP (bắt tay bit 4/12), PV, homing | xong |
| 10.8 | lỗi trục (EMCY, 0x603F, AL CONFIG), `cw_trace.py` | xong |
| 10.0 | bring-up IS620N thật R-01…R-09 (không enable) | PASS; R-07 sau bản vá recovery 0018 |
| 10.9 | W-01…W-08 trên servo thật + CSV/PV (W-09csv, W-10pv) | PASS |
| X-05b | ENI 0x1702/0x1B02 từ TwinCAT (multi-mode) | PASS: 14/14 InitCmd, ENI == bus, mode by PDO |
| X-05 | chuỗi controlword master vs TwinCAT NC | PASS (enable giống; disable khác có chủ đích) |
| 10.10 | soak 8 h CSP 4 trục ảo; `run_regression_gd10.sh` (+ bước `w`, `wmm`); tài liệu | soak PASS (3–4/10); hồi quy cuối: §7 |

## 3. DoD GĐ10
- [x] Không literal CiA402 trong `libecmaster` (K-05, CI).
- [x] Hook tắt mặc định; tắt → golden khớp từng dòng (Q-01).
- [x] Ngân sách hook đo trên Jetson: p99.99 2,2 µs / max 5,7 µs (4 trục, 8 h), 3,5 µs (2 servo thật, 1 h).
- [x] Soak 8 h CSP sin trên servo ảo: 0 overrun, 0 underrun, 0 WKC sai.
- [x] S1–S7 có test + đối chứng âm; trên servo thật: W-05 (S1/S5), W-06 (S5/S7), W-07 (S6).
- [x] Servo ảo hiệu chỉnh theo số đo thật (10.2).
- [x] So với TwinCAT NC (X-05) và ENI TwinCAT multi-mode (X-05b).
- [~] W-05 bằng nút dừng khẩn DI: **thay** bằng cắt CB (không mua được nút) — làm lại khi có nút.

## 4. Số liệu servo thật (IS620N, firmware V1.0, rev 0x00010001)
| Mục | Giá trị |
|---|---|
| Chu kỳ chạy được | 2 ms, 1 ms, 500 µs (min drive 125 µs); 500 µs cần `cpu_dma_latency` = 0 |
| DC \\|e\\| p99.99 | 13 µs (1 ms), 2–9 µs (500 µs); W-08 1 h: 11 µs |
| SAFEOP→OP | 297–299 ms; PREOP→SAFEOP 1–2 ms; enable 1,5–2,5 ms/bước |
| CSP bám | ~26,5 chu kỳ trễ (gain mặc định) |
| CSV/PV | 0,9963 × lệnh |
| Mất process data | Fault 0x0E08 = **Er.E08 Synchronization loss** (ngưỡng 200C-24h = 9 chu kỳ), AL 0x001B, **không EMCY**, tự xoá khi về OP |
| Statusword OE (CSP) | 0x1637; sau disable 0x0631 |
| 0x6502 | 0x3AD (PP PV TQ HM CSP CSV CST) |
| PDO cố định | 0x1701/0x1B01 (12/28 B); 0x1702/0x1B02 (19/25 B, có 0x6060/0x60FF/0x607F, **không** 0x606C) |
| Encoder | 2^23 inc/vòng |

## 5. Phát hiện đáng nhớ
- **Deep idle c7 của Orin** (thoát 5 ms) làm ngắt i226 trên CPU0 đến muộn → NOFRAME ở 500 µs. `ecm_run` giữ `/dev/cpu_dma_latency` = 0 (0017); đối chứng âm `--no-dma-latency` FAIL lại.
- **Recovery sau mất nguồn**: InitCmd FAILED nhưng slave vẫn lên OP → sửa (0018): mailbox polled khi reconfigure, lỗi → giữ PRE-OP, `FAILED(config)`. Giới hạn trong ENI nhờ vậy sống qua mất nguồn.
- **0x607F trong RxPDO** của 0x1702: output 0 đè giới hạn InitCmd — runner ghi qua `--pdo-set`, có đối chứng âm.
- **TwinCAT export ENI**: device chưa gắn task → SYNC0 4 ms, không `CycleTime`; ô "PDO Configuration" thêm 28 InitCmd; "Check Revision" mặc định tắt. Wireshark trên NIC RT của TwinCAT: bật promiscuous ở TwinCAT, **tắt** ở Wireshark.
- **TwinCAT disable bằng 0x06** thẳng từ OE và gửi reset 0x86 đầu phiên; master giữ 0x07→0x06→0x00 và không tự reset.
- **Laptop Windows chạy TwinCAT khựng 116/382 ms** → IS620N 0x0E08; Jetson 1 h chỉ 1 frame lẻ.
- Thao tác: `git am` bỏ CR của file CRLF (XML TwinCAT) → dùng `git am --keep-cr`; file do `sudo tshark` tạo thuộc root.

## 6. Còn treo, mang sang
- Nút dừng khẩn vào DI → W-05 gốc; tách CB nguồn điều khiển / động lực.
- Đọc 0x6067 trên servo thật (servo ảo dùng mặc định 734 của manual). Xác nhận ánh xạ Er.XYZ → 0x603F = 0x0XYZ khi gặp mã lỗi thứ hai.
- R-08 phần `--link etf` 10 phút.
- Nguyên nhân −0,37 % vận tốc (đơn vị trong drive?).
- GĐ11: trạng thái/lỗi trục + histogram hook vào `stats.json` (không điều khiển trục qua API); hàng đợi setpoint có timestamp là chỗ nối `ros2_control`.

## 7. Hồi quy cuối trên Jetson
`sudo -E tools/gd10/run_regression_gd10.sh` (GĐ9 + 10.2…10.8 + recover + `w` + `wmm`) — điền bảng `summary.md` khi chạy xong.
