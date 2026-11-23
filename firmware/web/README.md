# Trang web cấu hình MeowBox

Giao diện web chạy ngay trên loa (`http://meowbox.local`). Viết bằng HTML + CSS + JS thuần, không framework,
không CDN. Font tự host trong `assets/fonts/` (chỉ tải font của theme đang dùng). `build.py` đóng tất cả thành một file
`components/webui/web.pack` **nhúng trong app firmware**, nên web luôn cập nhật cùng firmware (OTA có rollback);
firmware phục vụ bằng `esp_http_server`.

| File | Nội dung |
|---|---|
| `src/index.html`, `src/app.css`, `src/app.js` | ứng dụng một trang, route bằng hash (`#/radio`, `#/look`…) |
| `src/lang/vi.json`, `src/lang/en.json` | mọi chuỗi giao diện, mỗi ngôn ngữ một file, cùng bộ khoá |
| `src/theme/<id>.css` | chi tiết riêng của từng theme, chỉ tải file của theme đang dùng |
| `assets/fonts/`, `assets/themes/*.png` | font (sinh bởi `tools/fonts/gen_theme_fonts.py`) và ảnh xem trước theme |
| `src/themes.css` | biến CSS của 4 theme, **sinh bởi** `python tools/gen_themes.py` (chạy trong `firmware/`), không sửa tay |
| `themes.json` | danh sách theme cho mock (`GET /api/themes`), cũng sinh bởi `gen_themes.py` |
| `API.md` | hợp đồng REST mà firmware phải làm đúng (endpoint, JSON, quy tắc đăng nhập) |
| `mock_server.py` | loa giả bằng Python (chỉ stdlib), làm đúng `API.md`, giữ trạng thái trong RAM |
| `test_api.py` | test hợp đồng: chạy mock, gọi mọi endpoint, kiểm tra mọi đường `/api/...` trong `app.js` đều có |
| `build.py` | rút gọn nhẹ, gắn hash vào tên file, gzip, đóng thành `components/webui/web.pack`, in kích thước |

## Chạy thử trên máy tính

```sh
python mock_server.py            # http://localhost:8080, đăng nhập meowbox / m3owb0x2026
python mock_server.py --no-auth  # bỏ qua đăng nhập khi đang sửa giao diện
python mock_server.py --theme zen --no-screenshot   # bắt đầu với theme Zen, /api/screenshot trả 404
python mock_server.py --dist     # phục vụ bản đã build (web.pack) y như firmware: tên có hash, ETag/304, cache
python test_api.py               # test API, phải ra "ALL PASSED"
node audit.mjs                   # sau build.py: 4 theme × 2 cỡ × mọi trang, so chức năng giữa các theme, ảnh vào screens/
node --check src/app.js          # kiểm tra cú pháp JS
```

Mở trên điện thoại cùng mạng: `http://<IP máy tính>:8080`. Tìm đài (Radio Browser) và tìm thành phố
(open-meteo) được gọi thẳng từ trình duyệt nên cần có internet.

## Build cho firmware

```sh
python build.py        # ghi firmware/components/webui/web.pack (rồi build lại firmware: idf.py build)
python build.py --dist out/   # thêm: ghi các file đã build ra out/ để xem
```

- Mọi file trừ `index.html`, font và ảnh xem trước có hash trong tên (`app.1be53fe1.js`). `index.html` được viết lại
  để trỏ tới tên có hash và có thêm `window.MB` (ngôn ngữ/theme → tên file); `window.mbP()` trong `index.html`
  dùng bảng này, chạy từ `src/` (không có bảng) thì dùng tên thường.
- Firmware gửi file có hash với `Cache-Control: public, max-age=31536000, immutable`, font và ảnh `max-age=604800`,
  `index.html` `no-cache`. Mọi file có `ETag`; `If-None-Match` khớp thì trả `304` (xem "Static files" trong `API.md`).
- Ngân sách: bản gzip của lần tải đầu (`index.html` + `themes.css` + `app.css` + `app.js` + một file ngôn ngữ + một
  file theme) ≤ 30 KB, `build.py` báo lỗi nếu vượt. Font và ảnh xem trước tải riêng.

## Theme

- `data-theme` trên `<html>` lấy từ `display.theme` của loa (`GET /api/theme` trước khi đăng nhập, `GET /api/config/display` sau đó);
  lưu tạm trong `localStorage` để trang mở ra đúng theme ngay. Tuỳ chọn "Trang web: luôn Swiss" chỉ lưu trong trình duyệt.
- `app.css` chỉ dùng biến của `themes.css` (biến của cả 4 theme, luôn tải: trang Giao diện dùng để vẽ ô xem trước);
  phần riêng của từng theme nằm trong `theme/<id>.css`, `index.html` tải file của theme đang dùng, `setTheme()` đổi file.
  Theme chỉ đổi cách trông, không đổi bố cục.
- Ảnh chụp kiểm tra giao diện (390 px và 1280 px) để trong `screens/` (không commit).

## Ghi chú

- Ngôn ngữ trang web mặc định tiếng Việt, nút `EN`/`VI` trên thanh trên cùng (lưu trong trình duyệt).
  Mọi chuỗi nằm trong `src/lang/<mã>.json`; trang chỉ tải file của ngôn ngữ đang chọn (`index.html` bắt đầu tải
  song song với `app.js`). Chuỗi có biến viết `{n}`, gọi `t('durM',{n:5})`. Thêm ngôn ngữ = thêm một file với đủ
  khoá như `vi.json` (`test_api.py` kiểm tra hai file cùng bộ khoá và mọi khoá `t('…')` trong code đều có).
  Ngôn ngữ trên màn loa chỉnh riêng ở trang Hiển thị.
- Không dùng `alert()`/`confirm()`: mọi hộp thoại là modal trong trang.
- Còn dùng mật khẩu mặc định `meowbox` thì có banner nhắc đổi.
- Tên thành phố từ open-meteo đôi khi dùng chữ `Ð` (U+00D0); trang web đổi sang `Đ` (U+0110) cho khớp font trên loa.
