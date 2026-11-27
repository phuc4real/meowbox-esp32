# Plan: MeowBox firmware v0.2.0

Bản minor tiếp theo, gồm 5 phần bên dưới.

| Phần | Trạng thái |
|---|---|
| 1. Web | đã lên plan |
| 2. PC Agent (tray + file cài đặt) | đã lên plan, gộp từ `agent_rework.md` |
| 3. Cập nhật firmware qua GitHub Release | đã lên plan |
| 4. Phát radio ổn định hơn trên Wi-Fi lạ | đã lên plan |
| 5. Hiệu năng: review code, chạy thử, tối ưu RAM/PSRAM | đã lên plan, làm cuối (đo mốc làm đầu) |

### Phiên bản và phát hành
- Firmware và PC Agent dùng **chung số phiên bản `0.2.0`** (`PROJECT_VER` trong `firmware/CMakeLists.txt` đang
  là `0.1.0`; agent thêm `__version__ = "0.2.0"`).
- Một GitHub Release `v0.2.0` trên `github.com/phuc4real/meowbox-esp32` chứa: file firmware (`meowbox.bin`, ảnh
  `storage`) và `MeowBoxAgent-Setup-0.2.0.exe`.
- Ngoài số phiên bản còn có số giao thức `proto` (số nguyên) để agent và loa biết có hợp nhau không, xem mục 2.6.

### Đổi chung cho cả bản: hostname mDNS thành `meowbox-xxxx.local`
- Hiện hostname cứng là `meowbox` (`components/net/wifi_mgr.c`), hai loa trong một nhà sẽ đụng nhau. Đổi thành
  `meowbox-xxxx` (chữ thường, cùng 4 ký tự với Wi-Fi cài đặt `MeowBox-XXXX`). Instance name là `MeowBox-XXXX`.
- Những chỗ phải sửa theo:
  - màn Hệ thống trên loa (`components/apps/system_app.c`) hiện đúng tên mới;
  - `web/src/app.js` (ví dụ lệnh `curl` ở trang Thông báo, đang lấy `meowbox.local` khi không có `location.host`);
  - `pc-agent`: bỏ đường dự phòng `meowbox.local`, chỉ tìm qua service `_meowbox._tcp`;
  - tài liệu: `firmware/README.md`, `web/README.md`, `web/API.md`.

---

## 1. Web

### Hiện trạng
Số đo lúc lên plan (`python build.py`):

| File | src | gzip |
|---|---|---|
| `app.js` | 57.0 KB | 21.4 KB |
| `app.css` | 29.2 KB | 6.0 KB |
| `index.html` + `themes.css` | 4.2 KB | 1.5 KB |
| **Tổng** | | **28.9 KB / ngân sách 30 KB (96%)** |

- Flash còn dư: phân vùng `storage` có 4.4 MB, mới dùng khoảng 277 KB. Cái làm loa nặng là số byte phải gửi qua Wi-Fi
  và số request mỗi lần mở trang.
- `components/net/web.c` trả `Cache-Control: no-cache` cho mọi file trừ font, nên lần nào mở trang trình duyệt cũng
  hỏi lại loa.
- `build.py` chỉ bỏ thụt lề và comment, chưa đổi tên biến hay rút gọn gì.
- Chuỗi ngôn ngữ nằm rải ở 3 chỗ: object `L` (khoảng 14 KB, tức 25% `app.js`, chứa cả vi lẫn en), object `BT` riêng
  cho trang Pin, và 3 chỗ `lang=='vi'` viết thẳng trong code.
- Gần nửa sau `app.css` là các khối `[data-theme=…]` của 4 theme. Trình duyệt tải cả 4 dù chỉ dùng 1.

### Kết quả mong muốn
- Lần đầu mở trang tải ít byte hơn hẳn hiện tại. Từ lần thứ hai trở đi gần như chỉ tải `index.html`.
- Chuỗi ngôn ngữ nằm trong file json riêng cho từng ngôn ngữ. Thêm ngôn ngữ mới chỉ cần thêm file, không phải sửa code.
- 4 theme có cùng một bộ chức năng, chỉ khác cách trông. Có test bắt lỗi khi một theme thiếu phần nào đó.
- Trang vẫn chạy đầy đủ khi không có internet (điện thoại nối hotspot của loa lúc cài Wi-Fi). Không phụ thuộc
  GitHub hay CDN.

### 1.1 Tách ngôn ngữ (làm trước vì đổi cấu trúc `app.js`)
- Gom `L`, `BT` và 3 chỗ `lang=='vi'` vào `web/src/lang/vi.json` và `web/src/lang/en.json`. Chuỗi có biến
  viết dạng `{n} phút`, hàm `t(k, vars)` thay biến lúc chạy. Chuỗi dạng hàm hiện có (ví dụ `eta:m=>…`) chuyển sang
  dạng template.
- Trang chỉ tải `lang/<lang>.json` của ngôn ngữ đang chọn, tiết kiệm khoảng 7 KB raw mỗi lần tải. Đổi ngôn ngữ thì
  tải file kia rồi vẽ lại trang hiện tại.
- Không dự phòng lúc chạy (không tải thêm `vi` khi đang dùng `en`): `test_api.py` bắt buộc mọi file ngôn ngữ có đủ
  khoá như `vi.json`. Thiếu khoá thì `t()` trả về tên khoá. **Đã xong** (2026-11-23): 234 khoá, lượt tải đầu gzip
  28.9 → 26.5 KB.
- Hằng số (`RB`, `GEO`, `MAXFW`) chỉ vài trăm byte, để nguyên trong code. Icon `IC` chuyển thành `<symbol>` trong
  sprite SVG có sẵn ở `index.html`, dùng bằng `<use href="#i-radio">`.
- `test_api.py` thêm phần kiểm tra: hai file ngôn ngữ có cùng bộ khoá, mọi khoá `t('…')` trong code đều có trong `vi.json`.

### 1.2 Tối ưu build và cache
> **Trạng thái (2026-11-23):** xong phần tên có hash + cache + ETag/304, tách CSS theo theme, nhúng web vào app (3.1).
> Khác plan: bảng ngôn ngữ/theme → tên file nằm ngay trong `index.html` (`window.MB`) thay vì `manifest.json` (đỡ một
> request); `themes.css` (biến của cả 4 theme, 640 B gzip) vẫn luôn tải để ô xem trước ở trang Giao diện dùng được, chỉ
> phần riêng của theme tách ra `src/theme/<id>.css`. **Chưa làm:** minify bằng esbuild (máy chưa có, phải tải từ npm),
> tách `app.js` thành module. Lượt tải đầu (gzip): 26.2 KB với Brutal, khoảng 25 KB với Swiss; lần mở sau chỉ còn
> `index.html` trả 304.

- **Minify thật:** `build.py` dùng `esbuild` nếu máy có (gọi qua `npx`), máy không có thì dùng lại minify hiện tại.
  Nhờ vậy build vẫn chạy được chỉ với stdlib. Dự đoán `app.js` gzip còn khoảng 15–16 KB, đo lại sau khi làm.
- **Tách `app.js` thành module lúc viết** (`src/js/core.js`, `src/js/pages/radio.js`…). `build.py` ghép lại thành
  một file nên lúc chạy vẫn chỉ một request.
- **Tách CSS theo theme:** phần chung để trong `app.css`. Biến và các chi tiết riêng của mỗi theme sinh ra
  `themes/<id>.css` (`tools/gen_themes.py` sinh luôn từ `firmware/themes/<id>.json` cộng phần flourish). Trang chỉ tải
  file của theme đang dùng, giống cách đang tải font. Script inline trong `<head>` vẫn đặt `data-theme` từ
  `localStorage` và chèn luôn `<link>` của theme đó để không bị chớp trang.
  - Lưu ý: `tvars()` (vẽ ô xem trước theme ở trang Hiển thị) đang đọc biến của cả 4 theme từ stylesheet. Phải chuyển
    sang lấy màu từ `/api/themes` (thêm vài màu chính vào `themes.json`) hoặc tải riêng css của từng theme khi mở trang đó.
- **Đặt hash vào tên file** (`app.3f9a.js`, `lang/vi.81c2.json`, `themes/zen.5d10.css`). `build.py` viết lại các đường dẫn
  trong `index.html` và ghi `manifest.json` để JS biết tên file ngôn ngữ và theme cần tải.
- **Firmware (`web.c`):** file có hash trong tên thì trả `Cache-Control: max-age=31536000, immutable`,
  `index.html` và `manifest.json` thì trả `no-cache`. Xoá file `.gz` cũ khi build (đã có bước dọn file thừa, sửa để
  hiểu tên có hash).
- Đo lại rồi hạ `GZ_BUDGET` trong `build.py` xuống sát mức mới, để sau này không phình lại. Ngân sách tính cho phần
  tải lần đầu: `index.html` + `app.js` + `app.css` + 1 file ngôn ngữ + 1 file theme.

### 1.3 Đồng bộ chức năng giữa các theme
> **Trạng thái (2026-11-23):** xong. `web/audit.mjs` (Chrome headless qua CDP, chạy mock `--dist`) duyệt 4 theme × 390/1280
> px × 10 trang, so các nút/ô nhập/link nhìn thấy được, tìm tràn ngang và lỗi console, chụp ảnh vào `web/screens/`.
> Kết quả: cùng số control ở mọi theme, không tràn, không lỗi. Đã sửa: mục Hiệu ứng chỉ hiện với theme có hiệu ứng
> (`fx` trong `/api/themes`: Cyberpunk, Brutal), nút ghost của Zen có viền nhạt (trước đó trông như chữ thường).
> Giữ nguyên có chủ đích (chỉ là kiểu, không đổi chức năng): Brutal ẩn icon menu, dấu đầu banner khác nhau, `.brush` của
> Zen. `test_api.py` kiểm tra mỗi theme có `theme/<id>.css`, ảnh xem trước, font, và theme có `fx` thì có khối `[data-fx=eco]`.
Hiện 4 theme dùng chung bố cục và code, chỉ khác CSS. Những chỗ lệch đã tìm thấy:

| Chỗ lệch | Swiss | Cyberpunk | Zen | Brutal | Hướng sửa |
|---|---|---|---|---|---|
| Hiệu ứng Đầy đủ/Tiết kiệm (`data-fx=eco`) | không tác dụng | có | không tác dụng | có | định nghĩa chế độ eco cho cả 4 theme (Zen bỏ nền giấy, Swiss bỏ bóng khi hover), hoặc ẩn tuỳ chọn khi theme không có hiệu ứng (cờ `fx` trong `themes.json`) |
| Icon trên menu | có | có | có | ẩn (`nav a .i{display:none}`) | hiện lại hoặc chuyển thành quyết định chung |
| Banner mật khẩu mặc định | có dấu | `!!` | không dấu | không dấu | thống nhất một dấu hiệu cảnh báo, theme chỉ đổi kiểu |
| Vạch bút lông `.brush` | — | — | có | — | trang trí, giữ nguyên |

- Màn hình loa (`components/ui/ui.c`) cũng tương tự: `fx_segments` và `fx_hud` chỉ có ở một số theme. Rà luôn cùng
  lúc để chế độ eco trên web và trên loa nghĩa giống nhau.
- Lập bảng đầy đủ: chụp màn hình tự động bằng `mock_server.py` cho 4 theme × 2 cỡ (390 px, 1280 px) × mọi trang
  (thêm script `web/shots.py`, ảnh lưu vào `screens/`, không commit). Thêm chỗ lệch mới tìm được vào bảng trên.
- Thêm vào `test_api.py`: mỗi theme trong `themes.json` phải có file css, font và ảnh xem trước. Theme nào khai báo
  `fx` thì file css của nó phải có khối `[data-fx=eco]`.

### 1.4 Kiểm thử
- `python test_api.py` ra `ALL PASSED`, `node --check` cho mọi file JS, so ảnh chụp trước và sau theo bảng ở 1.3.
- Trên loa thật:
  - chế độ hotspot (điện thoại không có internet): đăng nhập, đổi ngôn ngữ, đổi theme, cài Wi-Fi đều chạy;
  - chế độ có mạng: tìm đài, tìm thành phố vẫn chạy;
  - cache: mở trang, nạp bản build mới, mở lại thì trang phải dùng file mới, không lẫn file cũ.
- Cập nhật `web/README.md` (cấu trúc file mới, cách build) và `web/API.md` (phần Static files, header cache).

### Thứ tự
1.1 → 1.2 → 1.3 → 1.4. Phần trang PC trên web (tên/phiên bản agent, nút tải, kiểm tra relay) nằm ở mục 2.7.

### Không làm trong bản này
- Lấy chuỗi ngôn ngữ hay hằng số từ GitHub/CDN. Lúc cài Wi-Fi điện thoại không có internet nên vẫn phải giữ bản
  trên loa. Sau 1.1 và 1.2 thì phần tiết kiệm được rất nhỏ so với độ phức tạp phải thêm. Để dành cho lúc có
  ngôn ngữ thứ 3 do cộng đồng dịch.

---

## 2. PC Agent: tray icon và file cài đặt

> Since 0.3.0 the agent is written in Rust: same files, settings, protocol and installer, but egui instead of
> tkinter, its own tray code instead of pystray, `cargo build` instead of PyInstaller, and yt-dlp's onedir build
> instead of the zipapp. This section is the 0.2.x (Python) design as planned; the Rust port, its decisions and its
> results are in [rewrite_pc_agent.md](rewrite_pc_agent.md), current usage in [pc-agent/README.md](../pc-agent/README.md).

### Hiện trạng
- Agent là một file `pc-agent/agent.py` (asyncio), phải cài Python và pip, chạy bằng `pythonw` qua Task Scheduler
  (`install_autostart.ps1`). Không có UI nên không biết nó đang chạy hay đã chết, chỉ xem được trong `agent.log`.
- Agent tìm loa theo thứ tự: `host` cài tay, `last_host` (IP lần cuối kết nối được), rồi mDNS. Mạng chặn mDNS thì
  phải nhập IP mà loa hiện trên màn hình. Token và host vẫn phải cài bằng dòng lệnh `agent.py config`. Agent nối
  với loa đầu tiên mDNS tìm thấy, nên nhà có hai loa thì không chọn được.
- Loa lấy địa chỉ relay YouTube từ `hello` của agent đang kết nối. IP nhập trên web chỉ dùng dự phòng.
- **Relay YouTube (cổng 8765) nhận request từ bất kỳ máy nào trong LAN, với `url` bất kỳ.** Ai cùng Wi-Fi cũng bắt
  được PC chạy yt-dlp/ffmpeg.
- **Loa chỉ giữ một kết nối PC** (`s_fd` trong `components/apps/pc_app.c`), agent nào gửi sau thì chiếm. Hai PC
  cùng chạy agent sẽ thay nhau đẩy số liệu và địa chỉ relay.
- Log ghi bằng `logging.basicConfig(filename=…)`, không xoay vòng, `agent.log` phình mãi.
- ffmpeg và yt-dlp đang lấy từ PATH.
- Máy chưa có Inno Setup hay NSIS.

### Kết quả mong muốn
Có một file `MeowBoxAgent-Setup-0.2.0.exe`. Cài xong là agent tự chạy và hiện icon con mèo ở system tray,
kể cả khi máy không có Python. Lần đầu chạy có cửa sổ dẫn từng bước để nối với loa, không phải gõ lệnh hay sửa
file json. Mạng có mDNS thì agent tự tìm ra loa. Mạng chặn mDNS thì nhập IP hiện trên màn hình loa vào ô trong
cửa sổ là xong. Relay chỉ phục vụ loa đang nối. Agent và loa lệch giao thức thì cả hai bên đều báo rõ.

> **Status (2026-11-24):** functionally done and run against the real speaker (from source and as the built exe):
> discovery, name-based choice, busy + take-over, relay limited to the speaker, relay_test, a pre-0.2.0 agent still
> connecting (proto 0, the web page asks to update it), YouTube relay (video: AAC from m4a, no ffmpeg; live: ffmpeg
> downloaded on first use, AAC copied into MPEG-TS), Settings window. Installer built (20.1 MB, nothing third-party
> bundled; it downloads yt-dlp); install / uninstall / reinstall on the user's PC not yet checked. UI/UX moved to
> docs/roadmap.md.
>
> **Trạng thái (2026-11-23):** T1, T1b, T2 xong và đã chạy với loa thật; T3 đã build được bộ cài (18.9 MB, chưa kèm
> ffmpeg/yt-dlp), chưa cài thử. Khác plan / thêm:
> - **Loa rơi phần lớn gói broadcast/multicast gửi tới nó khi bật tiết kiệm điện Wi-Fi** (đo: truy vấn UDP 0–2/5 được
>   trả lời, mDNS cũng hụt; tắt tiết kiệm điện thì 5/5). Gói loa gửi đi thì không sao, nên loa phát gói thông báo
>   `{name,ip,ver,proto}` mỗi 2 s tới UDP 8767 và agent nghe cổng đó: 5/5 lần tìm thấy trong 0.1–1.6 s. Có lẽ đây cũng
>   là lý do agent cũ hay không thấy loa qua mDNS.
> - Trả lời UDP dùng một task nhỏ (stack 3 KB ở PSRAM, socket chặn) thay cho callback lwIP.
> - `MB_PROTO_MIN` = 0: agent cũ (chưa gửi `proto`) vẫn được nhận, web ghi "bản cũ, nên cập nhật".
> - Cùng một tên PC nối lại trước khi kết nối cũ hết hạn thì được nhận luôn (không báo bận chính nó).
> - Tên Wi-Fi của PC: `netsh` cần quyền Location trên Windows 11, nên lấy tên network profile thay thế.
> - Tray: tkinter giữ thread chính (cửa sổ), pystray chạy detached, core asyncio một thread; mọi kết quả từ thread
>   khác đưa về qua hàng đợi.
> - Bộ cài mở tường lửa theo chương trình (MeowBoxAgent.exe, meowbox.exe) cho mạng Private: bản exe chưa có rule thì
>   không nhận được mDNS/UDP của loa.

### 2.1 Tray app (`pystray` + Pillow)
- `pystray` chạy ở main thread, còn vòng asyncio hiện tại chạy ở một thread riêng. Hai bên dùng chung một
  object `AgentState` gồm: connected, host, tên loa, đang tìm loa bằng cách nào, tìm được bằng cách nào, lỗi cuối,
  số lần nối lại, PC khác đang giữ loa (nếu bị `busy`).
- `ws_loop` cập nhật `AgentState`. Khi Thoát thì hủy task, đóng relay và kill yt-dlp/ffmpeg đang chạy.
- Icon con mèo có thêm chấm màu: xanh là đã nối loa, xám là đang tìm loa, đỏ là lỗi (thiếu token, cổng 8765
  đang bị chiếm, không thấy loa trên mạng này, lệch giao thức...).
- Hover vào icon thì hiện tooltip `MeowBox-1A2B · 192.168.1.192 · đã kết nối`.

Menu chuột phải:

```
● Đã kết nối MeowBox-1A2B · 192.168.1.192 (mDNS)      (dòng trạng thái, không bấm được)
─────────────
Mở trang cài đặt MeowBox              (mặc định, double-click icon cũng mở) → http://<host>/
Gửi thông báo thử
Tạm dừng gửi số liệu        ☐
─────────────
Kết nối loa…                          (cửa sổ ở mục 2.2, đổi cổng relay cũng ở đây)
Tìm lại loa ngay                      (bỏ qua thời gian chờ nối lại, tìm luôn)
Chạy cùng Windows           ☑         (HKCU\...\Run)
Mở thư mục log
─────────────
Có bản 0.2.1 ↗                        (chỉ hiện khi có bản mới, mở trang Release)
GitHub ↗                              → https://github.com/phuc4real/meowbox-esp32
Giới thiệu (v0.2.0)
Thoát
```

Dòng trạng thái đổi theo `AgentState`:

```
● Đã kết nối MeowBox-1A2B · 192.168.1.192 (mDNS | IP lần trước | IP nhập tay)
○ Đang tìm loa: mDNS…
✕ Không thấy MeowBox-1A2B trên mạng này, mở "Kết nối loa…" để nhập IP (thử lại sau 30 s)
✕ Token sai, mở "Kết nối loa…" để sửa
✕ MeowBox-1A2B đang nối với DESKTOP-X                 [Giành quyền]
✕ Loa cần firmware mới hơn / Agent cần cập nhật        (lệch proto, xem 2.6)
```

- Chỉ cho chạy một bản: dùng named mutex. Mở thêm bản thứ hai thì nó chỉ mở trang cài đặt rồi tự thoát, nhờ vậy
  không bị lỗi trùng cổng 8765.
- Lần đầu chạy (config chưa có `speaker`) thì tự mở cửa sổ "Kết nối loa".
- Bỏ menu "Cài đặt nâng cao…" của plan cũ: nó chỉ có cổng relay, mà bước 3 của cửa sổ "Kết nối loa" đã cho đổi cổng.
- Kiểm tra cập nhật: lúc khởi động và mỗi ngày một lần, gọi
  `GET https://api.github.com/repos/phuc4real/meowbox-esp32/releases/latest`, tag mới hơn `__version__` thì hiện
  dòng "Có bản x.y.z ↗". Không tự tải, không tự cài. Lỗi mạng thì bỏ qua, không báo.

### 2.2 Cửa sổ "Kết nối loa"

#### Nhớ loa theo tên, không theo IP
- Config thêm `speaker` = tên loa trả về từ `/api/ping` (`MeowBox-1A2B`, giống tên Wi-Fi cài đặt của loa).
  Khi tìm loa, agent chỉ nhận loa đúng tên đó. Nhà có hai loa thì không nối nhầm. Loa đổi IP thì vẫn tìm ra.
- `host` là IP nhập tay ở bước 1. `last_host` vẫn giữ như hiện tại để lần sau nối ngay. Cả hai đều phải check tên
  qua `/api/ping` trước khi dùng, để IP cũ bị máy khác chiếm thì không nối nhầm.

#### Core: mDNS trả về cả danh sách
- Thêm `discover_all()`: gom hết các loa mDNS thấy trong khoảng 3 s (`{name, ip, ver}`), không lấy loa đầu tiên
  rồi dừng như bây giờ. Loa nào thấy thì báo ra luôn để cửa sổ hiện ngay. Instance name đã là `MeowBox-XXXX`
  nên lọc theo tên ngay trong mDNS.
- `ws_loop` vẫn dùng `find_speaker()`, chỉ nhận loa đúng `speaker`, và đẩy trạng thái (đang thử IP nào, đang tìm
  qua mDNS) vào `AgentState` để menu tray hiện đúng.
- mDNS không thấy loa thì `find_speaker()` thử tiếp UDP broadcast rồi mới báo không thấy (mục 2.8).
- Không quét cả mạng LAN. Loa đã vào Wi-Fi thì IP hiện trên màn hình loa, nhập tay là đủ.

#### Ba bước (tkinter, một cửa sổ có nút Tiếp/Quay lại)

```
┌ Kết nối loa MeowBox ─────────────────────────────────────┐
│ 1 Tìm loa  ›  2 Token  ›  3 Kiểm tra                     │
│                                                          │
│  Loa tìm thấy qua mDNS:                     [Quét lại]   │
│  ◉ MeowBox-1A2B   172.16.1.42   v0.2.0                   │
│  ○ MeowBox-77C0   172.16.1.57   v0.1.0                   │
│                                                          │
│  Không thấy loa? Nhập IP hiện trên màn hình loa:         │
│  [ 172.16.1.42      ] [Thử]   ✓ MeowBox-1A2B             │
│                                                          │
│                                                [Tiếp ›]  │
└──────────────────────────────────────────────────────────┘
```

**Bước 1: Tìm loa**

> Since agent 0.2.2 step 1 is only the IP box: no `discover_all()`, no list. Managed PCs (Sophos and the
> like) flag or block LAN discovery, so the agent connects only to the typed IP or the last working one. See 2.8.

- Mở cửa sổ là chạy `discover_all()` luôn. Tìm thấy loa nào thì thêm dòng đó vào danh sách ngay. Chỉ có một loa thì
  chọn sẵn. Loa có firmware quá cũ (lệch `proto`) thì vẫn hiện nhưng ghi "cần cập nhật firmware".
- Ô nhập IP luôn hiện, không giấu trong nút. Ghi rõ IP lấy ở đâu: tile Hệ thống trên màn hình loa, hoặc màn hình
  ngay sau khi cài Wi-Fi qua mạng `MeowBox-XXXX`. Bấm "Thử" thì gọi `/api/ping`:
  - đúng là loa: hiện ✓ kèm tên loa, và chọn IP này
  - không phải loa: báo "IP này không phải MeowBox"
  - không trả lời: báo "Không nối được", kèm danh sách lý do hay gặp ở dưới
- Lý do hay gặp khi không nối được:
  - PC và loa không cùng mạng Wi-Fi. Tên mạng của loa xem ở tile Hệ thống.
  - Mạng bật client isolation (wifi khách, quán cà phê): các máy không thấy nhau, không hỗ trợ.
  - Loa chưa có Wi-Fi: có nút "Hướng dẫn cài Wi-Fi cho loa" (vào mạng `MeowBox-XXXX`).

**Bước 2: Token** (dùng cho thông báo, `say`, `wifi-setup`; số liệu và media không cần token)
- Nút "Mở trang Thông báo trên loa" → `http://<ip>/#/notify`, có token ở đó để copy.
- Dán token xong thì check bằng `GET /api/notify/history`: được thì hiện ✓, sai thì báo "Token sai".
- Có nút "Bỏ qua", để sau vẫn sửa được.

**Bước 3: Kiểm tra relay YouTube**
- Cổng relay (mặc định 8765) có mở được không. Không mở được thì báo cổng đang bị app khác dùng và cho đổi cổng
  ngay tại đây.
- Thử từ phía loa: agent gọi `POST /api/pc/relay_test`, loa tự `GET http://<relay>/yt/ping` rồi báo kết quả.
  Firewall chặn chiều kết nối vào PC, nên chỉ cách này mới biết chắc YouTube phát được.
- Rule firewall chỉ thêm khi cần: `relay_test` lỗi **và** chưa có rule cho loại mạng đang dùng. Hay gặp nhất là
  Windows xếp mạng mới vào loại **Public** (category trong Network List Manager của card mạng nối với loa; agent
  không gọi PowerShell vì phần mềm diệt virus như Sophos có thể chặn), trong khi bộ cài chỉ mở cho Private. Lúc đó mới hiện nút
  "Cho phép trên mạng này". Nút này hỏi quyền admin (UAC), chạy `netsh` thêm rule cho `MeowBoxAgent.exe`, cổng relay,
  đúng profile đang thiếu, rồi chạy lại `relay_test`. Không tự đổi loại mạng của Windows.
- Ngoài cửa sổ này: đang chạy mà loa không gọi được relay khi phát YouTube thì tray hiện toast, kèm đúng nút đó.
- Xong thì lưu `speaker`, `token`, `last_host`, rồi báo cho `ws_loop` nối lại ngay.

### 2.3 Relay chỉ phục vụ loa đang nối
- `/yt/info`, `/yt/stream`, `/yt/ping` chỉ nhận request có `req.remote` bằng IP của loa đang nối WebSocket. Máy khác
  nhận 403 và ghi một dòng log. Chưa nối loa nào thì từ chối hết (riêng `/yt/ping` trong lúc chạy bước 3 thì nhận
  từ IP loa đang thử).
- Không cần token cho relay: loa lấy địa chỉ relay từ `hello` nên IP loa là đủ để phân biệt.
- Việc này phải xong trước T3, vì bộ cài mở firewall cổng relay.

### 2.4 Một loa chỉ nhận một PC
- Firmware (`pc_app.c`): đang có PC nối (`pc_online()`) mà PC khác gửi `hello` thì trả
  `{"t":"busy","pc":"DESKTOP-X"}` rồi đóng kết nối mới. PC cũ mất kết nối quá 15 s (đã có sẵn ngưỡng này) thì nhận PC mới.
- `hello` có thêm `"take":true` khi người dùng bấm "Giành quyền" ở tray. Lúc đó loa đóng kết nối cũ và nhận PC mới.
  PC cũ nhận `busy` và tray của nó hiện "đang nối với DESKTOP-Y", không tự giành lại.
- Agent nhận `busy` thì đợi lâu hơn trước khi thử lại (60 s) để hai máy không giành nhau liên tục.

### 2.5 Log
- Đổi sang `RotatingFileHandler`, 1 MB × 3 file, trong `%APPDATA%\MeowBox\`. Menu "Mở thư mục log" mở thư mục này.

### 2.6 Phiên bản và giao thức
- `hello` gửi thêm `name` (tên PC), `ver` (`0.2.0`) và `proto` (bắt đầu từ `1`, tăng khi đổi message hay API mà
  agent dùng). Loa lưu `name`/`ver` để `GET /api/config/pc` điền được `st.name` và `st.ver`, hiện giờ hai ô này
  đang để trống.
- `/api/ping` của loa trả thêm `proto` mà loa hỗ trợ (`proto_min`, `proto`).
- Lệch giao thức thì:
  - loa trả `{"t":"proto","need":N}` rồi đóng; tray báo "Agent cần cập nhật" kèm link Release;
  - agent thấy `proto` của loa cũ hơn thì báo "Loa cần firmware mới hơn";
  - trang PC trên web hiện cùng thông báo đó (mục 2.7).

### 2.7 Firmware và web cần thêm
- Hostname và instance mDNS `meowbox-xxxx` / `MeowBox-XXXX` (xem đầu file).
- `POST /api/pc/relay_test` (cần token hoặc phiên đăng nhập web): loa tự gọi `GET http://<relay>/yt/ping`,
  timeout 3 s, rồi trả `{"ok":true,"ms":12}` hoặc `{"ok":false,"err":"timeout"}`. Phía agent thêm route `/yt/ping`.
- `hello` nhận `name`, `ver`, `proto`, `take`. Thêm message `busy` và `proto` (mục 2.4, 2.6).
- `/api/ping` trả thêm `proto_min`, `proto`.
- Trang PC trên web:
  - hiện tên PC và phiên bản agent đang nối; lệch giao thức thì hiện cảnh báo;
  - nút "Kiểm tra relay" gọi `relay_test`, không cần mở cửa sổ trên PC;
  - nút "Tải MeowBox Agent" → trang Release trên GitHub (mở khi có internet, chỉ là link).
- `web/API.md`, `mock_server.py` và `test_api.py` cập nhật theo các endpoint và trường mới.

### 2.8 Mạng chặn mDNS

> Since agent 0.2.2 the agent does no discovery at all (no mDNS, no UDP broadcast/announcements): it tries
> `host` (typed) then `last_host` with `/api/ping` and checks the name. The firmware still answers mDNS and UDP 8766 and
> still announces on 8767; nothing in the agent listens any more.

Mạng chặn mDNS thì các máy vẫn nối được với nhau, chỉ là không tự tìm thấy nhau. Agent tìm loa theo thứ tự:
1. `last_host`, `host`: như hiện tại, check tên qua `/api/ping`.
2. mDNS `_meowbox._tcp`, 3 s.
3. **UDP broadcast** (mới): agent gửi gói `MEOWBOX?` tới `255.255.255.255` và địa chỉ broadcast của từng card mạng,
   cổng UDP 8766. Loa trả lời trực tiếp cho PC (unicast) bằng `{name, ip, ver, proto}`. Nhiều router lọc multicast
   (mDNS) nhưng vẫn cho broadcast qua, nên phần lớn trường hợp vẫn tự tìm được. Windows Firewall cho gói trả lời
   đi vào vì PC là bên gửi trước, không cần thêm rule.
4. Nhập IP tay, lấy IP trên màn hình loa (như cũ).
- Đã nối được loa bằng cách nào thì ghi vào `AgentState` để menu tray hiện (`mDNS | broadcast | IP lần trước | IP nhập tay`).
- Mạng chặn mDNS thì IP của loa đổi theo DHCP là phải nhập lại. Lúc nối được bằng IP nhập tay, cửa sổ gợi ý đặt IP
  cố định cho loa trên router (DHCP reservation), kèm địa chỉ MAC của loa (lấy từ `/api/ping`).
- Client isolation (router chặn các máy trong cùng Wi-Fi nói chuyện với nhau) không xử lý trong bản này.

Firmware cần thêm:
- Lắng nghe UDP 8766, nhận `MEOWBOX?` thì trả lời unicast `{name, ip, ver, proto}`. Chỉ trả lời khi đang ở chế độ
  station (không trả lời trên AP cài đặt), giới hạn 5 lần trả lời/giây.
  - Dùng callback `udp_recv()` của lwIP (gọi qua `tcpip_callback`), không tạo task riêng: tốn khoảng vài trăm byte
    RAM, không tốn CPU khi không có gói tới. Nếu thấy rắc rối thì gộp vào task mạng có sẵn bằng `select()`.
- `/api/ping` trả thêm `mac`.

### 2.9 Tách code
- `pc-agent/meowbox_agent/`:
  - `core.py`: phần logic hiện tại, sửa ít nhất có thể
  - `discovery.py`: `find_speaker`, `discover_all`, mDNS, UDP broadcast
  - `relay.py`: relay YouTube, có lọc IP loa
  - `tray.py`
  - `connect_dialog.py`: cửa sổ "Kết nối loa"
  - `update.py`: kiểm tra Release mới
  - `cli.py`: giữ `notify`, `say`, `wifi-setup`, `config`, thêm `discover` (in danh sách loa tìm được)
  - `__version__ = "0.2.0"`, `PROTO = 1`
- Vẫn chạy được từ source như cũ (`python -m meowbox_agent`) để tiện dev và dùng với `mock_speaker.py`.
- `mock_speaker.py` thêm `--name`, `--no-mdns`, `--no-udp` và `--proto`, để chạy hai loa giả cùng lúc mà test chọn loa và test
  lệch giao thức. Thêm chế độ trả `busy` để test 2.4.

### 2.10 Đóng gói bằng PyInstaller
- Dùng `--onedir` chứ không dùng onefile, vì onefile khởi động chậm hơn và hay bị antivirus báo nhầm.
- Một file spec tạo ra hai exe dùng chung thư mục `_internal`:
  - `MeowBoxAgent.exe`: windowed, có tray
  - `meowbox.exe`: bản console cho lệnh `meowbox notify "Build xong"` và `meowbox say ...`
- Build trong venv sạch, khai báo hidden-imports cho các module `winrt.*`, `comtypes` và `zeroconf`.
- Icon `.ico` nhiều cỡ (16 đến 256) tạo từ sprite mèo trong `firmware/components/cat_sprites`.
- Kèm ffmpeg trong bộ cài (file cài thêm khoảng 30 MB). Dùng **bản build LGPL** (ví dụ BtbN `lgpl`, vẫn có
  libmp3lame), vì phân phối lại bản GPL thì phải kèm source. Ghi license ffmpeg vào thư mục cài.
- Xử lý yt-dlp: lần đầu chạy thì copy `yt-dlp.exe` vào `%LOCALAPPDATA%\MeowBox\bin\`, vì thư mục này ghi được nên
  `yt-dlp -U` vẫn tự cập nhật hằng ngày. Agent ưu tiên PATH trước, sau đó mới tới bản kèm theo. ffmpeg cũng vậy.

### 2.11 File cài đặt bằng Inno Setup 6
Cài bằng `winget install JRSoftware.InnoSetup`.

- Cài cho cả máy vào `Program Files\MeowBox Agent`, cần quyền admin để:
  - thêm firewall rule cho `MeowBoxAgent.exe` (profile Private), không còn bị Windows hỏi
  - tùy chọn thêm `meowbox.exe` vào PATH
- Có shortcut ở Start Menu và checkbox "Chạy cùng Windows" (mặc định bật). Cài xong thì chạy luôn, và cửa sổ
  "Kết nối loa" tự mở.
- Lúc cài sẽ dọn bản cũ: gỡ task "MeowBox Agent" trong Task Scheduler và shortcut Startup, để không có hai agent
  chạy cùng lúc. Giữ nguyên `agent.json` cũ (token, `last_host`). Nếu chưa có `speaker` thì lần nối đầu tiên tự
  điền.
- Lúc gỡ: kill process, xóa Run key và firewall rule (cả rule Public nếu có). Có hỏi có xóa `%APPDATA%\MeowBox`
  (token, log) hay không.
- `pc-agent/build.ps1` chạy một lệnh là ra file cài: tạo venv → PyInstaller → `iscc` →
  `dist\MeowBoxAgent-Setup-0.2.0.exe`. Số phiên bản lấy từ `__version__`, không gõ tay hai chỗ.

### 2.12 Các giai đoạn

| | Việc | Xong khi |
|---|---|---|
| T1 | Tách code, tray và menu, single instance, log xoay vòng, relay lọc IP loa (chạy từ source) | Test với `mock_speaker.py` và loa thật: icon đổi màu đúng, Thoát sạch không để lại process ffmpeg nào. Gọi relay từ máy khác trong LAN thì bị 403 |
| T1b | Cửa sổ "Kết nối loa" và `discover_all`, nhớ loa theo tên, tìm loa khi chặn mDNS, (mục 2.8). Firmware: hostname/instance mDNS, UDP 8766, `mac` trong `/api/ping`, `relay_test`, `hello` mới (`name`/`ver`/`proto`/`take`), `busy`, trang PC trên web | Hai loa giả: chọn đúng loa, không nối nhầm. Mạng chặn mDNS (`--no-mdns`): tìm ra bằng broadcast. Chặn cả broadcast (`--no-mdns --no-udp`): nhập IP là nối được. Mạng Public: `relay_test` lỗi thì nút firewall hiện ra, bấm xong thì OK. Mạng Private đã có rule thì không hiện nút. Đổi mạng thì tự nối lại. Hai PC: máy thứ hai thấy `busy`, bấm "Giành quyền" thì chuyển sang được, không giành qua lại. Lệch `proto` thì cả tray và web đều báo |
| T2 | PyInstaller, kèm yt-dlp/ffmpeg (LGPL) | Exe chạy được trên máy không có Python: stats, media, relay YouTube, `say` đều OK |
| T3 | Inno Setup, `build.ps1`, kiểm tra cập nhật qua GitHub Releases | Cài, gỡ, cài lại đều sạch. Firewall không hỏi. Đăng nhập lại thì agent tự chạy. Có Release mới hơn thì menu hiện dòng "Có bản…" |
| Sau 0.2.0 | Ghép đôi bằng nút trên loa thay cho copy token, toast Windows khi mất/có lại kết nối | |

Ghép đôi bằng nút trên loa (để sau): agent gọi `POST /api/pair {"pc":"DESKTOP-X"}`, loa hiện hộp thoại "Cho máy
DESKTOP-X kết nối?". Bấm OK trên loa thì loa trả token về cho agent. Quá 60 s không ai bấm thì từ chối.

### Rủi ro
- Không có chữ ký số nên SmartScreen sẽ cảnh báo "Unknown publisher" khi cài, phải bấm *More info → Run anyway*.
  Dùng cá nhân thì chấp nhận được.
- Python 3.14 và PyInstaller: cần bản PyInstaller mới. Nếu các gói winrt có vấn đề thì build bằng Python 3.12
  trong venv.
- tkinter và pystray cùng chạy: cả hai đều muốn chạy ở main thread. Cửa sổ tkinter phải mở trong thread riêng (mỗi
  lần mở tạo một `Tk()` mới), hoặc chạy `pystray` bằng `run_detached()`. Cần thử sớm ở T1.
- Đổi hostname làm mất địa chỉ `meowbox.local` quen thuộc: bookmark cũ và lệnh `curl` trong script của người dùng
  sẽ hỏng. Ghi rõ trong release note.

---

## 3. Cập nhật firmware qua GitHub Release

### Hiện trạng
- Chỉ cập nhật được bằng cách chọn file `.bin` trên trang Hệ thống (`POST /api/ota`), hoặc cắm USB. File `.bin`
  phải tự build hoặc tự tải về.
- `POST /api/ota` chỉ ghi phần **app** (`ota_0`/`ota_1`, có rollback). Trang web nằm ở phân vùng `storage`
  (LittleFS), không cập nhật qua OTA được. Bản 0.2.0 đổi web rất nhiều (mục 1), nên OTA chỉ phần app sẽ ra firmware
  mới chạy với web cũ.
- `storage` hiện chỉ chứa web: 4 file gzip khoảng 30 KB, font khoảng 200 KB, ảnh xem trước theme khoảng 50 KB. App
  `meowbox.bin` khoảng 2.85 MB, mỗi slot OTA 5.4 MB, nên còn trống khoảng 2.5 MB.
- Loa đã gọi HTTPS được (`esp_crt_bundle` đã bật, `clock_app.c` và `player.c` đang dùng).

### Kết quả mong muốn
- Trang Hệ thống hiện "Có bản 0.2.1" kèm vài dòng thay đổi. Bấm "Cập nhật" thì loa tự tải từ GitHub Release, tự
  kiểm tra, khởi động lại. Không phải tải file về điện thoại hay máy tính.
- Web, font và firmware luôn đi cùng nhau: cập nhật xong không bao giờ ra firmware mới chạy web cũ. Cập nhật hỏng giữa
  chừng thì loa vẫn chạy bản cũ (rollback có sẵn).
- Không có internet vẫn cập nhật tay bằng file như bây giờ.

### 3.1 Đưa web vào trong app (thay đổi kiến trúc, làm trước)
> **Trạng thái (2026-11-23):** xong. `components/webui` (`web.pack` qua `EMBED_FILES`, 25 file, 281 KB, app 3.14 MB,
> slot còn trống 43%). Font và ảnh xem trước dời từ `storage/web/` sang `web/assets/`, phân vùng `storage` giờ trống.
> Đã chạy trên loa: mọi trang vi/en, header cache, 304.

- **Đã chốt.** Nhúng toàn bộ `storage/web/` vào app: `build.py` sinh một file pack (bảng tên file, kích thước, offset, hash) rồi
  nhúng bằng `EMBED_FILES`. `web.c` phục vụ static file từ bảng này thay vì `/storage/web`.
- Lợi ích: web cập nhật cùng app qua OTA A/B, có rollback, chỉ cần tải **một file**. App lớn thêm khoảng 300 KB,
  vẫn còn hơn 2 MB trống mỗi slot.
- Phân vùng `storage` giữ lại cho tài nguyên lớn sau này (âm thanh, sprite mèo theo `storage/README.txt`). Bản này
  không cập nhật `storage` qua mạng (xem "Không làm trong bản này" của mục 3).
- Ảnh hưởng tới mục 1.2: tên file có hash và header cache giữ nguyên, chỉ đổi chỗ `build.py` ghi ra (file pack cho app
  thay vì `storage/web/*.gz`). Ngân sách gzip vẫn áp dụng vì nó cũng là dung lượng app.
- `POST /api/ota` thủ công không phải đổi gì: một file `.bin` giờ đã có cả web.

> **Trạng thái 3.2–3.5 (2026-11-23):** xong (`components/apps/update_app.c`, `tools/release.py`, khối "Cập nhật firmware"
> ở trang Hệ thống). Khác plan: `app.url` trong manifest là link đầy đủ (dùng được với server thử); endpoint kiểm tra là
> `POST /api/update/check` (chạy nền, web hỏi `GET /api/update`); lỗi thêm `no_release` (GitHub trả 404) và
> `usb_only`; `/api/status` có thêm `elf` (8 ký tự đầu SHA-256 của ELF) để nhận ra bản build vì giờ build trong
> `esp_app_desc` không đổi nếu không build sạch. Đã thử trên loa với server trên PC (`CONFIG_MEOWBOX_UPDATE_MANIFEST_URL`
> + `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` tạm thời): 0.1.0 → 0.1.1 (3.1 MB tải ~28 s, kiểm tra ~1 s, khởi động lại ~13 s,
> `last` báo thành công); SHA-256 sai → `bad_hash`, giữ bản cũ; GitHub thật (chưa có Release) → `no_release`.
> Chưa thử: rollback thật, pin yếu, mất Wi-Fi giữa lúc tải; nút "kiểm tra relay"/`proto` agent (phụ thuộc mục 2).

### 3.2 File trong mỗi Release
`tools/release.py` (chạy trong `firmware/`) build và tạo Release bằng `gh release create`:

| File | Nội dung |
|---|---|
| `meowbox-0.2.0.bin` | app, đã có web bên trong |
| `manifest.json` | thông tin để loa tự cập nhật, xem dưới |
| `MeowBoxAgent-Setup-0.2.0.exe` | bộ cài PC Agent (mục 2.11) |

```json
{
  "ver": "0.2.0",
  "proto": 1,
  "date": "2026-12-01",
  "notes": {"vi": ["Giao diện web nhẹ hơn", "…"], "en": ["Lighter web UI", "…"]},
  "app": {"file": "meowbox-0.2.0.bin", "size": 3150000, "sha256": "…"},
  "min_ver": "0.1.0"
}
```

- `notes` chỉ vài dòng ngắn để hiện trên web. Ghi chú dài để trong phần mô tả của Release, web có link "Xem chi tiết ↗".
- `min_ver`: bản cũ hơn mức này thì không cập nhật thẳng được (dành cho lúc đổi bảng phân vùng), web ghi rõ phải cắm USB.
- Số phiên bản lấy từ `PROJECT_VER`, không gõ tay. `release.py` từ chối chạy nếu tag đã tồn tại hoặc working tree chưa sạch.
- Release nháp (draft) và prerelease không được coi là bản mới.
- Repo phải để public thì loa mới tải được mà không cần token.

### 3.3 Kiểm tra bản mới (mặc định chỉ khi bấm trên web)
- Loa tải `https://github.com/phuc4real/meowbox-esp32/releases/latest/download/manifest.json`. Link này tự chuyển
  tới `manifest.json` của Release mới nhất, nên không phải gọi GitHub API (không dính giới hạn 60 request/giờ, không
  phải đọc JSON lớn của API).
- Mặc định loa **không tự gọi GitHub**. Chỉ kiểm tra khi người dùng bấm "Kiểm tra ngay" trên trang Hệ thống
  (`POST /api/update/check`). Lỗi mạng thì trả lỗi để web báo "Không kiểm tra được, loa chưa có internet?".
- Bật "Tự kiểm tra bản mới" (tùy chọn) thì loa kiểm tra thêm 2 phút sau khi có Wi-Fi và sau đó mỗi 24 giờ. Lỗi mạng
  lúc tự kiểm tra thì im lặng, lần sau thử lại.
- Kết quả lưu trong RAM, trả trong `GET /api/update`: `{"cur":"0.2.0","new":"0.2.1","notes":{…},"checked":…}`.
  `GET /api/status` thêm `upd:"0.2.1"` (không có bản mới thì không có trường này) để web hiện chấm báo trên menu mà
  không phải gọi thêm.
- So sánh phiên bản theo số (`0.10.0` > `0.9.0`), không so chuỗi.
- Cài đặt (trang Hệ thống): "Tự kiểm tra bản mới" bật/tắt, **mặc định tắt**, lưu trong config của loa.
- Màn Hệ thống trên loa hiện thêm một dòng "Có bản 0.2.1" khi lần kiểm tra gần nhất thấy bản mới. Không hiện popup
  trên loa.

### 3.4 Cập nhật
- `POST /api/update {"ver":"0.2.1"}` (cần đăng nhập web). Loa kiểm tra trước, không đạt thì trả lỗi rõ ràng:
  - pin dưới 30% mà không cắm sạc → `low_battery`;
  - đang có cập nhật khác chạy → `busy`;
  - `ver` không khớp bản mới nhất đã kiểm tra → `stale` (web tự kiểm tra lại).
- Đạt thì trả `{"ok":true}` ngay rồi chạy task cập nhật:
  1. dừng phát nhạc, màn loa hiện "Đang cập nhật 0.2.1… 37%", không cho thao tác;
  2. `esp_https_ota` tải `app.file` từ link Release (tự đi theo redirect sang máy chủ file của GitHub), ghi vào slot
     OTA còn trống, đồng thời tính SHA-256;
  3. SHA-256 hoặc kích thước khác `manifest.json` → hủy, báo `bad_hash`, vẫn chạy bản cũ;
  4. đúng thì đặt slot mới làm boot rồi khởi động lại. Rollback như hiện tại: bản mới chạy không ổn thì bootloader
     quay về bản cũ.
- `GET /api/update` trong lúc chạy trả `{"state":"download","pct":37}` → `verify` → `reboot`, hoặc `error` kèm `err`.
  Web hỏi mỗi giây. Mất kết nối lúc loa khởi động lại thì dùng lại `waitBack()` có sẵn.
- Sau khi khởi động lại, nếu bootloader đã rollback thì loa ghi lại, và `GET /api/update` trả
  `"last":{"ver":"0.2.1","err":"rollback"}` để web báo "Cập nhật lên 0.2.1 lỗi, loa đã quay về 0.2.0".
- RAM: TLS cần khoảng 40 KB heap trong. Dừng player trước khi tải để không chạy hai kết nối TLS cùng lúc.

### 3.5 Trang web
Trang Hệ thống, khối "Cập nhật firmware":

```
Phiên bản 0.2.0 · đã kiểm tra lúc 08:30             [Kiểm tra ngay]
┌──────────────────────────────────────────────┐
│ Có bản 0.2.1 (01/12/2026)                    │
│ • Giao diện web nhẹ hơn                      │
│ • …                           Xem chi tiết ↗ │
│                              [Cập nhật ngay] │
└──────────────────────────────────────────────┘
☐ Tự kiểm tra bản mới mỗi ngày                (mặc định tắt)
▸ Cập nhật bằng file .bin                       (khối tải file lên như hiện tại, thu gọn)
```

- Bấm "Cập nhật ngay" thì hỏi xác nhận bằng modal (ghi rõ loa sẽ khởi động lại, mất khoảng 1–2 phút), rồi hiện thanh
  tiến độ.
- Lần kiểm tra gần nhất thấy bản mới thì menu Hệ thống có chấm nhỏ (từ `upd` trong `/api/status`). Không hiện banner trên mọi trang.
- `proto` trong `manifest.json` lớn hơn của agent đang nối thì ghi thêm dòng "Sau khi cập nhật cần cập nhật cả MeowBox
  Agent trên PC", kèm link Release.
- `mock_server.py` giả lập đủ các trạng thái (có/không có bản mới, đang tải, lỗi hash, rollback) để làm giao diện.
  `test_api.py` thêm test cho `/api/update`. `web/API.md` thêm `/api/update`.

### 3.6 Kiểm thử
- Firmware build debug cho đổi URL `manifest.json` qua menuconfig, để thử với một Release thử hoặc file đặt trên máy
  tính mà không đụng Release thật.
- Trên loa thật:
  - từ 0.2.0 lên bản thử: cập nhật xong có web mới, font mới, cài đặt giữ nguyên;
  - rút Wi-Fi giữa lúc tải: báo lỗi, loa vẫn chạy bản cũ, bấm lại được;
  - sửa `sha256` trong `manifest.json` cho sai: báo `bad_hash`, không đổi boot;
  - bản thử cố tình không gọi `esp_ota_mark_app_valid_cancel_rollback()`: loa quay về bản cũ và web báo rollback;
  - pin dưới 30%: nút bị chặn với lý do rõ ràng.
- **Lần lên 0.2.0 từ 0.1.0 phải nạp bằng USB**, vì 0.1.0 chưa có tính năng này và web của 0.1.0 còn nằm ở `storage`.
  Ghi rõ trong release note.

### Không làm trong bản này
- Cập nhật phân vùng `storage` qua mạng. Khi nào có âm thanh/sprite ở đó thì thêm `storage` vào `manifest.json`, tải
  bản nén gzip (ảnh LittleFS 4.4 MB phần lớn là số 0), giải nén và ghi thẳng vào phân vùng. Cần thêm trang cứu hộ nhỏ
  nằm trong app phòng khi ghi hỏng giữa chừng.
- Tự cài bản mới khi không có ai bấm.
- Kênh beta (prerelease).

---

## 4. Phát radio ổn định hơn trên Wi-Fi lạ

### Hiện trạng
- Đem loa ra ngoài, dùng Wi-Fi khác thì radio hay bị lag, chậm, có lúc lỗi hẳn. Chưa biết chắc nguyên nhân vì loa
  chưa ghi lại số liệu gì về luồng nhạc.
- `wifi_mgr.c` bật `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` suốt thời gian chạy. Chế độ tiết kiệm điện này làm tốc độ tải
  và độ trễ thất thường, nhất là với router lạ và Wi-Fi phát từ điện thoại.
- `player.c`: ring buffer 256 KB trong PSRAM (khoảng 16 s nhạc 128 kbps). Có 48 KB (`PREBUF`, khoảng 3 s) là bắt đầu
  phát. Hết dữ liệu (`underrun`) thì quay về trạng thái đệm và lại chờ đủ 48 KB. Mạng chập chờn thì cứ phát 3 s rồi
  dừng, rồi lại phát 3 s.
- Nối lại khi đứt: chờ 1 s, 1 s, 3 s, 3 s, sau đó 8 s (`retries`).
- `/api/status` đã có `rssi`. Màn Hệ thống đã hiện `Wi-Fi <tên>, <rssi> dBm`.

### Kết quả mong muốn
- Trên Wi-Fi yếu, nhạc ít bị ngắt hơn: thà chờ lâu hơn một chút lúc đầu còn hơn ngắt liên tục.
- Khi vẫn bị lag thì nhìn màn loa hoặc trang web là biết do sóng yếu, mạng chậm hay đài có vấn đề.
- Pin không bị ảnh hưởng khi không phát nhạc.

> **Trạng thái (2026-11-23):** 4.1–4.3 xong, đã chạy trên loa. Chưa làm: đo pin trước/sau 4.1, đem loa ra chỗ bị lag
> để đo (4.4). Thêm ngoài plan: phát hiện đứng luồng (đang chờ đệm mà 10 s nhận dưới 1/4 bitrate thì nối lại, lỗi
> `stall`) vì có máy chủ nhỏ giọt vài byte nên không bao giờ hết thời gian chờ đọc; bộ đệm ICY metadata 4 KB chuyển
> sang PSRAM. Đo với zeno.fm (MP3 128k) ở nhà, Wi-Fi -49 dBm: máy chủ có lúc gửi 0 kbps trong 20 s rồi tự đóng kết
> nối, loa nối lại và phát tiếp với đệm 10 s; các đài AAC và HLS khác không hết đệm lần nào.
> Đã sửa thêm: lúc khởi động loa quét trước rồi thử các mạng đã lưu đang có sóng (giữ thứ tự ưu tiên), mạng không thấy
> thử sau; không quét khi Wi-Fi cài đặt đang mở. Vào Wi-Fi nhà lúc 9.3 s thay vì 19.1 s khi mạng đầu danh sách không có ở đây.

### 4.1 Tắt tiết kiệm điện Wi-Fi khi đang phát
- Bắt đầu phát (trạng thái `BUF`/`PLAY`) thì gọi `esp_wifi_set_ps(WIFI_PS_NONE)`. Dừng phát hoặc lỗi thì quay về
  `WIFI_PS_MIN_MODEM`.
- Gom vào một hàm `wifi_mgr_set_streaming(bool)` có đếm số lượt dùng, vì không chỉ player cần (sau này còn tải OTA ở
  mục 3.4). Tải OTA cũng dùng hàm này.
- Đổi qua lại có độ trễ: dừng phát 10 s mới bật lại tiết kiệm điện, để chuyển đài hoặc bấm next không bị bật tắt liên tục.
- Pin: tắt tiết kiệm điện tốn thêm điện khi đang phát. Đo dòng tiêu thụ trước/sau bằng log pin có sẵn
  (trang Pin, `Ước tính đầy→cạn`) và ghi kết quả vào đây. Nếu tốn quá nhiều thì chỉ tắt khi đang cắm sạc hoặc khi
  vừa bị hết đệm.

### 4.2 Bộ đệm tự tăng
- Mức đệm trước khi phát (`PREBUF`) không cố định nữa mà theo bậc: 48 KB → 96 KB → 160 KB (khoảng 3 s → 6 s → 10 s ở
  128 kbps).
  - Mỗi lần hết đệm trong lúc phát thì lên một bậc.
  - Phát liền 5 phút không bị hết đệm thì xuống một bậc.
  - Đổi đài thì giữ bậc hiện tại (cùng một mạng thì đài khác cũng sẽ gặp như vậy), đổi mạng Wi-Fi thì về bậc đầu.
- Tính theo thời gian thay vì byte khi biết bitrate (đọc `icy-br` hoặc từ header frame MP3/AAC), để đài 64 kbps và
  320 kbps chờ cùng số giây. Không biết bitrate thì dùng byte như trên.
- Ring buffer giữ 256 KB. Bậc cao nhất 160 KB vẫn còn chỗ để tải tiếp trong lúc phát.
- Đang đệm thì màn loa và trang web hiện phần trăm đệm (`Đang tải 60%`), thay cho chữ "Đang kết nối…" đứng yên.

### 4.3 Số liệu luồng nhạc
Player giữ một struct số liệu, reset mỗi lần bắt đầu phát một đài:

| Trường | Ý nghĩa |
|---|---|
| `kbps` | tốc độ tải thực tế, trung bình 5 s gần nhất |
| `br` | bitrate của đài (từ `icy-br` hoặc header frame), 0 nếu chưa biết |
| `buf` | số giây nhạc đang có trong bộ đệm |
| `pre` | bậc đệm hiện tại (giây) |
| `under` | số lần hết đệm từ lúc bắt đầu phát |
| `reconn` | số lần nối lại |
| `fmt` | `mp3` / `aac` / `ts` / `hls` |
| `err` | lỗi cuối: `dns`, `connect`, `http_<mã>`, `tls`, `timeout`, `decode` |

- `GET /api/status` → trong `play` thêm `"net":{kbps, br, buf, pre, under, reconn, fmt, err}`. Không phát thì không có `net`.
- Màn Hệ thống trên loa: khi đang phát thì thêm một dòng, ví dụ `Radio 112/128 kbps · đệm 9 s · ngắt 2`. Tốc độ tải
  thấp hơn bitrate của đài thì tô màu cảnh báo.
- Trang web, trang Radio: dưới tên đài đang phát có một dòng nhỏ giống màn loa. Kèm gợi ý theo số liệu:
  - RSSI dưới -75 dBm: "Sóng Wi-Fi yếu, để loa gần router hơn";
  - `kbps` thường xuyên thấp hơn `br`: "Mạng chậm hơn bitrate của đài, chọn đài bitrate thấp hơn";
  - `err` là `dns`/`connect`: "Không tới được máy chủ của đài".
- Ghi log ra serial mỗi 30 s trong lúc phát (một dòng), để khi đem loa ra ngoài có thể cắm USB đọc lại.
- `mock_server.py`, `test_api.py` và `web/API.md` cập nhật theo trường mới.

### 4.4 Kiểm thử
- Ở nhà: phát một đài MP3 128 kbps và một đài HLS 30 phút, số liệu hợp lý, không bị ngắt.
- Giả lập mạng yếu: phát Wi-Fi từ điện thoại, để loa xa dần. Thấy bậc đệm tăng, số lần ngắt giảm so với bản 0.1.0
  (đo cùng đài, cùng chỗ đứng, mỗi bản 10 phút).
- Đem loa ra chỗ đã bị lag sáng nay, ghi số liệu vào đây. Nếu vẫn thiếu băng thông thì đó là dữ liệu để quyết định có
  làm MeowBox Cloud (`docs/roadmap.md`) hay không.
- Đo pin trước/sau 4.1 như đã ghi ở trên.

---

## 5. Hiệu năng: review code, chạy thử toàn bộ, tối ưu RAM/PSRAM

Làm **cuối cùng**, sau khi mục 1–4 đã xong, vì các mục đó đổi nhiều code (web nhúng vào app, task cập nhật, bộ đệm
tự tăng). Riêng bước đo mốc (5.1, 5.2) chạy trên 0.1.0 ngay từ đầu để có số so sánh.

### Status (2026-11-24)
- Done: 5.1 tools, measurement after sections 1-4, 5.3 code review (`docs/perf/review-0.2.0.md`, fixes in
  `d95bc15`), heaviest scenario. Targets met: lowest internal RAM 40.8 KB, largest block 31 KB, 0 underruns
  (`docs/perf/results-0.2.0.md`).
- 5.4: nothing else needed by the numbers.
- Still to do on the speaker: the hands-on rows of the 5.2 checklist (buttons, battery, alarms, hotspot, two
  devices, rollback) and the 24 h run, on the final build before the Release. `PROJECT_VER` is still `0.1.0`:
  bump it to `0.2.0` when releasing.

### Hiện trạng (lúc lên plan)
- ESP32-S3, PSRAM octal 80 MHz, CPU 240 MHz. `SPIRAM_USE_MALLOC`: `malloc` trên 1 KB tự vào PSRAM, giữ 64 KB RAM trong
  cho DMA/driver. Wi-Fi/lwIP và mbedTLS đã được phép cấp phát trong PSRAM. Cho phép stack task nằm ở PSRAM nhưng
  chưa task nào dùng.
- `/api/status` đã có `heap`/`heap_min` (RAM trong) và `psram`/`psram_tot`. Chưa có số liệu theo từng task, chưa
  biết khối trống lớn nhất (phân mảnh).
- Stack các task (đều ở RAM trong): `decode` 8 KB, `fetch` 8 KB, `httpd` 8 KB, `weather` 6 KB, `battery`, `mic`,
  `sfx`, `wifi_conn` mỗi cái 4 KB, `memplay` 3 KB, `dns` 3 KB.
- Bộ đệm vào của decoder (`IN_CAP` 8 KB, `player.c`) xin trong RAM trong. Ring buffer 256 KB và bộ đệm ra ở PSRAM.
- TCP window mặc định 5760 B (`LWIP_TCP_WND_DEFAULT`), Wi-Fi `RX_BA_WIN` 16. Cửa sổ nhỏ thì tốc độ tải bị giới hạn
  khi máy chủ của đài ở xa (độ trễ cao), có liên quan trực tiếp tới mục 4.
- Build đang để `COMPILER_OPTIMIZATION_PERF` và assert bật.

### Kết quả mong muốn
- Có số liệu trước/sau cho từng thay đổi, không tối ưu theo cảm tính.
- Kịch bản nặng nhất (radio HTTPS + web mở + thông báo + tải OTA) vẫn còn ít nhất **40 KB** RAM trong lúc thấp nhất,
  khối trống lớn nhất ít nhất **16 KB**.
- Chạy liên tục 24 giờ không rò rỉ: `heap_min` sau giờ đầu không giảm tiếp.
- Mỗi task còn dư ít nhất 25% stack ở lúc dùng nhiều nhất (hoặc 768 B, lấy số lớn hơn).
- Toàn bộ chức năng của loa được chạy thử theo checklist, lỗi tìm thấy được sửa hoặc ghi lại.

### 5.1 Công cụ đo
- `GET /api/debug` (cần đăng nhập web). Decided in the review (2026-11-24): always on in 0.2.0, no Kconfig switch; it
  only reads counters and needs a login:
  - RAM trong: trống, thấp nhất, khối lớn nhất (`heap_caps_get_largest_free_block`), riêng phần DMA;
  - PSRAM: trống, thấp nhất, khối lớn nhất;
  - từng task: tên, core, priority, stack còn dư thấp nhất (`uxTaskGetStackHighWaterMark`), % CPU (bật
    `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`).
- Script `tools/memwatch.py`: gọi `/api/debug` mỗi 10 s, ghi CSV, cuối buổi in bảng tóm tắt (thấp nhất/cao nhất từng
  cột). Dùng cho cả kịch bản 5.2 và chạy 24 giờ.
- RAM tĩnh và flash: `idf.py size`, `idf.py size-components`, `idf.py size-files`, lưu kết quả mốc vào
  `docs/perf/` để so sánh sau mỗi thay đổi.

### 5.2 Chạy thử toàn bộ chức năng
Checklist chạy trên loa thật, ghi kết quả (OK / lỗi + mô tả) và số liệu `memwatch` vào `docs/perf/checklist-<ver>.md`:

| Nhóm | Việc cần thử |
|---|---|
| Khởi động, Wi-Fi | bật nguồn từ lúc cạn pin và lúc đang sạc; Wi-Fi cài đặt `MeowBox-XXXX`, captive portal, cài mạng mới; mất Wi-Fi rồi có lại; đổi qua lại giữa các mạng đã lưu; mở lại Wi-Fi cài đặt bằng VOL+ VOL-, từ web, bằng `wifi-setup` |
| Màn loa | đủ các tile, đổi theme (4 theme × ngày/đêm × hiệu ứng đầy đủ/tiết kiệm), đổi ngôn ngữ, chụp màn hình qua web |
| Radio | đài MP3, AAC, HLS, `.pls`/`.m3u`; next/prev; âm lượng; hẹn giờ tắt; đài chết; đổi đài liên tục 20 lần |
| YouTube | video, playlist, live; relay qua PC agent; PC tắt giữa chừng |
| Báo thức | chuông có sẵn, chuông là đài radio; báo thức lúc đang phát nhạc |
| Thông báo | `POST /api/notify` thường và ưu tiên cao; `agent say` (TTS); lịch sử thông báo; 30 thông báo liên tiếp |
| PC agent | số liệu PC, media, nối lại khi PC ngủ/thức; hai PC (mục 2.4) |
| Thời tiết, giờ | đổi thành phố, mất mạng lúc lấy thời tiết, SNTP |
| Pin | log pin, ước tính thời gian, rút/cắm sạc, chế độ tiết kiệm tự bật khi chạy pin |
| Mic | phần test mic ở màn Hệ thống |
| Web | mọi trang ở 390 px và 1280 px, đăng nhập/đăng xuất, đổi mật khẩu, mở trang trên 2 thiết bị cùng lúc |
| Cập nhật | OTA bằng file, cập nhật qua GitHub Release, rollback (mục 3.6) |

Kịch bản nặng nhất (đo riêng, ghi số thấp nhất): phát đài HTTPS + mở web trang Radio trên 2 thiết bị + gửi thông báo
mỗi 10 s + lấy thời tiết + chụp màn hình qua web. Sau đó bấm cập nhật qua GitHub trong lúc đang như vậy.

Chạy 24 giờ: radio phát liên tục 4 giờ, web mở suốt, thông báo mỗi 5 phút, báo thức 2 lần, `memwatch` ghi suốt.

### 5.3 Review code
Đọc lại toàn bộ `firmware/components` và `main`, ưu tiên theo thứ tự `player`, `net`, `apps`, `ui`. Ghi kết quả vào
`docs/perf/review-0.2.0.md` (vị trí, vấn đề, mức độ, đã sửa chưa). Những điểm cần soi:
- rò rỉ trên đường lỗi: `esp_http_client_cleanup`, `cJSON_Delete`, `free` khi `return` sớm, task tự xoá mà quên giải phóng;
- không kiểm tra kết quả `malloc`/`heap_caps_malloc`;
- biến dùng chung giữa các task mà không có khoá hoặc chỉ dựa vào `volatile` (ví dụ `s_fd` trong `pc_app.c`);
- gọi LVGL ngoài task UI mà không giữ khoá LVGL;
- handler của `esp_http_server` làm việc lâu (gọi mạng, ghi flash) làm nghẽn các request khác;
- `snprintf`/`strcpy` có thể tràn, chuỗi từ người dùng không giới hạn độ dài;
- task ghi flash (NVS, LittleFS `/data`, OTA) phải có stack ở RAM trong;
- timeout và số lần thử lại cho mọi kết nối mạng.
- Công cụ hỗ trợ: `idf.py clang-check` (dùng `.clang-format` có sẵn), và `/code-review` trên từng phần thay đổi lớn.
- Phía web và agent: chạy lại `test_api.py`, `node --check`, `ruff` (`ruff.toml` có sẵn) cho `pc-agent`.

### 5.4 Hướng tối ưu (chỉ làm khi số đo ở 5.1–5.2 cho thấy cần)
Nguyên tắc: RAM trong chỉ dành cho thứ bắt buộc (DMA cho I2S và màn hình, ISR, bộ đệm Wi-Fi tĩnh, stack của task ghi
flash, dữ liệu dùng liên tục trong vòng lặp nóng). Còn lại cho vào PSRAM.

| Hướng | Chi tiết | Cần chú ý |
|---|---|---|
| Thu nhỏ stack | đặt lại stack từng task theo số dư đo được ở 5.1, giữ dư 25% | đo ở kịch bản nặng nhất, không phải lúc rảnh |
| Stack ở PSRAM | task không bao giờ ghi flash (`fetch`, `weather`, `dns`, `memplay`) tạo bằng `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` | task có stack ở PSRAM mà ghi flash sẽ crash; chậm hơn một chút, không dùng cho `decode` |
| Bộ đệm ở PSRAM | bộ đệm mạng 4 KB trong `fetch`/HLS, bộ đệm JSON lớn, bản chụp màn hình | `IN_CAP` của decoder giữ ở RAM trong nếu đo thấy decode chậm đi khi chuyển |
| Tăng tốc mạng | `LWIP_TCP_WND_DEFAULT`/`SND_BUF` lên 16–32 KB, `ESP_WIFI_RX_BA_WIN` 32, thêm bộ đệm RX động; với `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` chúng nằm ở PSRAM | đo `kbps` ở mục 4.3 trước/sau, trên cả Wi-Fi nhà và Wi-Fi yếu |
| LVGL | kiểm tra bộ đệm vẽ nằm ở đâu (phải ở RAM DMA nếu đẩy thẳng ra màn hình), ảnh/sprite và cache ảnh ở PSRAM | đo thời gian vẽ một khung trước/sau |
| `esp_http_server` | `max_open_sockets` 7 và stack 8 KB: xem có giảm được không khi web đã nhúng vào app và có cache (mục 1.2, 3.1) | trang web mở trên 2 thiết bị vẫn phải mượt |
| RAM tĩnh | mảng `static` lớn (theo `idf.py size-files`) chuyển sang cấp phát lúc cần hoặc `EXT_RAM_BSS_ATTR` (cần `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) | chỉ cho dữ liệu không dùng trong ISR/DMA |
| Phân mảnh | cấp phát một lần lúc khởi động cho các bộ đệm dùng mãi (player, httpd), tránh xin/trả liên tục | theo dõi khối trống lớn nhất trong lúc chạy 24 giờ |
| CPU và pin | bật quản lý nguồn (`CONFIG_PM_ENABLE`, hạ xung khi rảnh, ví dụ 80–240 MHz), xem % CPU của `decode` khi phát | đo pin trước/sau. Không dùng light sleep nếu ảnh hưởng màn hình hoặc âm thanh |
| Build phát hành | tắt assert hoặc để mức thấp hơn, xem `size` thay đổi bao nhiêu | giữ log lỗi |

### 5.5 Ghi kết quả
`docs/perf/results-0.2.0.md`, mỗi thay đổi một dòng:

| Thay đổi | RAM trong thấp nhất | Khối lớn nhất | PSRAM thấp nhất | `kbps` radio | Pin (mA lúc phát) | Ghi chú |
|---|---|---|---|---|---|---|
| mốc 0.1.0 | | | | | | |
| sau mục 1–4 | | | | | | |
| … | | | | | | |

### Thứ tự
1. 5.1 công cụ đo + đo mốc trên 0.1.0 (làm đầu tiên, trước khi bắt đầu mục 1).
2. Làm mục 1–4.
3. 5.2 chạy thử toàn bộ + 5.3 review code, sửa lỗi tìm thấy.
4. 5.4 tối ưu theo số đo, mỗi thay đổi đo lại và ghi vào 5.5.
5. Chạy lại checklist 5.2 và 24 giờ trên bản cuối trước khi tạo Release.
