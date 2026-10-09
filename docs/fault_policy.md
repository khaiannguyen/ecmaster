# Chính sách xử lý lỗi của master (Giai đoạn 7.3)

**Trạng thái:** đã duyệt (25/9, chọn phương án (a) ở §5.3) và đã code. Sandbox: `run_l5_policy.sh` 30/30, `test_policy_offline` 82/82. Chưa chạy trên Jetson.
**Nguồn:** `master_plan_v2.md` §2.10/§7.6, kế hoạch GĐ7 §3.3, SOEM v2.0.0 (`ecx_recover_slave`, `ecx_reconfig_slave`, `samples/ec_sample` hàm `ecatcheck`), ESC datasheet §6 (ESM), code `apps/ecm_run` hiện tại.

---

## 1. Nguyên tắc

1. **RT thread không bao giờ chặn quá hạn chót của chu kỳ.** Mọi lần chờ trong RT thread đều có hạn chót tuyệt đối, tính từ lúc thức dậy.
2. **RT thread không `fprintf`.** Sự kiện được đẩy vào một ring SPSC (RT → monitor); monitor in log và ghi snapshot.
3. **Mỗi frame gửi lên dây phải có đúng một `tx_order_ring_push`** (bài học GĐ5). Frame do thread khác gửi chỉ được phép trong một cửa sổ đã khai báo (mục 5.3).
4. **Phân biệt lỗi của bus và lỗi của slave.** Bus (mất frame) do RT thread phát hiện theo từng chu kỳ. Trạng thái slave (AL state, mất địa chỉ) do monitor phát hiện qua frame chẩn đoán của 7.2 (1 lần/giây).
5. **Phục hồi có giới hạn.** Mỗi hành động phục hồi có số lần thử và backoff. Hết lượt thì đánh dấu `FAILED` và chờ người can thiệp, không lặp vô hạn.

## 2. Sửa lỗi có sẵn: timeout nhận lớn hơn chu kỳ

Hiện `service_group()` gọi `ecx_receive_processdata_group(..., EC_TIMEOUTRET)` với `EC_TIMEOUTRET = 2000 µs`. Ở tick IO, motion và IO nhận nối tiếp nhau, nên một frame mất có thể chặn 2–4 ms. Vì vậy mất frame một lần là thành overrun dây chuyền (`overrun=24039` hôm 25/9).

**Mới:** hạn chót theo tick.

```
deadline      = t_target + cycle − GUARD         GUARD = 150 µs (phần còn lại của tick: DC, diag, telemetry)
rx_timeout_us = clamp(deadline − now, RX_MIN, RX_MAX)
                RX_MIN = 50 µs, RX_MAX = cycle
```

- `t_target` là thời điểm **dự kiến** của tick (không phải `t_wake`): tick thức dậy trễ thì ngân sách ngắn lại, chứ không đẩy lùi tick sau.
- Motion nhận trước với phần lớn ngân sách. IO nhận với phần còn lại.
- Turnaround đo trên Jetson (GĐ4/6): p50 ~16 µs, max ~630 µs. Với cycle 1 ms, motion có khoảng 800 µs, đủ cho reply thật.
- Frame không về kịp → `NOFRAME` cho chu kỳ đó. Reply về muộn sau đó sẽ nằm trong buffer SOEM với index cũ: nó được dọn khi index được cấp lại, và bị loại bởi kiểm tra header (tái dùng `reply_matches()` của 7.2 cho frame PD, xem L5-07 ở 7.4).
- Thay đổi này ảnh hưởng mọi chu kỳ, nên sau khi sửa phải chạy lại L3 và L6.

## 3. State machine của bus (RT thread quản lý)

| Trạng thái | Vào khi | RT thread làm gì | Ra khi |
|---|---|---|---|
| `RUN` | mặc định sau khi vào OP | chu kỳ bình thường | một group có WKC ≠ expected → `DEGRADED` |
| `DEGRADED` | WKC sai (`NOFRAME`/`ZERO`/`PARTIAL`/`OVER`) | chu kỳ tiếp tục, output giữ nguyên, đếm theo nguyên nhân và theo group | cả hai group đúng WKC trong 1 chu kỳ → `RUN`; motion `NOFRAME` liên tiếp ≥ `N_LOST` → `LOST` |
| `LOST` | ≥ `N_LOST` = 100 chu kỳ motion liên tiếp không có frame (100 ms) | ngừng process data; mỗi 10 ms gửi 1 BRD 0x0130 (không chặn, kiểu frame diag) để dò bus; báo ứng dụng output không còn hiệu lực | BRD về với WKC = số slave kỳ vọng → `RECOVER` |
| `RECOVER` | bus trả lời lại | **nhường bus cho monitor** (cờ sở hữu, mục 5.2); RT chỉ ngủ theo nhịp | monitor báo xong → `RUN`; hoặc thất bại → `LOST` (thử lại sau backoff) |

Ghi chú:
- `PARTIAL` kéo dài (một slave rớt khỏi OP, hoặc chuỗi đứt) giữ bus ở `DEGRADED`. Bus vẫn chạy cho các slave còn lại. Việc đưa slave đó về là của phục hồi theo slave (mục 4), không phải của state machine bus.
- `N_LOST = 100` là một hằng cấu hình. Watchdog của slave (3 ms) sẽ đưa slave về SAFEOP từ lâu trước đó. Ngưỡng này chỉ để quyết định lúc nào master thôi phí frame PD.
- Mỗi lần chuyển trạng thái là một sự kiện `{tick, from, to, reason}` trong ring sự kiện. Monitor in ra một dòng.

## 4. Phục hồi theo slave (monitor quyết định)

Monitor phân loại mỗi slave từ snapshot diag (AL status, AL status code, station address qua WKC của FPRD, BRD count):

| Loại | Dấu hiệu | Hành động | Ai gửi frame |
|---|---|---|---|
| **A. Rớt state, cấu hình còn** | AL = SAFEOP(+ERR) hoặc PREOP, code ≠ 0, địa chỉ đúng | ghi AL control `state|ACK` (0x10) để xoá lỗi → chờ AL status → ghi `OP` | **RT thread**, qua hàng lệnh monitor→RT: mỗi tick tối đa 1 frame lệnh (FPWR/FPRD 1 datagram), không chặn, có `tx_order_ring_push` |
| **B. Mất địa chỉ / mất nguồn** | slave đếm được bằng BRD nhưng FPRD theo địa chỉ không trả lời (WKC 0), hoặc AL = INIT | `ecx_recover_slave` (APWR địa chỉ tạm, kiểm tra SII ID khớp), rồi `ecx_reconfig_slave` (SM, FMMU, INIT→PREOP→SAFEOP), rồi SYNC0 nếu là slave motion, rồi A | **monitor thread**, blocking, trong cửa sổ khai báo (mục 5.3) |
| **C. Không phải slave cũ** | SII ID/vendor/revision khác | không cấu hình, báo `ERROR`, để địa chỉ 0 | — |

- Mỗi slave có `attempts` và `next_try`. Backoff 1 s, 2 s, 4 s… tối đa `MAX_ATTEMPTS = 5`, sau đó `FAILED`.
- L5-05 (`safeop k 0x001A`) và L5-13 (watchdog 0x001B) đi đường A. L5-11b (`restore_node`) và slave mất nguồn đi đường B.
- **DC khi đi đường B:** slave mất nguồn thì mất luôn offset system time (0x0920) và delay (0x0928). Phạm vi 7.3: ghi lại 0x0928 từ `pdelay` đã đo, tính 0x0920 = giờ ref − giờ local của slave (đọc trong một frame), rồi kích hoạt SYNC0. Nếu phần này phức tạp hơn dự kiến thì tách thành 7.3b, và trong lúc đó slave motion đi đường B chỉ được đưa về SAFEOP (báo rõ).

## 5. Ai được gửi frame, lúc nào

### 5.1. Mặc định: chỉ RT thread

Process data, frame diag, BRD dò bus ở `LOST`, frame lệnh của phục hồi loại A. Tất cả không chặn và đều có `tx_order_ring_push`.

### 5.2. `RECOVER`: bus thuộc về monitor

Cờ `g_bus_owner` (atomic, `RT` | `MONITOR`). RT thread chuyển sang `MONITOR` và ngừng gửi. Monitor làm phục hồi toàn bus: loại B cho từng slave cần, rồi đưa cả bus lên OP bằng chính cách lúc khởi động (một chu kỳ PD, rồi `request_op_keepalive`), re-anchor DC (`ecm_dc_anchor`), rồi trả bus cho RT. Không có tranh chấp index vì chỉ một thread gửi.

### 5.3. Loại B khi bus đang `DEGRADED` (các slave khác vẫn cần PD)

Không thể dừng PD của cả bus chỉ vì một slave, nên monitor gửi frame blocking **song song** với RT thread. Đây cũng là cách `ecatcheck` của SOEM làm (port của SOEM có `tx_mutex`, `rx_mutex`, `getindex_mutex`). Cái giá: turnaround/`g_tx_order` lệch trong lúc đó. Cách xử lý:
- Monitor đặt `g_foreign_tx_window = 1` trước khi gửi và xoá sau khi xong.
- Telemetry thread thấy cờ này thì đánh dấu mẫu turnaround trong cửa sổ là `excluded` (đếm riêng), và sau cửa sổ thì đồng bộ lại FIFO khớp TX.
- Sau cửa sổ in `[FOREIGN_TX] window=... frames=... excluded=...` để không ai nhầm với lỗi đo.

**Đã chốt (25/9): phương án (a)**, song song như trên. Cửa sổ loại trừ cũng dùng cho `LOST`/`RECOVER` (RT mở cửa sổ khi vào `LOST`, đóng khi về `RUN`). Telemetry in `exclusion windows: polls=… discarded tx_order=… tx_completions=… rx=…` cuối run; hàm mới `turnaround_resync()` xoá hàng đợi đang khớp nhưng giữ các bộ đếm.

## 6. Hằng số

| Tên | Giá trị | Lý do |
|---|---|---|
| `GUARD` | 150 µs | phần việc sau nhận trong tick (DC, diag, snapshot) đo được < 60 µs p99.99 |
| `RX_MIN` | 50 µs | tránh timeout 0 khi tick đã trễ |
| `N_LOST` | 100 chu kỳ | 100 ms: đủ xa nhiễu 1–2 frame, đủ gần để phản ứng |
| `LOST_PROBE` | 10 ms | BRD dò bus |
| `MAX_ATTEMPTS` | 5 | backoff 1/2/4/8/16 s |
| SM watchdog | motion 3 ms, IO 24 ms | giữ nguyên (GĐ5) |

## 7. Kiểm chứng (L5 thuộc 7.3)

| Ca | Tiêm | Đạt khi | Negative control |
|---|---|---|---|
| L5-01 | `wkc_short k n` | `DEGRADED` ≤ 1 chu kỳ, `PARTIAL` đếm đúng n, về `RUN` | — (đếm phải đúng n) |
| L5-02 | `mute 100` / `mute 200` | 100: `DEGRADED`, không `LOST`; 200: `LOST` ở chu kỳ thứ 100, 0 overrun dây chuyền, tự về OP | timeout cũ (2 ms): overrun dây chuyền xuất hiện lại |
| L5-03/04 | `ip link set veth_s down` 2 s rồi `up` | `LOST` → `RECOVER` → `RUN`, không crash, valgrind sạch rò rỉ | — |
| L5-05 | `safeop k 0x001A` | monitor gọi tên slave k + mã, đường A, về OP | tắt phục hồi: slave kẹt SAFEOP |
| L5-13 | `SIGSTOP` master 10 ms rồi `SIGCONT` | slave về 0x001B, master báo "SM watchdog", đường A về OP | `--no-sm-wd` ở soft_bus: không có 0x001B |
| soak | 30 phút (8 giờ còn nợ) | 0 WKC sai, 0 overrun, không đổi trạng thái | — |

## 8. Không thuộc 7.3

- L5-07/08/09/12: 7.4.
- Redundancy (2 port): ngoài phạm vi GĐ7.
- Dừng an toàn cho motion (ramp về 0 khi `LOST`): chỉ báo cờ cho ứng dụng, logic an toàn thuộc lớp CiA402 sau này.

## 9. Ghi chú hiện thực (25/9)

**Mã nguồn**
- `libecmaster/policy/ecm_policy.{h,c}`: logic thuần, không SOEM. Gồm `ecm_rx_timeout_us()`, state machine của bus, bộ lập kế hoạch theo slave, ring sự kiện (RT → monitor) và hàng lệnh (monitor → RT), dùng C11 acquire/release. Test: `test_policy_offline` 82/82, sạch ASan/UBSan.
- `apps/ecm_run/ecm_run.c`:
  - RT thread: hạn chót theo tick; `ecm_bus_on_cycle()`; ở LOST thì dò bằng BRD; ở RECOVER thì chờ `g_recover_done`; mỗi tick không phải IO chạy tối đa 1 lệnh đường A (`rt_exec_command`).
  - Monitor: vòng 10 ms (diag và bộ lập kế hoạch vẫn 100 ms); `recover_full()`, `recover_path_b()`.
  - Ghi SM watchdog và neo DC được tách thành hàm (`write_sm_watchdog`, `dc_anchor`) để đường B và RECOVER dùng lại.
  - CLI mới: `--no-recover`, `--rx-timeout-legacy`, `--n-lost N`. Cuối run in tổng kết `[POLICY]`.
- **DC ở đường B** làm ngay trong 7.3, không cần tách 7.3b. Móc `PO2SOconfig` chỉ đăng ký trong lúc `ecx_reconfig_slave()` chạy: nó ghi lại SM watchdog, 0x0928 = `pdelay`, 0x0920 = offset cũ + (giờ ref − giờ slave, đọc trong **một** frame, cộng chênh lệch `pdelay`), rồi `ecx_dcsync0()`. Sandbox: SYNC0 của slave được khôi phục lệch các slave khác đúng bội số 1 ms, Δt so với ref = 0.
- **soft_bus:** `ip link set veth_s down` trước đây làm soft_bus thoát (`recvfrom` → `ENETDOWN`). Giờ nó ghi `link down`/`link up` rồi chạy tiếp, như ESC thật khi rút cáp.
- `apps/ecm_diag/run_l5_diag.sh` chạy `ecm_run --no-recover`: bài 7.2 chỉ chấm chẩn đoán. Nếu không, 7.3 sẽ khôi phục slave trước khi snapshot kịp thấy nó.

**Kết quả sandbox** (không RT, `SB_ARGS=--no-sm-wd`, `--no-tx-ts`)

| Ca | Kết quả |
|---|---|
| L5-01 `wkc_short 2 50` | motion PARTIAL = 44 (50 frame có datagram logic, gồm cả frame IO), IO không bị ảnh hưởng, DEGRADED → RUN |
| L5-02 `mute 50` / `mute 200` | 50: chỉ DEGRADED; 200: LOST đúng lúc 100 NOFRAME liên tiếp → RECOVER → RUN, 8/8 OP, overrun 9 |
| L5-02 đối chứng âm (`--rx-timeout-legacy`) | overrun 111: lỗi dây chuyền quay lại |
| L5-03/04 link down 2 s | LOST → RECOVER → RUN, 8/8 OP; valgrind: 0 rò rỉ, 0 lỗi (1 lỗi của SOEM `ecx_setupnic` được suppress, xem `soem.supp`) |
| L5-05 `safeop 2 0x001A` | gọi đúng tên slave 3 và mã lỗi, ack → request OP → về OP, không LOST |
| L5-05 đối chứng âm (`--no-recover`) | slave 3 kẹt SAFEOP+ERR tới cuối |
| L5-11b drop/restore slave 6 (IO) và slave 2 (motion, DC) | đường B: địa chỉ ok → SAFE-OP → OP; slave 2 được khôi phục DC |
| L5-13 SIGSTOP 20 ms, watchdog bật | báo "Sync manager watchdog", 4/4 slave motion về OP (sandbox cần 2 lượt vì jitter làm watchdog trip lại) |
| L5-13 đối chứng âm (`--no-sm-wd`) | không có 0x001B |

Bản ASan/UBSan của `ecm_run` qua các ca L5-02/03/05/11b: không có báo cáo nào.

**Giới hạn đã biết**
- Frame về muộn hơn hạn chót nằm lại trong buffer của SOEM. Nếu index của nó được cấp lại, SOEM có thể ghép nhầm. Sandbox có thấy một lần sau khi bị treo 40 ms (IO PARTIAL 18 chu kỳ). Đây là L5-07, thuộc 7.4.
- Sau đường B, bộ đếm mailbox (Cnt) phía slave đã về 0 nhưng SOEM vẫn nhớ giá trị cũ. SDO ngay sau khi phục hồi có thể bị từ chối một lần. Chưa kiểm, để 7.4 (L5-12) xử lý.
- SOEM không được thiết kế cho hai thread cùng gọi các hàm cấu hình (đường B chạy song song với RT). `ecatcheck` của SOEM cũng làm vậy, và port có mutex, nhưng TSan ở 7.6 có thể báo các chỗ trong SOEM. Nếu có, phải xem từng chỗ.
- Trạng thái `FAILED` của một slave không tự hết: slave tự khoẻ lại, hoặc (sau này) có lệnh operator.

## 10. Kết quả trên Jetson (25/9 chiều)

`sudo -E SB_PRIO=79 SB_CPU=2 ./run_l5_policy.sh`, soft_bus **bật** watchdog (không `--no-sm-wd`):

- L5-01, L5-02 (+ đối chứng âm), L5-03/04, L5-05 (+ đối chứng âm), L5-11b, L5-13 (+ đối chứng âm): **tất cả đạt**.
- L5-02 `mute 200`: overrun motion = **0**; với timeout cũ (`--rx-timeout-legacy`) = **100**.
- L5-13: `SIGSTOP` đúng pid `ecm_run` 20 ms, soft_bus thấy khoảng trống ≥ 10 ms ở 4/4 slave motion, master báo "Sync manager watchdog", 4/4 về OP.
- L5-03/04 dưới valgrind (bản chép không có setcap, soft_bus `--no-sm-wd`): 0 rò rỉ, 0 lỗi.
- 7.2 `run_l5_diag.sh` (nay chạy `--no-recover`): 10/10.

**Lỗi của script chấm, tìm ra khi chạy trên Jetson** (bài học: kiểm xem hiện tượng quan sát được có đúng do lỗi mình tiêm gây ra không):
1. `SIGSTOP` gửi vào pid của `timeout --foreground`, vốn không chuyển tín hiệu xuống process con, nên master chưa hề bị dừng. Trong sandbox L5-13 vẫn "pass", nhưng là pass giả: jitter của sandbox tự làm watchdog trip. Đã sửa bằng cách dừng pid con (`ecm_pid`), và thêm bước kiểm: soft_bus phải thấy khoảng trống ≥ 10 ms ở mọi slave motion.
2. valgrind từ chối chạy binary có setcap → chạy bản chép (`cp` bỏ xattr, script đã chạy bằng root).
3. Dưới valgrind trên arm64, `ecm_run` không cập nhật kịp watchdog 3 ms khi chờ OP ("Failed to reach OPERATIONAL") → riêng ca valgrind, soft_bus chạy `--no-sm-wd`.

**Còn lại trước khi sang 7.4:** hồi quy L1/L6 với soft_bus mới, soak 30 phút `ecm_run` + DC (soak 8 h còn nợ).

## 11. Giai đoạn 7.4: reply muộn / trùng / đảo / dữ liệu cũ / mailbox (25/9, sandbox)

### 11.1. Lỗ hổng trong SOEM (đọc `nicdrv.c`, `ec_main.c`)
- Receive hết hạn → buffer về `EMPTY`. Reply muộn tới khi index của nó còn `EMPTY` thì bị bỏ. Nhưng SOEM chỉ có **16 index**, cấp xoay vòng (~1,1 frame/tick). Reply tới đúng lúc index đã được cấp cho frame mới (đang `TX`) thì được **nhận làm reply của frame mới**: WKC có thể đúng, dữ liệu là của ~15 ms trước, hoặc của nhóm khác.
- Mailbox tuần hoàn: SOEM **không kiểm `Cnt`** của reply từ slave. Một reply bị gửi hai lần sẽ được lần SDO kế tiếp lấy làm câu trả lời; từ đó mọi SDO lệch một lần. `ecx_SDOread` còn hiểu reply download (0x60) là "upload rỗng" và trả `rc = 0`.

### 11.2. Biện pháp phía master
| Biện pháp | Ở đâu | Bắt được |
|---|---|---|
| So header datagram của reply với frame đã gửi (lệnh, ADP/ADO, độ dài) | `ecm_run.c` `reply_header_matches()`, ngay sau receive | reply của frame khác (khác nhóm, khác loại) |
| Cổng tuổi reply: thời gian DC trong reply motion lệch > 1 chu kỳ so với đồng hồ host lúc **gửi** | `ecm_dc.c`, `last_rejected`, `implausible` | reply motion cũ cùng header |
| Cách ly index hết hạn 50 tick (tối đa 8) | `ecm_run.c` `quar_add/quar_expire`, giữ `EC_BUF_COMPLETE` dưới `getindex_mutex` | reply muộn lẻ tẻ (khi bão reply muộn thì tràn, hai biện pháp trên gánh) |
| Reply bị loại: input khôi phục từ bản chụp trước receive, chu kỳ tính là NOFRAME (WKC stats, state machine, DC) | `service_group()` | — |
| Kiểm "độ tươi" (§3.4): bộ đếm 16-bit trong input của slave (vị trí từ `--fresh-offset`, là hợp đồng PDO của slave) | `ecm_policy.c` `ecm_fresh_*` | L5-09 (đóng băng, WKC đúng), và oracle cho L5-07 (bộ đếm chạy lùi) |
| Ghép turnaround theo **index EtherCAT** thay vì theo thứ tự | `turnaround.c` `*_idx()` | trước đây 1 frame mất (NOFRAME) hoặc 1 reply trùng làm lệch vĩnh viễn mọi cặp sau (test T5: 7/9 cặp sai) |
| **Bản vá SOEM** `patches/soem-mbx-cnt.patch`: bỏ reply mailbox có `Cnt` trùng reply vừa nhận | `ecx_mbxinhandler` | L5-12 |

Các cờ đối chứng âm: `--no-quarantine`, `--no-reply-check` (chỉ đếm, không loại).

### 11.3. Kết quả sandbox (`run_l5_io.sh`, 18/18)
| Ca | Có bảo vệ | Đối chứng âm |
|---|---|---|
| L5-07 reply muộn 14 ms và 15 ms, mỗi lần 100 frame | 0 lần bộ đếm lùi, IO PARTIAL/OVER = 0, DC 0 unlock, về RUN; đã chặn 12 reply motion cũ (DC), 9 + 4 reply lạ (header) | `--no-quarantine --no-reply-check`: 13 + 9 reply lạ được nhận, 16 reply motion cũ, **12 lần bộ đếm lùi** (ứng dụng nhận dữ liệu cũ), IO PARTIAL 9 |
| L5-08 `dup 50`, `reorder 30` | về RUN, DC 0 unlock, 0 lần lùi, turnaround ghép 7931/8001 | (T5 offline: ghép theo thứ tự thì 7/9 cặp sai sau 1 frame mất) |
| L5-09 `stale 2 60` | báo "slave 3: inputs UNCHANGED" sau 20 chu kỳ, rồi "changing again"; không slave nào khác | `--fresh-stale 1000`: không báo |
| L5-12 100 vòng ghi+đọc SDO, 10 `mbx_dup` + 10 `mbx_repeat` | SOEM có vá: 100/100 đúng, bỏ 10 reply trùng | SOEM gốc: **50/100 sai giá trị với rc = 0** |

Hồi quy: 7.3 `run_l5_policy.sh` 29/30 trong lượt chạy đầy đủ (L5-13 fail do sandbox quá tải: jitter làm watchdog trip trước cả khi tiêm; chạy riêng 3/3, trên Jetson đã đạt); 7.2 `run_l5_diag.sh` 10/10; test offline: policy 97/97, telemetry (T1–T11) pass, `test_dc_offline` 13/13, soft_bus 75/75 + 116/116.

### 11.4. Giới hạn còn lại
- Reply IO cũ **cùng header** (frame IO cũ ghép vào frame IO mới) không có dấu thời gian để bắt; chỉ cách ly index giảm khả năng. Sandbox chưa thấy trường hợp nào (0 lần bộ đếm lùi ở slave IO).
- Không có DC (`--no-dc`) thì motion cũng chỉ còn phép so header + cách ly.
- Cổng DC: trên host không RT có thể báo nhầm nếu thread bị chiếm CPU ngay sau `sendto()` (sandbox: 0–1 lần / 6 s). Hậu quả: giữ input cũ 1 chu kỳ, tính NOFRAME.
- Bản vá SOEM là **thay đổi thư viện vendor**: cần quyết định. Không vá thì L5-12 fail; `ecm_run`/`l4_test` in rõ "Cnt patch ABSENT".

**Sửa sau khi đo L6 (25/9):** cổng tuổi reply ban đầu luôn bật trong `ecm_dc`. `l6_test` vẫn chờ reply tới `EC_TIMEOUTRET` (2 ms) và đưa `t_wake` làm mốc host, nên tiền đề của cổng không đúng ở đó: sandbox thấy 25 mẫu bị loại nhầm / 60 s. Cổng giờ là cấu hình `gate_ns` (mặc định 0 = tắt); chỉ `ecm_run` bật (1 chu kỳ). `l6_test` in thêm `reply_age_rejected`, sau khi sửa = 0. Test `G0` bắt được `ecm_dc_default_cfg()` quên khởi tạo trường mới (giá trị rác trên stack).

## 12. Soak 30 phút trên Jetson (25/9) và sửa lỗi tắt máy

`ecm_run` + DC, soft_bus FIFO 79 trên core 2, 1800 s, bản 7.3:
- motion 1 799 935 chu kỳ, IO 224 992: 0 WKC sai, 0 NOFRAME, 0 overrun (IO); bus RUN suốt run (DEGRADED=0, LOST=0)
- DC LOCKED, 0 unlock, 419 lần tràn vòng 32-bit; diag 1800 lần đọc, 0 finding
- wake_jitter p99.99 7,1 µs, max 15,8 µs; occupancy p99.99 100 µs, **max 510 µs**, dưới ngân sách nhận 850 µs (còn dư ~340 µs)

**Lỗi tìm ra:** watchdog của mọi slave hết hạn ngay **sau** vòng lặp (khoảng trống 170 ms): `ecm_run` in thống kê, chờ các thread kết thúc, rồi mới về INIT, trong lúc slave vẫn ở OP. Slave thật sẽ giữ SAFEOP+ERR 0x001B cho tới khi được ack. **Sửa:** rời OP (về SAFE-OP) ngay khi vòng lặp dừng, trong cửa sổ loại trừ của telemetry. Sandbox: không còn lần trip nào lúc tắt. Golden SHUTDOWN đổi theo và được sinh lại.

**Quyết định (25/9):** nhận biện pháp L5-07 như §11; **nhận bản vá SOEM** `patches/soem-mbx-cnt.patch` cho L5-12.

**Nợ còn lại:** soak 8 giờ.

## 13. GĐ9.7: EMCY và phân loại AL status code

### 13.1 AL status code → lớp
`ecm_al_code_class()` (`libecmaster/policy`). Danh sách mã lấy từ `ec_ALstatuscodelist` của SOEM (ETG.1000.6/ETG.1020). Datasheet ESC Section I §10.1.3 chỉ định nghĩa thanh ghi 0x0134 và dẫn sang ETG.1020.

| Lớp | Mã | Planner |
|---|---|---|
| CONFIG | 0x0003, 0x0004, 0x0006, 0x000E, 0x0013–0x0017, 0x001C–0x001F, 0x0024–0x0029, 0x002E, 0x0030, 0x0031, 0x0035–0x0037, 0x0041–0x0045, 0x004F, 0x0070 | `FAILED(config)` ngay, không ack, không thử lại, không tính attempt. Lý do: ack rồi xin lại thì master gửi đúng cấu hình cũ, slave sẽ từ chối lại |
| TRANSIENT | mọi mã khác, gồm 0x001A sync error, 0x001B SM watchdog, 0x0018/0x0019, 0x0020–0x0023, 0x002C, 0x0032–0x0034, điện áp, mã hãng 0x8000+ | Như GĐ7.3: ack → OP, tối đa 5 lần, backoff |

Slave `FAILED(config)` tự khoẻ lại (về OP) thì cờ được xoá, giống `FAILED` cũ. Lúc khởi động, nếu không đạt PREOP/SAFEOP/OP, ecm_run in từng slave kèm AL status, code, text và lớp, và nói rõ khi có lỗi cấu hình.

**Sửa lỗi tìm thấy khi làm 9.7:** `request_all_state()` trả nguyên giá trị của `ecx_statecheck()`, tức trạng thái *đọc được* (BRD = OR của mọi slave), và chỗ gọi coi mọi giá trị ≠ 0 là thành công. Bus có một slave kẹt ở PREOP (0x02 | 0x04 = 0x06) vì vậy bị coi là "đã tới SAFEOP", và ecm_run đi tiếp lên OP. Nay hàm trả `== target`. Golden không đổi, vì các bus khoẻ vẫn tới đúng trạng thái.

### 13.2 EMCY
- SOEM đưa EMCY vào `context->elist` từ `ecx_mbxinhandler` (cyclic mailbox, trong RT thread). Từ 9.7 **RT thread là reader duy nhất** của elist (`elist_drain()`, sau process data của mỗi tick RUN/DEGRADED). Mỗi entry thành một event cho monitor:
  - `ECM_EV_EMCY` → `ecm_diag_add_emcy()` (snapshot: mục `emcy:`, 4 bản tin gần nhất mỗi slave, finding WARN khi code ≠ 0) + log `[EMCY]`, tối đa 20 dòng/slave, sau đó chỉ đếm;
  - `ECM_EV_SDO_ABORT` → log `[SDO]` với abort code và text;
  - lỗi SOEM khác → log `[SOEM]`.
- Event ring đầy → `emcy lost` trong snapshot (finding WARN).
- Sau khi các thread chạy, `ecm_eni_soem` không đọc elist nữa (`ecm_eni_soem_elist_foreign`). InitCmd chạy lại khi recovery vẫn báo lỗi; abort code thì xem dòng `[SDO]`.
- **GĐ10.0 (R-07, IS620N thật mất nguồn khi OP):** cả 14 InitCmd PS chạy lại trong recovery đều FAILED (wkc=0) mà slave vẫn lên OP. Nguyên nhân: mailbox của slave vẫn ở chế độ CYCLIC của SOEM — yêu cầu xếp hàng chờ `ecx_mbxhandler()` của RT thread (dừng trong RECOVER toàn bus; slave ở INIT/PRE-OP không có process data), và `mbxincnt` chỉ reset **sau** `ecx_reconfig_slave`. Sửa (bản vá 0018): trong lúc reconfigure, mailbox của slave chuyển sang polled (`ECT_MBXH_NONE`), reset cả hai bộ đếm Cnt, xả response cũ, rồi bật lại cyclic. SOEM bỏ qua giá trị trả về của hook `PO2SOconfig`, nên lỗi được ghi lại riêng: InitCmd hỏng → slave **giữ ở PRE-OP, FAILED(config), không OP** (PDO layout không phải của ENI), trục CiA402 của nó nhận lỗi cấu hình; RECOVER toàn bus không reconfigure lại slave FAILED(config). Đối chứng âm `--recover-cyclic-mbx`. Test `tools/gd10/run_recover_10_0.sh` (RC-01…RC-06).
- Race còn lại (đã ghi trong header `ecm_run.c`): mailbox thread push SDO abort trong lúc RT pop, vì elist của SOEM không có lock. Entry đó hiếm khi bị mất hoặc đọc hai lần. Nó chỉ dùng để báo cáo, không dùng để quyết định.
- `soft_bus`: ctl `emcy <node> <code> [reg] [count]` (data = seq LE16, A5 5A C3, mỗi frame một bản tin khi SM1 trống); `reject_al <node> <code> <state> sticky`.

Test: `tools/gd9/run_emcy_9_7.sh` (M-01…M-03, P-01a/b, P-02, P-03).

## 14. GĐ9.8: kiểm độ tươi theo từng slave

- `--fresh LIST` (ví dụ `all=0,2=off,5=4:8`): mỗi slave có thể bật (byte offset, bộ đếm 8/16/32 bit) hoặc tắt. `--fresh-offset N` giữ nguyên nghĩa GĐ7.4, tương đương `--fresh all=N`, nên L5-09 không đổi.
- Cấu hình được giải sau khi map process data: bộ đếm nằm ngoài input của slave → từ chối trước SAFE-OP và nêu tên slave. Trước đây trường hợp này bị bỏ qua im lặng ở mỗi chu kỳ.
- Lúc khởi động in từng slave: bật (bit, byte) hoặc `OFF -- "WKC correct but data old" is NOT detected`.
- Servo thương mại (IS620N) thường không có bộ đếm như vậy → `off`. Ở GĐ10 dùng toggle bit của statusword hoặc EMCY ở lớp CiA402.

Test: `tools/gd9/run_fresh_9_8.sh` (F-01 = L5-09 + đối chứng âm; F-02a…d).
