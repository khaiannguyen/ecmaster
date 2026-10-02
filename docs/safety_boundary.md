# Ranh giới an toàn — lớp CiA402 phía master (Phase 10.6)

Tài liệu này nói rõ master **chịu trách nhiệm gì** và **không chịu trách nhiệm gì** khi điều khiển trục CiA402. Code: `libecm_cia402/ecm_cia402_axis.{h,c}`, `apps/ecm_run` (`--hook cia402`).

## 1. Master KHÔNG phải là chức năng an toàn

- Master chạy trên Linux PREEMPT_RT, không có chứng nhận SIL/PL. Mọi chốt dưới đây là **chốt vận hành** (giảm rủi ro do lỗi phần mềm / thao tác), không thay thế mạch an toàn.
- Dừng khẩn **phải** đi bằng phần cứng: nút dừng khẩn → DI của drive (chức năng dừng khẩn / cấm chạy), MCB / contactor cắt động lực (kế hoạch GĐ10 §2.1). Master không nằm trên đường dừng khẩn.
- Khi mất bus, **drive** tự dừng (SM watchdog, mất SYNC, 0x6007 / tham số hãng). Master không gửi được gì khi không có bus — nó chỉ bảo đảm sẽ **không tự enable lại** khi bus trở lại (S5).
- Giới hạn torque / tốc độ / following error đặt **trong drive** (0x6072, 0x6080, 0x6065 hoặc tham số hãng) trước lần enable đầu tiên. Giới hạn bước S4 của master là lớp thứ hai, không thay thế.

## 2. Các chốt của master

| Chốt | Master bảo đảm | Cài ở đâu | Test (đối chứng âm) |
|---|---|---|---|
| S1 | Không bao giờ tự enable. Enable chỉ do lệnh ENABLE của ứng dụng. Trục rời Operation enabled mà không có lệnh (recovery đưa slave ra khỏi OP, quick stop phía drive / DI) → chốt disabled, lỗi `DROPPED`, không đi lên lại | `axis_rt` bước 2 | S-01 offline + online (`drv_quickstop`, `safeop` L5-05) |
| S2 | Trước và trong lúc enable: target CSP = actual (0x607A ← 0x6064); CSV = 0 | bước 6 | S-02: drive ở 50 000, enable không nhảy |
| S3 | Hàng đợi setpoint cạn: CSP giữ vị trí cuối, **không ngoại suy**; CSV về 0 | bước 6 | S-03 offline (100 ms không setpoint) |
| S4 | \|setpoint − giá trị gửi lần trước\| > giới hạn/chu kỳ → không gửi, quick stop (0x02), lỗi `STEP`. Lần setpoint đầu so với actual | bước 6 | S-04: 0 tuyệt đối khi đang ở 50 000; +1e6 khi chạy; CSV 100 → 5000. Online: `--axis-max-step 0` là đối chứng âm |
| S5 | Mất bus (LOST) → disabled, lỗi `BUS_LOST`, giữ nguyên khi bus OP lại, tới lệnh ENABLE mới | bước 2 | S-05 (`mute 200`), T-08 |
| S6 | Thoát (`SIGINT`, `SIGTERM`, hết `--duration-sec`): đi xuống 0x07 → 0x06 → 0x00 **trước** khi slave rời OP; từ chối ENABLE (`SHUTDOWN`). Quá `3 × step timeout + 100 ms` thì rời OP dù chưa xong, in trục còn kẹt | `ecm_cia402_shutdown/all_down`, vòng chính `ecm_run` | S-06 online: chuỗi controlword phía drive; `ecm_run_neg6` là đối chứng âm |
| S7 | Fault reset chỉ khi có lệnh (một xung 0x80); sau reset trục **vẫn disabled** | `apply_cmd`, bước 2 | S-07: 1 s / 2 s không lệnh → vẫn Fault |

Giới hạn S4 mặc định trong `ecm_run`: vị trí 100 000 inc/chu kỳ, vận tốc tắt (`--axis-max-step POS[:VEL]`, `0` = tắt). Với encoder 23 bit (IS620N, 8 388 608 inc/vòng) và chu kỳ 1 ms, 100 000 inc/chu kỳ ≈ 715 rpm — trên mức 300 rpm của 10.9, nên trên servo thật đặt nhỏ hơn theo tốc độ thử.

## 3. Đối chứng âm

- `ECM_CIA402_BROKEN` (bit n = Sn) tắt chốt Sn — **chỉ build test**. `make -C libecm_cia402 negctl` build 7 bản, mỗi bản tắt một chốt: test S-0n tương ứng **phải fail** (CI kiểm). Build sản phẩm không đặt macro này (`ecm_run` in `NEGATIVE CONTROL BUILD` nếu có).
- `apps/ecm_run/ecm_run_neg6`: `ecm_run` với S6 tắt, dùng trong `tools/gd10/run_safety_10_6.sh`.

## 4. Những gì master KHÔNG phát hiện

- "WKC đúng nhưng dữ liệu cũ": CiA402 không có bit toggle chung cho CSP; IS620N không có bộ đếm ứng dụng (9.8). Xem `docs/cia402.md` (10.8).
- Drive làm sai state machine (ví dụ báo Operation enabled nhưng không bám): master chỉ thấy qua statusword / following error của drive.
- Ứng dụng gửi quỹ đạo hợp lệ từng bước nhưng sai về tổng thể (đi quá hành trình): dùng software limit 0x607D trong drive.

## 5. Trên servo thật (10.9)

S1, S2, S5, S6, S7 lặp lại trên IS620N (W-01, W-05, W-06, W-07). Điều kiện trước mỗi lần enable: motor không tải, giới hạn torque ≤ 20 %, tốc độ ≤ 300 rpm, tay ở dừng khẩn / MCB.
