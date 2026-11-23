# Kết quả đo hiệu năng v0.2.0

Theo mục 5 của `docs/firmware_v0.2.0.md`. Đo bằng `firmware/tools/memwatch.py` (`GET /api/debug`).

## Mốc: 0.1.0 (commit 0fad9dc + `/api/debug`), 2026-11-23

Loa ở nhà, Wi-Fi RSSI -47 đến -50 dBm, vừa xoá `data`/`nvs` (cài đặt mặc định, theme Swiss).

| Lượt | RAM trong trống (thấp nhất) | Khối lớn nhất (thấp nhất) | DMA trống (thấp nhất) | PSRAM trống (thấp nhất) | File |
|---|---|---|---|---|---|
| Rảnh, 10 phút | 33.0 KB | 14 KB | 25.3 KB | 7.62 MB | `baseline-idle.*` |
| Phát nhạc + web mở, 10 phút | **17.9 KB** | **8 KB** | **10.0 KB** | 7.51 MB | `baseline-play*` |

- `heap_min` từ lúc khởi động: **7.6 KB** (7796 B), xảy ra trước khi đo (khởi động / cài Wi-Fi qua hotspot / lần đầu lấy thời tiết), chưa biết chính xác lúc nào.
- Lượt phát nhạc: 99.8 Radio (MP3 128k, HTTPS, zeno.fm) 3 phút → Digital Radio HK (AAC+ 22 kHz, HTTP) 3 phút →
  VOV1 (HLS AAC 48 kHz mono, 237 kbps) 3 phút. Đồng thời gọi `/api/status` mỗi 2 s và `/api/screenshot` mỗi 10 s
  (251 + 51 request, không lỗi).
- Âm thanh:
  - zeno.fm: mất khoảng **58 s** mới phát được. Lỗi lặp lại `Connection failed, sock < 0` và
    `mbedtls_ssl_handshake returned -0x7280` (máy chủ đóng kết nối giữa lúc bắt tay TLS) ở link chuyển hướng 302,
    nối lại 3 lần. Sau đó **hết đệm 2 lần** trong phút đầu dù Wi-Fi mạnh. Trên PC, zeno.fm cũng mất 4.8 s mới có
    byte đầu, nên có thể một phần do máy chủ. Chưa phân biệt được phần nào do loa.
  - AAC+ HTTP và HLS: không hết đệm lần nào.
- RAM trong theo trạng thái: đang đệm thấp nhất 18.3 KB, đang phát 18.3 KB, dừng 32.2 KB. Phát nhạc tốn khoảng 14 KB
  RAM trong, và chia nhỏ RAM trong (khối lớn nhất từ 14–16 KB xuống còn 8 KB).

### Stack còn dư thấp nhất (byte) và CPU cao nhất

| Task | Stack cấp | Còn dư rảnh | Còn dư lúc phát | CPU cao nhất |
|---|---|---|---|---|
| `decode` | 8192 | 7224 | 6696 | 13.3% |
| `fetch` | 8192 | — | 3632 | 20.3% |
| `httpd` | 8192 | 3220 | 2324 | 2.4% |
| `taskLVGL` | ? | 3884 | **1932** | 17.9% |
| `weather` | 6144 | 2448 | 2448 | 6.1% |
| `battery` | 4096 | 3020 | 3036 | 0.1% |
| `mic` | 4096 | 2404 | 2532 | 1.5% |
| `sfx` | 4096 | 2572 | 2564 | 0.2% |
| `wifi_conn` | 4096 | 2104 | 2312 | 0.0% |
| `tcpip` | (IDF) | 1816 | 1808 | 3.5% |

### Nhận xét ban đầu (chưa sửa gì)
- Mục tiêu ở mục 5 (RAM trong thấp nhất ≥ 40 KB, khối lớn nhất ≥ 16 KB) còn xa: lúc phát chỉ còn khoảng 18 KB / 8 KB.
- `decode` dư nhiều (dùng chưa tới 1.5 KB/8 KB): thu nhỏ được khoảng 4–5 KB. `fetch`, `httpd`, `weather` cũng dư
  khoảng 2–3.5 KB mỗi cái. `fetch` và `weather` không ghi flash, có thể chuyển stack sang PSRAM.
- `taskLVGL` chỉ còn dư 1.9 KB lúc phát: không được thu nhỏ, cần xem vì sao tăng lúc phát.
- `taskLVGL` ăn 12–18% CPU kể cả lúc rảnh: xem có animation hay timer vẽ lại liên tục không.
- `/api/radio/play` lần 2 trả `vol` 80, lần 1 là 40, dù script không đổi âm lượng: cần xem lại.
- `Compile time` trong log không đổi khi build lại (ESP-IDF chỉ cập nhật khi build sạch): cần mã build đáng tin (commit git).
- Lúc khởi động loa thử mạng dùng lần cuối trước, mất khoảng 12 s rồi mới vào Wi-Fi nhà.

## Thay đổi 1: chuyển bộ nhớ của player sang PSRAM (2026-11-23)

- `fetch`: stack 8 KB chuyển sang PSRAM (`xTaskCreatePinnedToCoreWithCaps`, tự xoá bằng `vTaskDeleteWithCaps`).
  Task này chỉ làm việc mạng, không ghi flash.
- Bộ đệm vào của decoder (`IN_CAP` 8 KB) chuyển sang PSRAM.
- Stack `decode` giảm từ 8 KB xuống 4 KB (đo được dùng khoảng 1.5 KB), vẫn ở RAM trong.

| Lượt (cùng kịch bản phát nhạc + web) | RAM trong thấp nhất lúc phát | Khối lớn nhất | DMA thấp nhất | RAM trong lúc dừng |
|---|---|---|---|---|
| Mốc 0.1.0 | 18.3 KB | 8 KB | 10.2 KB | 32.2 KB |
| Sau thay đổi 1 | **38.6 KB** | **28 KB** | **30.5 KB** | 40.1 KB |

- Stack còn dư thấp nhất: `decode` 2600 B / 4096 (dư 63%), `fetch` 3632 B / 8192 (PSRAM).
- CPU: `decode` 14.3% (trước 13.3%), `fetch` 20.2% (trước 20.3%).
- Hết đệm: lượt này 4 lần ở Digital Radio HK. Phát lại riêng đài đó 4 phút sau đó: 0 lần. Nên coi là do mạng/máy chủ
  của đài. Trong lượt này có người bấm nút trên loa (dừng, phát lại 5 lần, đổi âm lượng lúc 114–131 s).
- `int_min` từ lúc khởi động vẫn 7.6 KB: chưa tìm ra lúc nào.

## Thay đổi 2: chữ chạy một vòng rồi nghỉ 8 s (2026-11-23)

CPU của `taskLVGL` ở màn Đồng hồ (đo 30 s mỗi theme):

| Theme | Trước | Sau | Vẽ lại / 10 s (trước → sau) |
|---|---|---|---|
| Swiss | 16.7% | **7.2%** | ~850 → ~220–310 |
| Brutal | 30.4% | **15.2%** | ~1210 → ~330–790 |
| Zen | 3.2% | 3.0% | 30 → 30 |
| Cyberpunk | 3.7% | 3.7% | ~110 → ~110 |

- Swiss vẫn còn vùng `0,41-219,143` (khu vực giờ chữ to) bị vẽ lại khoảng 30 lần/giây trong từng quãng, cùng vùng
  1 px `0,0-0,26…36`. Chưa rõ nguyên nhân, đã ghi vào mục "Tối ưu lại các theme về UI/UX" trong `docs/roadmap.md`.
- Log `ui: redraw 10s` (đếm vùng vẽ lại) là tạm thời, bỏ đi sau khi xong việc ở roadmap.

