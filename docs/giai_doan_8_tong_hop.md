# GĐ8 — Tổng hợp: đối chiếu chuẩn (IgH, TwinCAT, ESI/ENI) và launch time (ETF)

**Thời gian:** 27–29/9/2026 · **Kế hoạch:** `claude/giai_doan_8_ke_hoach.md` · **Nhật ký:** `claude/giai_doan_8_nhat_ky_8_1.md` … `_8_5.md` · **Trạng thái:** ĐÓNG (8.6 tuỳ chọn, không làm)

## 1. Kết quả theo bước

| Bước | Nội dung | Kết quả |
|---|---|---|
| 8.1 | IgH 1.6.13 làm master đối chiếu (X-01a/b/c + đối chứng âm) | ✅ SII trùng byte-for-byte N=1/8/32; IgH đưa N=8 lên OP 600 s, 0 lỗi WKC / 596 379 chu kỳ, DC bus-shift + SYNC0; bảng thanh ghi hai master chạm không có đường nào `soft_bus` bỏ sót |
| 8.2 | ESI cho `soft_bus` | ✅ `config/esi/softbus_esi.xml`, `esi_check.py` (SII ↔ ESI) 31/0/3; **hợp lệ theo ESI XML schema 1.17** + đối chứng âm (`tools/esi/check_xsd.sh`, CI) |
| 8.3 | TwinCAT 4024: ESI, 4 ENI offline, X-02s (TwinCAT làm master trên cáp thật) | ✅ OP, OP→PREOP, CoE online đọc/ghi, Device Scan |
| 8.4 | `ecm_run --eni` | ✅ E-01…E-05; golden ENI 573 dòng trong CI |
| 8.5 | ETF/LaunchTime trên i226, R-02 | ✅ launch offset ~314 ns, spread ~30 ns; R-02 A/B đạt ở N=8/32; hồi quy `--link etf` (veth, CI `etf-soft`) |
| 8.6 | Cable redundancy | ⬜ tuỳ chọn, không làm — ghi "chưa kiểm chứng" trong `ETG1500_COMPLIANCE.md` |

## 2. Lỗi `soft_bus` tìm ra nhờ master khác (10 lỗi)
- **IgH (#1–#4):** lệnh SII, thứ tự category, scs upload, abort.
- **ESI (#5):** CRC word 7 của SII.
- **TwinCAT (#6–#10):** promisc; object 0x1000 + echo abort; OP→PREOP; FMMU theo bit + SM1 status bit0 ("interrupt write", MBoxState FMMU 1 bit `0x080D.0 → 0x09000000.0`); AL Control cùng trạng thái + ack (Scan ghi `0x0011` khi slave đã ở INIT).
- SOEM không có lỗi nào lộ ra ở đây — các lỗi đều nằm ở mô phỏng slave. Đây chính là giá trị của đối chiếu: `soft_bus` giờ gần ESC thật hơn trước khi lên LAN9252.

## 3. ETF / launch time (8.5)
- **Nền đồng hồ:** `phc2sys -s CLOCK_REALTIME -c enP1p1s0 -O 0` (kernel TAI offset 0); |PHC−TAI| p99 143–175 ns; TAI−MONO ổn định.
- **Qdisc:** mqprio (priority 3 → TC0) + `etf delta 400 µs offload` trên hàng 0.
- **SOEM `patches/soem-txtime.patch` (v2):** chỉ frame đã hẹn giờ (motion) và còn ≥ asap mới đi ETF với `SCM_TXTIME`; mọi frame khác gửi ngay trên priority 0. Bất biến đơn điệu txtime trong khoá port. Tắt mặc định → golden af_packet không đổi (547/573).
- **`ecm_run --link etf`:** thức ở `target − lead` (lead 350 < delta 400 → thread tự dequeue, không có MISSED); DC host time = launch time; hạn chót nhận tính từ launch (`launch + cycle − asap − guard`); counter `late/bypass/send_err`, ETF missed/invalid từ error queue, bước nhảy TAI.
- **Số đo:** probe 1,2 triệu frame, 0 MISSED; launch_err offset 314 ns, p50→max ~30 ns (af_packet: p50 +24,8 µs, max 109 µs).

**R-02 A/B** (`ecm_run` i226 ↔ `soft_bus` `enP8p1s0`):

| | af N=8 | etf N=8 | af N=32 | etf N=32 |
|---|---|---|---|---|
| WKC noframe motion / IO | 4 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| DEGRADED | 1 | 0 | 0 | 0 |
| DC \|e\| p99 | 22 µs | **12 µs** | 24 µs | 22 µs |

- Lượt 1 (v1) N=32 etf mất 44 % chu kỳ IO: frame IO đi ETF chờ asap 150 µs sau motion, hạn chót tính từ lúc thức → sửa v2 như trên.
- Giới hạn: pha SYNC0 đo ở `soft_bus` bị chi phối bởi jitter nhận phần mềm trên r8168 → jitter SYNC0 thật đo ở GĐ 2.5 (LAN9252 + scope). Đuôi wake jitter 144–277 µs (không isolcpus/nohz_full) là sàn còn lại.
- Tài liệu vận hành: `i226.md` mục ETF (cấu hình, tham số, 7 điều kiện, bẫy).

## 4. Hồi quy cuối giai đoạn (29/9)
- Offline: soft_bus 116/0 (+ test_offline 98/0), telemetry, core 26/0, diag, policy 97/0, config 26/0.
- Golden af_packet 547 + ENI 573, hai đối chứng âm phát hiện khác biệt; L1 10/0; L5 policy 18/0, io 7/0, diag 10/0.
- `--link etf` trên veth nhiều hàng đợi: 10/0 (smoke + L5 policy/io).
- ESI schema 1.17: hợp lệ + đối chứng âm.

## 5. CI sau GĐ8
- Job build SOEM áp `soem-mbx-cnt.patch` + `soem-txtime.patch`.
- `offline` thêm `tools/esi/check_xsd.sh`.
- `golden` thêm ENI (E-05) + đối chứng âm ENI.
- Job mới `etf-soft` (không chặn): `tools/etf/run_etf_veth.sh all`.

## 6. ETG.1500 (tự đánh giá)
- Cập nhật `ETG1500_COMPLIANCE.md`: Reading ENI ✅; 101 bắt buộc từ khi có ENI; 302 so sánh identity/layout theo ENI.
- **Lỗ hổng mới: 503 Complete Access** thành `shall` cho Class B khi hỗ trợ ENI import. Hiện loader từ chối rõ ràng InitCmd có CompleteAccess → việc cho GĐ9.
- Mục mới "Related evidence": ESI schema, IgH X-01, TwinCAT X-02s, ETF.

## 7. Bẫy vận hành rút ra
- NetworkManager reset qdisc → i226 phải unmanaged; `enP8p1s0` khi làm loopback cũng nên nhả.
- `tc qdisc replace` không đổi được offload → del rồi add; sau khi del, i226 có lúc phải cắm lại cáp mới có link.
- Build lại `ecm_run`/`soft_bus` làm mất capabilities → setcap (sẽ gộp vào target `all`).
- File `/tmp` cũ của user thường chặn script chạy sudo (`fs.protected_regular`).
- Bash history expansion (`!!` trong dấu ngoặc kép) phá lệnh dán → `set +H`.

## 8. Mang sang sau
- **GĐ9:** 503 Complete Access (loader + `soft_bus` CA/segmented download); dashboard hiển thị counter ETF (`late/bypass/missed/invalid`) và `tx_launch_err` từ HW TX timestamp; Makefile setcap tự động; `UseLrdLwr`.
- **GĐ 2.5 (LAN9252):** X-02/X-03, ghi SII bằng TwinCAT từ ESI; R-01 cả hai backend ở 100 Mbit/s (`--etf-ns-per-byte 80`); đo jitter SYNC0 thật bằng scope.
- **Tối ưu RT:** isolcpus/nohz_full cho core 3 để hạ đuôi wake jitter (cho phép lead nhỏ hơn).
- Tuỳ chọn: 8.6 cable redundancy; X-02s với N=8; SDO Info "không hỗ trợ" và object 0x1C00 trong `soft_bus`.
