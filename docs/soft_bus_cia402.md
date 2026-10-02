# Servo CiA402 ảo trong `soft_bus` — GĐ10.2

`soft_bus --profile N=FILE --cia402 N[:AXES]`: node N (số slave SOEM, đếm từ 1) là một drive CiA402. Dùng để phát triển và kiểm lớp CiA402 phía master (`libecm_cia402`, 10.3–10.7) khi chưa có hoặc không muốn dùng servo thật, và để tiêm lỗi mà servo thật không cho làm an toàn.

## Nguyên tắc
- **Viết theo chuẩn**, không theo hành vi của master: state machine, controlword/statusword, các mode lấy từ IEC 61800-7-201 / CiA 402. Bảng chuyển trạng thái kỳ vọng trong `test_cia402.c` (A-01) là một ma trận viết riêng từ bảng của chuẩn, không suy ra từ code.
- **Hiệu chỉnh theo servo thật**: nhịp chuyển trạng thái và mã EMCY nằm trong `esc_cia402_cal_t`, giá trị mặc định là của chuẩn. Bước 10.0 đo trên IS620N rồi thay (A-05).
- **Không đổi gì khi tắt**: không có `--cia402` thì node như GĐ9.9 (A-04: input không bị đụng, golden và mọi test cũ không đổi).

## Gắn vào bus
- Node phải có `--profile`: drive đọc/ghi object theo (index, sub) qua **PDO đang được gán** (0x1C12/0x1C13 → object mapping) và qua **OD của profile**. Vì vậy mapping nào cũng chạy: P1 (0x1600/0x1A00), IS620N (0x1701/0x1B01 hoặc 0x1702/0x1B02), profile 4 trục.
- Nhiều trục: trục `a` dùng index + 0x800·a (ETG.6010). `config/profiles/cia402_4ax.prof` là drive 4 trục tự thiết kế (vendor 0x499, không có ESI), sinh lại bằng `tools/esi/make_multiaxis_prof.py --axes 4`.
- Object không có trong PDO lấy từ OD (SDO, InitCmd của ENI). Ví dụ IS620N: 0x6060 không nằm trong 0x1701, ENI ghi 0x6060 = 8 bằng InitCmd → drive vào CSP. Object trạng thái (0x6041, 0x6061, 0x6064, 0x606C, 0x603F, 0x60F4…) được ghi ngược vào OD, nên SDO đọc cũng thấy giá trị hiện tại.

## Nhịp
Drive chạy **một bước cho mỗi frame có process data**, ngay sau khi ESC xử lý frame đó: lấy output master vừa ghi, ghi input để master đọc ở frame kế. Từ lệnh tới trạng thái mất 1 chu kỳ, như drive lấy mẫu ở SYNC0. `dt` = chu kỳ SYNC0 (0x09A0) khi SYNC0 bật, ngược lại là khoảng cách đo giữa hai frame (kẹp trong 100 µs … 10 ms).

Output chỉ có hiệu lực ở **OP**. Ở SAFE-OP, input vẫn được cập nhật nhưng lệnh bị bỏ qua. Rời OP khi trục đang Operation enabled → Fault (mã `code_lost_op`, mặc định 0x8700); rời OP khi Ready/Switched on → Switch on disabled.

## Mô hình
| Phần | Hành vi |
|---|---|
| State machine | Not ready → Switch on disabled (sau `power_on_ms`), T2–T16 theo chuẩn; lệnh có bit 7 = 1 không phải lệnh; fault reset = sườn lên bit 7, chỉ trong Fault. Mỗi chuyển do lệnh mất `trans_cycles` (mặc định 1) + `drv_slow`; chuyển dừng (về SOD, Quick stop) không bị trễ |
| Statusword | bit 0–6 theo trạng thái, bit 4 (voltage enabled) và bit 9 (remote) luôn 1, bit 7 warning khi mode yêu cầu không có trong 0x6502, bit 10/12/13 theo mode |
| CSP | bám 0x607A, chỉ giới hạn bởi 0x60C5/0x60C6 (max accel/decel) và 0x6080 (max motor speed), mặc định không giới hạn → actual = target của chu kỳ trước. Following error 0x60F4 (+ `drv_ferr`) > 0x6065 → fault 0x8611. bit 12 = 1 (đang bám target) |
| CSV | bám 0x60FF, cùng giới hạn như CSP |
| PP | hình thang theo 0x6081/0x6083/0x6084 (mặc định 100 000 inc/s, 10⁶ inc/s²), giới hạn 0x607F. Bắt tay bit 4 ↔ bit 12, bit 5 change immediately, bit 6 relative, một set-point đệm, bit 10 target reached, halt (bit 8) |
| PV | ramp tới 0x60FF theo 0x6083/0x6084; bit 10 đạt tốc độ, bit 12 tốc độ = 0 |
| Homing | 35/37: vị trí hiện tại thành 0x607C. 19/21: chạy theo 0x6099:01 tới công tắc ảo (`drv_home_switch`), mép công tắc thành 0x607C (đơn giản hoá: không có lượt tiếp cận chậm). Method khác → bit 13 homing error |
| Quick stop | giảm tốc theo 0x6085; 0x605A ≤ 4 → về Switch on disabled khi dừng, 5–8 → ở lại Quick stop active |
| Fault | Fault reaction active (giảm tốc theo 0x6085) → Fault; 0x603F = mã lỗi; một EMCY được gửi qua SM1 (cơ chế GĐ9.7) |
| Không mô phỏng | CST, torque/current thật (0x6077 = 0), touch probe, digital output; lượt homing chậm thứ hai |

## Tiêm lỗi (FIFO `--ctl`, node đếm từ 0 như mọi lệnh soft_bus)
| Lệnh | Tác dụng |
|---|---|
| `drv_status [node]` | in mọi trục (cũng in khi soft_bus thoát) |
| `drv_fault <node> <axis> <code> [latched]` | fault + EMCY; nguyên nhân còn cho tới `drv_clear`; `latched`: fault reset bị từ chối tới khi `drv_clear` |
| `drv_clear <node> [axis]` | hết mọi tiêm lỗi của trục |
| `drv_refuse_enable <node> <axis> 0\|1` | Switched on → Operation enabled bị từ chối |
| `drv_slow <node> <axis> <ms>` | mỗi chuyển do lệnh chậm thêm `ms` |
| `drv_ferr <node> <axis> <inc>` | cộng vào following error |
| `drv_quickstop <node> <axis>` | quick stop phía drive (như DI dừng khẩn) |
| `drv_lose_sync <node>` | mọi trục: fault mất đồng bộ (`code_sync`) |
| `drv_pos <node> <axis> <inc>` | đặt vị trí thực (xoay tay khi disabled; dùng cho S2) |
| `drv_home_switch <node> <axis> <inc\|off>` | công tắc home ảo |

`axis` có thể là `all`. Tắt nguồn node (`drop_node`/`restore_node`) cũng khởi động lại drive (Not ready → Switch on disabled).

## Test
| ID | Ở đâu | Nội dung |
|---|---|---|
| A-01 | `tools/soft_bus/test_cia402` | 56 cặp (trạng thái, lệnh) so với bảng chuẩn; fault reset cần sườn; 0x0F từ SOD không bao giờ enable; SAFE-OP bỏ qua lệnh |
| A-02 | nt | CSP sin 1 Hz: actual = target sau 1 chu kỳ; CSV; PP hình thang (đỉnh = 0x6081, kết thúc đúng target, ~600 ms), set-point đệm, relative; PV; homing 37, 19, method không hỗ trợ |
| A-03 | nt | fault → FRA → Fault, 0x603F, EMCY; reset chỉ ở sườn; nguyên nhân còn → fault lại; latched; refuse enable; slow 5 ms = 6 chu kỳ; following error; quick stop; rời OP; mode không hỗ trợ → warning; TxPDO bị đóng băng (stale) không bị ghi |
| A-06/A-07 | nt | 4 trục độc lập (0x800·a), fault một trục; mapping IS620N, mode từ OD |
| A-04 | `make test-offline` + golden + `run_mixed_9_9` + `run_emcy_9_7` | không `--cia402` → không đổi |
| D-01…D-07, A-04 | `tools/gd10/run_servo_10_2.sh` | qua SOEM/`ecm_run` trên veth: lên OP ở SOD (0x0250), 0x0006 → 0x0231, 0x000F từ đầu không enable, fault + EMCY tới `ecm_run`, 4 trục, bus hỗn hợp P1 + IS620N (mode 8 từ InitCmd ENI), tắt/bật nguồn node |
| A-05 | sau 10.0 | nhịp chuyển trạng thái, mã EMCY, bit statusword so với số đo IS620N (±1 chu kỳ) |
