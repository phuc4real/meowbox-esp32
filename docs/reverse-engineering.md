# How the pin map was extracted from the vendor firmware

There was no schematic, and the online wiring diagrams were for different hardware. Everything came
from the device itself. You can repeat the method on any ESP32-S3 xiaozhi-style gadget.

## 1. Inspect the chip (non-destructive)

```powershell
pip install esptool
python -m esptool --port COM10 flash-id            # chip, PSRAM, flash size
python -m esptool --port COM10 get-security-info   # secure boot / flash encryption?
python -m esptool --port COM10 -b 921600 read-flash 0 ALL backup\speaker_full_16MB.bin
python -m esptool --port COM10 -b 921600 verify-flash 0 backup\speaker_full_16MB.bin
```

If flash encryption is on, stop here: the dump is ciphertext.

## 2. Partition table, app descriptor, NVS

```powershell
python tools\parse_partitions.py backup\speaker_full_16MB.bin
python tools\dump_nvs.py backup\speaker_full_16MB.bin
```

This shows the project name, version, IDF version, build date, and saved settings.
Extract the app partition for the next steps (offset/size from the table):

```bash
dd if=backup/speaker_full_16MB.bin of=backup/ota0_app.bin bs=4096 skip=32 count=1344   # 0x20000, 0x540000
python -m esptool --chip esp32s3 image-info backup/ota0_app.bin                       # segment map
```

## 3. Boot log

Open the USB COM port, pulse RTS to reset, and keep reading. Re-open the port if it drops.
`logs/vendor_boot.txt` shows the codec chips, LCD driver, touch IC id, board SKU and the
`OSTB_XIAOZHI_3ST:` log tag. That tells you *what* parts are on the board, but not the pins.

## 4. Pins from the code

ESP-IDF `ESP_ERROR_CHECK()` embeds `__PRETTY_FUNCTION__`. So strings like
`void OSTB_XIAOZHI_3ST::InitializeI2c()` stay in the binary and mark the board's init functions.

1. **Find strings and the literal-pool slots that hold their address**

   ```powershell
   python tools\find_string_refs.py OSTB_XIAOZHI_3ST Initialize
   ```

   On ESP32-S3, IROM code loads 32-bit constants with `l32r` from a shared `.literal` area at the
   start of IROM (`0x42000020…`). The output lists those slot addresses.

2. **Find the code that loads those slots**

   ```powershell
   python tools\find_l32r.py 42003ef4 42003f58 42003fec 42003ffc
   ```

   This brute-forces every 3-byte `l32r` encoding in IROM and gives the instruction addresses.
   They cluster around the board functions (here `0x420409c8`–`0x42041800`).

3. **Disassemble the area**

   ```powershell
   pip install --pre capstone==6.0.0a11      # first capstone with Xtensa / ESP32-S3
   python tools\disasm.py 42040900 42041800 > board.txt
   ```

   The capstone alpha cannot decode a few narrow branches (`bnez.n`/`beqz.n`). `disasm.py` steps
   over unknown opcodes by length so it stays in sync. `l32r` lines are annotated with the loaded
   value and string.

4. **Read the config structs.** Pins are small immediates (`movi.n aX, N`) stored into a stack
   struct (`s32i.n aX, a1, off`) just before the IDF call named in the nearby error string.
   Match the offsets against the IDF struct layout:

   | Struct (IDF 5.5) | Offsets that mattered |
   |---|---|
   | `i2c_master_bus_config_t` | +0 port, +4 SDA, +8 SCL, +0x10 glitch (u8), +0x1c flags |
   | `spi_bus_config_t` | +0 MOSI, +4 MISO, +8 SCLK, +0xc WP, +0x10 HD, +0x28 max_transfer_sz |
   | `esp_lcd_panel_io_spi_config_t` | +0 CS, +4 DC, +8 spi_mode, +0xc pclk_hz, +0x10 queue, +0x1c/+0x20 cmd/param bits |
   | `esp_lcd_panel_dev_config_t` | +0 reset_gpio, +4 rgb order, +0xc bits_per_pixel, +0x14 vendor_config |

5. **Constructor arguments.** For C++ objects (`BoxAudioCodec`, `PwmBacklight`, `Button`,
   `PowerManager`, `SpiLcdDisplay`), Xtensa windowed `call8` passes args in `a10…a15`, and the
   rest go on the stack at `a1+0, +4, …`. Example, `GetAudioCodec` @ `0x420406b0`:

   ```
   a11=i2c_bus  a12=a13=24000 (movi 0xc0 + addmi 0x5d00)  a14=5 (MCLK)  a15=15 (BCLK)
   [a1+0]=16 WS  [+4]=6 DOUT  [+8]=7 DIN  [+0xc]=4 PA  [+0x10]=0x30 ES8311  [+0x14]=0x80 ES7210  [+0x18]=1 ref
   ```

   Large constants are built as `movi` + `addmi` (e.g. 25000 = `0xa8 + 0x6100`).

6. **Virtual methods.** Methods like `GetBacklight` and `GetAudioCodec` are reached through the
   class vtable. The constructor stores its address (`s32i.n a8, a2, 0` after `l32r a8, <vtable>`).
   Dump the vtable words and disassemble the entries that fall inside the board's code range.

7. **Data tables.** The NV3023 init table is an array of `{u32 cmd, u32 data_ptr, u32 len, u32 delay}`
   pointed to by the `vendor_config` the display function builds. `rd()` in `tools/fwimage.py`
   follows the pointers.

## 5. Cross-check against upstream

The vendor code was obviously derived from upstream boards: the same LCD pins as
`nologo/xingzhi-cube-0.85tft-wifi`, and the same `power_manager.h` as `xingzhi-cube-1.54tft-wifi`.
Diffing behaviour against upstream source fills the gaps, such as button lambdas and the ADC channel.

## Tools

All in `tools/`. Python 3, `capstone>=6` for disassembly. Set `FW_APP` to analyse another app image.

| Script | Does |
|---|---|
| `parse_partitions.py <dump>` | partition table + app descriptors |
| `dump_nvs.py <dump>` | NVS key/values (passwords masked) |
| `fwimage.py` | library: image → memory map, `rd/u32/cstr`, `find_str`, `find_word`, capstone handle |
| `find_string_refs.py <text>…` | strings and the literal slots pointing at them |
| `find_l32r.py <lit_hex>…` | `l32r` instructions loading those slots |
| `disasm.py <start> <end>` | annotated Xtensa disassembly |
