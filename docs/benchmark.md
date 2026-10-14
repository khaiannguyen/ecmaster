# Benchmark — ecmaster trên Jetson Orin + i226 + 2 servo IS620N

Phase 11.7 (BM-01, BM-02, BM-05). Mục đích: đặt số đo của dự án cạnh bài đo của acontis "EC-Master on NVIDIA Jetson Orin Nano" **mà không so sai**. Mọi số ở đây đo trên phần cứng thật. Có thể tái lập bằng lệnh ở §5.

## 1. Cấu hình
| | |
|---|---|
| Máy | Jetson Orin, JetPack r39.2, kernel `6.8.12-1021-rt-tegra` (PREEMPT_RT), MAXN_SUPER + `jetson_clocks` |
| Cmdline RT | `isolcpus=3 irqaffinity=0-2 transparent_hugepage=never` (profile P0 — `docs/rt_profiles.md`) |
| Thread RT | `ecm_run`, SCHED_FIFO 80, CPU 3; giữ `/dev/cpu_dma_latency` = 0 |
| NIC | Intel i226 (igc build tay), 100 Mbit/s Full, `rx-usecs 0`, GRO/GSO/TSO off |
| Bus | 2 × Inovance IS620N (ENI `eni_2servo`), DC SYNC0 1 ms, setpoint 30 % |
| Ứng dụng | Hook CiA402 2 trục, CSP (1 − cos) 1/2 vòng 0,2 Hz (W-08) |
| API | `--api --api-cpus 1-2` (đọc số), không web UI |
| Backend | `af_packet`, hoặc `etf` (SO_TXTIME + qdisc ETF offload, delta 100 / lead 50 / asap 25 µs) |
| Tải | `stress-ng --cpu 3 --vm 1 --vm-bytes 512M` ghim CPU 0–2 |
| Thời lượng | **1 h mỗi ca** (khoảng 3,6 M chu kỳ), 14–15/10/2026, commit `a0ff867` |

## 2. Kết quả BM-02 (1 ms, 1 h mỗi ca)
**Toàn bộ 4 ca:**
- 0 overrun, **0 NOFRAME**, 0 WKC lỗi;
- DC LOCKED, 0 unlock;
- không lỗi trục, 0x603F = 0;
- dừng có walk-down (S6).

ETF: missed 0, invalid 0.

### 2.1 Bảng chính (µs)
| Đại lượng | | af, không tải | af, có tải | etf, không tải | etf, có tải |
|---|---|---|---|---|---|
| **Wake jitter** (trễ thức so với lịch) | min / avg / max | 2,4 / 3,1 / 14,4 | 2,5 / 3,2 / 17,8 | 2,3 / 3,1 / 64,6 | 2,3 / 3,2 / 70,8 |
| | p99.99 | 5,6 | 6,3 | 5,6 | 6,3 |
| **Occupancy** (gửi + nhận + DC + hook) | min / avg / max | 33,8 / 43,8 / 159,4 | 36,4 / 46,8 / 486,1 | 64,2 / 77,2 / 174,7 | 59,3 / 78,6 / 532,4 |
| | p99.99 | 79,4 | 141,3 | 112,2 | 158,5 |
| **prep_send** (dựng + gửi frame) | avg / p99.99 / max | 5,2 / 17,8 / 51,3 | 6,3 / 28,2 / 95,7 | 6,4 / 11,2 / 27,1 | 7,5 / 15,8 / 45,5 |
| **Turnaround** (TX sw → RX sw) | avg / p99.99 / max | 35,7 / 70,8 / 150,8 | 37,1 / 125,9 / 474,0 | 68,1 / 100,0 / 163,6 | 67,4 / 158,5 / 522,2 |
| **Hook CiA402** 2 trục | avg / p99.99 / max | 0,8 / 4,5 / 8,7 | 1,2 / 4,5 / 11,4 | 0,7 / 3,5 / 7,6 | 1,2 / 4,5 / 15,5 |
| **DC \|e\|** (lỗi điều khiển DC) | p99.99 / max | 11 / 46 | 22 / 88 | **1 / 1** | **1 / 7** |

Phân vị là cận trên của bucket (rộng khoảng 12 %) và không bao giờ vượt max.

### 2.2 Đọc bảng
- **Thời điểm frame lên dây.** Với etf, NIC tự giữ frame đến launch time:
  - sai số launch < 0,2 µs (IS-04, HW TX timestamp);
  - wake jitter (max 65–71 µs) không ảnh hưởng, vì thread thức sớm `lead` 50 µs.

  Với af_packet, frame ra dây khoảng wake + prep_send sau tick: p99.99 khoảng 23 µs không tải, 35 µs có tải.
- **DC \|e\| ≤ 1 µs ở p99.99 với etf, kể cả có tải.** af_packet là 11–22 µs.
- **Tải trên CPU 0–2 làm phình đuôi max** của turnaround và occupancy (khoảng 475–530 µs). Trong khi đó prep_send, hook và wake gần như không đổi.
  - Phần đuôi nằm ở **đường nhận**: mọi ngắt của i226 nằm cố định trên CPU0 (Tegra không cho dời MSI), mà CPU0 thuộc nhóm bị tải.
  - Ở 1 ms vẫn còn dư biên nhiều: 0 NOFRAME trong 4 h.
- **Occupancy của etf cao hơn af khoảng 33 µs.** Đó không phải thời gian CPU. Turnaround và occupancy gồm cả thời gian chờ reply, mà với etf frame được giao cho driver trước launch time.
- **CRC trên i226:** +1 đến +5 mỗi giờ. Lỗi chỉ nằm ở chiều drive 1 → Jetson (mọi bộ đếm lỗi ESC bằng 0) và không gây NOFRAME.
- ETF `bypass` khoảng 5 400 mỗi giờ là các frame không phải motion (diag, cấu hình), gửi ngay theo thiết kế. `late` 1 là một frame motion được gửi ngay thay vì hẹn giờ, trong 3,6 M frame.

## 3. Đặt cạnh acontis
Nguồn: bài "EC-Master on NVIDIA Jetson Orin Nano" của acontis (cổng onboard Realtek 8169). Ở đây chỉ tóm lược, không chép lại đồ thị.

| | acontis, SockRaw (≈ af_packet) | acontis, driver RT riêng | ecmaster af_packet | ecmaster etf |
|---|---|---|---|---|
| Chu kỳ | 1 ms | 250 µs | 1 ms (250 µs: `docs/rt_profiles.md` §14) | 1 ms (250 µs: §14) |
| Task duration max | 207–253 µs | ~30 µs | 159 µs (có tải 486) | 175 µs (có tải 532) |
| Cơ chế nhận | socket, ngắt | poll MAC trực tiếp (user space) | socket, ngắt (CPU0) | socket, ngắt (CPU0) |
| Thời điểm phát | phần mềm | phần mềm | phần mềm | **phần cứng (LaunchTime)** |
| Sai số DC / CtlErr | đồ thị | đồ thị | p99.99 11–22 µs | **p99.99 1 µs** |

**So sánh được:**
- cùng SoC họ Orin;
- cùng loại kernel RT;
- cùng kiểu cấu hình cô lập core;
- ý nghĩa của wake jitter, task duration (≈ occupancy) và lỗi DC.

**Không so sánh được trực tiếp:**
- NIC khác (Realtek onboard và i226 PCIe);
- slave khác (thiết bị IO của acontis và 2 servo IS620N 100 Mbit/s);
- occupancy của ecmaster gồm cả thời gian **chờ reply** và hook CiA402; định nghĩa task duration của acontis có thể khác;
- tải nền khác;
- không biết slave và ESC của acontis.

→ **Không kết luận "nhanh hơn" hay "chậm hơn".** Điều đo được là: với ngắt + socket, đuôi nằm ở đường nhận (giống SockRaw của acontis). Driver poll của acontis tránh được đuôi đó. Ecmaster có LaunchTime phần cứng, nên thời điểm phát và DC chính xác tới mức µs dù dùng socket. Hướng tiếp theo cho đuôi nhận: spike AF_XDP (11.8).

## 4. Chưa có trong tài liệu này
- **BM-03:** độ tăng network time 0x0910 từ pcap. Cần rig loopback.
- **BM-04:** chu kỳ ngắn nhất. Số 250 µs (P1 + etf + `--rx-guard-us 60`, 5 NOFRAME / 1 h) nằm ở `docs/rt_profiles.md` §13–§14. Chưa chạy lại ở dạng bảng benchmark có tải.

## 5. Tái lập
```bash
sudo systemctl stop systemd-timesyncd
sudo phc2sys -s CLOCK_REALTIME -c enP1p1s0 -O 0 -m --step_threshold=1      # in another terminal
sudo -E PROFILE=P0 tools/bench/run_bm_11_7.sh                               # 4 x 1 h, writes benchmark.md
python3 tools/bench/ecm_bench.py --label af_idle=LOG/af_idle/stats.json ... # tables from final stats.json
```
`ecm_bench.py` từ chối file không có `"final": true` hoặc không có `rt.isolated_cpus` (`tools/bench/test_bench_offline.sh`).
