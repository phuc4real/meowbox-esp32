# Plan: MeowBox Agent có tray icon và file cài đặt

## Hiện trạng
- Agent là một file `pc-agent/agent.py` (asyncio), phải cài Python và pip, chạy bằng `pythonw` qua Task Scheduler
  (`install_autostart.ps1`). Không có UI nên không biết nó đang chạy hay đã chết, chỉ xem được trong `agent.log`.
- ffmpeg và yt-dlp đang lấy từ PATH.
- Máy chưa có Inno Setup hay NSIS. Repo chưa có git remote nên chưa biết link GitHub.

## Kết quả mong muốn
Có một file `MeowBoxAgent-Setup-1.0.0.exe`. Cài xong là agent tự chạy và hiện icon con mèo ở system tray,
kể cả khi máy không có Python.

## 1. Tray app (`pystray` + Pillow)
- `pystray` chạy ở main thread, còn vòng asyncio hiện tại chạy ở một thread riêng. Hai bên dùng chung một
  object `AgentState` gồm: connected, host, lỗi cuối, số lần nối lại.
- `ws_loop` cập nhật `AgentState`. Khi Thoát thì hủy task, đóng relay và kill yt-dlp/ffmpeg đang chạy.
- Icon con mèo có thêm chấm màu: xanh là đã nối loa, xám là đang tìm loa, đỏ là lỗi (thiếu token, cổng 8765
  đang bị chiếm...).
- Hover vào icon thì hiện tooltip `MeowBox · 192.168.1.192 · đã kết nối`.

Menu chuột phải:

```
● Đã kết nối loa 192.168.1.192        (dòng trạng thái, không bấm được)
─────────────
Mở trang cài đặt MeowBox              (mặc định, double-click icon cũng mở) → http://<host>/
Gửi thông báo thử
Tạm dừng gửi số liệu        ☐
─────────────
Cấu hình agent…                       (cửa sổ tkinter nhỏ: host, token, cổng relay)
Chạy cùng Windows           ☑         (HKCU\...\Run)
Mở thư mục log
─────────────
GitHub ↗
Giới thiệu (v1.0.0)
Thoát
```

- Chỉ cho chạy một bản: dùng named mutex. Mở thêm bản thứ hai thì nó chỉ mở trang cài đặt rồi tự thoát, nhờ vậy
  không bị lỗi trùng cổng 8765.
- Lần đầu chạy mà chưa có token: tự mở cửa sổ Cấu hình và kèm nút "Mở web loa để lấy token".

## 2. Tách code
- `pc-agent/meowbox_agent/`:
  - `core.py`: phần logic hiện tại, sửa ít nhất có thể
  - `tray.py`
  - `config_dialog.py`
  - `cli.py`: giữ `notify`, `say`, `config`
  - `__version__`
- Vẫn chạy được từ source như cũ (`python -m meowbox_agent`) để tiện dev và dùng với `mock_speaker.py`.

## 3. Đóng gói bằng PyInstaller
- Dùng `--onedir` chứ không dùng onefile, vì onefile khởi động chậm hơn và hay bị antivirus báo nhầm.
- Một file spec tạo ra hai exe dùng chung thư mục `_internal`:
  - `MeowBoxAgent.exe`: windowed, có tray
  - `meowbox.exe`: bản console cho lệnh `meowbox notify "Build xong"` và `meowbox say ...`
- Build trong venv sạch, khai báo hidden-imports cho các module `winrt.*`, `comtypes` và `zeroconf`.
- Icon `.ico` nhiều cỡ (16 đến 256) tạo từ sprite mèo trong `firmware/components/cat_sprites`.
- Xử lý yt-dlp: lần đầu chạy thì copy `yt-dlp.exe` vào `%LOCALAPPDATA%\MeowBox\bin\`, vì thư mục này ghi được nên
  `yt-dlp -U` vẫn tự cập nhật hằng ngày. Agent ưu tiên PATH trước, sau đó mới tới bản kèm theo.

## 4. File cài đặt bằng Inno Setup 6
Cài bằng `winget install JRSoftware.InnoSetup`.

- Cài vào `Program Files\MeowBox Agent`, cần quyền admin để:
  - thêm firewall rule cho `MeowBoxAgent.exe` (profile Private), không còn bị Windows hỏi
  - tùy chọn thêm `meowbox.exe` vào PATH
- Có shortcut ở Start Menu và checkbox "Chạy cùng Windows" (mặc định bật). Cài xong thì chạy luôn.
- Lúc cài sẽ dọn bản cũ: gỡ task "MeowBox Agent" trong Task Scheduler và shortcut Startup, để không có hai agent
  chạy cùng lúc.
- Lúc gỡ: kill process, xóa Run key và firewall rule. Có hỏi có xóa `%APPDATA%\MeowBox` (token, log) hay không.
- `pc-agent/build.ps1` chạy một lệnh là ra file cài: tạo venv → PyInstaller → `iscc` →
  `dist\MeowBoxAgent-Setup-x.y.z.exe`.

## 5. Các giai đoạn

| | Việc | Xong khi |
|---|---|---|
| T1 | Tách code, làm tray và menu, cửa sổ cấu hình, single instance (chạy từ source) | Test với `mock_speaker.py` và loa thật: icon đổi màu đúng, Thoát sạch không để lại process ffmpeg nào |
| T2 | PyInstaller và kèm yt-dlp/ffmpeg | Exe chạy được trên máy không có Python: stats, media, relay YouTube, `say` đều OK |
| T3 | Inno Setup và `build.ps1` | Cài, gỡ, cài lại đều sạch. Firewall không hỏi. Đăng nhập lại thì agent tự chạy |
| T4 (tùy chọn) | Menu "Kiểm tra cập nhật" qua GitHub Releases, toast Windows khi mất hoặc có lại kết nối | |

## Rủi ro
- Không có chữ ký số nên SmartScreen sẽ cảnh báo "Unknown publisher" khi cài, phải bấm *More info → Run anyway*.
  Dùng cá nhân thì chấp nhận được.
- Python 3.14 và PyInstaller: cần bản PyInstaller mới. Nếu các gói winrt có vấn đề thì build bằng Python 3.12
  trong venv.

## Còn phải chốt
1. Link GitHub cho menu là gì? Repo hiện chưa có remote.
2. ffmpeg: kèm luôn trong bộ cài (file cài thêm khoảng 30 MB) hay bắt người dùng tự cài vào PATH?
   Đề xuất: kèm luôn để cài xong là chạy được.
3. Cài cho cả máy (Program Files, cần admin, firewall tự cấu hình) hay chỉ user hiện tại (không cần admin,
   nhưng firewall vẫn hỏi lần đầu)? Đề xuất: cài cho cả máy.
