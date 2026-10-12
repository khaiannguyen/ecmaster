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

## 5. Kết quả phần 1 (12/10/2026, Jetson Orin, `6.8.12-1021-rt-tegra`, MAXN_SUPER + `jetson_clocks`)

### 5.1 IS-02 cyclictest core 3, 1 ms, 600 000 mẫu (µs)
| Profile | Load | min | avg | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| P0 | idle | 1 | 2 | 2 | 3 | 3 | 4 | 7 |
| P0 | load | 1 | 2 | 2 | 2 | 3 | 3 | 8 |
| P0n | idle | 1 | 1 | 1 | 2 | 2 | 3 | 6 |
| P0n | load | 1 | 1 | 2 | 2 | 2 | 3 | 10 |
| P1 | idle | 1 | 2 | 2 | 3 | 3 | 4 | 7 |
| P1 | load | 1 | 2 | 2 | 2 | 3 | 3 | 8 |
| P1n | idle | 1 | 2 | 2 | 3 | 3 | 4 | 8 |
| P1n | load | 1 | 2 | 2 | 3 | 3 | 4 | 6 |

### 5.2 IS-03 ecm_run N=8, 1 ms, veth, 600 000 chu kỳ (µs, P0)
| | P0 idle | P0 load | P0n idle | P0n load | GĐ8 8.5 (29/9) |
|---|---|---|---|---|---|
| wake_jitter p99.99 / max | 5,0 / 11,3 | 5,6 / 17,1 | 4,5 / 10,8 | 4,5 / 17,9 | **144 / 277** |
| prep_send p99.99 / max | 7,1 / 20,6 | 11,2 / 24,5 | 7,9 / 25,2 | 10,0 / 28,6 | |
| occupancy p99.99 / max | 63 / 94 | 112 / 847 | 63 / 81 | 100 / 125 | |
| turnaround p99.99 / max | 35,5 / 56 | 79 / 630 | 35,5 / 60 | 63 / 118 | |
| overrun / NOFRAME / DC unlock | 0/0/0 | 0/1/0 | 0/0/0 | 0/0/0 | |

Ghi chú:
- Đuôi turnaround/occupancy lớn ở các ca có tải là đuôi của `soft_bus`: nó chạy trên core 2, nằm trong nhóm core 0–2 bị tải. Phần 2 ghim `soft_bus` lên core 4.
- Lượt GĐ8 chạy khi chưa có `isolcpus`.

### 5.3 IS-06 điện năng (VDD_IN trung bình, 600 s)
| Profile | nghỉ | bus 1 ms |
|---|---|---|
| P0 | 6847 mW (CPU max 55,0 °C) | 6886 mW (55,3 °C) |
| P1 | 6890 mW (52,2 °C) | 6918 mW (54,4 °C) |

### 5.4 Kết luận phần 1
- Sau `isolcpus` + `jetson_clocks` (+ fd c7), đuôi wake của thread RT trên core 3 chỉ còn khoảng **10–18 µs max** (cyclictest ≤ 10 µs), kể cả có tải. Số GĐ8 (277 µs) không còn đúng. Đây là cơ sở để quét delta ETF xuống 100 µs ở IS-04.
- **Trên core 3, P1 không hơn P0.** Chi phí điện của P1 là khoảng +30–40 mW, cỡ nhiễu đo.
- Câu hỏi c7 còn lại nằm ở **CPU0, nơi nhận ngắt RX của i226**. Rig veth không thấy được, nên IS-05n ở phần 2 trả lời: chạy `af_packet` 500 µs `--no-dma-latency` dưới P0 và dưới P1.
- Khuyến nghị tạm thời: giữ **P0** (cấu hình xuất xưởng không cần đổi cmdline). P1 chỉ đáng dùng nếu IS-05n cho thấy P1n tốt hơn P0n rõ rệt trên i226.

## 6. Phần 2a (IS-04, IS-05): ETF và chu kỳ ngắn trên i226

Rig: cáp loopback i226 `enP1p1s0` ↔ `enP8p1s0`, ép 100 Mbit/s. `soft_bus` chạy core 4 FIFO 79, ngoài nhóm core 0–2 bị tải. Đây vẫn là phép đo hệ thống của master với slave giả; IS-07 dùng 2 IS620N thật.

```bash
sudo jetson_clocks
sudo systemctl stop systemd-timesyncd
sudo phc2sys -s CLOCK_REALTIME -c enP1p1s0 -O 0 -m --step_threshold=1 &      # để chạy suốt buổi
sudo -E PROFILE=P0 tools/gd11/run_is2_11_5.sh                                  # ~2 h 40 min
# IS-05n dưới P1 (sau khi boot P1, §2):
sudo -E PROFILE=P1 CASES="rig is05n" tools/gd11/run_is2_11_5.sh
python3 tools/gd11/is_report.py log_gd11_is2_P0_*/results.csv log_gd11_is2_P1_*/results.csv
```

| Ca | Nội dung | Thời lượng mặc định |
|---|---|---|
| rig | Kiểm điều kiện (NM, SSH, GRO, coalescing, profile, phc2sys, timesyncd, 100 Mbit/s Full); thiếu thì dừng | ~30 s |
| IS-04 | `txtime_probe` với delta ∈ {400, 300, 200, 150, 100} µs, lead = delta − 50 µs; mỗi delta chờ 90 s rồi chạy `clock_check`. Ghi lại delta nhỏ nhất có 0 missed / 0 invalid | 5 × (600 + 110) s |
| IS-05 | `ecm_run` N=8 ở 1000 / 500 / 250 µs: trước tiên `--link etf` (delta của IS-04, lead = delta − 50, asap = lead/2, tối thiểu 25 µs), sau đó `af_packet` | 6 × 600 s |
| IS-05n | `af_packet` 500 µs `--no-dma-latency`: so với hàng `af_500us` của IS-05 | 600 s |
| IS-05 P4 | `soft_bus` 1 node `cia402_4ax.prof`, `--hook cia402`, CSP sine 4 trục, 250 µs, `etf` rồi `af_packet` | 2 × 600 s |

- **Đặt lại delta và lead:** `ETF_DELTA`, `ETF_LEAD`, `ETF_ASAP`.
- **Xác nhận delta đã chọn:** sau IS-04 chạy lại 1 h với `CASES="rig is04" DELTAS=<delta> PROBE_S=3600`. Lượt 600 000 frame không lỗi chỉ cho cận trên tỉ lệ lỗi khoảng 5·10⁻⁶.
- **Ngân sách nhận ở 250 µs** (IS-10): deadline = `next + cycle − asap − 150 µs` (`RX_GUARD_NS`, cố định trong code). Với `af_packet` còn 100 µs kể từ thời điểm chu kỳ, với `etf` còn 100 − asap µs kể từ lúc frame phát. Ca nào FAIL ở 250 µs thì đọc turnaround so với con số này trước khi kết luận.

## 7. Kết quả phần 2a (12/10, P0, rig loopback)
- **IS-04:** delta 400 → 100 µs đều 0 missed / 0 invalid trên 600 000 frame. launch_err p99.99 166 ns. Cặp mới **delta 100 / lead 50 / asap 25** thay cho 400 / 350 / 150 (còn chờ một lượt xác nhận 1 h).
- **IS-05 N=8:**
  - 1 ms và 500 µs: 0 NOFRAME với cả af_packet lẫn etf.
  - 250 µs: NOFRAME 0,4 % (af) và 6,3 % (etf).
  - P4 250 µs: 80 % NOFRAME, LOST/RECOVER lặp 18 293 lần.
- **Giới hạn ở 250 µs là turnaround của slave giả**, không phải phía master: `soft_bus` nhận qua NIC Realtek và xử lý trong user space, cho p50 79 µs (N=8) và 178 µs (P4), đuôi 250–400 µs. Trong khi đó ngân sách nhận chỉ còn `cycle − asap − 150 µs` (100 µs với af).
- Phần 2a in "36/0" là sai: tiêu chí PASS khi đó không kiểm NOFRAME và kiểm LOST quá lỏng. Từ 0037, tiêu chí dùng chung nằm ở `tools/gd11/is_lib.sh`.
- **P0n ≈ P0** trên rig loopback. Phép so bị nhiễu: ngắt của cả hai NIC đều nằm trên CPU0, nên CPU0 không có lúc nào rảnh để vào c7.

## 8. IS-07: hai IS620N thật

```bash
# servo: chỉ cần nguồn điều khiển (L1C/L2C); không trục nào được enable
# rút cáp loopback khỏi enP8p1s0, cắm i226 -> IS620N #1 -> #2
sudo -E PROFILE=P0 tools/gd11/run_is7_11_5.sh            # ~75 min
```
| Ca | Chu kỳ | Backend |
|---|---|---|
| af1000 / af500 / af250 | 1 ms / 500 / 250 µs | af_packet |
| nodma500 | 500 µs | af_packet `--no-dma-latency` (câu hỏi c7, lần này CPU0 chỉ phục vụ i226) |
| etf500 / etf250 | 500 / 250 µs | etf delta 100 / lead 50 / asap 25 |

- Mỗi lượt dùng một bản ENI riêng cho chu kỳ đó (`eni_cycle.py`). Không có `--hook`, `--axis` hay `--pdo-set`, nên controlword luôn bằng 0.
- PASS khi:
  - rc 0, 0 overrun, NOFRAME ≤ 2;
  - policy không lần nào vào LOST;
  - DC LOCKED, 0 unlock;
  - 0x603F = 0 trên cả hai drive (không Er.E08);
  - bộ đếm lỗi của NIC không tăng.

## 9. Phần 2b còn lại
- IS-08: thời gian chết đầu-cuối.
- IS-09: lọc EtherType RX.
- IS-10: bảng ngân sách 250 µs (sau IS-07).
