# Buổi TwinCAT — bước 9.4 (GĐ9)

Một buổi trên laptop có TwinCAT 3 XAE (không cần license runtime, làm offline từ ESI). Kết quả là 4 file ENI mang về Jetson rồi chạy `tools/gd9/import_eni_9_4.sh`. Kế hoạch: `claude/giai_doan_9_ke_hoach.md` §6.

## 0. Chuẩn bị (trên Jetson, trước buổi)
Chép sang laptop:

| File | Dùng cho |
|---|---|
| `config/esi/softbus_esi_ca.xml` | ENI A (CA) |
| `config/esi/softbus_esi.xml` | ENI D (LRD/LWR) |
| `config/esi/p1_draft_esi.xml` | ENI B/C, Box P1 (nháp, CiA402) |
| ESI của servo ứng viên (file của hãng) | ENI B/C, Box servo |
| `config/eni/twincat/softbus_eni*` | project TwinCAT cũ (GĐ8), để mở lại nếu tiện |

Thư mục ESI của TwinCAT: `C:\TwinCAT\3.1\Config\Io\EtherCAT\`. `softbus_esi.xml` và `softbus_esi_ca.xml` **cùng identity** (0x499/0x1/0x1): mỗi lần chỉ để **một** file trong thư mục, rồi trong XAE: *TwinCAT → EtherCAT Devices → Reload Device Descriptions* (hoặc khởi động lại XAE).

Ghi cho mỗi ENI: ảnh chụp tab **Startup** của từng Box, tab **Process Data**, tab **DC**. Đây là tư liệu cho `docs/eni.md` (§5).

## A. `eni_8node_ca.xml` — 8 × SOFTBUS-PD4-CA (cho C-03/C-04)
1. Chỉ để `softbus_esi_ca.xml` trong thư mục ESI, reload.
2. Project mới → *I/O → Devices → Add New Item → EtherCAT Master* (không cần gắn adapter thật).
3. *Append Box* × 8: `SOFTBUS-PD4-CA` (group "soft_bus test slaves").
4. Mỗi Box:
   - **DC**: *DC-Synchron (SYNC0)*.
   - **Process Data**: đánh dấu *PDO Assignment* ở phần *Download* (để TwinCAT sinh Startup SDO cho 0x1C12/0x1C13).
   - **Startup**: kiểm có dòng `PS` cho `0x1C12` / `0x1C13`. Ghi lại: TwinCAT viết **một** lệnh Complete Access (cột Index có `C` / "Complete Access"), hay chuỗi SI0=0, SI1, SI0=n?
5. Task 1 ms (*SYSTEM → Tasks*), liên kết I/O nếu XAE đòi (có thể tạo biến mẫu hoặc để *Free Run*).
6. Xuất: chọn *Device 1 (EtherCAT)* → tab **EtherCAT** → **Export Configuration File…** → `eni_8node_ca.xml`.

Kỳ vọng sau khi import: `.enicfg` có các dòng `coe k trans PS ccs 1 index 0x1C12 sub 0x00 ca 1 ...`; C-03 lên OP với `soft_bus --coe-ca`, C-04 bị từ chối với `soft_bus` mặc định.

## B. `eni_mixed.xml` — Box 1 = P1 nháp, Box 2 = servo
1. Đặt `p1_draft_esi.xml` + ESI của servo vào thư mục ESI (bỏ hai file soft_bus ra cũng được), reload.
2. **Revision của servo** (§6.1 kế hoạch): ESI của hãng thường có nhiều `RevisionNo`. Chọn đúng revision của firmware servo sẽ mua (xem nhãn / tài liệu hãng; khi có servo thật: `ecm_diag --standalone` hoặc `slaveinfo`, 0x1018:03). Ghi revision đã chọn.
3. Project mới, EtherCAT Master, *Append Box*:
   - Box 1: `P1-H723-CIA402-DRAFT` (group "ecmaster draft slaves"). Process Data: 0x1600 + 0x1A00 + 0x1A01.
   - Box 2: servo. Process Data: nếu ESI có *Predefined PDO Assignment* cho **CSP** (Cyclic Synchronous Position) thì chọn; nếu không, ghi lại các PDO mặc định.
4. Cả hai Box: DC SYNC0, chu kỳ 1 ms. Ghi: Box nào TwinCAT chọn làm **reference clock** (thường Box đầu có DC).
5. Tab **Startup** của servo: chụp **toàn bộ** (register + CoE, transition, timeout). Hãng thường thêm 0x6060 (mode), 0x60C2 (interpolation time), 0x1C32/0x1C33 (sync mode)...
6. Xuất → `eni_mixed.xml`.

## C. `eni_mixed_rev.xml` — đảo thứ tự
Cùng project B, kéo Box servo lên trước Box P1 (hoặc tạo lại: servo trước, P1 sau). Kiểm lại reference clock (DC-02 cần biết TwinCAT chọn slave nào). Xuất → `eni_mixed_rev.xml`.

## D. `eni_8node_lrdlwr.xml` — LRD/LWR thay LRW (chỉ cho test âm E-06)
1. Chỉ để `softbus_esi.xml` trong thư mục ESI, reload; 8 × `SOFTBUS-PD4` như GĐ8 (hoặc mở `config/eni/twincat/softbus_eni.sln`).
2. Tìm tuỳ chọn bắt TwinCAT dùng LRD/LWR riêng thay vì LRW. Nơi hay gặp: *Device (EtherCAT) → tab EtherCAT → Advanced Settings… → Cyclic Frames* (hoặc tuỳ chọn tương tự như "LRW" / "separate input and output"). **Ghi lại đúng tên và vị trí tuỳ chọn** trong phiên bản TwinCAT đang dùng.
3. Nếu không có tuỳ chọn nào: ghi "không có", E-06 sẽ dùng ENI sửa tay (đổi `Cmd` 12 → 10/11 trong `Cyclic/Frame`).
4. Xuất → `eni_8node_lrdlwr.xml`.

Lưu ý: ENI của TwinCAT **luôn** có một LRD đọc trạng thái mailbox ở `Master/MailboxStates/StartAddr` — đó không phải process data. `eni_audit.py` tách hai loại này; E-06 ở 9.6 cũng phải nhìn địa chỉ, không chỉ số lệnh.

## Mang về Jetson
```powershell
scp eni_8node_ca.xml eni_mixed.xml eni_mixed_rev.xml eni_8node_lrdlwr.xml khaian@192.168.55.1:~/eni_9_4/
```
Ảnh chụp Startup/Process Data/DC: để chung thư mục (`~/eni_9_4/shots/`).

Trên Jetson:
```bash
cd ~/projects/ecmaster
tools/gd9/import_eni_9_4.sh ~/eni_9_4              # convert + audit + kiểm tra
sudo -E RUN=1 tools/gd9/import_eni_9_4.sh ~/eni_9_4  # + C-03/C-04 trên veth
```
Báo cáo: `docs/eni_audit_9_4.md` (mọi InitCmd, CA, LRD/LWR, DC; register InitCmd ngoài `config/eni/known_regs.txt` đánh dấu **UNKNOWN** → đầu vào bảng của 9.6).

## 5. Ghi vào `docs/eni.md` sau buổi
- TwinCAT biểu diễn CA thế nào: thuộc tính `CompleteAccess` trên `<InitCmd>` hay phần tử con; dữ liệu có SI0 16 bit không (đọc hex trong `eni_8node_ca.xml`).
- TwinCAT viết PDO assign bằng 1 lệnh CA hay chuỗi SI0=0/SI1/SI0=n.
- LRD/LWR: tên tuỳ chọn, và ENI thể hiện bằng `Cmd` 10/11 trong `Cyclic/Frame` hay cờ khác.
- Box servo: toàn bộ InitCmd (register + CoE), transition, timeout; những cái UNKNOWN trong báo cáo audit.
- Revision servo đã chọn và nguồn thông tin.
