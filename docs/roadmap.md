# MeowBox roadmap

Các ý tưởng để dành, chưa xếp vào bản nào. Khi quyết định làm thì chuyển sang plan của bản đó
(ví dụ `firmware_v0.2.0.md`) và ghi lại ở đây là đã chuyển.

| Ý tưởng | Khi nào làm | Trạng thái |
|---|---|---|
| MeowBox Cloud (Cloudflare Worker) | khi số liệu ở mục 4 của v0.2.0 cho thấy vẫn thiếu băng thông, hoặc cần thông báo khi loa ở ngoài | để dành |
| Tối ưu lại các theme về UI/UX | sau v0.2.0 | để dành |

---

## MeowBox Cloud

### Vì sao
- Đem loa ra ngoài thì radio hay bị lag trên Wi-Fi lạ (2026-11-23). v0.2.0 sửa phần trong loa trước (tắt tiết kiệm
  điện Wi-Fi khi phát, bộ đệm tự tăng, số liệu luồng nhạc). Cloud chỉ làm nếu sau đó vẫn chưa đủ.
- Thông báo hiện chỉ tới được loa khi gửi từ cùng mạng LAN (PC agent hoặc `POST /api/notify`). Ra ngoài thì không
  nhận được gì.

### Ý tưởng
Loa tự mở **một WebSocket ra ngoài** tới một Durable Object trên Cloudflare (mỗi loa một object). Loa là bên nối
ra nên NAT, client isolation hay đổi mạng đều không ảnh hưởng: ở đâu có internet là dùng được. Dùng WebSocket
Hibernation API để Durable Object không tốn tiền khi không có gì để gửi.

| Việc | Cách làm | Đánh giá |
|---|---|---|
| Nhận thông báo từ webhook | `POST https://<worker>/hook/<secret>` (GitHub, Uptime Kuma, Home Assistant…) → Worker chuẩn hoá thành `{title,text,priority}` → Durable Object đẩy xuống loa | hợp nhất, nên làm đầu tiên |
| Gộp luồng HLS | Worker tải playlist `.m3u8` và các đoạn, trả cho loa một luồng HTTP liền | giúp nhiều với đài HLS trên mạng yếu. Worker chỉ chuyển byte nên ít CPU |
| Proxy đài MP3/AAC thường | Worker chuyển tiếp luồng | lợi ít: Wi-Fi yếu vẫn là Wi-Fi yếu. Chỉ dùng cho đài có vấn đề HTTPS/chứng chỉ |
| Điều khiển loa từ xa | trang web gọi Worker, Worker chuyển lệnh qua WebSocket | để sau, cần đăng nhập và phân quyền |

### Những việc Worker không làm được
- **Chuyển mã sang Opus bitrate thấp** (mono 32–48 kbps, nhẹ hơn khoảng 3–4 lần so với 128 kbps stereo; loa vốn trộn
  về mono): Worker không chạy được ffmpeg và bị giới hạn CPU. Cần một trong hai:
  - PC ở nhà (PC agent đã có ffmpeg) mở ra internet bằng Cloudflare Tunnel: miễn phí, nhưng PC phải bật;
  - Cloudflare Containers: gói Workers trả phí (5 USD/tháng) cộng tiền dùng.
  - Phía loa: kiểm tra `esp_audio_codec` có giải mã Opus/OGG qua `simple_dec` không, và CPU còn đủ không.
- **YouTube**: YouTube chặn mạnh IP của các datacenter (kể cả Cloudflare). Phải chạy yt-dlp trên IP mạng nhà, tức là
  PC agent qua Cloudflare Tunnel. Relay phải có xác thực khi mở ra internet (hiện chỉ lọc theo IP loa trong LAN).
- **UDP** (cách xiaozhi-esp32 dùng MQTT + UDP cho giọng nói): Worker không nhận UDP. Với radio thì cũng không cần:
  trễ vài giây không sao, quan trọng là không đứt, nên HTTP/WebSocket qua TCP hợp hơn.

### Về việc xiaozhi đổi MP3 sang OGG
Hai lý do (chưa đọc lại repo để kiểm tra):
- dùng chung bộ giải mã Opus mà pipeline giọng nói đã có, khỏi phải kèm bộ giải mã MP3;
- file Opus nhỏ hơn MP3 ở cùng chất lượng, nhất là với giọng nói và loa nhỏ.

Với MeowBox thì lý do đầu không áp dụng: loa đã phải giải mã MP3/AAC cho radio. Chỉ đáng đổi sang Opus khi có máy
chủ chuyển mã luồng nhạc như trên.

### Bảo mật
- Mỗi loa có một khoá thiết bị riêng (sinh lúc ghép với Cloud, lưu trong `/data`) để nối WebSocket.
- Mỗi nguồn webhook có một secret riêng trong URL, xoá được từng cái. Giới hạn số thông báo mỗi phút.
- Cho tắt hẳn Cloud trong cài đặt. Tắt thì loa không nối ra ngoài.

### Cần chốt khi bắt đầu
1. Tài khoản Cloudflare và domain dùng cho Worker.
2. Gói miễn phí hay gói trả phí (cần nếu dùng Containers).
3. Có dựa vào PC ở nhà (Tunnel) cho YouTube và chuyển mã khi ở ngoài không.

---

## Tối ưu lại các theme về UI/UX

### Vì sao
- Đo ngày 2026-11-23 (`docs/perf/results-0.2.0.md`): ở màn Đồng hồ, chữ quá dài so với khung (thời tiết, âm lịch) chạy
  liên tục nên màn hình bị vẽ lại khoảng 85 lần/giây với Swiss (17% CPU một nhân) và khoảng 120 lần/giây với Brutal (30%).
  Zen và Cyberpunk có chữ vừa khung nên chỉ 3–4%.
- Tạm thời (v0.2.0): chữ chạy một vòng rồi nghỉ 8 s (`one_line()` trong `components/ui/ui.c`). Chưa sửa tận gốc.

### Việc cần làm
- Rà từng theme × từng màn (Đồng hồ, Radio, PC, Mèo, Hệ thống) × ngày/đêm: dòng nào hay bị tràn khung, sắp xếp lại
  bố cục hoặc rút gọn nội dung để vừa khung thay vì phải chạy chữ (ví dụ thời tiết của Swiss chỉ rộng 74 px).
- Quy ước chung: chỗ nào được chạy chữ (tên bài đang phát), chỗ nào cắt bằng "…", chỗ nào xuống dòng.
- Đo chi phí vẽ của từng hiệu ứng (lưới của Brutal, scanline và lệch màu của Cyberpunk, chữ nghiêng) và xem lại chế độ
  tiết kiệm (mục 1.3 của v0.2.0) cho cả màn loa.
- Có một vùng 1 px ở `x=0` (`0,0-0,26` đến `0,0-0,36`) bị vẽ lại liên tục trên mọi theme: tìm ra object nào.
- Đồng bộ với giao diện web (mục 1.3 của v0.2.0) để cùng một theme trông và hoạt động giống nhau trên loa và trên web.
