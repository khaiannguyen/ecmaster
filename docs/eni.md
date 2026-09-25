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

## 5. Thiết kế đề xuất cho `ecm_run --eni`

Không vá SOEM. Dùng lại cấu trúc sẵn có của SOEM và hook PO2SO đã có từ GĐ7.3.

```
TwinCAT ENI (.xml)
   │  tools/eni/eni2cfg.py   (reuse eniconv.py's parsing rules, extended)
   ▼
config/eni/<name>.enicfg     (simple line-based text, versioned in git)
   │  libecmaster/config/ecm_eni.c   (loader, non-RT, at startup)
   ▼
ecm_eni_t  { all slaves: pos, vendor, product, rev, Isize/Osize from ProcessData;
             CoE InitCmds with full transition mask }
```

- **Không gán `context->ENI`.** SOEM bỏ qua đường ENI của nó; `libecmaster` tự chạy InitCmds:
  - IP (INIT→PREOP): chạy sau khi slave lên PREOP, trước `ecx_config_map_group`.
  - PS: chạy trong hook PO2SO (GĐ7.3 đã có hook PO2SO khôi phục watchdog + DC; thêm vào đó).
  - **Mọi SDO thất bại → hủy cấu hình, không lên SAFEOP.**
- **Kiểm danh tính trước khi cấu hình (E-03):** so `slavecount` sau khi scan và (vendor, product, rev) theo **từng vị trí** với danh sách **đầy đủ** slave của ENI. Lệch bất kỳ → từ chối, báo rõ vị trí và giá trị kỳ vọng/thực tế.
- **Kiểm layout sau map (thay E-02):** Isize/Osize từng slave sau `config_map_group` phải bằng giá trị tính từ `ProcessData` của ENI. Lệch → từ chối OP. Đây là chỗ bắt được lỗi PDO assign bị bỏ qua (mục 3.3).
- `eni2cfg.py` đọc thêm so với `eniconv.py`: danh sách đầy đủ slave (kể cả không có CoE InitCmd), `ProcessData` → kích thước I/O. Mục cyclic/DC/register InitCmds: bỏ qua có chủ đích (SOEM + `libecmaster` tự làm), ghi rõ trong README.

## 6. Test E-series (bản sửa)

| ID | Hành động | Kỳ vọng |
|---|---|---|
| E-01 | ENI 1 node 1 PDO, có InitCmd ghi 0x1C12/0x1C13 | Lên OP; tshark thấy đúng SDO download ở PS |
| E-02 | Isize/Osize sau map so với ENI | Khớp; đối chứng âm: `soft_bus` từ chối SDO 0x1C12 → **không** lên SAFEOP (SOEM thuần sẽ lên OP) |
| E-03 | ENI N=8, bus 7 node / sai product / sai revision | Từ chối, báo đúng vị trí |
| E-04 | InitCmd trỏ tới object không tồn tại (abort) | Hủy cấu hình, log abort code |
| E-05 | Golden cho chế độ ENI | Thêm vào CI, có đối chứng âm |

Đối chứng âm cho mục 3.2/3.3 (chứng minh lý do không dùng đường ENI của SOEM): build `eni_test` với cùng ENI, cho `soft_bus` trả Abort ở 0x1C12 → ghi lại việc SOEM vẫn tiếp tục. Chạy trên cặp veth riêng, không chạy lúc đang soak.
