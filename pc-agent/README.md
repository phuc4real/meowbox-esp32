# MeowBox PC agent

App nhỏ chạy trên PC Windows, làm mấy việc này cho loa MeowBox:

- Gửi số liệu máy (CPU, RAM, GPU, nhiệt độ, mạng, ổ đĩa) lên loa mỗi giây, qua WebSocket `ws://<loa>/ws/pc`.
- Gửi bài đang phát (Spotify, Chrome, Edge...) và nhận lệnh ⏮ ⏯ ⏭ 🔉 🔊 từ loa.
- Gửi thông báo từ dòng lệnh (`notify`), hoặc đọc thành tiếng bằng TTS Windows (`say`).
- Làm **YouTube relay**: loa xin `http://<pc>:8765/yt/stream?url=...` thì agent dùng yt-dlp + ffmpeg đổi
  sang MP3 128 kbps rồi stream về như một đài radio.

## Cài

Cần Python 3.11 trở lên (đã test với 3.14), cộng thêm `ffmpeg` và `yt-dlp` nằm trong PATH.

```powershell
cd pc-agent
python -m pip install --user -r requirements.txt
```

Nhiệt độ CPU/GPU thì Windows không cho đọc trực tiếp. Muốn có thì chạy
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor), bật
*Options → Remote Web Server* (cổng 8085). Agent tự đọc `http://localhost:8085/data.json`. Không có thì các ô đó để `null`.
Máy có card NVIDIA thì agent cũng đọc `nvidia-smi` để lấy GPU %.

## Cấu hình

File nằm ở `%APPDATA%\MeowBox\agent.json`:

```powershell
python agent.py config --token <token lấy trên web loa>
python agent.py config --set-host 192.168.1.50    # không bắt buộc, bỏ trống thì tự tìm qua mDNS
python agent.py config --set-host ""              # quay lại tự tìm
python agent.py config                            # xem cấu hình
```

Agent tìm loa theo thứ tự: `--host`, rồi `host` trong config, rồi mDNS `_meowbox._tcp`, cuối cùng là `meowbox.local`.
Host có thể kèm cổng, ví dụ `127.0.0.1:8080`.

## Chạy

```powershell
python agent.py                 # = python agent.py run
python agent.py --host 192.168.1.50 -v
python agent.py run --no-relay  # không mở YouTube relay
```

Mất kết nối thì agent tự nối lại (chờ 1, 2, 4... tối đa 30 s). Lúc kết nối xong, agent gửi
`{"t":"hello","relay":"http://<ip-pc>:8765"}` để loa biết địa chỉ relay.

Mỗi ngày agent chạy `yt-dlp -U` một lần (lỗi thì kệ, ví dụ khi cài yt-dlp bằng winget). Không muốn thì thêm `--no-update`.

### Thông báo

```powershell
python agent.py notify "Build xong" "esp32-playground OK" --sound chime --priority high
python agent.py say "Build xong rồi nha"            # TTS → WAV → /api/notify/audio
python agent.py say "test" --save test.wav           # chỉ lưu file, không gửi
```

`say` ưu tiên giọng tiếng Việt (Microsoft An) nếu máy có. Chưa có thì vào *Settings → Time & language →
Speech → Add voices* cài gói tiếng Việt. Không có thì nó dùng giọng mặc định.

### YouTube relay

```
GET http://<pc>:8765/yt/info?url=<link>                → JSON: type, title, duration, is_live, entries (playlist)
GET http://<pc>:8765/yt/stream?url=<link>              → audio/mpeg (chunked), header icy-name = tên bài
GET http://<pc>:8765/yt/stream?url=<playlist>&index=N  → bài thứ N trong playlist (đếm từ 0)
```

Stream có thêm header `X-Duration`, `X-Is-Live`, `X-Playlist-Index`, `X-Playlist-Count`. Client ngắt là agent kill yt-dlp + ffmpeg luôn.

Lần đầu chạy, Windows Firewall sẽ hỏi có cho Python mở cổng không. Chọn **Private network**. Lỡ bấm nhầm thì mở bằng tay (PowerShell admin):

```powershell
netsh advfirewall firewall add rule name="MeowBox relay" dir=in action=allow protocol=TCP localport=8765 profile=private
```

## Tự chạy khi bật máy

```powershell
powershell -ExecutionPolicy Bypass -File .\install_autostart.ps1            # Task Scheduler, lúc đăng nhập
powershell -ExecutionPolicy Bypass -File .\install_autostart.ps1 -Startup   # hoặc shortcut trong shell:startup
powershell -ExecutionPolicy Bypass -File .\install_autostart.ps1 -Uninstall
```

Chạy bằng `pythonw.exe` nên không hiện cửa sổ. Log ghi vào `%APPDATA%\MeowBox\agent.log`.

## Test không cần loa (mock)

`mock_speaker.py` giả làm loa: có `/ws/pc`, `/api/notify`, `/api/notify/audio`, và quảng bá mDNS `_meowbox._tcp`.

```powershell
# cửa sổ 1: loa giả, token "test", cổng 8080
python mock_speaker.py              # thêm --quiet để ẩn tin stats

# cửa sổ 2: agent (tự tìm mock qua mDNS)
python agent.py --config test.json config --token test
python agent.py --config test.json

# cửa sổ 3: thử
curl "http://127.0.0.1:8080/mock/cmd?cmd=vol_up"            # loa giả gửi media_cmd cho agent
python agent.py --config test.json notify "Xin chào" "test"
curl -X POST http://127.0.0.1:8080/api/notify -H "Authorization: Bearer test" -H "Content-Type: application/json" -d "{\"title\":\"hi\"}"
curl "http://127.0.0.1:8765/yt/info?url=https://www.youtube.com/watch?v=jNQXAC9IVRw"
curl -o test.mp3 "http://127.0.0.1:8765/yt/stream?url=https://www.youtube.com/watch?v=jNQXAC9IVRw"
```

## Giới hạn

- Chỉ chạy trên Windows (Media Session, pycaw, SAPI).
- Không có LibreHardwareMonitor hay nvidia-smi thì không có GPU % và nhiệt độ (gửi `null`).
- `pos`/`len` của bài hát tuỳ app. Chrome với video live thì báo 0.
- Relay chỉ dùng cá nhân. YouTube đổi cách chặn thì phải đợi yt-dlp bản mới.
