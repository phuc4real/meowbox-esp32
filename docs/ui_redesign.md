# MeowBox UI redesign

> Dựa trên buổi review ngày 2026-11-22. Ảnh chụp thật của loa lấy qua `GET /api/screenshot?tile=N`,
> ảnh web chụp ở khổ 390 px và 1280 px.
> Áp dụng cho cả hai giao diện: **màn hình loa** (LVGL, 296×240) và **trang web cài đặt** (`firmware/web`).

---

## 1. Hiện trạng: những gì review tìm ra

### Màn hình loa

| # | Vấn đề | Mức |
|---|---|---|
| D1 | Nút "Ngủ" hiện thành "Naủ": nút cao 40 px chứa 2 dòng × 22 px nên đuôi chữ "g" bị cắt | lỗi |
| D2 | Tắt đèn ở màn Mèo phủ một khối chữ nhật tối cạnh sắc lên vùng chơi, trông như lỗi vẽ | lỗi |
| D3 | Màn PC khi chưa kết nối hiện đường dẫn dev `pc-agent/agent.py` | lỗi |
| D4 | Màn Hệ thống ngắt `meowbox.local` thành "meowbox." / "local" | lỗi |
| D5 | Không có dấu chỉ trang: 5 màn vuốt ngang nhưng không biết đang ở đâu | điều hướng |
| D6 | Màn Đồng hồ: khi thời tiết chưa tải thì 35% dưới trống, còn một vạch kẻ lơ lửng; icon trạng thái sát góc bo | bố cục |
| D7 | Màn Radio: tên bài trống khi dừng làm khoảng cách lệch; tên đài không trông như bấm được nên phải có dòng hướng dẫn | bố cục |
| D8 | Màn Mèo: mèo con nhỏ (~60 px); chữ chỉ số dính thanh; cột chỉ số sát mép; dải trống giữa vùng chơi và nút; "Sức" khó hiểu | bố cục |
| D9 | Tiêu đề mỗi màn mỗi kiểu (Đồng hồ không có; Radio cam, PC xanh); dấu "·" ở mọi dòng | nhất quán |
| D10 | Chế độ đêm chỉ giảm độ sáng, màu vẫn như ban ngày | tính năng |

### Trang web

| # | Vấn đề | Mức |
|---|---|---|
| W1 | Chữ to nhất trang Tổng quan là "Đã dừng": trạng thái tiêu cực chiếm vị trí tiêu điểm | thứ bậc |
| W2 | 9 ô thông số nặng ngang nhau; phiên bản/uptime/RAM/PSRAM ngang hàng với pin, Wi-Fi | thứ bậc |
| W3 | Mọi khối là thẻ bo góc giống hệt nhau, Tổng quan có thẻ lồng thẻ | phong cách |
| W4 | Icon menu là emoji (📶📻⏰): khác nhau theo hệ điều hành, lệch với logo mèo SVG | phong cách |
| W5 | Thẻ Pin: biểu đồ không trục thời gian, không giãn hết chiều ngang; lịch sử là chuỗi chữ nối "·" | dữ liệu |
| W6 | Nút ↑ ↓ ✎ ✕ trong danh sách đài khoảng 24 px (cần ≥ 44 px) | chạm |
| W7 | Banner/link dùng mũi tên "→" kiểu mẫu chung | nhỏ |

Phần làm tốt và **giữ nguyên**: thứ bậc giờ to ở màn Đồng hồ, dòng lịch âm màu vàng, sprite mèo pixel,
chip ngày T2…CN của trang Báo thức, ô chọn màu lông, layout mobile không tràn ngang.

---

## 2. Hướng thiết kế

**Chủ đề:** một chiếc đèn ngủ có mèo ở đầu giường. Loa nằm trong phòng ngủ hoặc trên bàn, được nhìn
nhiều nhất lúc sáng sớm và ban đêm. Giao diện vì vậy phải dịu mắt, đọc được từ xa, và ấm như ánh đèn
chứ không lạnh như dashboard.

**Một điểm nhấn duy nhất: thế giới pixel của con mèo.** Con mèo pixel là tài sản riêng nhất của project.
Mình mở rộng ngôn ngữ pixel đó cho **mặt đồng hồ** (chữ số pixel, cùng lưới với sprite mèo) và cho con
mèo nhỏ sống ở góc màn đồng hồ. Mọi thứ còn lại (chữ, thanh, nút) giữ yên lặng, gọn, dùng một font.

### So với các lối mòn

| Lối mòn hay gặp | Bản hiện tại | Đổi thành |
|---|---|---|
| Nền gần đen + đúng một màu nhấn chói | `#0E1116` + cam `#FFB347` | Nền **mực đêm xanh tím** + **ánh đèn hổ phách** + màu kem như giấy cũ. Ban đêm chuyển hẳn sang một tông đỏ hổ phách |
| Thẻ bo góc giống hệt nhau | Mọi khối là thẻ 12 px | Thứ bậc bằng khoảng trắng và cỡ chữ; chỉ dùng khung cho thứ bấm được |
| Dấu "·" nối thông tin | Gần như mọi dòng | Xuống dòng, hoặc đặt cạnh nhau theo cột |
| Emoji làm icon | Menu web | Bộ icon pixel 12×12 vẽ cùng lưới với mèo |

---

## 3. Token

### Màu

| Tên | Hex | Dùng cho |
|---|---|---|
| `ink` (mực đêm) | `#161A2E` | nền màn hình loa, nền web chế độ tối |
| `ink-2` | `#323A5E` | nền nút và vùng chạm, đường kẻ chia |
| `paper` (kem giấy) | `#F3E6CC` | chữ chính trên nền tối; nền web chế độ sáng |
| `lamp` (đèn hổ phách) | `#F2B256` | màu nhấn: giờ đang chọn, nút chính, lịch âm |
| `moss` (rêu) | `#8CC7A1` | trạng thái tốt (đang phát, đã kết nối, chỉ số cao) |
| `ember` (than hồng) | `#FF7B5C` | cảnh báo, chỉ số thấp, báo thức |
| `dusk` (chữ phụ) | `#9097B5` | chữ phụ, nhãn |
| `night-red` | `#FF5A2E` | **toàn bộ** chữ và icon khi ở chế độ đêm, nền `#000000` |

Tương phản trên nền `ink` (đã tính theo WCAG): `paper` 13.9:1, `lamp` 9.2:1, `moss` 8.8:1, `ember` 6.7:1, `dusk` 6.0:1, đều đạt AA. `night-red` trên nền đen: 6.8:1. `ink-2` so với `ink` là 1.6:1, đủ để thấy mặt nút; chữ `paper` trên `ink-2` là 8.9:1. Nút nào cần nhận ra chắc chắn (yêu cầu 3:1 cho thành phần giao diện) thì thêm viền 1 px màu `dusk`.

### Chữ

| Vai trò | Font | Cỡ |
|---|---|---|
| Mặt đồng hồ | **Chữ số pixel MeowBox**: vẽ tay trên lưới 6×10, phóng ×6 bằng nearest-neighbour, cùng lưới với sprite mèo. Chỉ cần `0–9 : -` | 60 px chiều cao |
| Chữ giao diện | Be Vietnam Pro: font của người Việt, dấu đẹp, đã có sẵn | 14 / 20 (Regular), 20 (Bold) |
| Web | Be Vietnam Pro (tự host, tập con tiếng Việt), số dùng `tabular-nums` | thang 14 / 16 / 20 / 28 / 40 |

Quy tắc:
- Câu viết hoa chữ đầu (sentence case), không viết hoa toàn bộ cho nhãn.
- Không chèn nhãn nhỏ thừa phía trên nội dung.
- Không dùng dấu "·" để nối thông tin.

### Lưới và khoảng cách (loa)

- **Safe area** (measured 2026-11-25, docs/pinmap.md "Visible area"): the edges are usable to the pixel (left ~3 px hidden), but the glass rounds each corner with a radius of ~60 px. Content keeps the `UI_SAFE_X` margin; anything in the top or bottom ~60 px rows uses `ui_inset_l()` / `ui_inset_r()`. Page indicators are centred. Check with `tools/ui_shots.py`.
- **Nhịp dọc 8 px.** Mỗi màn có 3 vùng: **đầu** (cao 24, tiêu đề + trạng thái) / **thân** / **chân** (cao 44: chấm chỉ trang hoặc hàng nút).
- **Nút chạm** tối thiểu 44×40 px. Một nút chỉ có icon *hoặc* chữ một dòng, không xếp icon trên chữ trong 40 px.

---

## 4. Wireframe màn hình loa (296×240)

### Đồng hồ: màn mặc định, nơi đặt điểm nhấn

```
┌──────────────────────────────────────┐
│   Wi-Fi                    82%  ▮▮▮▯ │  đầu: icon nhỏ, cách góc ≥ 24 px
│                                      │
│      ██ ██   ██ ██                    │  chữ số PIXEL màu paper,
│      ██ ██ ▪ ██ ██        /\_/\       │  dấu ":" nhấp nháy theo giây
│      ██ ██ ▪ ██ ██       ( -.- ) zz   │  mèo ngồi góc phải dưới (×2):
│                                      │  ngủ ban đêm, kêu đói khi đói
│   Chủ Nhật 22 tháng 11               │  paper 20
│   14 tháng Mười, năm Bính Ngọ        │  lamp 14
│   22°  Trời quang     25° / 16°      │  dusk 14; chưa có dữ liệu → ẩn cả dòng
│               ● ○ ○ ○ ○              │  chấm chỉ trang
└──────────────────────────────────────┘
```

- Bỏ số giây cạnh giờ; dấu hai chấm nhấp nháy thay cho giây.
- Báo thức kế tiếp hiện thành icon chuông nhỏ trên đầu, cạnh Wi-Fi, chỉ khi có báo thức.
- Mèo ở góc lấy trạng thái thật từ `cat_app` (ngủ / ngồi / đói / ốm). **Chạm vào mèo thì chuyển sang màn Mèo.**

### Radio

```
┌──────────────────────────────────────┐
│ Radio                       ◂)) 65   │
│                                      │
│   Lofi Girl                  ▾       │  tên đài dạng nút: viền ink-2, có ▾
│   Snowman – Kudasai                  │  tên bài (luôn giữ chỗ 1 dòng)
│   ▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂ Đang phát          │  thanh đệm + trạng thái chung 1 dòng
│                                      │
│        ( ⏮ )   (  ▶  )   ( ⏭ )       │
│               ○ ● ○ ○ ○              │
└──────────────────────────────────────┘
```

- Bỏ dòng "Chạm tên đài để chọn" vì tên đài đã trông như nút.
- Khi dừng: dòng tên bài hiện "Bấm ▶ để nghe" (dusk) thay vì để trống.

### PC

- Đã kết nối: giữ 4 vòng (CPU / RAM / GPU / nhiệt độ) và thẻ nhạc. Bỏ khung thẻ, chỉ cần một đường kẻ `ink-2` ngăn cách.
- Chưa kết nối: icon máy tính pixel + "Máy tính chưa kết nối" + "Mở MeowBox trên máy tính để xem CPU, RAM và điều khiển nhạc."

### Mèo

```
┌──────────────────────────────────────┐
│ Mướp, mèo con                        │
│                          No     ▮▮▮▮▯│  nhãn bên trái, thanh bên phải, cùng hàng
│        /\_/\             Vui    ▮▮▮▯▯│
│       ( o.o )            Sạch   ▮▮▮▮▮│
│        > ^ <             Năng lượng ▮▮│
│   ────────────────                   │
│   Rừ rừ…                             │  dòng trạng thái của mèo (lamp)
│  [ Ăn ] [ Chơi ] [ Tắm ] [ Tắt đèn ] │  một dòng chữ mỗi nút, cao 40
└──────────────────────────────────────┘
```

- Mèo con hiện ×4 (sprite 23 px thành ~92 px); mèo lớn ×3. Kích thước nhìn trên màn ổn định qua các tuổi.
- Tắt đèn: nền vùng chơi chuyển dần sang `#0A0C16` (animation 400 ms), thêm vài ngôi sao pixel. Không phủ khối chữ nhật lên trên nữa.
- Nhãn "Sức" đổi thành "Năng lượng". Nút "Ngủ" đổi thành "Tắt đèn" / "Bật đèn" cho đúng việc nó làm.

### Hệ thống

- Khung đo vùng an toàn chỉ hiện khi bật "Hiệu chỉnh màn hình" (giữ lâu tiêu đề). Mặc định ẩn.
- Mỗi thông tin một dòng: tên Wi-Fi, địa chỉ (`meowbox.local` và IP tách 2 dòng), pin, phiên bản.

### Thành phần dùng chung

- **Chấm chỉ trang:** 5 chấm 6 px, cách nhau 8 px, ở chân màn. Chấm đang chọn màu `lamp`, còn lại `ink-2`. Khi có hàng nút ở chân (Radio, Mèo), chấm đặt ngay trên hàng nút.
- **Đầu màn:** chữ `dusk` 14 px không icon; màn Đồng hồ không có đầu.
- **Thẻ thông báo:** nền `ink-2`, viền trái 4 px theo mức ưu tiên (`lamp` bình thường, `ember` cao), không viền bao quanh.
- **Chế độ đêm:** đổi cả theme. Nền `#000`, mọi chữ/icon `night-red`, mèo vẽ bằng bảng màu đỏ đơn sắc (thêm palette `night`), ẩn thời tiết và chấm chỉ trang. Chạm vào thì sáng lên 10 giây như hiện nay.

---

## 5. Trang web

### Tổng quan: tiêu điểm là chính cái loa

```
┌─────────────────────────────────────────────┐
│  ┌───────────────┐   Mướp đang ngủ           │
│  │ ảnh màn hình  │   Lofi Girl ▸ Đang phát   │
│  │ loa trực tiếp │   ◂)) ━━━━━━━○──── 65     │
│  │ (296×240 ×1.5)│   [⏮] [ ⏸ ] [⏭]           │
│  └───────────────┘                           │
│  Pin 82%, còn khoảng 5 giờ 10 phút           │
│  Wi-Fi NhaMeo, sóng mạnh            │
└─────────────────────────────────────────────┘
```

- **Tiêu điểm là ảnh màn hình loa trực tiếp** từ `/api/screenshot`, cập nhật mỗi 5 giây, hiển thị pixel sắc nét (`image-rendering: pixelated`). Không trang cài đặt loa nào khác làm vậy, và nó cho người dùng thấy ngay loa đang hiện gì.
- Phiên bản, uptime, RAM, PSRAM chuyển sang trang Hệ thống (W2).
- Trạng thái dừng không còn là chữ to nhất (W1).

### Các trang khác

- **Icon menu:** bộ icon pixel SVG 12×12 tự vẽ cùng lưới với mèo, đổ màu bằng `currentColor` (W4).
- **Khung thẻ:** chỉ khung những nhóm có thể thao tác (form, danh sách). Thông tin chỉ để đọc thì dùng khoảng trắng và đường kẻ. Bỏ thẻ lồng thẻ (W3).
- **Thẻ Pin:** biểu đồ rộng hết thẻ, trục thời gian (giờ) và trục % (0/50/100). Lịch sử thành bảng gồm các cột *Bắt đầu, Thời gian, %, Kết thúc, Ước tính đầy→cạn* (W5).
- **Danh sách đài:** các nút sắp xếp/sửa/xoá gom vào một menu "⋯" 44 px; kéo thả để đổi thứ tự trên desktop (W6).
- **Chữ:** bỏ "→" cuối link, bỏ "·" nối thông tin (W7).
- **Chế độ sáng** dùng `paper` làm nền và `ink` làm chữ, cùng token với loa.

---

## 6. Hệ thống theme

Giao diện hỗ trợ nhiều style. Đã chốt (2026-11-22):

| # | Theme | Trạng thái |
|---|---|---|
| 1 | **Cyberpunk** | đã mockup |
| 2 | **Swiss** (Minimalism & Swiss Style) | đã mockup, **theme mặc định** (thân thiện, dễ đọc nhất) |
| 3 | **Zen** (Japanese Zen, wabi-sabi) | đã mockup |
| 4 | **Brutalism** | đã mockup |
| – | "Đèn ngủ" (token mục 3) | bản nháp; token dùng làm biến thể đêm chung nếu theme không tự định nghĩa |

Mockup cả hai theme (loa + web), mỗi theme một trang: https://claude.ai/artifact/VBCNukSw2vGKQ6ZBZdG9Zw

### Nguyên tắc

- **Một nguồn sự thật.** Mỗi theme là một file JSON. Script sinh ra struct C cho LVGL và biến CSS cho web, nên loa và web luôn khớp nhau.
- **Theme chỉ đổi cách trông, không đổi bố cục.** Vị trí, lưới, vùng an toàn và kích thước nút chạm giữ nguyên cho mọi theme. Nhờ vậy app không phải viết riêng cho từng theme, và theme nào cũng đạt yêu cầu chạm (≥ 44×40) và vùng an toàn.
- **Hiệu ứng ở mức tối thiểu:** chỉ vừa đủ để nhận ra theme, không dùng cái nào làm chậm loa (xem bảng chi phí bên dưới). Không có hiệu ứng chuyển động liên tục.
- **Mỗi theme có biến thể đêm riêng.** Đêm luôn là nền đen với một màu chữ tối, độ sáng thấp.
- **Code app không đặt màu cứng.** Mọi màu, font và kiểu viền đều lấy từ theme đang dùng (`ui_theme()`).

### Cấu trúc file

```
firmware/themes/
  swiss.json         theme mặc định
  cyberpunk.json
  zen.json
  brutal.json
firmware/tools/gen_themes.py
  → components/ui/themes_gen.c     mảng theme_t (màu, font, cờ hiệu ứng, bảng màu mèo)
  → web/src/themes.css             [data-theme="lamp"] { --ink: …; … }
```

Mỗi file theme gồm:

```json
{
  "id": "cyberpunk",
  "name": { "vi": "Cyberpunk", "en": "Cyberpunk" },
  "color": { "bg": "#05060A", "surface": "#1A1E30", "text": "#E8F6FF", "dim": "#7A86A8",
             "accent": "#00F0FF", "accent2": "#FF2BD6", "ok": "#39FF88", "warn": "#F5F032", "alert": "#FF3B4E" },
  "night": { "bg": "#000000", "text": "#B0156E" },
  "font":  { "clock": "vt323_digits_64", "ui": "chakra_petch", "data": "jetbrains_mono" },
  "shape": { "radius": 0, "button": "cut" },
  "fx":    { "aberration": "clock", "scanline": true, "hud_corners": true, "segmented_bars": true },
  "cat":   { "palette": "neon" },
  "web":   { "fonts": ["chakra-petch-vi.woff2", "jetbrains-mono-vi.woff2", "vt323-digits.woff2"] }
}
```

### Hiệu ứng: được dùng gì và tốn gì

Trên loa có hai chỗ nghẽn: SPI tới màn (vẽ cả màn ~28 ms) và RAM nội (~35 KB trống). Radio giải mã chạy trên cùng chip.

| Hiệu ứng | Cách làm trên loa | Chi phí | Dùng |
|---|---|---|---|
| Màu, bo góc, viền, khoảng cách | style LVGL dùng chung | gần 0 | ✅ |
| Font riêng theo theme | font sinh bằng `lv_font_conv`, nằm trong flash | flash 70–120 KB/bộ, không tốn RAM | ✅ |
| Bảng màu mèo theo theme | bảng 16 màu, sprite giữ nguyên | gần 0 | ✅ |
| Nút cắt góc, khung HUD góc | ảnh 9-slice nhỏ hoặc vài đường kẻ | rẻ | ✅ |
| Thanh chỉ số chia khúc | vẽ thanh có khe (pattern) | rẻ | ✅ |
| Chữ lệch màu RGB | 3 lớp label lệch 1 px, **chỉ cho giờ** (vẽ lại 1 lần/phút) | rẻ ở đúng chỗ này | ✅ chỉ ở mặt đồng hồ |
| Scanline | vẽ **vào nền** từng màn, nằm dưới nội dung | gần 0 | ✅ |
| Scanline phủ lên trên toàn màn | lớp trong suốt trên cùng | chậm hơn 20–40% mỗi lần vẽ | ❌ |
| Phát sáng / bóng mờ (blur) | shadow LVGL | nặng | ❌ thay bằng viền 1 px màu sáng |
| Glitch chuyển động, nhiễu hạt | vẽ lại cả màn liên tục | chiếm SPI và CPU, radio dễ giật | ❌ |
| Ảnh nền full màn | 142 KB trong PSRAM | đọc chậm hơn | ❌ nền vẽ bằng code |

Trên web, theme chỉ là biến CSS nên không tốn gì. Web vẫn giữ mức hiệu ứng tối thiểu như trên loa để hai bên cùng một cảm giác. Font phải **tự host** (tập con tiếng Việt, woff2) trong phân vùng `storage`, vì lúc cài Wi-Fi trang web không có internet.

### Cyberpunk ở mức tối thiểu

Chỉ giữ những thứ làm nên cảm giác cyberpunk:
- Nền đen sâu, màu nhấn cyan + magenta, vàng cho cảnh báo nhẹ, đỏ cho báo thức.
- Chữ số VT323 với lệch màu RGB ở mặt đồng hồ.
- Nút cắt góc, khung HUD ở góc, thanh chỉ số chia khúc.
- Scanline vẽ vào nền.
- Mèo bảng màu neon.

Bỏ khỏi mockup: lát cắt glitch trên chữ giờ, cột dữ liệu chạy dọc, sọc cảnh báo chéo ở màn báo thức (thay bằng viền đỏ), con trỏ nhấp nháy trên web.

### Swiss ở mức tối thiểu

- Nền giấy `#F2F0EA`, chữ đen `#000000`, chữ phụ xám `#5E5E5E` (5,7:1). Đường kẻ trang trí `#C9C5BB`.
- **Đỏ Thuỵ Sĩ `#D7141A`** là màu nhấn duy nhất (4,6:1 trên nền giấy, chữ trắng trên đỏ 5,2:1): báo thức, mục đang chọn, chỉ số thấp. Xanh `#0047BB` chỉ cho viền focus trên web.
- **Một họ font: Archivo** (grotesque, có trục độ rộng, có tiếng Việt). Giờ và số lớn dùng bản hẹp (~70%), đậm 800, căn trái, khoảng cách chữ âm.
- Lưới bất đối xứng, mọi thứ căn trái. Đường kẻ 1 px đen thay cho khung/thẻ. Không bo góc, không bóng trên loa.
- Mèo bảng màu đơn sắc: thân đen, mắt trắng ngà, chỉ mũi và tai trong màu đỏ.
- Màn báo thức: nền đỏ trọn màn, giờ trắng cỡ rất lớn, kiểu poster.
- Hiệu ứng duy nhất (chỉ web): hover đổi màu/gạch chân 150 ms, nút có bóng cứng `4px 4px 0 #000` khi hover. Tắt khi `prefers-reduced-motion`.
- Đêm: nền đen, chữ `#8A8A8A` (6,1:1), giờ vẫn căn trái cỡ lớn; đỏ chuyển thành `#E0383C` (4,8:1), và vẫn dịu nhờ độ sáng đèn nền thấp.
- Chi phí trên loa: chỉ màu, font, đường kẻ, nên thuộc nhóm rẻ nhất trong bảng hiệu ứng.

### Zen ở mức tối thiểu

- Nền giấy dó `#F3EDE1`, chữ mực sumi `#24211D` (13,8:1), chữ phụ nâu đất `#74664F` (4,8:1), lịch âm chàm `#3E4A6B` (7,5:1).
- **Đỏ con dấu hanko `#A8322A`** (5,7:1) là màu nhấn duy nhất: báo thức, nhiệt độ cao, chỉ số thấp, con dấu 猫.
- Xanh tre `#5B7A4A` (4,2:1) chỉ cho nét vẽ và số lớn. Hồng sakura `#E8B7BE` chỉ làm mảng tô mờ (vòng nền sau con mèo).
- Chữ: **Cormorant Garamond** (serif thanh, có tiếng Việt) cho giờ, tiêu đề, tên đài. Trên loa chỉ dùng từ 18 px trở lên, vì nét quá mảnh ở cỡ nhỏ. **Be Vietnam Pro Light** cho chữ nhỏ. Thêm một glyph **猫** (Noto Serif JP) cho con dấu.
- **Điểm nhấn: vòng ensō** vẽ bằng nét cọ quanh giờ, bản đỏ quanh báo thức. Trên loa là một ảnh tĩnh (RGB565A8 ~150×150, khoảng 67 KB trong flash), chỉ vẽ lại vùng chữ số khi đổi phút.
- Bố cục: chừa nhiều khoảng trống (ma), bất đối xứng. Nút là chữ gạch chân mảnh, nút chính là viên bo tròn viền mực. Đường phân cách là nét cọ thay cho kẻ thẳng.
- Mèo bảng màu thuỷ mặc: lông xám, mũi và tai hồng, mắt chàm.
- Vân giấy: **chỉ trên web** (ảnh nền lặp). Trên loa dùng màu giấy phẳng, vì ảnh nền lặp phải vẽ lại mỗi vùng thay đổi.
- Hiệu ứng web: chuyển màu 300 ms khi hover, nút chuyển sang nền mực. Tắt khi `prefers-reduced-motion`.
- Đêm: nền sumi `#24211D`, chữ giấy dó mờ `#A89878` (5,7:1), ensō chuyển sang nét giấy dó.
- Chi phí trên loa: nhóm rẻ; ensō là ảnh tĩnh duy nhất.

### Brutalism ở mức tối thiểu

- Nền trắng với **lưới kẻ hiện rõ** (xám `#D0D0D0`, ô 16 px trên loa), chữ đen. Màu cơ bản thuần làm **mảng khối**: vàng `#FFFF00`, đỏ `#FF0000`, xanh `#0000FF`; một màu neon `#00FF00` cho trạng thái OK.
- Quy tắc tương phản: đỏ thuần trên trắng chỉ 4:1, nên **đỏ chỉ làm nền với chữ đen** (5,3:1) hoặc chữ ≥ 24 px. Chữ đen trên vàng 19,6:1, chữ trắng trên xanh 8,6:1. Link web giữ màu mặc định `#0000EE` (9,4:1) / đã xem `#551A8B`, gạch chân.
- **Font mặc định**: web dùng font hệ thống (Times New Roman cho chữ, Courier New cho dữ liệu), không tải font nào nên nhẹ nhất trong 4 theme. Loa không có "font mặc định" nên dùng **IBM Plex Mono** (tập con tiếng Việt) cho chữ và một bản serif đậm cho số giờ.
- Viền đen 3 px, **bóng cứng** không mờ (`5px 5px 0 #000`), góc vuông. Vài khối nghiêng 1–2° hoặc tràn ra mép cho cảm giác "vỡ" có chủ đích; nội dung chữ thì luôn đọc được.
- Thanh trên cùng màu đen kiểu đường dẫn (`meowbox / radio`); số trang dạng `[1] 2 3 4 5`. Chỉ số mèo là thanh ASCII `[#####·····]`.
- Mèo dùng **bảng màu gốc** (mướp), không chỉnh, đặt trong khung vàng viền đen.
- Không có transition hay animation. Bấm nút thì khối lệch xuống 3 px (mất bóng) ngay lập tức. Focus bàn phím là viền đỏ 4 px.
- Đêm: nền đen, lưới xám tối `#222`, chữ trắng `#BFBFBF`, khối màu giảm còn viền màu (không tô nền) để đỡ chói.
- Chi phí trên loa: rẻ. Lưới vẽ vào nền, bóng cứng chỉ là hình chữ nhật lệch, khối nghiêng thì vẽ sẵn thành ảnh tĩnh (LVGL xoay ảnh lúc chạy sẽ tốn).

### Chọn theme trên web quản lý

Theme chỉ đổi từ **trang web quản lý**, mục **Cài đặt → Giao diện**. Trên loa không có menu chọn theme để giữ màn hình gọn.

```
Cài đặt → Giao diện
┌──────────────────────────────────────────────────────────┐
│ Giao diện                                                 │
│ ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌──────────┐ │
│ │ [ảnh màn   │ │ [ảnh màn   │ │ [ảnh màn   │ │ [ảnh màn │ │
│ │  Swiss]    │ │ Cyberpunk] │ │   Zen]     │ │ Brutal]  │ │
│ │ ● Swiss    │ │ ○ Cyberpunk│ │ ○ Zen      │ │ ○ Brutal │ │
│ │ mặc định   │ │            │ │            │ │          │ │
│ └────────────┘ └────────────┘ └────────────┘ └──────────┘ │
│ Chế độ đêm   [tự động theo giờ ▾]  22:00 → 07:00           │
│ Hiệu ứng     [đầy đủ ▾]  (tiết kiệm: tắt khi chạy pin)     │
│ Trang web    [theo theme của loa ▾]                       │
└──────────────────────────────────────────────────────────┘
```

- Mỗi theme là một thẻ chọn (radio) có **ảnh xem trước** của màn Đồng hồ. Ảnh tĩnh sinh sẵn lúc build, lưu trong phân vùng `storage` (`/web/themes/<id>.png`, mỗi ảnh khoảng 10–20 KB), nên xem được cả khi loa đang dùng theme khác.
- Bấm chọn là **áp dụng ngay**, không cần nút Lưu: web gửi `PUT /api/config/display {"theme":"zen"}`, loa dựng lại giao diện trong 0,1–0,3 s, web chuyển sang theme mới cùng lúc. Có thông báo nhỏ "Đã đổi sang Zen" kèm nút "Hoàn tác" trong 10 giây.
- Bên dưới có thêm nút **"Chụp màn hình loa"** (`/api/screenshot`) để xem theme thật trên loa.
- Lưu ở `settings` nhóm `display`, khoá `theme` (mặc định **`"swiss"`**). Factory reset đưa về Swiss.
- **Web đổi theo theme của loa** (`data-theme` trên `<html>`, đọc từ `/api/config/display` lúc tải trang). Tuỳ chọn "Trang web" cho phép giữ web ở Swiss dù loa dùng theme khác (lưu trong trình duyệt, chỉ ảnh hưởng máy đang xem).
- **Chế độ đêm**: tự động theo giờ (mặc định), luôn bật, hoặc tắt. Mỗi theme dùng biến thể đêm của chính nó.
- **Hiệu ứng**: đầy đủ (mức tối thiểu đã chốt của theme) hoặc tiết kiệm (tắt lệch màu, scanline, khối nghiêng; tự bật khi chạy pin nếu chọn).
- API: thêm `theme`, `night_mode` (`auto`|`on`|`off`), `fx` (`full`|`eco`) vào `GET/PUT /api/config/display`, và `GET /api/themes` trả danh sách `[{id, name, preview}]` để web tự dựng thẻ chọn (thêm theme mới không phải sửa web).

### Đo hiệu năng

- Bật `LV_USE_SYSMON` trong bản build debug để xem FPS và % CPU của LVGL.
- Mỗi theme phải đạt: vuốt chuyển màn ≥ 25 FPS **khi đang phát radio**, không có underrun trong log `player` sau 10 phút, RAM nội tối thiểu không thấp hơn trước khi thêm theme.

---

## 7. Kế hoạch thực hiện

### R1: sửa lỗi (nhỏ, làm ngay)
- [x] D1 nút Mèo một dòng chữ, đổi nhãn "Tắt đèn"/"Bật đèn"
- [x] D2 tắt đèn bằng đổi màu nền vùng chơi + sao, bỏ khối phủ
- [x] D3 lời nhắn màn PC chưa kết nối
- [x] D4 thông tin màn Hệ thống mỗi mục một dòng
- [x] D5 chấm chỉ trang (component dùng chung trong `ui.c`)

**Xong khi:** chụp lại 5 màn bằng `/api/screenshot`, không còn chữ bị cắt hoặc ngắt dòng sai.

### R2: token và bố cục loa
- [x] `theme_t` + `ui_theme()` trong `components/ui`; mọi app lấy màu/font/style từ theme, bỏ màu đặt cứng (`UI_COL_*`, hex rải trong app)
- [x] Style LVGL dùng chung theo vai trò (nền, chữ chính, chữ phụ, nút, nút chính, thanh chỉ số, đường kẻ), tạo từ theme đang dùng
- [x] Lưới đầu/thân/chân cho cả 5 màn
- [x] Safe area from real measurements (2026-11-25: corner radius ~60 px, left 3 px hidden)
- [x] D6 Đồng hồ: ẩn dòng thời tiết khi chưa có dữ liệu, bỏ số giây
- [x] D7 Radio: tên đài dạng nút ▾, giữ chỗ dòng tên bài
- [x] D8 Mèo: bố cục mới, kích thước sprite theo tuổi
- [x] D9 đầu màn thống nhất, bỏ dấu "·"

### R3: hệ thống theme
- [x] `firmware/themes/cyberpunk.json`, `swiss.json`, `zen.json`, `brutal.json` (mục 6, mức tối thiểu); font Archivo (Swiss), Cormorant Garamond + glyph 猫 (Zen), IBM Plex Mono (Brutal), tập con tiếng Việt
- [x] Ảnh ensō sinh bằng script, lưu **A8** (chỉ alpha, 22 KB) và tô màu lúc chạy bằng `image_recolor`, nên một ảnh dùng cho mực, đỏ và đêm
- [x] `tools/gen_themes.py` sinh `themes_gen.c` + `web/src/themes.css`; font theo theme sinh bằng `lv_font_conv` (VT323 chỉ chữ số, Chakra Petch và JetBrains Mono tập con tiếng Việt)
- [x] Hiệu ứng theo cờ: nút cắt góc, khung HUD, thanh chia khúc, scanline vẽ vào nền, lệch màu RGB cho giờ
- [x] Đổi theme lúc chạy (dựng lại các tile), lưu `display.theme` (mặc định `swiss`), `night_mode`, `fx`; `GET /api/themes`; cập nhật `web/API.md`
- [x] Ảnh xem trước theme sinh lúc build vào `storage/web/themes/`
- [x] Bảng màu mèo theo theme (`neon` cho Cyberpunk, `mono` cho Swiss, `sumi` cho Zen, bảng gốc cho Brutal) trong `tools/cat_sprites`
- [ ] Đo bằng `LV_USE_SYSMON`: ≥ 25 FPS khi vuốt lúc đang phát radio. Đã đo được: đổi theme 150–180 ms, đổi qua lại 4 theme lúc đang phát radio không underrun. Chưa đo FPS khi vuốt

**Xong khi:** đổi qua lại hai theme trên web, loa đổi theo trong chưa tới 0,5 s, ảnh chụp 5 màn của mỗi theme khớp mockup ở mức tối thiểu.

### R4: điểm nhấn
- [x] ~~Font chữ số pixel~~ thay bằng font giờ riêng cho từng theme (`mb_*_clock`)
- [x] Mèo nhỏ ở góc màn Đồng hồ, chạm để mở màn Mèo
- [x] D10 chế độ đêm dùng biến thể `night` của theme (đã kiểm tra cả 4 theme)
- [x] Night palette for the cat (0.2.3): a darker, warmer copy of the day palette, computed at runtime

### R5: web
- [x] Dùng `themes.css` sinh từ theme, `data-theme` theo loa; font tự host
- [x] Trang **Cài đặt → Giao diện**: thẻ chọn theme có ảnh xem trước, áp dụng ngay + hoàn tác, chế độ đêm, mức hiệu ứng, tuỳ chọn web theo loa
- [x] Tổng quan: ảnh màn hình loa trực tiếp làm tiêu điểm; thông số kỹ thuật sang trang Hệ thống
- [x] Icon pixel SVG thay emoji
- [x] Thẻ Pin: biểu đồ có trục, lịch sử dạng bảng
- [x] Danh sách đài: menu "⋯", nút ≥ 44 px
- [x] Kiểm tra lại ở 390 px và 1280 px, đủ hai chế độ sáng/tối, có focus khi dùng bàn phím, tôn trọng `prefers-reduced-motion`

**Ràng buộc:** gói web gzip vẫn ≤ 25 KB, không dùng CDN (trang chạy cả khi loa ở chế độ phát Wi-Fi riêng
không có internet). Font tiếng Việt tự host thì chỉ lấy tập con và chỉ dùng cho tiêu đề nếu quá nặng.

### Cách kiểm tra mỗi vòng
- Loa: `curl -b cookie "http://meowbox.local/api/screenshot?tile=N" -o tileN.bmp` cho N = 0…4, so sánh trước/sau.
- Web: `python web/mock_server.py --no-auth`, chụp ở 390 px và 1280 px, sáng và tối.
