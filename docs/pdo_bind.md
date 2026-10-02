# Bind PDO theo (slave, index, sub) — GĐ9.10

SOEM dựng IOmap chỉ từ **kích thước** bit của từng slave, không giữ danh sách entry. Để biết "0x6041:00 của slave 2 nằm ở đâu", `libecmaster/pdo` tự dựng bảng entry. `libecmaster` chỉ tra bảng, không hiểu nghĩa object (nghĩa thuộc lớp CiA402, GĐ10).

## Nguồn bảng
| Nguồn | Cách lấy | Khi nào |
|---|---|---|
| ENI | `.enicfg` bản ghi `pdo <pos> dir out\|in pdo 0x.... index 0x.... sub 0x.. bits N` (eni2cfg). Thứ tự PDO theo InitCmd 0x1C12/0x1C13 của ENI (CA hoặc chuỗi SI0/SIk); nếu khác tập PDO gán vào SM trong ENI → eni2cfg từ chối. Loader kiểm tổng bit = kích thước slave | `--eni` (enicfg 2) |
| Scan | CoE: 0x1C12/0x1C13 → 0x16xx/0x1Axx (SDO thường, ở PREOP, trước khi bật cyclic mailbox). Không có CoE hoặc 0x1C12 không đọc được → SII category 51/50, duyệt đúng như `ecx_siiPDO()` (chỉ PDO có SM hợp lệ) | `--pdo-scan` (tùy chọn, vì thêm traffic cấu hình; golden không đổi) |

Có cả hai thì phải khớp từng entry, nếu không thì từ chối (ENI không đúng với thiết bị trên bus). Bảng đang dùng phải khớp `Obits/Ibits` mà SOEM map.

**GĐ10.1 — bind vào slave khác hãng cần `--pdo-scan`.** Bài học N-03: mapping lệch ESI nhưng cùng kích thước thì mọi kiểm tra kích thước đều qua, và bảng chỉ từ ENI sẽ bind sai byte mà không báo. Vì vậy khi bảng đang dùng **không** phải bảng scan từ bus, mọi slave được bind có vendor id (SII) không thuộc danh sách "nhà mình" sẽ bị từ chối trước SAFE-OP, kèm tên slave và vendor:
- danh sách nhà mình mặc định `0x00000499` (soft_bus, ESI nháp P1), đổi bằng `--pdo-own-vendor 0xV[,0xV...]`;
- `--pdo-trust-eni`: vẫn bind theo ENI, in `WARNING` nêu slave (dùng có chủ đích, ví dụ đã kiểm mapping bằng cách khác);
- hàm thuần `ecm_pdo_scan_required()` (`ecm_pdo.h`) — lớp CiA402 (GĐ10) gọi lại đúng quy tắc này khi bind trục.

## API (`ecm_pdo.h`)
- `ecm_pdo_bind(table, loc, slave, index, sub, &h)`: trả về handle gồm group, hướng, bit tuyệt đối trong IOmap của group, độ dài. Object không được map → lỗi, kèm danh sách những gì slave có map. Padding (index 0) không bind được.
- `ecm_pdo_get/set(h, iomap)`: little endian, 1..64 bit, mọi bit offset (kể cả không căn byte), không syscall.
- `loc` lấy từ SOEM (`ecm_pdo_locate`): `(sl->outputs - IOmap_group) * 8 + Ostartbit`.

## ecm_run
`--pdo-scan`, `--pdo-dump`, `--pdo-set S:IDX:SUB=VAL,...` (RT thread ghi mỗi tick, vì RT là chủ IOmap), `--pdo-get S:IDX:SUB,...` (in khi thoát), `--pdo-own-vendor`, `--pdo-trust-eni` (GĐ10.1). Đây là công cụ kiểm tra. Buffer trao đổi giữa thread ứng dụng và RT thread: thiết kế ở `docs/app_rt_exchange.md` (GĐ10.1), code ở 10.4.

## Test
`libecmaster/pdo/test_pdo_offline` (B-03/B-04 offline), `test_eni_offline` (bản ghi pdo), `tools/gd9/run_pdo_9_10.sh` (B-01 ENI = scan SII/CoE, B-01c SII = CoE, B-01n âm, B-02 ghi/đọc thật qua `soft_bus pdo_out/pdo_in`, B-02b entry 64 bit, B-03 âm). B-02 trên bus hỗn hợp ảo (P1 + IS620N) làm cùng 9.9. GĐ10.1: B-05 offline (`test_pdo_offline`), B-05a..d và N-03c trong `run_mixed_9_9.sh`.
