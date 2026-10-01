# soft_bus giả lập slave khác hãng từ ESI — GĐ9.9

`soft_bus --profile N=FILE`: node N (số slave SOEM, đếm từ 1) đóng vai slave mô tả trong `FILE`. Nhờ vậy dựng được **bus hỗn hợp ảo**: P1 (ESI nháp của mình) + servo IS620N, chạy `ecm_run --eni eni_mixed.enicfg` như với bus thật.

## Profile
Sinh từ ESI bằng `tools/esi/esi2profile.py` (định dạng dòng, đọc được bằng mắt):

| Bản ghi | Nội dung (lấy từ ESI) |
|---|---|
| `identity` | Vendor Id, ProductCode, RevisionNo |
| `config` | `Eeprom/ConfigData` word 0..6. soft_bus tự tính CRC word 7 |
| `mailbox`, `coe` | SM MBoxOut/MBoxIn (địa chỉ, cỡ); cờ CoE SdoInfo / PdoAssign / PdoConfig / CompleteAccess |
| `sm 0..3` | start, cỡ, control byte, enable. SM process data có `DefaultSize 0` (IS620N) lấy cỡ của các PDO gán mặc định |
| `dc` | có `Dc/OpMode` → có DC; AssignActivate. `dc 0` → node không có DC (như `--no-dc-nodes`) |
| `pdo` | **mọi** RxPdo/TxPdo: index, SM mặc định (255 = không gán), Fixed, entry, danh sách Exclude |
| `obj` / `sub` | object dictionary (`Profile/Dictionary`): từng subindex với cỡ bit, quyền (`ro` / `rw` / `rw_preop`), giá trị mặc định (hex little endian). RECORD/ARRAY được bung từ DataTypes |

Quy tắc của esi2profile:
- 0x1C12/0x1C13 là `rw_preop` (ETG.1020), giá trị đầu = các PDO có thuộc tính `Sm` trong ESI, **không** lấy DefaultData của dictionary (IS620N để 0).
- Object mapping có DefaultData khác mô tả PDO → lấy theo mô tả PDO (cái TwinCAT và SII dùng), in cảnh báo. ESI IS620N v2.6.9 có 3 chỗ như vậy: 0x1702 (0x60B8 hai lần, thiếu 0x60FF), 0x1B03 (SI0 = 0x10 thay vì 10), 0x1B04 (thứ tự entry khác).
- ESI không có 0x1C00 / 0x1C12 / 0x1C13 / object mapping (ESI nháp P1) → dựng từ mô tả PDO, ghi chú ở cuối file.

## Node profile làm gì
- **SII**: identity, config, mailbox, category Strings (tên thiết bị → tên slave trong SOEM/IgH), General (CoE details), SyncM, TxPDO/RxPDO đủ mọi PDO. Kiểm ngược bằng `esi_check.py ESI sii.bin` (`sii_dump --profile FILE > sii.bin`).
- **CoE**: dictionary của ESI, đọc/ghi SDO thường và segmented, Complete Access khi ESI cho phép. Lỗi: object/sub không có (0x06020000 / 0x06090011), ghi `ro` (0x06010002), `rw_preop` ngoài PREOP (0x08000022), sai cỡ (0x06070012/13).
- **0x1C12/0x1C13**: chỉ nhận PDO đúng hướng có trong ESI (0x06090030); đổi SIk khi SI0 ≠ 0 → 0x08000022 (ETG.1020); SI0 kích hoạt một tập vi phạm Exclude hoặc lặp PDO → 0x06090030. Ghi CA thì kiểm cả tập mới một lần.
- **PREOP → SAFEOP**: cỡ SM2/SM3 master ghi phải bằng tổng bit các PDO đang gán (đọc từ object mapping hiện tại), nếu không → PREOP+ERR **0x001D** (output) / **0x001E** (input), như servo thật khi cấu hình PDO lệch.
- Tắt/bật nguồn (`drop_node` / `restore_node`) → node vẫn là slave đó, dictionary về mặc định ESI.
- **Không** mô phỏng: hành vi CiA402, chuyển động. Input chỉ là cái `pdo_in` đặt vào (GĐ10 làm servo ảo).

## ESI hãng và giấy phép
ESI của Inovance chỉ để dùng cục bộ (roadmap GĐ9, ghi chú 30/9): **không** commit ESI, **không** commit profile sinh từ nó (`config/profiles/is620n.prof` nằm trong `.gitignore`). Repo có `config/profiles/is620n_min.prof`: bản rút gọn viết tay, chỉ identity, SM, 4 PDO test dùng (0x1701/0x1702/0x1B01/0x1B02) và các object cần cho ENI. Máy có ESI (Jetson):
```bash
python3 tools/esi/esi2profile.py ~/esi/IS620N-Ecat_v2.6.9.xml -o config/profiles/is620n.prof
IS620N_ESI=~/esi/IS620N-Ecat_v2.6.9.xml sudo -E tools/gd9/run_mixed_9_9.sh
```

## Ví dụ
```bash
./tools/soft_bus/soft_bus --iface veth_s --n 2 --dc 32 \
    --profile 1=config/profiles/p1_draft.prof --profile 2=config/profiles/is620n_min.prof
./apps/ecm_run/ecm_run --iface veth_m --n 2 --eni config/eni/eni_mixed.enicfg --pdo-scan --pdo-dump
```

## Test (`tools/gd9/run_mixed_9_9.sh`, `tools/soft_bus/test_profile`)
| ID | Nội dung |
|---|---|
| V-01 | test_profile (offline); esi2profile tái tạo đúng `p1_draft.prof`; `esi_check` ESI ↔ SII của profile (IS620N khi có `IS620N_ESI`) |
| V-02 | `eni_mixed` trên bus P1 + IS620N → OP, 9 InitCmd CoE ok, bảng PDO ENI == bus (27 entry), DC ref = P1 |
| V-03 | `eni_mixed_rev` trên bus đảo → OP; DC-02: IS620N làm reference clock; `eni_mixed` trên bus đảo → từ chối ở identity |
| B-02m | bind trên bus hỗn hợp: ghi 0x607A (IS620N), 0x6040 (P1), đọc 0x6041 do soft_bus đặt |
| N-01 | master map IS620N theo SII (profile bỏ 0x1C00), ENI gán 0x1B02 qua CoE → slave từ chối SAFE-OP 0x001E, ecm_run báo lỗi cấu hình, không thử lại; đối chứng giữ 0x1B01 → OP |
| N-02 | ENI gán 0x1A00 hai lần cho P1 bằng CA → abort 0x06090030, từ chối SAFE-OP |
| V-04 | IgH `ethercat slaves`/`pdos` (tùy chọn, `IGH=1`) |
