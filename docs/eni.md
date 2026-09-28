# ENI support in SOEM v2.0.0 — findings (GĐ8 §5.1)

**Date:** 2026-09-25 · **SOEM:** v2.0.0 + `patches/soem-mbx-cnt.patch`
**Sources read:** `cmake/AddENI.cmake`, `scripts/eniconv.py`, `include/soem/ec_main.h` (ENI structs),
`src/ec_main.c` `ecx_mbxENIinitcmds()`, `src/ec_config.c` (2 call sites).

Kết luận ở đây **thay thế** giả định trong `giai_doan_8_ke_hoach.md` §5.

---

## 1. Cơ chế

- **Build-time, không phải runtime.** `add_eni(<target> <eni.xml>)` gọi `scripts/eniconv.py`, sinh ra một file `.c` định nghĩa `ec_enit ec_eni`, biên dịch chung vào ứng dụng. Ứng dụng gán `context->ENI = &ec_eni`. SOEM không có parser XML nào chạy lúc runtime.
- **ENI chỉ là lớp phủ CoE.** SOEM vẫn online scan, vẫn tự dựng SM/FMMU/IOmap như bình thường. ENI chỉ thêm SDO InitCmds.

## 2. `eniconv.py` đọc gì

| Phần của ENI | Được đọc? |
|---|---|
| `Config/Slave/Info/{VendorId, ProductCode, RevisionNo}` | Có |
| `Config/Slave/Info/AutoIncAddr` → vị trí (`Slave = 1 − AutoIncAddr`, mod 16 bit) | Có (mặc định: thứ tự trong file) |
| `Slave/Mailbox/CoE/InitCmds/InitCmd` (Transition, Ccs, Index, SubIndex, CompleteAccess, Timeout ms→µs, Data hex; bỏ qua `Disabled=1`) | Có |
| `Slave/InitCmds` (ghi thanh ghi: station address, SM, FMMU, AL control…) | **Không** |
| `ProcessData` (Sm, RxPdo/TxPdo, offset trong process image) | **Không** |
| `Config/Cyclic` (frame, datagram, task, chu kỳ) | **Không** |
| DC (`Slave/DC`, SYNC0 cycle/shift) | **Không** |
| Mailbox SoE/FoE/EoE | **Không** |

## 3. Hành vi runtime cần lưu ý (chỗ nguy hiểm)

1. **`ec_eni.slavecount` ≠ số slave của mạng.** Script chỉ đưa vào mảng các slave **có ít nhất một** CoE InitCmd (`noSlaves = sum(n > 0 ...)`). Vì vậy từ `ec_enit` không thể biết ENI mô tả bao nhiêu slave, và không kiểm được topology.
2. **Lệch danh tính → bỏ qua âm thầm.** `ecx_mbxENIinitcmds` chỉ áp lệnh nếu Vendor, Product **và Revision** khớp SII. Không khớp thì không áp, không báo lỗi, quá trình cấu hình vẫn tiếp tục.
3. **Lỗi SDO bị nuốt.** Hàm trả 0 khi SDO thất bại, nhưng cả hai chỗ gọi trong `ec_config.c` đều ép `(void)`. Hậu quả: nếu ghi PDO assign 0x1C12/0x1C13 thất bại, SOEM vẫn map theo SII/mặc định và **có thể lên OP với layout PDO sai**. Với hệ điều khiển động cơ, đây là lỗi an toàn.
4. **Chỉ gọi ở transition PS** (PREOP→SAFEOP). Lệnh gắn transition khác (IP, SO, …) được biên dịch vào nhưng không bao giờ chạy trên đường cấu hình chuẩn. Lưu ý thêm: nếu ENI có transition mà không có macro `ECT_ESMTRANS_<X>` tương ứng thì file `.c` sinh ra sẽ lỗi biên dịch.
5. **Revision phải khớp tuyệt đối.** ESI dùng cho TwinCAT (GĐ8 §3) phải ghi đúng `RevisionNo` như trong ảnh SII của `soft_bus`, và sau này đúng như SII của LAN9252. Lệch revision chỉ làm InitCmds âm thầm không chạy, không có lỗi nào hiện ra.

## 4. Hệ quả cho "master là hằng số"

Dùng `add_eni` nghĩa là **mỗi platform phải build lại `ecm_run`** với ENI của nó. Code không đổi, nhưng binary thì khác, và tag `master-v1.0` không còn là một binary duy nhất. Không nhận cách này.

## 5. Thiết kế đã hiện thực (GĐ8 8.4, 28/9)

Không vá SOEM, không gán `context->ENI`.

```
TwinCAT ENI (.xml)
   │  tools/eni/eni2cfg.py        (build/offline, Python)
   ▼
config/eni/<name>.enicfg          (line-based text, commit cùng ENI; có sha256 của ENI nguồn)
   │  libecmaster/config/ecm_eni.c        (loader + kiểm tra, không phụ thuộc SOEM)
   │  libecmaster/config/ecm_eni_soem.c   (glue SOEM)
   ▼
ecm_run --eni config/eni/<name>.enicfg
```

**eni2cfg.py đọc:** mọi `Config/Slave` (kể cả slave không có CoE InitCmd) → vị trí (từ AutoIncAddr), vendor/product/revision, `check_rev` (có InitCmd "check revision number"), địa chỉ trạm, `ProcessData/{Send,Recv}/BitLength` (đối chiếu với tổng BitLen các PDO có `Sm`), khối `DC` (ReferenceClock, CycleTime0/1, ShiftTime) và AssignActivate (lấy từ InitCmd PS ghi 0x0980 — khối `DC` của ENI TwinCAT không chứa nó), `Mailbox/CoE/InitCmds` (bỏ `Disabled`), `Cyclic/CycleTime`. Lỗi nhất quán (vị trí không liên tục, địa chỉ trùng, >1 ref clock, BitLength ≠ tổng PDO) → exit 1.
**Bỏ qua có chủ đích:** register InitCmds (SM/FMMU/AL/địa chỉ/DC do SOEM + libecmaster làm), địa chỉ logic và offset process image (SOEM tự dựng IOmap), khung cyclic.

**Thứ tự trong ecm_run (chế độ ENI):**
1. Nạp `.enicfg`; `--n` không nhập → lấy từ ENI, nhập khác → lỗi. CoE InitCmd có transition ngoài IP/PS hoặc CompleteAccess → từ chối.
2. `ecx_config_init` → **kiểm danh tính (E-03)**: số slave + vendor/product từng vị trí, revision nếu `check_rev`. Lệch → dừng, in từng vị trí (kỳ vọng / thực tế). Không còn "tiếp tục với số slave tìm thấy".
3. Chờ PRE-OP → chạy CoE InitCmd **IP**.
4. Gắn hook `PO2SOconfig` → CoE InitCmd **PS** chạy bên trong `ecx_config_map_group`, trước khi SOEM đọc PDO assign (cùng điểm SOEM gọi `ecx_mbxENIinitcmds`). SOEM bỏ qua giá trị trả về của hook → lỗi được đếm, kiểm ngay sau map: ≥1 lỗi → dừng trước SAFE-OP, log abort code (`ecx_elist2string`).
5. **Kiểm layout**: `Obits/Ibits` từng slave = ENI.
6. DC: ENI không có DC slave → như `--no-dc`. Có DC: ref clock phải là slave 1, `CycleTime0` = motion cycle, AssignActivate 0x0300, `CycleTime1` = 0 — không thì từ chối. **SYNC0 bật theo cờ DC của ENI** (có thể gồm slave GROUP_IO; SYNC0 độc lập chu kỳ frame), shift = `ShiftTime`. Đo trễ/offset/start time vẫn là `ecx_configdc` + DC(a) GĐ6 (ENI TwinCAT để 0x0990 = 0 cho runtime).
7. Recovery GĐ7.3 (`reconfig_po2so_hook`, `dc_restore_slave`): chạy lại PS InitCmds và khôi phục SYNC0 theo cùng quy tắc ENI.

Không `--eni`: hành vi không đổi (golden 547/547).

**Địa chỉ trạm:** TwinCAT ghi `PhysAddr` thập phân (1001 = 0x03E9), SOEM đặt 0x1001… `ecm_run` giữ địa chỉ SOEM; địa chỉ ENI chỉ để tham khảo khi so pcap.

## 6. Test E-series (kết quả 28/9, soft_bus 8 node, `sudo chrt -f 79 taskset -c 2`)

| ID | Hành động | Kết quả |
|---|---|---|
| E-01 | `eni_8node_dc_sdo` | OP; `CoE download 0x8000:01 ok`; 8/8 SYNC0; A/B 30 s với/không ENI giống hệt (motion 30001/0 noframe, io 3751/0) |
| E-02 | Layout Obits/Ibits so ENI | Đạt trong E-01 (kiểm tự động mỗi lần chạy); layout logic TwinCAT (in/out chồng, LRW WKC 3/slave) khác SOEM (in sau out) — có chủ đích |
| E-03a | Bus 7 node, ENI 8 | Từ chối: `slave count: ENI 8, bus 7` |
| E-03b | `soft_bus --sii-poke 0x0A=0x0002` | Từ chối, 8 vị trí `found 0x499 0x2 0x1` |
| E-03c | `soft_bus --sii-poke 0x0C=0x0002` | Từ chối, lệch revision (ENI `check_rev 1`) |
| E-03d | ENI không kiểm revision | Chỉ kiểm offline (`test_eni_offline`); `ecm_run` yêu cầu motion_slaves < n nên không chạy bus 1 node |
| E-04 | CoE InitCmd tới 0x8002 (`.enicfg` sửa tay) | `FAILED … 06020000` → `refusing SAFE-OP` (SOEM thuần sẽ bỏ qua) |
| E-05 | Golden chế độ ENI | xem `tools/golden/` |
