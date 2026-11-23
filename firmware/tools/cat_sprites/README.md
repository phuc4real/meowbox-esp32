# cat_sprites: sprite mèo cho MeowBox

`sprites.py` là **nguồn gốc duy nhất**. Mọi sprite đều vẽ bằng chữ ngay trong file: mỗi ký tự là 1 pixel.
Code C trong `firmware/components/cat_sprites/` được **sinh tự động**, đừng sửa tay mấy file đó.

## Sửa sprite

1. Mở `sprites.py`, tìm `SPRITES["ADULT"]["SIT"]` (hoặc `PROPS["HEART"]`...).
2. Sửa từng ký tự. Sprite mèo là 32x32, prop là 16x16 (script kiểm tra luôn kích thước).
3. Chạy:

```
python sprites.py            # kiểm tra + sinh C + render preview
python sprites.py gen        # chỉ sinh C
python sprites.py preview    # chỉ render preview (cần: pip install pillow --user)
python sprites.py dump adult sit   # in sprite ra để copy/so sánh
```

4. Mở `preview/cat_<lông>.png` xem lại (15 pose × 4 tuổi), `preview/all.png` (đủ 6 bộ lông),
   `preview/props.png`, `preview/screen_mock.png` (giả lập màn 296x240, mèo phóng ×3).

## Bảng ký tự

| ký tự | slot | ý nghĩa | đổi theo lông? |
|---|---|---|---|
| `.` | 0 | trong suốt | |
| `O` | 1 | viền (mèo đen dùng viền sáng) | có |
| `F` `S` | 2, 3 | lông chính / lông tối (bóng) | có |
| `T` | 4 | sọc mướp (lông trắng/tam thể thì = màu lông, nên sọc "biến mất") | có |
| `P` `Q` | 5, 6 | mảng cam / mảng đen của mèo tam thể (lông khác = màu lông) | có |
| `L` | 7 | bụng, mõm, chân | có |
| `E` | 8 | mắt (đen thì vàng hổ phách, trắng thì xanh dương, xám thì xanh lá) | có |
| `W` `K` `D` | 9–11 | trắng, hồng (tai/mũi/má/lưỡi), tối (con ngươi, viền prop) | **cố định** |
| `B` `R` `Y` `N` | 12–15 | xanh, đỏ, vàng, nâu (đồ vật) | **cố định** |

Prop chỉ được dùng các ký tự cố định (`W K D B R Y N`), script sẽ báo lỗi nếu lỡ dùng màu lông,
vì prop vẽ bằng palette của con mèo đang hiển thị.

## Mẹo

- Đặt `T` cho sọc mướp và `P`/`Q` cho mảng tam thể ở **chỗ khác nhau**, để lông nào nhìn cũng ổn.
- Mặt dùng `O` cho miệng và mắt nhắm (mèo đen có viền sáng nên vẫn thấy). Con ngươi dùng `D`.
- Tuổi SENIOR cũng là chữ thường, sửa thoải mái: mí mắt trễ (`O` đè hàng trên của mắt), mõm `L` rộng hơn, lông mày `W`.
- Màu nằm ở `PALETTES` (hex), lúc sinh C sẽ tự đổi sang RGB565.
- Bộ sprite ban đầu được ghép từ các phần (đầu, tai, thân, đuôi) rồi tự thêm viền, sau đó lưu thành chữ.
  Giờ cứ sửa chữ trực tiếp là được.
