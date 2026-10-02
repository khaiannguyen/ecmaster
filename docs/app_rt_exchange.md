# Trao đổi dữ liệu giữa thread ứng dụng và RT thread — thiết kế (GĐ10.1)

**Trạng thái:** thiết kế ở 10.1, **code ở 10.4** (`libecmaster/xchg/`, hook trong `apps/ecm_run`). Mục 10 dưới đây ghi những chỗ code khác bản thiết kế và cách chạy. Lớp CiA402 (10.5) dùng lại cơ chế này.

## 1. Vấn đề

Hiện tại chỉ RT thread chạm IOmap. `--pdo-set` là giá trị hằng, bind lúc khởi động, RT ghi lại mỗi tick. `--pdo-get` chỉ in khi thoát (`docs/pdo_bind.md`). `app_thread_fn` là thread rỗng (`usleep` vòng).

GĐ10 cần:
- ứng dụng (non-RT) gửi lệnh trục (enable, disable, đổi mode, fault reset, quick stop) và **setpoint theo từng chu kỳ** (CSP/CSV), có timestamp;
- ứng dụng đọc trạng thái trục và input mới nhất mà không chặn RT;
- sau này `ros2_control` (sau `master-v1.0`) nạp quỹ đạo vào cùng chỗ đó;
- GĐ11 (`api` thread) đọc trạng thái trục qua cùng cơ chế handoff một chiều.

## 2. Ràng buộc (giữ từ GĐ4–GĐ9)

| # | Ràng buộc | Nguồn |
|---|---|---|
| C1 | RT thread là chủ duy nhất của IOmap. Không thread nào khác đọc/ghi IOmap | GĐ4, `pdo_bind.md` |
| C2 | Trong RT: không syscall, không lock, không malloc, thời gian bị chặn trên (O(số trục), số lệnh/tick có giới hạn) | GĐ4, kế hoạch GĐ10 §6 |
| C3 | Không đăng ký → nhánh không chạy, không cấp phát: golden 547/573/89/548 khớp từng dòng | Q-01, nguyên tắc 3 GĐ10 |
| C4 | Writer RT không bao giờ chờ reader. Reader non-RT được phép thử lại | GĐ4 |
| C5 | Đúng trên arm64 (memory ordering yếu): mọi trường chia sẻ đi qua atomic C11, TSan sạch, có đối chứng âm | GĐ7.6 |
| C6 | `libecmaster` không biết nghĩa object: lớp trao đổi chỉ chở số, CiA402 nằm ở `libecm_cia402` | DoD GĐ10 |

## 3. Thời điểm trong một tick

Cấu trúc tick hiện tại (`ecm_run.c`, vòng RT): ngủ tới `next` → `ecm_pdo_set` các giá trị `--pdo-set` → `service_group(motion)` (gửi, nhận, mailbox pump) → IO nếu tới hạn → `elist_drain` → DC(b) → telemetry.

Hook trao đổi (và hook CiA402 10.4) chạy **ngay sau receive của motion ở tick k, trước lần gửi ở tick k+1**:

```
tick k:   [send(out_k) → receive(in_k)] → elist_drain → DC(b) → HOOK(k) → telemetry → sleep
                                                          │
                                                          ├─ đọc in_k (input vừa nhận)
                                                          ├─ lấy lệnh, lấy setpoint gắn tick k+1
                                                          ├─ ghi out_{k+1} vào IOmap
                                                          └─ publish trạng thái (seqlock)
tick k+1: [send(out_{k+1}) → receive(in_{k+1})] → ...
```

Hệ quả, ghi rõ trong `docs/cia402.md`:
- Setpoint gắn tick `t` đi ra trên dây trong frame của tick `t`.
- Input → output qua master: **1 chu kỳ** (đọc ở cuối k, gửi đầu k+1).
- Hook chạy sau DC(b), nên không làm lệch thời điểm send (đại lượng #2 không đổi). Thời gian hook cộng vào occupancy, đo riêng bằng histogram (Q-02, Q-03).
- Khi nhận lỗi (`EC_NOFRAME`, WKC sai): hook vẫn chạy, nhưng nhận cờ `in_valid = 0` và **không** cập nhật giá trị actual từ input cũ. Quyết định "giữ/dừng" thuộc lớp trên (CiA402: S5).

## 4. Ba kênh

### 4.1 Lệnh: app → RT (SPSC ring, phần tử cố định)

```c
typedef struct {
    uint64_t tick;        /* apply at this tick; 0 = next hook             */
    uint16_t target;      /* axis or bind slot, meaning set by the owner    */
    uint16_t op;          /* opaque to libecmaster, owner-defined           */
    uint32_t seq;         /* app-side sequence, echoed in the status        */
    int64_t  arg0, arg1;
} ecm_xcmd_t;             /* 32 byte, _Static_assert */
```

- Một ring cho toàn bộ lệnh rời rạc (enable, mode, fault reset…). Dung lượng 256 (lũy thừa 2).
- RT pop **tối đa `ECM_XCMD_PER_TICK` = 8** lệnh mỗi tick (C2). Lệnh còn lại chờ tick sau, đếm `cmd_deferred`.
- Lệnh có `tick` ở tương lai: RT xem đầu ring, chưa tới thì không pop (thứ tự lệnh giữ nguyên, FIFO).
- `seq` được RT ghi lại vào trạng thái (`last_cmd_seq`) → app biết lệnh đã được áp dụng mà không cần kênh trả lời riêng.

### 4.2 Setpoint: app → RT (một ring mỗi trục/slot)

```c
typedef struct {
    uint64_t tick;        /* the frame this setpoint goes out in            */
    int64_t  v[3];        /* e.g. CSP: position, velocity offset, torque offset */
} ecm_xsp_t;              /* 32 byte */
```

- Ring riêng mỗi trục, dung lượng 512 (≈ 0,5 s ở 1 ms). Một trục chỉ có một producer (thread ứng dụng hoặc sau này `ros2_control`), nên vẫn là SPSC.
- Ở `HOOK(k)`, RT tìm setpoint cho tick `k+1`:
  - bỏ qua phần tử có `tick < k+1` (đến trễ), đếm `sp_late`;
  - phần tử `tick == k+1` → dùng, pop;
  - phần tử đầu có `tick > k+1` hoặc ring rỗng → **không có setpoint**: đếm `sp_underrun`, giữ giá trị lần cuối (chuẩn bị S3: không ngoại suy).
- Số phần tử RT đọc mỗi tick bị chặn: tối đa `ECM_XSP_SKIP_MAX` = 16 phần tử trễ được bỏ qua mỗi tick; phần còn lại để tick sau (C2).
- App cần biết "bây giờ là tick nào" → kênh 4.3 publish `tick` và thời điểm send của tick đó. App đặt setpoint cho `tick_now + lead`, `lead` ≥ 2 (khuyến nghị 4–10 chu kỳ, đo ở 10.4).

### 4.3 Trạng thái: RT → app (seqlock, một bản ghi mỗi trục + một bản ghi đồng hồ)

```c
typedef struct {
    _Atomic uint32_t seq;            /* odd while the RT thread writes        */
    _Atomic uint64_t tick;           /* hook tick that produced this record   */
    _Atomic uint64_t w[ECM_XST_WORDS];/* payload words, owner-defined (CiA402:
                                        statusword, actual pos/vel/torque,
                                        mode display, error code, axis state,
                                        last_cmd_seq, counters)              */
} ecm_xst_t;
```

Writer (RT, mỗi tick, không bao giờ chờ):
```c
uint32_t s = atomic_load_explicit(&st->seq, memory_order_relaxed);
atomic_store_explicit(&st->seq, s + 1, memory_order_relaxed);
atomic_thread_fence(memory_order_release);
/* payload: relaxed atomic stores */
atomic_store_explicit(&st->seq, s + 2, memory_order_release);
```

Reader (non-RT), thử lại có giới hạn:
```c
for (int tries = 0; tries < ECM_XST_TRIES; tries++) {
    uint32_t s1 = atomic_load_explicit(&st->seq, memory_order_acquire);
    if (s1 & 1) continue;
    /* payload: relaxed atomic loads into a local copy */
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&st->seq, memory_order_relaxed) == s1) return 0;
}
return -1;   /* torn every time: caller retries later, counts it */
```

Vì sao seqlock mà không phải ring:
- reader chỉ cần **bản mới nhất**, không cần mọi tick (ring sẽ đầy khi app chậm);
- writer O(1), không phụ thuộc reader (C4);
- payload là atomic từng word nên TSan không báo data race giả (C5). Không dùng `memcpy` trên vùng chia sẻ.

Bản ghi đồng hồ (`ecm_xclock`): `tick`, thời điểm send của tick đó (CLOCK_MONOTONIC, hoặc launch time khi `--link etf`), `cycle_ns`, trạng thái bus (`ecm_bus` RUN/DEGRADED/LOST). App dùng nó để đặt `tick` cho setpoint.

## 5. Vị trí trong mã nguồn

| Thành phần | Thư mục | Nội dung |
|---|---|---|
| `ecm_xchg` | `libecmaster/xchg/` (mới) | 3 kênh trên, chung, không biết nghĩa `op` / payload. Thuần C11, test offline + TSan |
| Ring tổng quát | `libecmaster/xchg/` | Ring SPSC cho phần tử 32 byte. **Không** sửa `telemetry/ring_spsc.c` (đường telemetry hiện tại giữ nguyên, C3) |
| Hook | `apps/ecm_run` | `ecm_run_register_hook(fn, ctx)`; không đăng ký → `if (g_hook)` không vào (C3) |
| CiA402 | `libecm_cia402/` (10.3) | Định nghĩa `op`, payload trạng thái, hàm hook; gọi `ecm_pdo_bind/get/set` trên IOmap qua con trỏ hook nhận được |

Hook nhận con trỏ IOmap từ RT thread, đúng như `--pdo-set` hiện nay (C1). Lớp trao đổi không tự ghi IOmap.

`--pdo-set` / `--pdo-get` giữ nguyên làm công cụ kiểm tra. Có thể viết lại `--pdo-get` theo thời gian thực qua kênh 4.3 sau, không bắt buộc ở GĐ10.

## 6. Mất bus, dừng, khởi động

- **Bus LOST / recovery (GĐ7):** RT tiếp tục chạy hook với `in_valid = 0`. Setpoint đến trong lúc LOST bị bỏ (đếm `sp_dropped_lost`), không dồn lại để phát khi bus lên OP. Lớp CiA402 chốt trục disabled (S5); kênh trao đổi chỉ chở thông tin.
- **Thoát (SIGINT/SIGTERM):** RT ngừng nhận lệnh mới từ ring, chạy chuỗi tắt của lớp trên (S6) trong số tick có giới hạn, rồi mới dừng cyclic. Cơ chế này nằm trong hook CiA402, ring chỉ cần cờ `closing`.
- **Khởi động:** ring và seqlock cấp phát trong `main()` trước `mlockall`/`pthread_create`, giống `g_mbx`. Không cấp phát nào trong RT.

## 7. Đo đạc

Mỗi kênh có bộ đếm do RT ghi (chỉ RT ghi, đọc qua bản ghi trạng thái): `cmd_applied`, `cmd_deferred`, `sp_used`, `sp_late`, `sp_underrun`, `sp_dropped_lost`, và histogram thời gian hook (p50/p99/p99.99/max) dùng `telemetry/histogram`. Đưa vào diag snapshot và sau này `stats.json` (GĐ11).

## 8. Test sẽ viết ở 10.4

| ID | Nội dung |
|---|---|
| X-01o | Offline: ring lệnh FIFO, tối đa 8/tick, lệnh `tick` tương lai không vượt hàng |
| X-02o | Offline: setpoint đúng tick dùng đúng; trễ bị bỏ và đếm; rỗng → underrun, giữ giá trị cuối; tối đa 16 phần tử trễ/tick |
| X-03o | Offline: seqlock 1 writer + 1 reader, payload là hàm của `tick` → reader không bao giờ thấy bản ghi trộn |
| Q-05 | TSan (cùng khung `tests/tsan`): ring lệnh, ring setpoint, seqlock. **Đối chứng âm:** build test bỏ fence/đổi `release` → `relaxed` → TSan hoặc X-03o phải fail |
| Q-01 | Không đăng ký hook: golden 547/573/89/548 khớp từng dòng |
| Q-02 | Hook rỗng đăng ký: occupancy p99.99 tăng < 5 µs (Jetson, 4+4) |
| Q-04 | Setpoint cạn 100 ms trên `soft_bus`: underrun đếm đúng, output giữ giá trị cuối |

## 9. Câu hỏi để ngỏ (quyết ở 10.4)

1. `lead` mặc định cho setpoint: đo khoảng cách thực tế giữa wake của thread app và tick RT trên Jetson (có tải ROS2/AI mô phỏng như D-01 GĐ11).
2. Có cần ring setpoint 2 producer (app + homing nội bộ) không: hiện thiết kế 1 producer/trục; homing do drive tự làm (mode HM), master chỉ gửi lệnh.
3. Kích thước `ECM_XST_WORDS`: chốt khi 10.3 liệt kê đủ payload trạng thái trục (dự kiến 12 word).

## 10. Code 10.4 — khác thiết kế và cách dùng

### Khác bản thiết kế
- Payload trạng thái 8 word (`ECM_XST_WORDS`), không phải 12: đủ cho slot (giá trị, used, late, underrun, dropped_lost, valid) và bản ghi đồng hồ. Lớp CiA402 sẽ có bản ghi riêng của nó.
- Slot = một `ecm_pdo_handle_t` (không phải "trục"): `libecmaster` không biết nghĩa object; trục là việc của `libecm_cia402`. Lệnh chung chỉ có `ECM_XOP_SET` (ghi một lần), `NOP`; op ≥ `ECM_XOP_USER` để chủ hook (CiA402) xử lý.
- Underrun chỉ đếm sau khi slot đã "armed" (đã nhận setpoint/SET đầu tiên); trước đó output không bị đụng.
- Khi bus mất (`bus_lost`): setpoint của tick đó bị bỏ (`dropped_lost`), không đếm underrun, không ghi output.
- Hook chạy cả khi LOST/RECOVER (với `bus_lost = 1`) để lớp trên chốt trạng thái (S5). Thời gian hook cộng vào occupancy và có histogram riêng.

### Phát hiện khi test
- **`memcpy` cỡ cố định làm TSan mù**: gcc nội tuyến `memcpy(…, 32)` thành lệnh copy không được TSan instrument → ring hỏng (publish `relaxed`) **không bị báo**. Đối chứng âm của Q-05 bắt được điều này. Phần tử ring giờ copy từng `uint64_t`. Hàng đợi cũ (GĐ7.6) copy bằng gán struct, TSan thấy được (đối chứng âm GĐ7.6 vẫn đạt).
- Kiểm echo của bộ sinh test lúc đầu kiểm lại cùng một bản ghi hai lần khi RT chưa chạy tick mới → báo lệch giả. Sửa: chỉ kiểm bản ghi mới liền sau bản trước. Có đối chứng âm `--xchg-echo-off 1` (lệch > 90 %).

### Dùng trong `ecm_run` (công cụ kiểm, lớp CiA402 sẽ đăng ký hook của nó ở 10.5)
```bash
ecm_run ... --pdo-scan --hook empty                                   # Q-02: chi phí hook rỗng
ecm_run ... --pdo-scan --hook xchg --xchg-out 1:0x607A:0 --xchg-in 1:0x6064:0 \
        --xchg-sine 10000:1 [--xchg-lead 4] [--xchg-starve 2:100]     # bộ sinh sin trong thread ứng dụng
```
Báo cáo khi thoát: `[HOOK]` (p50/p99/p99.99/max), `[XCHG] slot …` (used/late/underrun/dropped_lost), `[XCHG-APP]` (pushed, gaps, echo_checked/mismatch, torn = reader bỏ cuộc sau 64 lần thử).

### Test
| ID | Ở đâu | Kết quả sandbox |
|---|---|---|
| X-01o…X-04o, Q-04o | `libecmaster/xchg/test_xchg_offline` (+ ASan) | 42/0 |
| Q-05 | `libecmaster/xchg/run_xchg_tsan.sh`: 3 thread dưới TSan; âm: ring `relaxed` → TSan báo; seqlock bỏ kiểm seq → thấy bản ghi xé | 3/0 |
| Q-01 | `tools/gd10/run_hook_10_4.sh q01`: golden 4+4, N=1 không hook; 4+4 với `--hook empty` vẫn y hệt | đạt |
| H-01…H-05 | nt: 1 lần gọi/tick; setpoint dùng, echo 0 lệch + đối chứng âm; Q-04 dừng 100 ms → underrun 98 (lead 4), giữ giá trị; lead 0 → mọi setpoint trễ; 4 trục 8 slot | đạt |
| Q-02, Q-03 | nt, `STRICT=1 SB_PRIO=79 Q03_SEC=1800` trên Jetson | chỉ có nghĩa trên Jetson |
