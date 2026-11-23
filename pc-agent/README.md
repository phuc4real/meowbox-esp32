# MeowBox PC agent

App nhỏ chạy trên PC Windows, làm mấy việc này cho loa MeowBox:

- Gửi số liệu máy (CPU, RAM, GPU, nhiệt độ, mạng, ổ đĩa) lên loa mỗi giây, qua WebSocket `ws://<loa>/ws/pc`.
- Gửi bài đang phát (Spotify, Chrome, Edge...) và nhận lệnh ⏮ ⏯ ⏭ 🔉 🔊 từ loa.
- Gửi thông báo từ dòng lệnh (`notify`), hoặc đọc thành tiếng bằng TTS Windows (`say`).
- Bật lại Wi-Fi cài đặt `MeowBox-XXXX` trên loa (`wifi-setup`).
- Làm **YouTube relay**: loa xin `http://<pc>:8765/yt/stream?url=...` thì agent dùng yt-dlp + ffmpeg đổi
  sang MP3 128 kbps rồi stream về như một đài radio. Chỉ loa đang nối mới dùng được relay, máy khác nhận 403.

## Cài bằng file cài đặt (khuyên dùng)

Tải `MeowBoxAgent-Setup-x.y.z.exe` ở [Releases](https://github.com/phuc4real/meowbox-esp32/releases), chạy, bấm Next.
Không cần Python. Bộ cài:

- cài vào `Program Files\MeowBox Agent` (hỏi quyền admin), có shortcut ở Start Menu;
- mở tường lửa cho agent trên mạng **Private** (relay, mDNS, gói thông báo của loa);
- tùy chọn: chạy cùng Windows (mặc định bật), thêm `meowbox.exe` vào PATH;
- tải yt-dlp bản mới nhất về `%LOCALAPPDATA%\MeowBox\bin` (không đóng gói kèm, để luôn cập nhật được);
- gỡ bản cũ chạy bằng Python (task "MeowBox Agent" trong Task Scheduler, shortcut Startup), giữ cấu hình cũ.

Cài xong agent chạy luôn, hiện icon con mèo ở khay hệ thống, và lần đầu tự mở cửa sổ **Kết nối loa**:

1. **Tìm loa**: loa trong mạng tự hiện (mDNS và broadcast). Không thấy thì nhập IP hiện trên tile Hệ thống của loa.
2. **Token**: dán token ở trang Thông báo của loa (cần cho `notify`, `say`, `wifi-setup`; có thể bỏ qua).
3. **Kiểm tra**: loa thử gọi relay trên máy này. Lỗi trên mạng Windows xếp là Public thì có nút
   "Cho phép trên mạng này" (hỏi quyền admin, chỉ thêm rule cho đúng loại mạng đó).

Chấm trên icon: xanh = đã nối loa, xám = đang tìm, đỏ = lỗi (di chuột vào icon để xem). Menu chuột phải có
mở trang cài đặt của loa, gửi thông báo thử, tạm dừng gửi số liệu, kết nối loa, tìm lại ngay, chạy cùng Windows,
thư mục log. Loa đang nối với PC khác thì có "Giành quyền nối loa". Có agent mới trên GitHub thì menu hiện
"Có bản x.y.z".

Agent tìm loa theo thứ tự: IP lần trước, IP nhập tay, mDNS `_meowbox._tcp`, rồi UDP (hỏi `MEOWBOX?` cổng 8766 và
nghe gói thông báo loa phát mỗi 2 giây ở cổng 8767). Nó nhớ loa theo **tên** (`MeowBox-1A2B`), nên nhà có hai loa không
nối nhầm, loa đổi IP vẫn tìm ra. Mỗi lúc một loa chỉ nhận một PC.

Cấu hình: `%APPDATA%\MeowBox\agent.json`. Log: `%APPDATA%\MeowBox\agent.log` (xoay vòng 1 MB × 3).

**Cài đặt…** (menu tray): phiên bản và cập nhật của agent (tải bộ cài mới rồi tự chạy), yt-dlp, ffmpeg; tự kiểm tra
mỗi ngày; chạy cùng Windows; cổng relay; thư mục log.

### YouTube

- Video thường: yt-dlp tải audio AAC (m4a, itag 140, ~128 kbps), agent bỏ vỏ MP4 thành ADTS cho loa. Không mã hoá lại,
  không cần ffmpeg.
- Live: yt-dlp không tải luồng live HLS nếu không có ffmpeg, nên lần đầu phát live agent tự tải ffmpeg bản LGPL
  (BtbN `win64-lgpl-shared`, ~75 MB, kiểm tra SHA-256) về `%LOCALAPPDATA%\MeowBox\bin\ffmpeg`. Âm thanh chỉ được chép
  nguyên (AAC trong MPEG-TS). Trong lúc tải loa tự thử lại, tray hiện thông báo. Máy có ffmpeg trong PATH thì dùng luôn.
- yt-dlp là bản zip thuần Python (~3 MB) chạy bằng `meowbox.exe ytdlp …`; xem/cập nhật: `meowbox tools status|update`.

## Dòng lệnh

Sau khi cài (và chọn thêm vào PATH), hoặc `python agent.py ...` khi chạy từ source:

```powershell
meowbox notify "Build xong" "esp32-playground OK" --sound chime --priority high
meowbox say "Build xong rồi nha"            # TTS Windows → WAV → /api/notify/audio
meowbox say "test" --save test.wav           # chỉ lưu file, không gửi
meowbox wifi-setup [on|off|status]           # Wi-Fi cài đặt MeowBox-XXXX (mật khẩu meowbox2026)
meowbox discover                             # liệt kê loa tìm được
meowbox config --token <token> --speaker MeowBox-1A2B
meowbox run                                  # agent không có icon (như bản cũ), --no-relay, --no-update
```

`say` ưu tiên giọng tiếng Việt (Microsoft An) nếu máy có. Chưa có thì vào *Settings → Time & language →
Speech → Add voices* cài gói tiếng Việt.

Nhiệt độ CPU/GPU: chạy [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor), bật
*Options → Remote Web Server* (cổng 8085). Máy có card NVIDIA thì agent cũng đọc `nvidia-smi`.

## Chạy từ source

Python 3.11+ (đã test 3.14). yt-dlp: `python agent.py tools update` (vào `%LOCALAPPDATA%\MeowBox\bin`).

```powershell
cd pc-agent
python -m pip install --user -r requirements.txt
python agent.py tray        # icon ở khay hệ thống
python agent.py run -v      # không icon, log ra console
```

Code nằm trong `meowbox_agent/` (xem docstring của `meowbox_agent/__init__.py`). `agent.py` chỉ gọi `cli.main()` để
các lệnh cũ vẫn chạy.

### YouTube relay

```
GET http://<pc>:8765/yt/info?url=<link>                → JSON: type, title, duration, is_live, entries (playlist)
GET http://<pc>:8765/yt/stream?url=<link>              → audio/aac (live: video/mp2t), header icy-name = tên bài
GET http://<pc>:8765/yt/stream?url=<playlist>&index=N  → bài thứ N trong playlist (đếm từ 0)
GET http://<pc>:8765/yt/ping                           → {"ok":true} (loa dùng cho relay_test)
```

Chỉ IP của loa đang nối được phép, máy khác nhận 403.

## Build file cài đặt

Cần PyInstaller (`pip install --user pyinstaller`) và Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1          # icon + PyInstaller (MeowBoxAgent.exe + meowbox.exe) + Inno Setup
.\build.ps1 -Venv    # build trong .venv sạch
```

Ra `packaging\build\MeowBoxAgent-Setup-<version>.exe` (~20 MB, không kèm yt-dlp hay ffmpeg). Phiên bản lấy từ
`meowbox_agent/__init__.py` (`__version__`, chung số với firmware).

## Test không cần loa (mock)

`mock_speaker.py` giả làm loa: `/ws/pc` (một PC mỗi lúc, `busy`, `take`), `/api/ping`, `/api/notify*`,
`/api/pc/relay_test`, mDNS và UDP như firmware.

```powershell
python mock_speaker.py --port 8081 --name MeowBox-AAAA
python mock_speaker.py --port 8082 --name MeowBox-BBBB --no-udp     # loa thứ hai, chỉ mDNS
python agent.py discover
python agent.py --config test.json config --token test --speaker MeowBox-AAAA
python agent.py --config test.json run
curl "http://127.0.0.1:8081/mock/cmd?cmd=vol_up"     # loa giả gửi media_cmd cho agent
```

## Giới hạn

- Chỉ chạy trên Windows (Media Session, pycaw, SAPI).
- Không có LibreHardwareMonitor hay nvidia-smi thì không có GPU % và nhiệt độ (gửi `null`).
- `pos`/`len` của bài hát tuỳ app. Chrome với video live thì báo 0.
- Relay chỉ dùng cá nhân. YouTube đổi cách chặn thì cập nhật yt-dlp trong Cài đặt (hoặc chờ lần tự kiểm tra hằng ngày).
- yt-dlp cảnh báo thiếu runtime JavaScript (deno): một số định dạng YouTube có thể thiếu. Video thường vẫn có AAC.
- Hai agent cùng chạy trên một máy (ví dụ `meowbox run` khi bản tray đang chạy) sẽ giành loa qua lại.
- File cài đặt chưa ký số: SmartScreen sẽ báo "Unknown publisher", bấm *More info → Run anyway*.
