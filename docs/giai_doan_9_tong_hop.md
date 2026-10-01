# Giai đoạn 9 — Tổng hợp: sẵn sàng bus thật và bus hỗn hợp

**Thời gian:** 29/9 – 1/10/2026 · **Kế hoạch:** `claude/giai_doan_9_ke_hoach.md` · **Nền:** GĐ8 (`651c336`, CI xanh)
**Nhánh:** `gd9-bus-ready` (9.0, 9.1) → `gd9-3-complete-access` (9.2 … 9.11). Tác giả khaiannguyen; patch tạo trong sandbox, áp bằng `git am` và push từ Jetson.
**Nhật ký từng bước:** `claude/giai_doan_9_nhat_ky_9_*.md` (Project).

## 1. Mục tiêu
Master chạy được, hoặc từ chối rõ ràng khi không chạy được, trên:
- bus **1 slave**;
- bus **hỗn hợp**: slave tự làm (P1, ESI nháp) cộng servo thương mại (Inovance IS620N, 2 × IS620NS2R8I đã đặt), cấu hình bằng ENI TwinCAT sinh từ ESI thật.

Lớp CiA402 chưa làm, để GĐ10.

## 2. Kết quả từng bước (Jetson, PREEMPT_RT)

| Bước | Nội dung | Kết quả trên Jetson |
|---|---|---|
| 9.0 | `make caps` / `test-offline`, L4-04 abort code, `check_env.sh`; isolcpus còn nguyên (thiếu là `nohz_full`) | check_env 18 pass, L4-04 `0x06020000` |
| 9.1 | Gán nhóm theo slave (`--io-slaves`), nhóm rỗng không gửi/nhận, N = 1 | golden 547/573 khớp từng dòng, golden N=1 89; `run_groups` 38/0; L5 N=1 21/0 + 17/0; soak 30 phút N=1: 1 NOFRAME đơn lẻ / 1,8 triệu, DC 0 unlock |
| 9.2 | 100 Mbit/s (tốc độ của LAN9252) | `run_100m` 20/0; ETF offload missed 0 / invalid 0, launch_err max 177 ns; ns/byte theo tốc độ link |
| 9.3 | 503 Complete Access, download normal/segmented (soft_bus, loader) | `run_coe` 36/0; **IgH X-04 12/0** |
| 9.4 | Buổi TwinCAT: ENI CA, LRD/LWR, mixed, mixed_rev, 2servo | import đều sạch; TwinCAT ghi CA một InitCmd/object, SI0 đệm 16 bit |
| 9.5 | DC theo từng slave từ ENI (ref clock, 0x0300/0x0700, SYNC1, shift) | `run_dc_9_5` 24/0 (gồm L6 32 bit 40 s) |
| 9.6 | Loader ENI thật: bảng register InitCmd đã biết, từ chối LRD/LWR, timeout theo ENI | `run_eni_9_6` 27/0 |
| 9.7 | EMCY theo slave vào diag; AL status code lớp CONFIG → FAILED(config), không thử lại | `run_emcy_9_7` 27/0; L5 policy 31/0 |
| 9.8 | Độ tươi input theo từng slave (`--fresh`) | `run_fresh_9_8` 19/0; L5 io 18/0 |
| 9.9 | soft_bus giả lập slave khác hãng từ ESI (`--profile`), bus hỗn hợp ảo | `run_mixed_9_9` 24/0/2 skip (profile rút gọn) |
| 9.10 | Bind PDO theo (slave, index, sub), bảng từ ENI hoặc scan | `run_pdo_9_10` 27/0 |
| 9.11 | Hồi quy toàn bộ, CI, tổng hợp, ETG.1500 503, `i226.md` 100 Mbit/s, N-03 | xem §4 |

## 3. DoD (`giai_doan_9_ke_hoach.md` §0)

| Mục | Trạng thái |
|---|---|
| `ecm_run` N = 1, N = 2, N = 8 (4+4); golden 547/573 khớp từng dòng | ✅ 9.1 |
| Kết luận ETF/LaunchTime + EEE ở 100 Mbit/s trong `i226.md` | ✅ 9.2 (`docs/i226.md`, mục 100 Mbit/s) |
| 503: soft_bus CA + download normal/segmented, IgH xác nhận độc lập, loader thực thi CA, `ETG1500_COMPLIANCE.md` dòng 503 (đã tra PDF ETG.1500 V1.0.2 Table 1) | ✅ 9.3, 9.4, 9.11 |
| ENI hỗn hợp nạp được hoặc bị từ chối có lý do | ✅ 9.6 (E-07), 9.9 (lên OP trên bus ảo) |
| DC theo từng slave từ ENI; không `--eni` → hành vi cũ | ✅ 9.5 |
| EMCY nhận, log theo slave, có trong diag | ✅ 9.7 |
| AL code lỗi cấu hình → FAILED có lý do, không thử lại | ✅ 9.7 (P-01), 9.9 (N-01: 0x001E) |
| Kiểm độ tươi bật/tắt theo slave | ✅ 9.8 |
| soft_bus giả lập servo ứng viên; bus hỗn hợp ảo lên OP | ✅ 9.9 |
| API bind PDO, không hardcode trong `libecmaster` | ✅ 9.10 |
| Hồi quy GĐ8 không đổi; CI xanh | ⏳ `run_regression_gd9.sh` trên Jetson + CI sau khi push |

## 4. Bước 9.11
- `tools/gd9/run_regression_gd9.sh`: chạy toàn bộ hồi quy GĐ9 trong một lần (offline, golden 4+4/ENI/N=1/G-01b/CA + đối chứng âm, G, C+L4, E, DC+L6, M/P, F, V/N, B, L5 4+4, L5 N=1). Mỗi bước có log riêng; bảng kết quả ở `summary.md`. Không gồm: 9.2 (cần cáp loopback), X-04 (IgH), L1, soak.
- **N-03** (`run_mixed_9_9.sh`): firmware lệch ESI nhưng **cùng kích thước** (0x60F4 ↔ 0x60FD trong 0x1B01, đúng loại lỗi của ESI IS620N ở 0x1B04). Không có `--pdo-scan` thì bus lên OP và dữ liệu bị đọc sai mà không báo gì. Có `--pdo-scan` thì bị từ chối, nêu đúng hai entry.
- CI: thêm `test_profile`, kiểm `p1_draft.prof` sinh lại giống hệt, `run_mixed_9_9.sh` (V-01..V-03, B-02m, N-01..N-03). Đã có từ trước: golden N=1, golden CA + đối chứng âm, E-06/E-08.
- `ETG1500_COMPLIANCE.md`: 503 đóng (IgH X-04, ENI CA của TwinCAT); cập nhật 101 (LRD/LWR), 302 (bus hỗn hợp, so từng entry PDO), 505 (EMCY), 1101 (DC theo slave), bằng chứng 100 Mbit/s và bus ảo.
- `docs/i226.md`: mục 100 Mbit/s.

## 5. Phát hiện đáng nhớ
1. **Nhóm rỗng trong SOEM** (`ecx_receive_processdata_group` trả `EC_NOFRAME` khi không gửi gì): master không bao giờ gửi/nhận nhóm rỗng.
2. **`request_all_state()` coi BRD (OR mọi slave) ≠ 0 là thành công**: bus có slave kẹt PREOP từng được coi là đã SAFEOP. Lỗi có từ trước, sửa ở 9.7.
3. **Timeout trạng thái nằm trong ENI** (Timeout của register InitCmd 0x0120), không chỉ trong ESI. IS620N: PREOP 3 s, SAFEOP/OP 9 s, lâu hơn mặc định 2 s của SOEM.
4. **Ref clock phải là slave DC đầu tiên trên bus** (SOEM đo delay từ slave đó). Bỏ ràng buộc "ref = slave 1".
5. **TwinCAT ghi CA một InitCmd mỗi object**, SI0 đệm 16 bit, cùng layout với soft_bus và IgH.
6. **ENI TwinCAT luôn có LRD 0x09000000** (poll trạng thái mailbox): phải xét địa chỉ, không chỉ mã lệnh, khi từ chối LRD/LWR.
7. **ESI IS620N v2.6.9**: DefaultData của 0x1702, 0x1B03, 0x1B04 lệch mô tả PDO. Lệch nguy hiểm nhất là 0x1B04: cùng kích thước nên kiểm SM không thấy. Đã có biện pháp (`--pdo-scan`, N-03). Cần đọc trên servo thật.
8. **Ép tốc độ bằng `autoneg off` → đầu kia về half duplex**; dùng `advertise 0x008`.
9. **Giấy phép**: ESI hãng và profile sinh từ nó không vào repo; repo dùng `is620n_min.prof` viết tay.

## 6. Còn treo, mang sang
| Việc | Khi nào |
|---|---|
| Chạy `run_regression_gd9.sh` trên Jetson, dán `summary.md` vào §7; push; CI xanh; merge `gd9-3-complete-access` (và `gd9-bus-ready`) vào `main` | ngay |
| Soak 8 h N=1 (xác nhận NOFRAME đơn lẻ của soak 30 phút 9.1) | qua đêm, trước GĐ10 |
| Servo thật về: đọc 0x1018:03 (revision), 0x1702, 0x1B03, 0x1B04 qua SDO; sinh lại ENI nếu revision khác (kế hoạch §6.1) | khi có hàng |
| `--pdo-scan` bắt buộc cho slave hãng khi bind theo (index, sub) | GĐ10 |
| Buffer trao đổi giữa thread ứng dụng và RT thread (hiện chỉ RT chạm IOmap) | GĐ10 |
| EMCY mã hãng IS620N nối vào báo lỗi trục | GĐ10 |
| Đuôi wake jitter (cần `nohz_full`, kernel chưa bật `CONFIG_NO_HZ_FULL`) | GĐ tối ưu RT |
| `check_xsd.sh` đối chứng âm với file IS620N; X-04n lọc dmesg theo thời điểm | nhỏ |
| V-04 (IgH trên bus hỗn hợp ảo) chưa chạy | tùy chọn |

## 7. Hồi quy cuối trên Jetson
_(dán `log_gd9_regression_*/summary.md` vào đây)_
