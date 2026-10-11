# API chẩn đoán từ xa (`ecm_run --api`)

Giai đoạn 11. Kế hoạch: `claude/giai_doan_11_ke_hoach.md`. Tài liệu này là **hợp đồng** của API: schema, đơn vị, quy tắc version. `ecm_diag`, web UI và node ROS2 sau này đều là client của nó.

Trạng thái: **11.1 `stats.json`** (bản vá 0031), **11.2 `ctl.sock`** (bản vá 0032, §10), **11.3 `ecm_diag` qua API + cấu hình RT** (bản vá 0033, §4 `rt`, §11).

## 1. Bật / tắt

| Tuỳ chọn | Mặc định | Ý nghĩa |
|---|---|---|
| `--api` | tắt | Tạo thread `api` và file `stats.json`. Không có cờ này thì không có thread, không có handoff nào được ghi, không có file (D-06, A-01) |
| `--api-dir DIR` | `/run/ecmaster` | Thư mục của `stats.json`. Phải là **tmpfs** (`/run`, `/dev/shm`): trên ext4 mỗi `rename` là một lần ghi journal (đo trong sandbox: 333 µs so với 14 µs) |
| `--api-cpus LIST` | `1-2` | CPU của thread `api` (`1-2`, `1,2`, `2`). **CPU 3 bị từ chối** vì là core RT. CPU 0 vẫn chạy nhưng in cảnh báo, vì ngắt RX của i226 cố định ở đó (§0.5 của kế hoạch) |
| `--api-allow FILE` | không có | Allowlist của `sdo_write` (§10.4). Không có file thì allowlist rỗng: mọi `sdo_write` bị từ chối. Mẫu rỗng: `config/api_sdo_allow.json` |
| `--api-group NAME` | nhóm của user gọi `sudo` | Nhóm của `ctl.sock` (quyền `0660`). Khi chạy bằng `sudo` mà không có tuỳ chọn này, socket thuộc nhóm `SUDO_GID`, nên user đã gọi `sudo` kết nối được mà không cần `sudo` |
| `--api-sdo-timeout-ms N` | 500 | Timeout của một SDO từ API (50…5000). Trong lúc chờ SDO, thread `api` không publish (§10.3) |

- Không tạo được `--api-dir`, không ghi được, hoặc `--api-cpus` sai thì `ecm_run` dừng **trước khi chạm bus** (A-07).
- `/run` chỉ root ghi được. Khi chạy `ecm_run` bằng capabilities (không sudo), cần dùng `--api-dir` trỏ vào thư mục ghi được (ví dụ `/dev/shm/ecmaster`). Ở GĐ12, unit systemd sẽ tạo `/run/ecmaster` bằng `RuntimeDirectory=`.
- `--api-dir`, `--api-cpus`, `--api-allow`, `--api-group`, `--api-sdo-timeout-ms` mà không kèm `--api` thì bị từ chối, để tránh tưởng API đang bật.
- Allowlist sai, hoặc `ctl.sock` đã có một `ecm_run` khác phục vụ, thì `ecm_run` dừng trước khi chạm bus. Socket cũ không còn ai nghe thì được thay; một file thường cùng tên thì không bị xoá.

## 2. Dữ liệu đi từ đâu

```
RT thread (core 3) ──H3──┐      2 slot, ghi đè-không-chờ (libecmaster/api)
telemetry          ──H1──┤
monitor            ──H2──┼──> thread api (core 1-2, 10 Hz) ──> stats.json (tmp + rename)
lớp CiA402 (seqlock) ────┘   (thread api đọc thẳng, seqlock cho phép nhiều reader)
```

| Handoff | Producer | Nội dung | Nhịp |
|---|---|---|---|
| H1 | telemetry | 4 histogram: `wake_jitter`, `prep_send`, `occupancy`, `turnaround`; ring drop | 10 Hz |
| H2 | monitor | bộ đếm lỗi ESC (0x0300–0x0313), AL, DL status, EMCY (4 bản tin mới nhất), recovery, `findings` | 10 Hz |
| H3 | RT | bộ đếm nhóm (cycles, WKC theo lớp, overrun), DC, `txtime_*`, `tai_steps`, số lần gọi hook | 10 Hz |
| H3 | RT | histogram DC \|e\| (8 KB) và histogram hook | mỗi cái 1 Hz, lệch pha |

**Ngoại lệ của §0.2 mục 1 (chốt 9/10/2026).** H3 là nhánh duy nhất được thêm vào RT thread:
- Chỉ chạy khi có `--api`.
- Nằm sau `ring_push`, ngoài đoạn đo occupancy, và không bao giờ chờ: khi cả hai slot đều đầy thì bỏ bản cập nhật đó và tăng `drops`.
- Pha publish: scalar ở giữa mỗi khe 100 ms; histogram DC ở 1/4 giây và histogram hook ở 3/4 giây, cả hai đều lệch khỏi tick của scalar.
- Chi phí được đo ở D-01 (bước 11.5) trên Jetson.

Khi chương trình dừng:
1. RT thoát vòng lặp và giao giá trị cuối (thử lại tối đa ~2 s, ngoài đường cyclic).
2. telemetry và monitor giao giá trị cuối trước khi thoát.
3. `main` join hai thread đó, bật `g_api_stop`, rồi join thread `api`.
4. Thread `api` ghi lần cuối với `"final": true`.

Nhờ thứ tự này, file cuối khớp với bản tổng kết in ra ở cuối run (D-10, A-04).

## 3. Cách đọc file

- Mỗi lần đọc: mở, đọc hết, đóng. File được thay bằng `rename`, nên reader luôn thấy bản cũ nguyên vẹn hoặc bản mới nguyên vẹn, không bao giờ thấy nửa file (A-03; đối chứng âm ghi tại chỗ: 883 795 lượt đọc rách).
- **Phát hiện stale:** `pub_seq` tăng 1 mỗi lần ghi. `mono_ns` là `CLOCK_MONOTONIC` của máy chạy `ecm_run` và chỉ so được trên cùng máy đó; client từ xa dùng `pub_seq` cùng đồng hồ của chính nó. Mỗi phần còn có tuổi riêng trong `sources.{rt,telemetry,monitor}.mono_ns`. Ví dụ: monitor bị chặn vài giây khi recovery thì `sources.monitor` cũ đi, trong khi `pub_seq` vẫn tăng.
- `ecm_run` chết (`kill -9`) thì file cuối vẫn nguyên vẹn và có `"final": false` (A-08). Khi đó `pub_seq` đứng yên.
- `"final": true` nghĩa là run đã kết thúc bình thường và số liệu là số cuối.

## 4. Schema v1

Khoá tiếng Anh. Mọi **mã/thanh ghi** (statusword, 0x603F, AL code, DL status, vendor/product, EMCY) là chuỗi hex `"0x1637"`; bộ đếm là số nguyên. Một khoá **vắng mặt** nghĩa là nguồn của nó chưa publish lần nào, hoặc không áp dụng; một khoá có giá trị **`null`** nghĩa là có chỗ cho nó nhưng không có dữ liệu.

| Khoá | Kiểu | Ghi chú |
|---|---|---|
| `schema` | 1 | xem §6 |
| `pub_seq`, `mono_ns`, `pid` | int | §3 |
| `final` | bool | §2 |
| `buckets` | object | định nghĩa bucket histogram (§5), có trong mọi file |
| `bus.fsm` | `STARTING` `RUN` `DEGRADED` `LOST` `RECOVER` | `STARTING` khi RT chưa publish lần nào |
| `bus.cycle_ns`, `bus.link` (`af_packet`/`etf`), `bus.n_slaves` | | |
| `bus.ports[]` | **list** | v1.0 có 1 phần tử `primary`; GĐ13 thêm cổng phụ (§1.6 kế hoạch). Không có `bus.iface` |
| `ports[].name`, `iface`, `link` (`up`/`down`/`unknown`) | | `link` lấy từ `operstate` |
| `ports[].tx`, `noframe` | int | frame cyclic gửi qua cổng / không có reply. Khi chỉ có một cổng: tổng qua các nhóm. **Không có `rx`** |
| `ports[].crc_err`, `rx_missed`, `carrier_changes` | int \| null | sysfs `statistics/rx_crc_errors`, `rx_missed_errors`, `carrier_changes`, đọc mỗi 1 s bởi thread `api`. `null` = driver không có |
| `bus.path` | `single` | v1.1 thêm `ring_closed`, `ring_open` |
| `groups[]` | list | `name` (`motion`/`io`), `slaves[]` (vị trí SOEM), `cycle_ns`, rồi khi RT đã publish: `cycles`, `ok`, `noframe`, `zero`, `partial`, `over`, `max_run_bad`, `overrun`. Luôn có `ok+noframe+zero+partial+over == cycles` |
| `hist.{wake_jitter,prep_send,turnaround,occupancy,hook}` | object \| null | kiểu `log20_100ns_v1`, đơn vị ns, có thêm `count`, `sum`, `min`, `max` |
| `hist.dc_err` | object \| null | DC \|e\|, kiểu `lin1us_v1`, chỉ tính mẫu sau 5 s đầu (như `[DC]` cuối run). `null` khi DC tắt |
| `dc` | object \| null | `state` (`UNANCHORED`/`ACQUIRE`/`LOCKED`), `ref_pos`, `locks`, `unlocks`, `clamps`, `wraps`, `stale`, `err_ns` (sai pha gần nhất) |
| `link` | object | `etf_missed`, `etf_invalid`; khi SOEM có bản vá txtime thì thêm `txtime_late`, `txtime_bypass`, `send_err`, `tai_steps` |
| `slaves[]` | list | luôn có: `pos`, `vendor`, `product`, `revision`, `name` |
| `slaves[]` (khi diag bật và slave đã trả lời) | | `station`, `al` (`INIT`/`PREOP`/`BOOT`/`SAFEOP`/`OP`), `al_err` (bit 0x10), `al_code`, `dl_status` |
| `slaves[]` (khi diag bật) | | `answered`, `inv[4]`, `rx_err[4]`, `fwd_err[4]`, `lost_link[4]` (theo cổng 0–3, thanh ghi 0x0300–0x0313, cộng dồn từ đầu run), `pu_err`, `pdi_err` |
| `slaves[]` (khi monitor đã publish) | | `policy` (`none`/`ack`/`op`/`reconfig`: quyết định gần nhất), `recover` (`ok`/`retrying`/`failed`/`failed_config`), `attempts`, `recoveries`, `initcmd_failed` (số lần InitCmd của ENI thất bại khi reconfigure), `emcy_count`, `emcy[]` (≤ 4, mới nhất trước: `mono_ns`, `tick`, `code`, `reg`, `data` 10 ký tự hex, `class`) |
| `axes[]` | list | chỉ khi `--hook cia402`. Luôn có `name`, `slave`, `n`; khi đọc được seqlock thì thêm `state` (§7), `sw`, `cw`, `mode` (yêu cầu), `mode_display` (0x6061), `err` (§7), `error_code`, `error_src` (`603F`/`EMCY`/`none`), `error_text` (từ `--emcy-map`), `pos`, `vel`, `tick`, `used`, `late`, `underruns`, `dropped`, `step_refused`, `pp_acked`, `homed` |
| `rt` | object | `dma_latency_held`, `hook` (tên \| null), `hook_calls`, `api_cpus`; *(11.3)* cấu hình RT của máy, đọc một lần lúc khởi động: `rt_cpu` (3), `isolated_cpus` (`/sys/devices/system/cpu/isolated`, `""` = không có, `null` = không đọc được), `cpuidle` (`driver` \| null, `off` = `cpuidle.off=1` hoặc không có driver, `states` = số idle state của core RT, `states_disabled` = tên các state đang bị tắt), `kernel` (`uname -r`), `cmdline_flags` (chỉ các cờ RT của `/proc/cmdline`: `isolcpus=`, `nohz_full=`, `rcu_nocbs=`, `irqaffinity=`, `cpuidle.off=`, `transparent_hugepage=`, …; không chép cả dòng) |
| `findings[]` | list | `type`, `sev` (`info`/`warn`/`error`), `text`, `a`, `b`, `c`, `count`, `active`. `type` gồm các phát hiện của `ecm_diag_analyze` (§7) và `recover_failed`, `recover_failed_config` |
| `sources` | object | `rt` / `telemetry` / `monitor`: `seq`, `mono_ns` (\| null); telemetry có thêm `ring_drops`, monitor có thêm `diag`, `diag_reads`, `diag_frames_lost`, `emcy_total`, `emcy_lost`; `handoff_drops` là tổng số cập nhật bị bỏ ở các handoff |

`error_code` và `error_src` theo cùng quy tắc với dòng `[AXIS]` của 10.8 (`ecm_cia402_axis_code`). 0x603F ưu tiên trước. EMCY chỉ được dùng khi trục đang ở Fault và lỗi gán được cho đúng trục đó. IS620N không gửi EMCY, nên mã lỗi của nó đến từ 0x603F (ví dụ 0x0E08 = Er.E08).

Đã bỏ khỏi v1 (so với bản nháp §2.3 cũ): `ports[].rx`, `axes[].tracking_max`. Lý do của `tracking_max`: số này chỉ có trong driver test `--cia402-script`; khi node ROS2 là nguồn setpoint thì master không tính được.

## 5. Histogram

Cả hai kiểu đều **cộng dồn từ đầu run**. Muốn phân vị theo cửa sổ thời gian, client lấy hiệu `counts` giữa hai lần đọc.

`counts` **bỏ các số 0 ở cuối**: phần tử thiếu được hiểu là 0, và `sum(counts) == count`.

| Kiểu | Định nghĩa |
|---|---|
| `log20_100ns_v1` (ns) | `buckets.log20_100ns_v1.upper[i]`, i = 0…119: cận trên của bucket i, 20 bucket mỗi thập phân, từ 100 ns tới 100 ms (`ecm_hist_t`). Bucket 0 = mọi giá trị ≤ upper[0] (kể cả < 100 ns); bucket i = (upper[i−1], upper[i]]; bucket 120 = lớn hơn 100 ms |
| `lin1us_v1` (µs) | bin b = \|e\| trong [b, b+1) µs, b = 0…999; bin 1000 = ≥ 1000 µs (`n` = 1001) |

Phân vị p: cộng dồn `counts` tới khi vượt `p × count`; giá trị là `upper[i]` (log), hoặc b (lin, đúng như dòng `[DC] |e| p99.99` cuối run).

## 6. Version và tương thích

- **Client phải bỏ qua khoá lạ và giá trị enum lạ** (ví dụ `bus.path = "ring_open"`, một `findings[].type` mới, một trạng thái bus mới).
- Các thay đổi **không** tăng `schema`: thêm khoá, thêm giá trị enum, thêm phần tử vào `ports[]`.
- Các thay đổi **phải** tăng `schema`: đổi tên hoặc xoá khoá, đổi nghĩa hay đơn vị, đổi kiểu (số ↔ chuỗi, object ↔ list), đổi định nghĩa bucket. Riêng bucket thì thêm kiểu mới với tên mới (`…_v2`) thay vì sửa kiểu cũ.
- Tên khoá enum ổn định nằm trong code (`ecm_api_*_key`, `ecm_cia402_ds_key/err_key`). Test offline bắt lỗi khi enum gốc lớn thêm mà chưa có khoá tương ứng (`_Static_assert`, K11).

## 7. Giá trị enum

- `axes[].state`: `UNKNOWN`, `NOT_READY_TO_SWITCH_ON`, `SWITCH_ON_DISABLED`, `READY_TO_SWITCH_ON`, `SWITCHED_ON`, `OPERATION_ENABLED`, `QUICK_STOP_ACTIVE`, `FAULT_REACTION_ACTIVE`, `FAULT`.
- `axes[].err` (lỗi trục của `libecm_cia402`, xem `docs/cia402.md`): `none`, `timeout`, `fault`, `bus_lost`, `mode`, `in_fault`, `dropped` (S1), `step` (S4), `shutdown` (S6), `handshake` (PP), `homing`, `config`, `slave_failed`.
- `findings[].type`: `chain_broken`, `missing_link_up`, `no_address`, `cable_fwd`, `cable_ret`, `fwd_no_origin`, `pu_only`, `pdi`, `lost_link`, `state`, `saturated`, `diag_lost`, `emcy`, `emcy_lost`, `recover_failed`, `recover_failed_config`.

## 8. Kiểm thử

| Lệnh | Nội dung |
|---|---|
| `make -C libecmaster/api test test_asan` | handoff, khoá enum, JSON (escape, tràn bộ đệm, cắt số 0), sysfs giả, `--api-cpus`, publish không rách (160 kiểm) |
| `make -C libecmaster/api negctl` | đối chứng âm: ghi tại chỗ (không `rename`) **phải** cho đọc rách |
| `make -C libecmaster/api tsan` | handoff dưới TSan; đối chứng âm `ECM_API_HO_BROKEN` (state publish relaxed) **phải** bị TSan báo. Test tự báo FAIL nếu consumer không chạy song song đủ 1000 lần get (tránh PASS do may mắn) |
| `make -C libecmaster/api schema` | tài liệu mẫu qua `tools/gd11/stats_check.py`; `selftest`: 16 tài liệu hỏng **phải** bị từ chối, khoá lạ **phải** được chấp nhận |
| `sudo -E tools/gd11/run_api_11_1.sh` | A-01…A-10 trên soft_bus (2 IS620N ảo, `is620n.cal`, `eni_2servo_mm`) |
| §10.5 | `ctl.sock` |

`stats_check.py compare FILE LOG` so file cuối với bản tổng kết của `ecm_run` (D-10): nhóm, WKC theo lớp, DC, hook, 4 histogram telemetry, trục CiA402, diag, policy.

## 9. Hạn chế đã biết (11.1, 11.2)

- Không đổi trạng thái bus, không điều khiển trục, không xoá `FAILED` qua API (kế hoạch §0.3).
- `ecm_mailbox` (GĐ5): nếu worker mailbox bị kẹt lâu hơn timeout + 500 ms, `ecm_mailbox_sdo_*` trả -2 (`timeout`) trong khi job vẫn nằm trên stack của thread gọi, và worker có thể ghi vào job đó về sau. Với API là caller duy nhất lúc chạy, worker luôn xong trước timeout (SOEM tự hết hạn), nên điều kiện này chỉ xảy ra khi worker chết. Hạn chế này có từ GĐ5 và chưa sửa; ghi để không quên.
- `stats.json` 10 Hz. Cần nhanh hơn (ROS2) thì thêm shm và thư viện đọc C, không đổi schema.
- `crc_err` / `rx_missed` là bộ đếm của NIC, không phải của ESC. Lỗi phía slave nằm ở `slaves[].rx_err` / `inv`.
- `sources.*.mono_ns` không so được giữa hai máy.
- Chi phí trên RT thread chỉ đo trên Jetson (D-01, STRICT=1). Số trong sandbox chỉ chứng minh chức năng.

## 10. `ctl.sock` (11.2)

### 10.1 Giao thức
- `AF_UNIX SOCK_STREAM` tại `<api-dir>/ctl.sock`, quyền `0660`. Mỗi request là **một object JSON trên một dòng** (≤ 1023 byte, kết thúc bằng `\n`), và nhận **đúng một dòng reply**. Một kết nối có thể gửi nhiều request liên tiếp; reply giữ đúng thứ tự.
- `"id"` (số, chuỗi hoặc `null`, ≤ 69 ký tự) được gửi lại nguyên văn trong reply.
- Reply thành công: `{"id":…,"ok":true,…}`. Reply lỗi: `{"id":…,"ok":false,"error":"<mã>","msg":"<giải thích>"}`. Client phân nhánh theo `error`; `msg` chỉ để đọc.
- Giới hạn:
  - Tối đa **4 client** cùng lúc. Client thứ 5 nhận `too_many_clients` rồi bị đóng.
  - Tổng cộng **20 lệnh/s** cho mọi client (token bucket); mỗi dòng đều tính, kể cả dòng sai. Vượt thì nhận `rate_limited`.
  - Dòng dài hơn 1023 byte nhận `line_too_long`, phần còn lại của dòng đó bị bỏ, và dòng sau vẫn được phục vụ.
  - Client không đọc reply tới khi bộ đệm 160 KB của nó đầy thì bị ngắt. Các client khác không bị ảnh hưởng.
- Client mẫu: `tools/gd11/ecm_ctl.py` (`info`, `read`, `write`, `dump`, `raw`, `spam`, `hold`).

### 10.2 Lệnh

| `cmd` | Tham số | Reply |
|---|---|---|
| `info` | — | `info`: *(11.3)* `rt` (cùng các khoá RT như `stats.json`), `ecm_run` (version = `git describe`, pid), `schema`, `cycle_ns`, `link`, `ports[]`, `path`, `dc_ref`, `slaves[]` (pos, vendor, product, revision, name, group, mailbox, coe), `eni` (path, source, **sha256** của file `.enicfg`, số CoE InitCmd / download / register InitCmd / PDO entry, cycle) hoặc `null`, `pdo` (source `eni`/`bus`/`none`, `entries[]`: slave, dir `out`/`in`, pdo, index, sub, bits), `axes[]`, `api` (dir, cpus, sdo_timeout_ms, dumps), `ctl` (giới hạn và bộ đếm lệnh), `allow` (path, entries) |
| `sdo_read` | `slave`, `index`, `sub`, `type` (mặc định `raw`) | `size`, `raw` (hex), `value` (khi `type` có kiểu số và kích thước khớp), `ms` |
| `sdo_write` | `slave`, `index`, `sub`, `type`, `value` | `written`, `raw`, `readback` (đọc lại ngay sau khi ghi), `readback_ok`, `ms` |
| `diag_dump` | — | `files`: `diag_<thời gian>.txt` (snapshot chẩn đoán: findings, bộ đếm, EMCY, trục) và `stats_<thời gian>.json` (bản sao `stats.json`), nằm trong `<api-dir>`. Tối đa 32 lần mỗi run (tmpfs) |

- `index`, `sub`, `slave` nhận số hoặc chuỗi `"0x6061"`. Không nhận số bát phân (`"010"`).
- `type`: `u8 i8 u16 i16 u32 i32 u64 i64 f32 raw`. Mọi giá trị là little-endian như CoE.
- **Không có Complete Access** qua API: có khoá `ca` là `bad_request`.

Mã lỗi:

| Mã | Khi nào |
|---|---|
| `bad_request` | không phải JSON, thiếu hoặc sai tham số, `cmd` thiếu, `id` sai |
| `unknown_cmd` | `cmd` không thuộc 4 lệnh trên |
| `rate_limited`, `too_many_clients`, `line_too_long` | §10.1 |
| `no_slave` | không có slave đó |
| `not_ready` | bus không ở RUN/DEGRADED, đang có recovery, `ecm_run` đang dừng, hoặc slave không có mailbox |
| `busy` | hàng mailbox đầy, hoặc recovery vừa bắt đầu |
| `timeout` | SDO không xong trong timeout |
| `sdo_failed` | slave abort (kèm `abort` = mã 32 bit và `msg` = mô tả của SOEM), hoặc không trả lời (`abort: null`) |
| `size_mismatch` | `type` không khớp kích thước object (reply kèm `raw`) |
| `too_large` | > 256 byte |
| `denied_comm_area`, `denied_hard`, `denied_rxpdo`, `denied_no_pdo_table`, `denied_initcmd`, `not_allowed`, `type_mismatch`, `out_of_range` | `sdo_write` bị từ chối (§10.4) |
| `dump_failed` | không ghi được file, hoặc đã đủ 32 lần |

### 10.3 Thread và an toàn

```
client ──► ctl.sock ──► thread api (core 1-2) ──► ecm_mailbox_sdo_read/write (blocking)
                                                    └► mailbox worker ──► hàng mailbox cyclic của SOEM ──► RT pump (1 frame/chu kỳ, đường L4-03)
```

- Lệnh được xử lý ở các khe giữa hai lần publish `stats.json`: tối đa 4 lệnh mỗi lượt, mỗi client 1 lệnh. Một SDO làm lượt publish đó trễ tối đa bằng `--api-sdo-timeout-ms`. Client theo dõi stale nên đặt ngưỡng lớn hơn timeout này (D-04: 2 s).
- **Fence với recovery.** Recovery của monitor (path B, RECOVER) chuyển mailbox của slave sang chế độ polled và gửi thẳng lên socket. Monitor bật cờ chặn rồi lấy một mutex mà thread `api` giữ suốt mỗi SDO, nên monitor chờ SDO đang chạy kết thúc (tối đa bằng timeout). Trong lúc recovery, API trả `not_ready` hoặc `busy`.
  - Khi bus ở RECOVER, RT không bơm mailbox. Nếu đúng lúc đó có một SDO đang chạy, SDO ấy hết timeout rồi recovery mới tiếp tục: recovery trễ thêm tối đa 500 ms.
  - `[API] … recovery fence waits=… max=…` ghi lại số lần chờ và thời gian chờ dài nhất.
- **Mã abort.** SOEM đưa abort vào error list; RT chuyển nó sang monitor; monitor lưu abort gần nhất (slave, index, sub, mã). Thread `api` chờ tối đa 50 ms để ghép abort đó vào reply.
- Khi vòng cyclic kết thúc, mọi SDO mới nhận `not_ready` ("ecm_run is stopping"). Socket bị xoá khi `ecm_run` thoát bình thường.

### 10.4 `sdo_write`: thứ tự kiểm

Mỗi bước dưới đây từ chối ngay, **trước khi có bất kỳ frame nào được gửi**:

1. `0x1000…0x1FFF` → `denied_comm_area`. Gồm PDO mapping và assignment, sync manager, 0x1C32/0x1C33. Rộng hơn danh sách 0x1C12/0x1C13/0x1600–0x1BFF của kế hoạch, có chủ đích.
2. Danh sách cứng của ứng dụng (`ecm_run`): controlword `0x6040` và mode of operation `0x6060` của mọi trục (+0x800·n, n = 0…7) → `denied_hard`. Danh sách này nằm ở `ecm_run` chứ không ở `libecmaster` (K-05).
3. Object **đang được map trong RxPDO** của chính slave đó, theo bảng PDO đang dùng (ENI hoặc `--pdo-scan`; mọi sub của index đó) → `denied_rxpdo`. Không có bảng PDO (không `--eni`, không `--pdo-scan`) → `denied_no_pdo_table`, tức là không cho ghi gì.
4. Object mà ENI ghi bằng **CoE InitCmd** cho slave đó (ví dụ giới hạn 0x6072/0x6065/0x607F của 10.9) → `denied_initcmd`.
5. Không có trong allowlist (theo vendor + product của slave) → `not_allowed`. `type` khác với allowlist → `type_mismatch`.
6. Ngoài `[min, max]` của mục allowlist, hoặc ngoài kiểu dữ liệu → `out_of_range`.
7. Bus chưa sẵn sàng → `not_ready`.

Allowlist (`--api-allow FILE`, JSON):
```json
{ "schema": 1,
  "allow": [ { "vendor": "0x00100000", "product": "0x000C0108", "index": "0x2008", "sub": 0,
               "type": "u16", "min": 0, "max": 1000, "note": "..." } ] }
```
- Mỗi mục bắt buộc có `vendor`, `product`, `index`, `sub`, `type` (không được là `raw`), `min` ≤ `max` trong phạm vi kiểu. Không trùng mục; tối đa 64 mục.
- Một mục mà bước 1–2 chắc chắn từ chối thì làm `ecm_run` **không khởi động**, vì file không được hứa điều master không bao giờ làm.
- Bước 3–4 chỉ biết được khi bus đã lên, nên được kiểm lúc ghi. Đó là lý do mục `0x607F` trong runner được chấp nhận khi nạp file nhưng bị `denied_rxpdo` khi ghi.

### 10.5 Kiểm thử 11.2
| Lệnh | Nội dung |
|---|---|
| `make -C libecmaster/api test test_asan` | `test_ctl_offline` (132): tokenizer JSON (cú pháp, độ sâu, số nguyên, escape), SHA-256 (vector FIPS, 1 triệu `a`), mã hoá kiểu, allowlist (1 file đúng, 13 file sai), mọi nhánh từ chối của `sdo_write` với backend giả (và đếm rằng không lần ghi nào tới slave), rate limit, server trên socket thật (client thứ 5, hai request trong một lần ghi, dòng quá dài, client không đọc, socket còn sống / socket cũ / file thường) |
| `make -C libecmaster/api negctl` | `ECM_CTL_BROKEN` = 1 (bỏ kiểm RxPDO), 2 (bỏ rate limit), 4 (bỏ vùng 0x1000 và danh sách cứng), 8 (bỏ kiểm min/max), 16 (bỏ kiểm InitCmd): mỗi bản **phải** làm test tương ứng FAIL |
| `sudo -E tools/gd11/run_ctl_11_2.sh` | C-01…C-09 trên soft_bus (§ đầu file runner) |

## 11. `ecm_diag` qua API (11.3)

`ecm_diag` là client mẫu của API. Chế độ cũ (đọc snapshot text `/tmp/ecm_diag.txt`, `--standalone`) giữ nguyên, `run_l5_diag.sh` vẫn 10/10.

| Lệnh | Nguồn | Exit |
|---|---|---|
| `ecm_diag --api [DIR]` (mặc định `/run/ecmaster`) | `stats.json` | 0 khoẻ; **1** khi có finding `error`, bus `LOST`/`RECOVER`, trục ở `FAULT`/`FAULT_REACTION_ACTIVE` hoặc có lỗi trục; **2** khi không có file, file `final` (ecm_run đã dừng) hoặc cũ hơn `--max-age` (mặc định 3 s, so `mono_ns` với đồng hồ của chính máy đó) |
| `ecm_diag --api [DIR] --watch [SEC]` | như trên, làm mới | — |
| `ecm_diag [--api DIR] info` | `ctl.sock` | 0 ok, 1 bị từ chối/lỗi, 2 không có socket, 4 reply hỏng |
| `ecm_diag [--api DIR] sdo read SLAVE INDEX SUB [TYPE]` | `ctl.sock` | như trên |
| `ecm_diag [--api DIR] sdo write SLAVE INDEX SUB TYPE VALUE` | `ctl.sock` (allowlist) | như trên; đọc lại khác giá trị đã ghi → 1 |
| `ecm_diag [--api DIR] dump` | `ctl.sock` (`diag_dump`) | như trên |
| `--json` (đặt trước lệnh) | in nguyên JSON thay vì bảng | — |

- Tuỳ chọn đặt trước lệnh: `ecm_diag --api /dev/shm/ecmaster --json sdo read 1 0x6061 0 i8`.
- Màn hình `--api` gồm:
  - một dòng nguồn (pid, `pub_seq`, tuổi, `FINAL`/`STALE`);
  - bus và các cổng;
  - dòng `rt:` (core, isolated, cpuidle, kernel, các cờ cmdline, `cpu_dma_latency`, hook, CPU của API);
  - các nhóm, DC, link;
  - bảng timing (p50/p99/p99.9/p99.99/max, đơn vị µs);
  - slave (AL, bộ đếm theo cổng, recovery, EMCY);
  - trục (state, mode, statusword, lỗi trục, mã lỗi và nguồn của nó: 0x603F hay EMCY);
  - findings.
- **`--standalone` giờ cũng từ chối (exit 3) khi API của `ecm_run` còn sống**: `ctl.sock` trả lời được, hoặc `stats.json` mới và chưa `final`. Lý do: `ecm_run --no-diag` không ghi snapshot text, nên kiểm tra của GĐ7 không thấy nó. `--force` vẫn bỏ qua được như trước.
- Kiểm thử: `tools/gd11/run_diag_11_3.sh` (G-01…G-07); `rt.*` trong `run_api_11_1.sh` A-11.
