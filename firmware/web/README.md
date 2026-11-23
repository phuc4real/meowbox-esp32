# Trang web cấu hình MeowBox

Giao diện web chạy ngay trên loa (`http://meowbox.local`). Viết bằng HTML + CSS + JS thuần, không framework,
không CDN. Font tự host trong `storage/web/fonts/` (chỉ tải font của theme đang dùng). Được nén gzip và lưu trong LittleFS, firmware phục vụ bằng `esp_http_server`.

| File | Nội dung |
|---|---|
| `src/index.html`, `src/app.css`, `src/app.js` | ứng dụng một trang, route bằng hash (`#/radio`, `#/look`…) |
| `src/themes.css` | biến CSS của 4 theme, **sinh bởi** `python tools/gen_themes.py` (chạy trong `firmware/`), không sửa tay |
| `themes.json` | danh sách theme cho mock (`GET /api/themes`), cũng sinh bởi `gen_themes.py` |
| `API.md` | hợp đồng REST mà firmware phải làm đúng (endpoint, JSON, quy tắc đăng nhập) |
| `mock_server.py` | loa giả bằng Python (chỉ stdlib), làm đúng `API.md`, giữ trạng thái trong RAM |
| `test_api.py` | test hợp đồng: chạy mock, gọi mọi endpoint, kiểm tra mọi đường `/api/...` trong `app.js` đều có |
| `build.py` | rút gọn nhẹ + gzip vào `firmware/storage/web/*.gz`, in kích thước |

## Chạy thử trên máy tính

```sh
python mock_server.py            # http://localhost:8080, đăng nhập admin / meowbox
python mock_server.py --no-auth  # bỏ qua đăng nhập khi đang sửa giao diện
python mock_server.py --theme zen --no-screenshot   # bắt đầu với theme Zen, /api/screenshot trả 404
python test_api.py               # test API, phải ra "ALL PASSED"
node --check src/app.js          # kiểm tra cú pháp JS
```

Mở trên điện thoại cùng mạng: `http://<IP máy tính>:8080`. Tìm đài (Radio Browser) và tìm thành phố
(open-meteo) được gọi thẳng từ trình duyệt nên cần có internet.

## Build cho firmware

```sh
python build.py        # ghi firmware/storage/web/{index.html,themes.css,app.css,app.js}.gz
```

Firmware trả các file này với `Content-Encoding: gzip` (xem phần "Static files" trong `API.md`).
Ngân sách: tổng bản gzip của 4 file ≤ 30 KB (build.py báo lỗi nếu vượt). Font và ảnh xem trước theme là file riêng.

## Theme

- `data-theme` trên `<html>` lấy từ `display.theme` của loa (`GET /api/theme` trước khi đăng nhập, `GET /api/config/display` sau đó);
  lưu tạm trong `localStorage` để trang mở ra đúng theme ngay. Tuỳ chọn "Trang web: luôn Swiss" chỉ lưu trong trình duyệt.
- `app.css` chỉ dùng biến của `themes.css`; phần riêng của từng theme nằm trong các khối `[data-theme="…"]` cuối file.
  Theme chỉ đổi cách trông, không đổi bố cục.
- Ảnh chụp kiểm tra giao diện (390 px và 1280 px) để trong `screens/` (không commit).

## Ghi chú

- Ngôn ngữ trang web mặc định tiếng Việt, nút `EN`/`VI` trên thanh trên cùng (lưu trong trình duyệt).
  Mọi chuỗi nằm trong object `L` đầu `app.js`; thêm ngôn ngữ = thêm một khoá. Ngôn ngữ trên màn loa
  chỉnh riêng ở trang Hiển thị.
- Không dùng `alert()`/`confirm()`: mọi hộp thoại là modal trong trang.
- Còn dùng mật khẩu mặc định `meowbox` thì có banner nhắc đổi.
- Tên thành phố từ open-meteo đôi khi dùng chữ `Ð` (U+00D0); trang web đổi sang `Đ` (U+0110) cho khớp font trên loa.
