# Profile RT của Jetson (GĐ11 bước 11.5)

Tài liệu này ghi các cấu hình khởi động (boot profile) mà master được đo trên đó, cách chuyển qua lại giữa chúng, và số đo của từng cấu hình. Kế hoạch: `claude/giai_doan_11_ke_hoach.md` §6.

Phạm vi GĐ11: **chỉ đo**. Không đổi giá trị mặc định nào của `ecm_run` và không đổi `DEFAULT` của extlinux. GĐ12 mới chọn cấu hình xuất xưởng dựa trên bảng ở §5.

## 1. Các profile

| Mã | Cmdline | Trong `ecm_run` | Ghi chú |
|---|---|---|---|
| **P0** | `isolcpus=3 irqaffinity=0-2 transparent_hugepage=never` (hiện tại) | giữ `/dev/cpu_dma_latency` = 0 (mặc định từ 0017) | Đường cơ sở |
| **P0n** | như P0 | `--no-dma-latency` | Đối chứng âm: biết là xấu ở 500 µs (GĐ10) |
| **P1** | P0 + `cpuidle.off=1` | giữ fd như P0 | Tương đương arm64 của `cpuidle.off=1 idle=poll` bên acontis |
| **P1n** | như P1 | `--no-dma-latency` | Nếu P1n ≈ P1 thì cmdline thay được việc giữ fd |

Hai cách tắt deep idle khác nhau ở phạm vi:
- **Giữ fd** chỉ có tác dụng trong lúc `ecm_run` chạy.
- **`cpuidle.off=1`** có tác dụng mọi lúc kể từ khi boot. Vì vậy IS-06 đo thêm điện năng.

`cyclictest` mặc định cũng giữ `/dev/cpu_dma_latency` = 0. Cờ `--laptop` tắt việc đó, và đó là biến thể `nodma` của IS-02.

### Tham số không dùng (và lý do)
| Tham số (bên acontis) | Lý do bỏ |
|---|---|
| `nohz_full=3`, `rcu_nocbs=3` | Kernel RT của JetPack không build `CONFIG_NO_HZ_FULL` / `CONFIG_RCU_NOCB_CPU` (GĐ1 §3.6), nên kernel bỏ qua hai tham số này. Core 3 vẫn nhận tick |
| `idle=poll`, `processor.max_cstate=0` | Tham số của x86, không có tác dụng trên arm64. Trên Orin, cách tương đương là `cpuidle.off=1` |
| `cpufreq.off=1` | `jetson_clocks` đã khoá tần số. Mục này chỉ ghi lại để đối chiếu |

## 2. Thêm và chuyển sang P1

Script `tools/gd11/extlinux_p1.py` chép entry `DEFAULT` hiện tại thành `LABEL real-time-noidle`, đổi `MENU LABEL` và thêm `cpuidle.off=1` vào cuối `APPEND`. Các entry cũ giữ nguyên, và trước mỗi lần ghi file được sao lưu thành `extlinux.conf.bak-<ngày giờ>`.

```bash
python3 tools/gd11/extlinux_p1.py                      # xem diff, không ghi
sudo python3 tools/gd11/extlinux_p1.py --apply         # thêm entry, DEFAULT không đổi
# Không có UART/HDMI để chọn ở boot menu: đổi DEFAULT tạm thời
sudo python3 tools/gd11/extlinux_p1.py --set-default real-time-noidle && sudo reboot
tools/gd11/rt_profile.sh --expect P1                   # sau boot: PROFILE=P1, exit 0
# ... đo ...
sudo python3 tools/gd11/extlinux_p1.py --set-default real-time && sudo reboot   # về P0
tools/gd11/rt_profile.sh --expect P0
```

- Tên entry gốc (`real-time` ở trên) lấy theo `DEFAULT` trong file của bạn. Xem bằng `grep -E '^(DEFAULT|LABEL)' /boot/extlinux/extlinux.conf`.
- Nếu boot hỏng: chọn entry cũ ở boot menu qua UART, hoặc chép lại bản `.bak` từ máy khác (thẻ/NVMe).
- Mỗi lần reboot phải làm lại các bước sau trước khi đo:
  - chạy `sudo jetson_clocks`;
  - tắt GRO/GSO/TSO và đặt `rx-usecs 0` trên i226;
  - chạy `phc2sys` cho các ca ETF;
  - tắt `systemd-timesyncd`.

  `tools/jetson/check_env.sh` có kiểm các mục này (và in dòng profile).

## 3. IS-01 — kiểm cấu hình

`tools/gd11/rt_profile.sh [--expect P0|P1] [--iface enP1p1s0]` in `PROFILE=…` và kiểm:
- kernel PREEMPT_RT;
- core 3 isolated;
- trạng thái cpuidle khớp profile;
- governor `performance` (cần `jetson_clocks`);
- nvpmodel;
- khi có `--iface`: GRO/GSO/TSO off và `rx-usecs 0`.

Exit 0 là đạt, 1 là chỉ có cảnh báo, 2 là sai profile hoặc có kiểm FAIL.

## 4. Cách đo phần 1 (IS-02, IS-03, IS-06)

```bash
sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up
sudo apt install rt-tests stress-ng          # nếu chưa có
sudo -E PROFILE=P0 tools/gd11/run_is_11_5.sh            # ~105 phút, chạy trong tmux
# reboot sang P1 (§2), rồi:
sudo -E PROFILE=P1 tools/gd11/run_is_11_5.sh
python3 tools/gd11/is_report.py log_gd11_is_P0_*/results.csv log_gd11_is_P1_*/results.csv
```

| Ca | Nội dung | Thời lượng (DUR = 600 s) |
|---|---|---|
| IS-01 | `rt_profile.sh --expect PROFILE`; đối chứng âm `--expect` profile kia → exit 2 | vài giây |
| IS-02 | `cyclictest -m -p 80 -a 3 -i 1000 -h 1000` × {dma, nodma} × {idle, load} | 4 × DUR |
| IS-03 | `ecm_run` N=8 (4 motion + 4 IO) 1 ms, `soft_bus` trên veth core 2 FIFO 79, × {dma, `--no-dma-latency`} × {idle, load}: `wake_jitter`, `occupancy`, `prep_send`, `turnaround`, overrun, NOFRAME, DC unlock | 4 × DUR |
| IS-06 | `tegrastats` 1 s: {rest (không chạy gì), bus (rig IS-03)}; VDD_IN trung bình, nhiệt CPU max | 2 × DUR |

- **Tải** (giống bài acontis): `stress-ng --cpu 3 --vm 1 --vm-bytes 512M --sock 1` ghim core 0–2. Có thể thêm lưu lượng TCP qua USB-C: `IPERF_PEER=192.168.55.100`, với `iperf3 -s` chạy trên laptop.
- **Cách đọc số:**
  - Phân vị của `ecm_run` là cận trên của bucket log (rộng khoảng 12 %), đã kẹp không vượt max.
  - Phân vị của `cyclictest` theo bin 1 µs. Giá trị `>1000` nghĩa là rơi ra ngoài histogram.
- **Mốc so sánh:** GĐ8 8.5 (29/9) đo wake p99.99 144 µs, max 277 µs. Lượt đó **chưa có isolcpus** và chưa có bản vá c7.

## 5. Kết quả

*(Chưa đo. Điền từ `is_report.py` sau hai lượt P0/P1.)*

### 5.1 IS-02 cyclictest core 3, 1 ms (µs)
### 5.2 IS-03 ecm_run N=8, 1 ms (µs)
### 5.3 IS-06 điện năng
### 5.4 Khuyến nghị
- Robot chạy pin: …
- Máy cắm điện cần chu kỳ ngắn: …

## 6. Phần 2 (sẽ thêm)
Các ca sau dùng cấu hình tốt nhất của §5 và rig loopback i226 ↔ `enP8p1s0` ở 100 Mbit/s:
- IS-04: quét ETF delta/lead.
- IS-05: chu kỳ 1 ms / 500 µs / 250 µs, kèm ca 4 trục P4.
- IS-08: thời gian chết đầu-cuối.
- IS-09: lọc EtherType RX.
- IS-10: ngân sách 250 µs.
