# Bind PDO theo (slave, index, sub) — GĐ9.10

SOEM dựng IOmap chỉ từ **kích thước** bit của từng slave, không giữ danh sách entry. Để biết "0x6041:00 của slave 2 nằm ở đâu", `libecmaster/pdo` tự dựng bảng entry. `libecmaster` chỉ tra bảng, không hiểu nghĩa object (nghĩa thuộc lớp CiA402, GĐ10).

## Nguồn bảng
| Nguồn | Cách lấy | Khi nào |
|---|---|---|
| ENI | `.enicfg` bản ghi `pdo <pos> dir out\|in pdo 0x.... index 0x.... sub 0x.. bits N` (eni2cfg). Thứ tự PDO theo InitCmd 0x1C12/0x1C13 của ENI (CA hoặc chuỗi SI0/SIk); nếu khác tập PDO gán vào SM trong ENI → eni2cfg từ chối. Loader kiểm tổng bit = kích thước slave | `--eni` (enicfg 2) |
| Scan | CoE: 0x1C12/0x1C13 → 0x16xx/0x1Axx (SDO thường, ở PREOP, trước khi bật cyclic mailbox). Không có CoE hoặc 0x1C12 không đọc được → SII category 51/50, duyệt đúng như `ecx_siiPDO()` (chỉ PDO có SM hợp lệ) | `--pdo-scan` (tùy chọn, vì thêm traffic cấu hình; golden không đổi) |

Có cả hai thì phải khớp từng entry, nếu không thì từ chối (ENI không đúng với thiết bị trên bus). Bảng đang dùng phải khớp `Obits/Ibits` mà SOEM map.

## API (`ecm_pdo.h`)
- `ecm_pdo_bind(table, loc, slave, index, sub, &h)`: trả về handle gồm group, hướng, bit tuyệt đối trong IOmap của group, độ dài. Object không được map → lỗi, kèm danh sách những gì slave có map. Padding (index 0) không bind được.
- `ecm_pdo_get/set(h, iomap)`: little endian, 1..64 bit, mọi bit offset (kể cả không căn byte), không syscall.
- `loc` lấy từ SOEM (`ecm_pdo_locate`): `(sl->outputs - IOmap_group) * 8 + Ostartbit`.

## ecm_run
`--pdo-scan`, `--pdo-dump`, `--pdo-set S:IDX:SUB=VAL,...` (RT thread ghi mỗi tick, vì RT là chủ IOmap), `--pdo-get S:IDX:SUB,...` (in khi thoát). Đây là công cụ kiểm tra. GĐ10 cần thêm buffer trao đổi giữa thread ứng dụng và RT thread (hiện ứng dụng không được ghi IOmap trực tiếp).

## Test
`libecmaster/pdo/test_pdo_offline` (B-03/B-04 offline), `test_eni_offline` (bản ghi pdo), `tools/gd9/run_pdo_9_10.sh` (B-01 ENI = scan SII/CoE, B-01c SII = CoE, B-01n âm, B-02 ghi/đọc thật qua `soft_bus pdo_out/pdo_in`, B-02b entry 64 bit, B-03 âm). B-02 trên bus hỗn hợp ảo (P1 + IS620N) làm cùng 9.9.
