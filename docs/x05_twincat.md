# X-05 / X-05b — buổi TwinCAT với IS620N thật (Phase 10.8)

Cần: laptop TwinCAT 3 (NIC có driver RT của Beckhoff), 2 IS620N (sau 10.0), Wireshark trên laptop, Jetson có `tools/gd10/cw_trace.py`. Điều kiện an toàn như 10.9: motor không tải, giới hạn torque/tốc độ đặt trong drive, tay ở dừng khẩn/MCB.

## 1. X-05b — ENI multi-mode (`eni_2servo_mm`)
1. TwinCAT: scan bus, cả 2 IS620N; mỗi drive chọn PDO **0x1702 (Rx) / 0x1B02 (Tx)** — có 0x6060/0x6061 để đổi mode lúc chạy (`claude/is620n_esi_review.md`). DC: SYNC0 1 ms như `eni_2servo`.
2. Export ENI: *EtherCAT master → Export Configuration File* → `eni_2servo_mm.xml`.
3. Jetson:
   ```bash
   cp eni_2servo_mm.xml config/eni/
   python3 tools/eni/eni2cfg.py config/eni/eni_2servo_mm.xml -o config/eni/eni_2servo_mm.enicfg
   tools/eni/check_enicfg.sh
   sudo ./apps/ecm_run/ecm_run --iface enP1p1s0 --n 2 --eni config/eni/eni_2servo_mm.enicfg --pdo-scan --pdo-dump --duration-sec 10
   ```
   Kỳ vọng: OP, `--pdo-scan` ENI == bus, `--axis 1,2 --axis-modes csp,csv,pp,hm` bind được (0x6060/0x6061 trong PDO, không còn `mode_by_sdo`).

## 2. X-05 — chuỗi controlword TwinCAT vs master
1. TwinCAT NC: thêm 2 trục NC nối với 2 drive (CiA402), cấu hình đơn vị/encoder theo manual; **giới hạn tốc độ thấp**.
2. Bắt frame: thử Wireshark trực tiếp trên NIC TwinCAT trước. NIC đã gắn driver RT của TwinCAT có thể không cho Wireshark thấy frame (kiểm tra tại chỗ) — khi đó bắt qua một thiết bị chen giữa master và drive #1: switch có port mirror, hoặc probe EtherCAT (ví dụ Beckhoff ET2000) vào một NIC thứ hai. Lưu capture dạng pcapng.
3. Online: Enable trục 1 → jog chậm vài giây → Disable. Rồi Enable → Reset sau một lỗi (bấm dừng khẩn DI) nếu muốn có cả nhánh Fault. Lưu `twincat_x05.pcapng`.
4. Master trên Jetson, cùng drive, cùng kịch bản (enable / chạy / disable):
   ```bash
   sudo tshark -i enP1p1s0 -F pcap -w master_x05.pcap -f "ether proto 0x88a4" &
   sudo ./apps/ecm_run/ecm_run --iface enP1p1s0 --n 2 --eni config/eni/eni_2servo.enicfg --pdo-scan \
        --axis 1 --hook cia402 --cia402-script "1 enable 0; 3 disable 0" --duration-sec 5
   sudo kill -INT %1
   ```
5. So:
   ```bash
   python3 tools/gd10/cw_trace.py master_x05.pcap --ref twincat_x05.pcapng
   ```
   Nếu tự tìm word không chắc (2 trục), lấy địa chỉ logic: master từ `--pdo-dump` (0x10000 + byte offset), TwinCAT từ *Process Image* (offset của Controlword / Statusword trục 1) → `--cw/--sw`, `--ref-cw/--ref-sw`.
6. Kỳ vọng `X-05 PASS`: mọi chuyển trạng thái của master (SOD -06-> RTSO -07-> SO -0F-> OE -07-> SO -06-> RTSO -00-> SOD) có trong chuỗi TwinCAT, cùng thứ tự. TwinCAT có thể có thêm bước (ví dụ 0x80 reset đầu phiên, Quick stop) — được phép. Khác biệt (ví dụ TwinCAT đi OE → SOD bằng 0x00 thay vì 0x07/0x06) ghi vào `claude/giai_doan_10_nhat_ky_10_8.md` và cân nhắc trước 10.9.

Ghi lại cùng buổi: thời gian chuyển trạng thái thật của IS620N (từ capture TwinCAT: số frame giữa cạnh controlword và statusword đổi) → đầu vào hiệu chỉnh servo ảo (A-05) và `--axis-step-timeout-ms`.
