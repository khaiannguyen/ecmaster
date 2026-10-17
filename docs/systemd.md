# Cấu hình lúc boot và chạy ecm_run bằng systemd (GĐ12 bước 12.2)

Mục tiêu: sau mỗi lần boot, Jetson tự về đúng cấu hình đã đo, không phải gõ lệnh tay. Mọi thứ **tắt cho tới khi người dùng bật**. Cài xong mà không enable gì thì máy boot y như trước.

## 1. Thành phần
| Thành phần | Việc làm | Bật bằng |
|---|---|---|
| `ecmaster-net@IFACE.service` | Lưu cấu hình cũ của cổng, rồi đặt `rx-usecs 0`, tắt GRO/GSO/TSO, tắt EEE, `ip link set up`, chờ carrier. Kiểm lại: `rx-usecs` hoặc GRO không ăn thì unit báo lỗi. **Stop/disable trả lại cấu hình đã lưu.** | `systemctl enable --now ecmaster-net@enP1p1s0` |
| `/etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf` | NetworkManager không quản lý cổng EtherCAT. `nmcli device set … managed no` **mất sau reboot**, file này thì không | `install_system.sh --iface …` |
| `ecmaster-clock@IFACE.service` | Chỉ cho `--link etf`: chạy `phc2sys -s CLOCK_REALTIME -c IFACE -O 0 -m --step_threshold=1`, dừng `systemd-timesyncd` khi chạy; khi dừng thì **bật lại timesyncd nếu trước đó nó đang chạy** (lỗi đồng hồ lệch 2 ngày ở GĐ11) | `systemctl start ecmaster-clock@enP1p1s0` |
| `ecm_run@NAME.service` | Chạy `ecm_run` theo `/etc/ecmaster/NAME.conf` qua `ecmaster-run` | `systemctl start ecm_run@NAME` |
| `ecmaster-bootprofile` | Profile boot P0/P1 trong `extlinux.conf` (`docs/rt_profiles.md` §2.0) | lệnh tay |

Lý do có `ecmaster-net@`: IS-09 đo 31 936 NOFRAME với `rx-usecs` mặc định của driver, so với 87 khi `rx-usecs 0` (`docs/i226.md`). Tốc độ link không bị ép: slave tự thương lượng 100 Mbit/s.

## 2. Cài và gỡ
Từ GĐ12.3 có hai đường cài, cùng bố cục `/opt/ecmaster` (README mục Install):
- **Gói `.deb`:** `sudo apt install ./dist/ecmaster_*.deb`, rồi `sudo ecmaster-port add enP1p1s0` (NetworkManager bỏ qua cổng, bật `ecmaster-net@enP1p1s0`). Gỡ: `sudo ecmaster-port remove enP1p1s0`, `sudo apt purge ecmaster`.
- **Từ mã nguồn:** `sudo ./install.sh --iface enP1p1s0 --enable-net`, gỡ bằng `sudo ./uninstall.sh`. `install.sh` cài binary, cấu hình, tài liệu, rồi gọi `packaging/install_system.sh` (phần dưới).

Chỉ phần tích hợp hệ thống (không binary), dùng khi phát triển:
```bash
sudo packaging/install_system.sh --iface enP1p1s0 --enable-net    # cài + bật ecmaster-net@enP1p1s0
sudo packaging/install_system.sh --dry-run --iface enP1p1s0       # xem trước, không ghi gì
sudo packaging/install_system.sh --uninstall                      # gỡ
```
- Helper vào `/opt/ecmaster/libexec`, `ecmaster-bootprofile` vào `/opt/ecmaster/sbin`, unit vào `/etc/systemd/system`, mẫu cấu hình vào `/etc/ecmaster/example.conf`.
- Danh sách file đã cài nằm ở `/var/lib/ecmaster/system.list`. `--uninstall` dừng và disable mọi instance `ecm_run@` → `ecmaster-clock@` → `ecmaster-net@` (cổng được trả cấu hình cũ), rồi xoá **đúng** các file trong danh sách. File `/etc/ecmaster/*.conf` do người dùng viết được giữ lại.
- Binary (`/opt/ecmaster/bin`) do `install.sh` của bước 12.3 cài. Trước đó, cấu hình trỏ `ECM_BIN` về binary trong repo.

## 3. File cấu hình `/etc/ecmaster/NAME.conf`
`ecmaster-run` **đọc** file, không bao giờ `source` nó. Cú pháp `KEY=VALUE`, dòng `#` là chú thích.

| Khoá | Ý nghĩa |
|---|---|
| `ECM_IFACE` | Cổng EtherCAT (bắt buộc) |
| `ECM_ARGS` | Các tuỳ chọn khác của `ecm_run` (bắt buộc). Tách như dòng lệnh shell: `'…'` / `"…"` gom từ, ví dụ `--cia402-script '0.5 enable all; 1.5 cos 1 100000 1'`. Không có biến (`$`, `` ` ``); `; \| & < > ( )` nằm ngoài dấu nháy bị từ chối |
| `ECM_API` | `yes` (mặc định): thêm `--api --api-dir /run/ecmaster/NAME`, trừ khi `ECM_ARGS` đã có `--api-dir` |
| `ECM_NET_CHECK` | `yes` (mặc định): từ chối chạy nếu cổng chưa được `ecmaster-net@` cấu hình. `no` chỉ dùng cho rig veth |
| `ECM_BIN` | Binary `ecm_run` (mặc định `/opt/ecmaster/bin/ecm_run`) |

**`ecmaster-run` từ chối chạy (exit 2, `ecm_run` không được khởi động) khi:**
- có khoá lạ, thiếu `ECM_IFACE` hoặc `ECM_ARGS`, có `--iface` trong `ECM_ARGS`, có cú pháp shell;
- `ECM_IFACE` đang mang **default route** (cổng SSH/Internet, không bao giờ là bus EtherCAT);
- cổng chưa được cấu hình (khi `ECM_NET_CHECK=yes`);
- dùng `--link etf` mà `phc2sys` không chạy.

Xem lệnh sẽ chạy mà không chạy: `/opt/ecmaster/libexec/ecmaster-run --print NAME`.

Mẫu `example.conf` đưa 2 IS620N lên OP và xuất `stats.json`, **không enable trục nào**. Muốn chạy chuyển động thì phải thêm `--axis` / `--hook cia402` và theo `docs/safety_boundary.md`.

## 4. Vận hành
```bash
sudo cp /etc/ecmaster/example.conf /etc/ecmaster/demo.conf     # rồi sửa bản chép
sudo systemctl start ecm_run@demo
journalctl -u ecm_run@demo -f
cat /run/ecmaster/demo/stats.json                              # docs/api.md
sudo systemctl stop ecm_run@demo                               # SIGTERM = kết thúc bình thường
```
- **`Restart=no`:** master dừng khi đang điều khiển trục thì không tự chạy lại sau lưng người vận hành. Sau lỗi, người vận hành quyết định (`docs/fault_policy.md`).
- `systemctl stop` gửi SIGTERM, đi đúng đường kết thúc của `ecm_run`: walk-down S6 các trục, `stats.json` với `final: true`. Sau 30 s (`TimeoutStopSec`) mà chưa xong thì systemd SIGKILL.
- `systemctl enable ecm_run@demo` làm master tự chạy khi boot. **Chỉ bật khi đã chắc cấu hình an toàn**, nhất là khi có `--hook cia402`.
- Thứ tự khi dùng ETF: `ecmaster-net@IF` (enable) → `ecmaster-clock@IF` → `ecm_run@NAME`.

## 5. Kiểm tra
| Test | Ở đâu | Nội dung |
|---|---|---|
| `tools/release/test_sys_offline.sh` | sandbox, CI, `make test-offline` | Giả lập ethtool / ip / systemctl. N-01…05: `ecmaster-net.sh` (đối chứng âm: NIC không nhận `-C` → unit FAIL). C-01…03: `ecmaster-clock.sh` (đối chứng âm: timesyncd vốn tắt → không bật). R-01…06: `ecmaster-run`. B-01…06: `ecmaster-bootprofile` (SYS-05). I-01…04: `install_system.sh --destdir` |
| `sudo -E tools/release/run_sys_12_2.sh sys01a` → reboot → `sys01b` | Jetson | SYS-01: sau reboot, cổng đã cấu hình mà không gõ lệnh. Đối chứng âm: disable unit → `check_env --boot` FAIL |
| `… sys02 sys03` | Jetson (veth cho sys02) | SYS-02: `ecm_run@` → OP, `stats.json`, stop → S6 + `final`. Đối chứng âm: `--iface` trong `ECM_ARGS` → unit failed. SYS-03: `ecmaster-clock@` |
| `… sys04` | Jetson | SYS-04: gỡ → không còn unit, cổng về như trước khi cài |

`tools/jetson/check_env.sh --boot`: các mục cổng (`rx-usecs`, GRO, unit enabled + active, NM unmanaged lúc boot) báo FAIL thay vì WARN.

## 6. Chưa làm ở bước này
- `jetson_clocks` không tự chạy lúc boot (các số đo đều ở MAXN_SUPER + `jetson_clocks`): vẫn chạy tay. Có thể thêm unit ở bước sau nếu cần.
- `ecmaster-port status` liệt kê cổng đã khai báo và trạng thái unit.
