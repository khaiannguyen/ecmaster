# Golden pcap và CI (Giai đoạn 7.5)

## Mục đích

Mỗi lần sửa master, CI kiểm xem **chuỗi datagram** mà master gửi lên dây còn đúng như trước không: lệnh gì, theo thứ tự nào, địa chỉ nào, dài bao nhiêu, WKC trả về bao nhiêu. Đây là kiểu hồi quy mà test offline không bắt được, ví dụ: vô tình đổi thứ tự cấu hình, quên đưa bus về INIT, thêm một frame thừa vào chu kỳ.

## Thành phần (`tools/golden/`)

| File | Việc |
|---|---|
| `capture_ecm_run.sh` | Kịch bản cố định: soft_bus 8 node, PDO 4 byte, DC 32-bit, `--no-sm-wd`; `ecm_run` 4 motion + 4 IO, 2 s tuần hoàn (có frame chẩn đoán 1 s), về INIT. Bắt gói bằng `tshark -F pcap` trên `veth_m`, in cấu trúc ra stdout |
| `pcap2struct.py` | Đọc pcap, tách datagram, chuẩn hoá (xem dưới) |
| `check_golden.sh` | So với `golden_ecm_run.txt`. `UPDATE=1` ghi lại golden, `EXPECT_DIFF=1` dùng cho đối chứng âm, `ATTEMPTS` (mặc định 2) |
| `negative_control.sh` | Build một bản `ecm_run` đảo thứ tự hai lệnh ghi SM watchdog (0x0400, 0x0420); golden **phải** thấy khác |
| `golden_ecm_run.txt` | Golden, 547 dòng cấu trúc |

## Chuẩn hoá

- **Giữ:** lệnh, thứ tự datagram trong frame, ADP/ADO (hoặc địa chỉ logic), độ dài dữ liệu, WKC của reply.
- **Bỏ:** index, dữ liệu, thời gian.
- **CONFIG** (trước frame process-data đầu tiên) và **SHUTDOWN** (sau frame cuối): giữ thứ tự. Dòng giống hệt lặp liền nhau gộp thành một (số lần poll "EEPROM busy" là chuyện thời gian); khối 2–8 dòng lặp lại gộp thành một bản + `x+` (vòng qua các slave, các word SII, vòng keepalive).
- **CYCLIC**: chỉ là tập các dạng frame có reply, không đếm số lần. Frame chẩn đoán 1 s rơi vào tick cuối hay không (run dừng ở tick 2000 hay 2001) được tính vào CYCLIC. Chỉ frame **nhiều datagram** được chuyển như vậy; chuỗi tắt (AL control INIT, statecheck) toàn frame một datagram nên luôn nằm ở SHUTDOWN.
- **Chiều của frame:** ESC đầu tiên đặt bit 1 của byte đầu MAC nguồn trên frame quay về. Từ 7.5 soft_bus làm đúng như vậy (trước đó reply giữ nguyên MAC `01:01:01:01:01:01`, không phân biệt được với request).

**Vì sao không dùng `tshark -T fields` như plan ghi:** Wireshark trải các datagram của một frame ra các trường `ecat.sub1.cmd`, `ecat.sub2.cmd`… với tên và cách xếp phụ thuộc phiên bản. Jetson và runner CI không nhất thiết cùng phiên bản; định dạng frame EtherCAT thì không đổi. `tshark` chỉ dùng để bắt gói.

## Độ ổn định đã đo (sandbox, không RT, nhiễu)

10/10 lần bắt cho cùng một cấu trúc. Hai nguồn dao động đã phải xử lý: số lần poll EEPROM busy (1 hay 2) và reply bị mất trong pha tuần hoàn. Một frame mất lúc cấu hình làm SOEM đọc lại một lần; `check_golden.sh` bắt lại tối đa `ATTEMPTS` lần. Thay đổi thật thì lần nào cũng khác, nên việc thử lại không che được nó.

## Khi nào sinh lại golden

Chỉ khi **cố ý** đổi hành vi trên dây (thêm bước cấu hình, đổi frame chẩn đoán…):
```bash
sudo -E tools/golden/check_golden.sh            # xem diff, chắc chắn đó là điều mình muốn
sudo -E UPDATE=1 tools/golden/check_golden.sh   # ghi đè golden
git add tools/golden/golden_ecm_run.txt          # commit cùng thay đổi code
```

## CI (`.github/workflows/ci.yml`)

| Job | Nội dung | Chặn merge |
|---|---|---|
| `offline` | test offline soft_bus, telemetry, core (ecm_dc), diag, policy, kèm ASan/UBSan | có |
| `golden` | SOEM v2.0.0 + `EC_MAXGROUP=4` + `patches/soem-mbx-cnt.patch`, veth, golden, đối chứng âm; giữ pcap khi fail | có |
| `l5-smoke` | 7.2 `run_l5_diag.sh`; 7.3 L5-01/02/05 + đối chứng âm; 7.4 L5-09/12 + đối chứng âm | không (`continue-on-error`): runner dùng chung, không RT |

CI cũ build SOEM **không** có `-DEC_MAXGROUP=4`, trong khi `ecm_run` cần group 2.

Đã mô phỏng cả ba job trên một bản chép sạch của repo với SOEM clone đúng tag + bản vá: đạt hết. Chưa chạy trên GitHub thật.

**Lần dùng thật đầu tiên (25/9):** sửa lỗi tắt máy (rời OP ngay khi vòng lặp dừng, xem `fault_policy.md` §12) làm SHUTDOWN có thêm một vòng "AL control + statecheck". `check_golden.sh` báo đúng dòng đó, golden được sinh lại có chủ đích (546 → 547 dòng).
