# Kế hoạch: firmware "MeowBox" cho loa OSTB xiaozhi-3st

> **Trạng thái 2026-11-22:** P0–P6 đã chạy trên loa (trừ mini game và ntfy.sh); P7 còn vài mục. Các ô chưa đánh dấu bên dưới là việc còn lại.

> Thay hẳn xiaozhi bằng firmware tự viết: **đồng hồ + radio internet +
> loa thông báo + màn phụ PC + mèo ảo**, cấu hình qua trang web ngay trên loa.
> Không AI, không Home Assistant, không phụ thuộc server bên thứ ba (ngoài nguồn radio/thời tiết công khai).

Phần cứng, pin map: xem [hardware.md](hardware.md), [pinmap.md](pinmap.md), `hw/ostb_xiaozhi_3st.h`.

---

## 1. Mục tiêu

| # | Tính năng | Mô tả ngắn |
|---|---|---|
| F1 | Đồng hồ | Giờ (SNTP), **lịch âm**, thời tiết (open-meteo), báo thức, hẹn giờ tắt |
| F2 | Radio internet | Phát stream MP3/AAC (+ HLS cho VOV ở giai đoạn sau), tìm đài qua Radio Browser, **YouTube qua PC agent** |
| F3 | Trang web cấu hình | Wi-Fi, đài, báo thức, thời tiết, âm lượng, độ sáng, thú ảo, cập nhật firmware |
| F4 | Loa thông báo | HTTP API: gửi chữ + tiếng chuông (hoặc file âm thanh), hiện đè lên màn hiện tại |
| F5 | Màn phụ PC | App nhỏ trên PC gửi CPU/RAM/GPU/nhiệt độ/nhạc đang phát; điều khiển nhạc từ loa |
| F6 | Mèo ảo | Nuôi một con mèo bằng chạm: đói, vui, sạch, ngủ; lớn dần theo thời gian; có tiếng meo |

**Không làm:** AI/nhận giọng nói, Bluetooth audio (ESP32-S3 không có BT Classic), app điện thoại riêng.

### Yêu cầu chung
- Chạy ổn định 24/7, tự kết nối lại Wi-Fi/stream khi rớt.
- Giao diện **tiếng Việt có dấu** (mặc định), có thể chuyển sang **tiếng Anh**. Chuỗi giao diện nằm trong một bảng
  `i18n` nên thêm ngôn ngữ dễ. Font tiếng Việt chỉ lấy dải ký tự cần (khoảng 230 ký tự), 3 cỡ chữ tốn khoảng 300 KB
  trong phân vùng storage 5.4 MB, nên không nặng. Nếu bị chậm hay thiếu RAM thì giao diện tiếng Anh vẫn là phương án dự phòng.
- Mọi cấu hình lưu NVS, giữ qua khởi động lại và cập nhật firmware.
- Luôn quay về được firmware gốc bằng `backup/speaker_full_16MB.bin`.

---

## 2. Kiến trúc

```
┌──────────────────────────── ESP32-S3 (firmware/) ────────────────────────────┐
│                                                                              │
│  UI (LVGL 9)          ┌─ Clock ─┬─ Radio ─┬─ PC ─┬─ Pet ─┐  ← vuốt trái/phải  │
│                       └─────────┴─────────┴──────┴───────┘                    │
│                       Overlay: thông báo, âm lượng, báo thức                  │
│         ▲ events                                                             │
│  ┌──────┴──────────────── app core (event bus) ─────────────────────────┐    │
│  │ settings(NVS) │ clock/lunar │ weather │ alarm │ pet sim │ power/batt │    │
│  └──────┬──────────────────────────────────────────────────────┬────────┘    │
│         │                                                      │             │
│  audio service                                   net services                │
│  ├ radio player: HTTP → ringbuf(PSRAM) → decoder │ Wi-Fi STA / SoftAP portal  │
│  ├ sfx player: chuông, tiếng thú (từ LittleFS)   │ HTTP server: web UI + API  │
│  └ mixer + ducking → I2S → ES8311 → loa         │ WebSocket /ws/pc           │
│                                                  │ SNTP, mDNS (meowbox.local)  │
│  drivers: NV3023 LCD, CST816S touch, ES8311/ES7210, buttons, PA, backlight   │
└──────────────────────────────────────────────────────────────────────────────┘
          ▲ WebSocket (Wi-Fi)                         ▲ HTTP POST /api/notify
┌─────────┴────────── PC (pc-agent/) ──────┐    curl / điện thoại / script / ntfy
│ psutil + LibreHardwareMonitor (nhiệt độ) │
│ Windows Media Session (bài đang phát)    │
│ nhận lệnh play/pause/next/prev           │
│ gửi thông báo (tuỳ chọn: TTS Windows)    │
└──────────────────────────────────────────┘
```

### Stack kỹ thuật

| Phần | Chọn | Ghi chú |
|---|---|---|
| SDK | ESP-IDF **6.1** (`release/v6.1`) | |
| UI | LVGL 9 + `esp_lvgl_port` | double buffer trong PSRAM, 296×240 RGB565 |
| LCD | `78/esp_lcd_nv3023` + `hw/nv3023_init_cmds.h` | thông số lấy từ board xiaozhi đã chạy |
| Touch | `esp_lcd_touch_cst816s`, polling 20 ms | đã xác minh: toạ độ khớp màn hình |
| Codec | `esp_codec_dev` (ES8311 out, ES7210 in) | như `BoxAudioCodec` của xiaozhi |
| Giải mã | `espressif/esp_audio_codec` (MP3, AAC, WAV) | HLS tự viết: tải m3u8 → TS → tách AAC |
| Web | `esp_http_server` + trang tĩnh (HTML/JS nhỏ, gzip) trong LittleFS | không framework nặng |
| FS | LittleFS trên phân vùng `storage` | font, âm thanh, sprite thú, web |
| Font | `lv_font_conv` → Noto Sans/Be Vietnam Pro, dải Latin + tiếng Việt, 14/20/48 px | font số to cho đồng hồ |
| PC agent (Windows) | Python 3: `psutil`, `websockets`, `winsdk` (Media Session), WMI/HTTP của LibreHardwareMonitor, `yt-dlp` + `ffmpeg` (YouTube relay) | đóng gói `.exe` bằng PyInstaller (tuỳ chọn) |

### Bố cục thư mục mới

```
firmware/                     ESP-IDF project
  main/                       app_main, khởi tạo, event bus
  components/
    board/                    pin (từ hw/), LCD, touch, codec, nút, pin, đèn nền
    ui/                       LVGL: các màn + overlay
    audio/                    player, decoder, mixer, sfx
    net/                      wifi, portal, http server, api, ws, sntp, mdns
    apps/clock/  apps/radio/  apps/pc/  apps/pet/  apps/notify/
    lunar/                    lịch âm (thuật toán Hồ Ngọc Đức, múi giờ +7)
  storage/                    nội dung LittleFS (web/, sounds/, pet/, fonts/)
  partitions.csv
pc-agent/                     app PC (Python)
docs/plan.md                  file này
```

### Phân vùng flash (16 MB)

Giữ vị trí hai slot app như firmware gốc; vùng assets cũ được chia làm hai, vì NVS chỉ 16 KB không đủ chứa cấu hình.

| Tên | Offset | Size | Dùng cho |
|---|---|---|---|
| nvs | 0x9000 | 16 KB | driver Wi-Fi, PHY |
| otadata | 0xd000 | 8 KB | |
| phy_init | 0xf000 | 4 KB | |
| ota_0 | 0x20000 | 5.25 MB | firmware |
| ota_1 | 0x560000 | 5.25 MB | firmware (OTA qua web, có rollback) |
| storage | 0xaa0000 | 4.375 MB | LittleFS chỉ đọc: web, web font, ảnh xem trước theme |
| data | 0xf00000 | 1 MB | LittleFS: cấu hình, danh sách Wi-Fi, mật khẩu, lưu mèo, log pin |

Firmware hiện khoảng 2.8 MB (kèm font của 4 theme), còn trống gần một nửa slot.

---

## 3. Điều khiển

| Thao tác | Tác dụng |
|---|---|
| Vuốt trái / phải | đổi màn: Đồng hồ ↔ Radio ↔ PC ↔ Mèo ↔ Hệ thống |
| Chạm | thao tác trên màn hiện tại (nút, danh sách); chạm mèo ở góc đồng hồ để mở màn Mèo |
| Nút GPIO40 / 39 | âm lượng +/− 5 (giữ để chỉnh liên tục) |
| Nút giữa (GPIO1) bấm | tắt báo thức · đóng thông báo · phát/dừng radio |
| Nút giữa bấm 2 lần | về màn Đồng hồ |
| Nút giữa giữ lâu | **tắt nguồn** (phần cứng, firmware không chặn được) |
| Cài Wi-Fi | nút "Wi-Fi" trên màn Hệ thống, giữ cùng lúc VOL+ và VOL− 3 giây, trang Wi-Fi trên web, hoặc `agent.py wifi-setup` mở SoftAP `MeowBox-XXXX` (mật khẩu `meowbox2026`). Tự tắt 10 giây sau khi nối mạng, hoặc 10 phút không ai vào |
| Báo thức kêu | chạm màn hình = báo lại 5 phút, nút âm lượng = báo lại, nút giữa = tắt |

Sau 60 s không thao tác: giảm độ sáng. Ban đêm (giờ cấu hình): chế độ đêm theo theme đang dùng.

---

## 4. Chi tiết từng tính năng

### F1 Đồng hồ
- SNTP (`pool.ntp.org`, `time.google.com`), TZ `ICT-7`. Mất mạng thì vẫn chạy theo RTC nội.
- Màn: giờ:phút to, giây nhỏ; thứ, ngày dương; **ngày âm** (vd "Mùng 3 tháng Chạp, Ất Tỵ"); thời tiết hiện tại + nhiệt độ cao/thấp hôm nay; icon pin, Wi-Fi.
- Thời tiết: `api.open-meteo.com/v1/forecast` mỗi 30 phút. Vị trí đặt bằng tên thành phố, tra qua open-meteo geocoding trên trang web.
- Báo thức: tối đa 5, chọn ngày trong tuần, âm thanh = chuông hoặc một đài radio (nếu stream lỗi thì dùng chuông).
- Hẹn giờ tắt radio: 15/30/60/90 phút.
- Tuỳ chọn sau: Pomodoro.

### F2 Radio
- Danh sách đài lưu JSON trong NVS/LittleFS, sửa qua web. Mặc định có vài đài MP3 công khai để test.
- Pipeline: HTTP client (hỗ trợ ICY metadata) → ring buffer 256 KB trong PSRAM → decoder → resample nếu cần → mixer → I2S.
- Hiện: tên đài, tên bài (ICY `StreamTitle`), trạng thái buffer, nút trước/sau/play.
- Tự kết nối lại khi rớt (backoff 1, 2, 5, 10 s).
- **Giai đoạn sau: HLS** (`.m3u8`, như VOV `audio-lss.vov.vn`): tải playlist, tải segment `.ts`, tách AAC ADTS rồi đưa vào decoder.
- Nhớ đài và âm lượng cuối cùng.
- **Tìm đài trên web:** trang cấu hình tra [Radio Browser](https://www.radio-browser.info) (API miễn phí, khoảng
  50k đài, lọc theo nước/thể loại, có link stream trực tiếp), bấm một cái là thêm vào danh sách.

#### YouTube
ESP32 **không phát trực tiếp YouTube được**. Lý do:
- YouTube không có link stream cố định. Link phải lấy qua bước giải mã chữ ký bằng JavaScript (việc của yt-dlp).
- Link lấy được chỉ sống vài giờ.
- Định dạng là Opus/AAC trong DASH, chỉ thỉnh thoảng mới có HLS.

Cách làm: **PC agent làm cầu nối (relay)**
```
Web MeowBox: dán link YouTube (video, playlist, live như lofi) ──► PC agent
PC agent: yt-dlp lấy audio ──► ffmpeg chuyển thành MP3 128 kbps ──► HTTP stream http://<pc>:8765/yt/<id>.mp3
MeowBox: phát stream đó như một đài radio bình thường (có tên bài, nút next cho playlist)
```
- Chỉ chạy khi PC bật và agent đang chạy. Tắt PC thì radio thường vẫn chạy được.
- Dùng cho cá nhân. yt-dlp phải cập nhật thường xuyên vì YouTube hay đổi; agent tự chạy `yt-dlp -U`.
- Lưu "đài YouTube" như đài thường (lofi live, playlist yêu thích).
- Sau này có máy chạy 24/7 (Raspberry Pi, NAS) thì chạy relay ở đó, khỏi phụ thuộc PC.

### F3 Trang web cấu hình
- Lần đầu (chưa có Wi-Fi): SoftAP `MeowBox-XXXX` (WPA2, mật khẩu `meowbox2026`, hiện trên màn loa) + captive portal → chọn mạng, nhập mật khẩu.
- Sau đó: `http://meowbox.local` (mDNS) hoặc IP hiện trên màn.
- Các trang: Tổng quan (trạng thái, pin, IP, version) · Wi-Fi · Radio (thêm/xoá/sắp xếp đài, test phát) · Báo thức · Thời tiết/vị trí · Hiển thị (độ sáng, giờ đêm) · Thú ảo (tên, reset) · Thông báo (token API, test) · Hệ thống (OTA upload `.bin`, khởi động lại, xoá cấu hình).
- **Bảo vệ bằng mật khẩu, bật sẵn.**
  - Mặc định: user `meowbox`, mật khẩu **`m3owb0x2026`** (trước 2026-11-23 là `admin` / `meowbox`; loa còn mật khẩu cũ tự chuyển sang mặc định mới).
  - Còn dùng mật khẩu mặc định thì web hiện banner nhắc đổi.
  - Đăng nhập bằng cookie session (HTTP Basic chỉ dùng cho API).
  - Mật khẩu lưu dạng hash (SHA-256 + salt) trong NVS.
  - Quên mật khẩu: giữ BOOT + vol− lúc khởi động để reset về mặc định.
- API thông báo dùng token riêng, tạo ngẫu nhiên lần đầu, xem và đổi trên web.
- Backend: REST JSON (`GET/PUT /api/config/<mục>`). Frontend: một file HTML + JS thuần, gzip.

### F4 Loa thông báo
```
POST /api/notify
Authorization: Bearer <token>
{ "title": "Build xong", "text": "esp32-playground OK", "sound": "chime" | "alert" | "none",
  "priority": "low" | "normal" | "high", "duration": 10 }

POST /api/notify/audio   (body = file WAV/MP3, ≤ 1 MB)   phát một file âm thanh
GET  /api/notify?text=...&token=...                     dạng ngắn cho script/điện thoại
```
- Hiện overlay trên màn hiện tại. Radio tự hạ nhỏ (ducking) trong lúc phát tiếng thông báo.
- Hàng đợi 10 tin; giữ lịch sử 20 tin gần nhất (xem lại trên web).
- `high`: tự bật màn, kêu cả khi ở chế độ đêm. `low`: không kêu trong giờ đêm.
- Ví dụ dùng: `curl` cuối script build, Task Scheduler, iOS Shortcuts / Android HTTP Shortcuts.
- Tuỳ chọn: subscribe một topic **ntfy.sh** để nhận thông báo khi không cùng mạng LAN.

### F5 Màn phụ PC
- Loa mở WebSocket server `/ws/pc`. PC agent tự tìm loa qua mDNS, hoặc dùng IP cấu hình.
- Tin PC → loa, mỗi 1 s:
  ```json
  {"t":"stats","cpu":23,"ram":61,"gpu":12,"cpu_temp":54,"gpu_temp":47,"net_up":120,"net_down":3400,"disk":71}
  {"t":"media","app":"Spotify","title":"...","artist":"...","state":"playing","pos":73,"len":215}
  {"t":"notify", ...giống /api/notify...}
  ```
- Tin loa → PC: `{"t":"media_cmd","cmd":"play_pause"|"next"|"prev"|"vol_up"|"vol_down"}`.
- Màn hiển thị: 4 vòng/thanh (CPU, RAM, GPU, nhiệt độ), mạng lên/xuống, card nhạc có nút ⏮ ⏯ ⏭.
- Khi PC tắt: màn hiện "PC offline", lúc vuốt qua thì bỏ qua màn này (tuỳ chọn).
- **PC agent** (`pc-agent/agent.py`):
  - `psutil`: CPU, RAM, mạng, ổ đĩa.
  - Nhiệt độ: đọc web server JSON của LibreHardwareMonitor (`localhost:8085/data.json`) nếu đang chạy; không có thì bỏ trống.
  - Nhạc: `winsdk` → `GlobalSystemMediaTransportControlsSessionManager` (Spotify, Chrome, Edge, foobar... đều được).
  - Chạy nền khi khởi động Windows (Task Scheduler hoặc shortcut trong Startup). Có icon khay (tuỳ chọn).
  - CLI gửi thông báo: `python agent.py notify "Build xong"`.
  - Tuỳ chọn: dùng TTS của Windows (giọng tiếng Việt "An") tạo WAV rồi gửi `/api/notify/audio` để loa **đọc** thông báo.

### F6 Mèo ảo
- Một **con mèo** pixel art (khoảng 64×64, phóng to ×2), sprite trong LittleFS. Vẽ mới hoàn toàn, không lấy hình của game có sẵn.
- Hoạt cảnh: ngồi, đi, ngủ cuộn tròn, liếm lông, đòi ăn, vờn len, giận (xù lông). Tiếng meo, rừ rừ khi được vuốt ve.
- Chọn màu lông trên web (mướp, đen, trắng, tam thể…), đặt tên.
- Chỉ số 0–100: **No, Vui, Sạch, Năng lượng**, cộng thêm **Sức khoẻ** và **Tuổi**. Giảm dần theo thời gian thật, kể cả lúc tắt máy (tính bù theo giờ SNTP khi khởi động).
- Tương tác: nút Cho ăn / Chơi / Tắm / Ngủ (tắt đèn). Vuốt vuốt lên con thú = vuốt ve (+Vui).
- Mini game đơn giản: chạm đúng lúc, hoặc đoán trái/phải.
- Trạng thái: vui, buồn, đói, ốm, ngủ. Ốm lâu thì có thể "bỏ đi" (mức độ khó chỉnh trên web).
- Lớn dần: mèo con → mèo nhỡ → mèo trưởng thành → mèo già, theo số ngày được chăm tốt.
- Tiếng kêu ngắn (sfx). Đói hoặc ốm thì kêu nhắc, tôn trọng giờ đêm.
- Lưu trạng thái vào NVS mỗi 5 phút và khi có thay đổi lớn.
- Tuỳ chọn vui: mic nghe tiếng vỗ tay → thú giật mình hoặc nhảy (dùng ES7210, chỉ đo âm lượng, không ghi âm).

---

## 5. Giai đoạn và tiêu chí xong

Mỗi giai đoạn kết thúc bằng một bản chạy được trên loa thật.

### P0 Nền tảng
- [x] Tạo `firmware/` (ESP-IDF 6.1), partition table, sdkconfig (PSRAM octal 80 MHz, flash QIO 16 MB, CPU 240 MHz, console USB-Serial/JTAG)
- [x] Component `board`: LCD + LVGL, touch, nút, đèn nền PWM, PA, đọc pin + sạc
- [x] Phát tiếng beep/WAV qua ES8311; thu mic ES7210 và in mức âm lượng ra log
- [x] Bộ font tiếng Việt, màn test "Xin chào" có dấu
- [x] Vuốt trái/phải giữa các màn, chạm, giữ (vuốt lên/xuống chưa dùng)

**Xong khi:** màn hiện đúng chiều và đúng màu; vuốt đổi được 4 màn trống; loa kêu; mic có số; xác định được nút nào là vol+; xác nhận chân đọc pin. Các mục `[UNVERIFIED]` trong docs được cập nhật.

### P1 Mạng + trang web
- [x] Wi-Fi STA, SoftAP + captive portal, lưu nhiều mạng
- [x] SoftAP tự tắt (10 giây sau khi nối mạng; 10 phút không ai vào nếu mở tay), bật lại bằng nút, web, PC agent
- [x] mDNS `meowbox.local`, SNTP
- [x] HTTP server, LittleFS, trang web khung + REST `/api/config/*`
- [x] OTA upload qua web (ghi vào slot OTA còn lại, có rollback)

**Xong khi:** cài Wi-Fi từ điện thoại không cần cắm máy tính; đổi cấu hình trên web thì loa áp dụng ngay; cập nhật được firmware qua web.

### P2 Đồng hồ
- [x] Màn đồng hồ, lịch âm (có test so với lịch tham khảo cho vài năm), thời tiết
- [x] Báo thức (chuông) + báo lại, hẹn giờ tắt, chế độ đêm, giảm sáng
- [x] Trang web: báo thức, vị trí thời tiết, hiển thị

**Xong khi:** để chạy 24 h không treo, giờ đúng, ngày âm đúng, báo thức kêu đúng giờ.

### P3 Radio
- [x] Player MP3/AAC qua HTTP, ICY metadata, ring buffer PSRAM, tự kết nối lại
- [x] Nhớ đài/âm lượng; âm báo **tạm dừng** radio thay vì trộn tiếng (chưa có mixer)
- [x] Màn radio, trang web quản lý đài
- [x] Báo thức bằng radio
- [x] HLS (VOV)

**Xong khi:** nghe liên tục 2 h không giật; đổi đài dưới 3 s; rút router rồi cắm lại thì tự phát tiếp.

### P4 Loa thông báo
- [x] `/api/notify` (JSON + GET ngắn), token, hàng đợi, lịch sử, overlay, sfx, ưu tiên, giờ đêm
- [x] `/api/notify/audio`
- [ ] (tuỳ chọn) ntfy.sh subscribe

**Xong khi:** `curl` từ PC hoặc điện thoại làm loa kêu và hiện chữ dưới 1 s, kể cả đang phát radio.

### P5 Màn phụ PC
- [x] WebSocket `/ws/pc` trên loa, màn PC
- [x] `pc-agent`: stats, media, nhận lệnh, tự tìm loa, chạy cùng Windows
- [x] CLI notify; (tuỳ chọn) TTS Windows
- [x] YouTube relay (yt-dlp + ffmpeg → MP3 HTTP), trang web dán link, lưu thành đài

**Xong khi:** số liệu cập nhật mỗi giây; bấm ⏯ trên loa thì Spotify/YouTube dừng hoặc phát; tắt agent thì màn báo offline.

### P6 Thú ảo
- [x] Vẽ sprite mèo (4 độ tuổi × các hoạt cảnh, vài màu lông), tiếng meo và rừ rừ
- [x] Mô phỏng chỉ số + bù thời gian khi tắt máy, lưu NVS
- [x] Tương tác, lớn dần theo ngày chăm tốt, nhắc nhở
- [ ] Mini game
- [x] Trang web: tên, độ khó, reset

**Xong khi:** nuôi thật 3 ngày thấy hợp lý (không chết quá nhanh, không quá nhàm).

### P7 Hoàn thiện
- [x] Đo thời lượng pin (log mỗi phút, phiên dùng pin, ước tính còn lại, `/api/battery`)
- [ ] Chạy pin tới cạn một lần để chỉnh thang %
- [x] Tắt màn khi không dùng (`display.sleep_s`, mặc định 5 phút): tắt đèn nền và panel, ngừng vẽ, tắt mic, tắt ampli
  khi im 3 s, CPU 80 MHz khi không phát (DFS)
- [ ] Light sleep tự động khi màn tắt và không phát (cần đóng I2S và giãn các timer LVGL/touch)
- [ ] Watchdog, log lỗi lưu lại xem trên web
- [x] Cập nhật docs, README (firmware/README.md, docs/)
- [x] Hệ thống theme (Swiss, Cyberpunk, Zen, Brutalism): xem [ui_redesign.md](ui_redesign.md)

---

## 6. Rủi ro và cách xử lý

| Rủi ro | Xử lý |
|---|---|
| RAM nội hạn chế khi chạy cùng lúc Wi-Fi, TLS, LVGL, decoder | buffer LVGL và audio để trong PSRAM; ưu tiên stream HTTP thường; TLS chỉ dùng cho thời tiết và OTA |
| Stream radio HTTPS / HLS phức tạp | P3 làm MP3/AAC HTTP trước, HLS làm sau cùng |
| Chân đọc pin (GPIO17) chưa chắc | kiểm tra ở P0; nếu sai thì quét các kênh ADC khi đang sạc và khi rút sạc |
| Màn NV3023 sai chiều hoặc màu | thông số đã chạy được ở xiaozhi; chỉnh mirror/BGR ở P0 |
| Font tiếng Việt tốn flash | chỉ lấy dải ký tự cần; font to (48 px) chỉ chứa chữ số |
| Gói `winsdk` đổi API | phương án dự phòng: `pywinrt` (`winrt-Windows.Media.Control`) |

## 7. Còn phải quyết

- [x] Tên: **MeowBox** (mDNS `meowbox.local`, Wi-Fi AP `MeowBox-XXXX`)
- [x] Thú ảo: **mèo**
- [x] PC agent: **chỉ Windows** (giai đoạn này)
- [x] Trang web: **có mật khẩu mặc định** (`meowbox` / `m3owb0x2026`, nhắc đổi); AP cài đặt có mật khẩu `meowbox2026`
- [x] Ngôn ngữ: tiếng Việt mặc định, có tiếng Anh; nếu tiếng Việt gây vấn đề thì dùng tiếng Anh
- [x] YouTube: phát qua relay trong PC agent
- [ ] Danh sách đài radio mặc định fen hay nghe (thêm sau trên web cũng được)
