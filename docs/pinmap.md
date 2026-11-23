# Pin map: OSTB xiaozhi-3st

Machine-readable version: [`hw/ostb_xiaozhi_3st.h`](../hw/ostb_xiaozhi_3st.h).

**Verification levels**
- **FW**: read from the vendor firmware's own init code (disassembly, see [reverse-engineering.md](reverse-engineering.md))
- **BOOT**: a firmware built with this value boots and the chip answers on its bus
- **HW ✔**: checked by hand on the real device (sound, picture, button, measured value)

## By function

| Function | GPIO / value | Detail | Status |
|---|---|---|---|
| I2C0 SDA | **12** | internal pull-up on, glitch filter 7 | FW, BOOT |
| I2C0 SCL | **11** | 400 kHz | FW, BOOT |
| ES8311 (DAC, speaker) | I2C `0x18` | drivers that take the 8-bit form use `0x30` | FW, **HW ✔** |
| ES7210 (4-ch mic ADC) | I2C `0x40` | 8-bit `0x80`, MIC1–4 on, TDM | FW, **HW ✔** |
| CST816S touch | I2C `0x15` | chip id 184 (0xB8); INT/RST not wired, poll it; coordinates are screen space (x 0–295, y 0–239), no swap/mirror | FW, **HW ✔** |
| I2S MCLK | **5** | 256 × fs | FW, **HW ✔** |
| I2S BCLK | **15** | | FW, **HW ✔** |
| I2S WS / LRCK | **16** | | FW, **HW ✔** |
| I2S DOUT → ES8311 | **6** | | FW, **HW ✔** |
| I2S DIN ← ES7210 | **7** | mics on TDM slots 0 and 2 | FW, **HW ✔** |
| Speaker amp enable | **4** | active high | FW, **HW ✔** |
| LCD SPI host | SPI3 | DMA auto | FW, **HW ✔** |
| LCD MOSI | **10** | | FW, **HW ✔** |
| LCD SCLK | **9** | MISO / WP / HD not used | FW, **HW ✔** |
| LCD CS | **14** | SPI mode 0, 40 MHz, 8-bit cmd / param | FW, **HW ✔** |
| LCD DC | **8** | | FW, **HW ✔** |
| LCD RST | **18** | | FW, **HW ✔** |
| LCD backlight | **13** | LEDC PWM 25 kHz, active high | FW, **HW ✔** |
| Volume up | **40** | active low | FW, **HW ✔** |
| Volume down | **39** | active low | FW, **HW ✔** |
| Middle (power) button | **1** | **active high**; idle 0, ~100–150 ms pulse on a short press; a long press powers off in hardware | **HW ✔** (found by watching all free GPIOs) |
| BOOT | **0** | strapping pin; no button reaches it from outside the case | chip |
| Charge detect | **47** | input, high = charging | FW, **HW ✔** |
| Battery voltage | **17** (ADC2 CH6) | 12 dB attenuation; raw 2461 on USB, 2430 on battery when full | **HW ✔** (divider ratio assumed 1:2) |
| USB D− / D+ | 19 / 20 | USB-Serial/JTAG: console, flashing, JTAG | chip |
| Flash + octal PSRAM | 26–37 | **do not use** | chip |

## By GPIO

| GPIO | Use | | GPIO | Use |
|---|---|---|---|---|
| 0 | BOOT (strapping, no button outside) | | 21 | free* |
| 1 | middle button (active high) | | 38 | free* |
| 2 | free* (reads 4095 on the ADC: pulled high?) | | 39 | volume down |
| 3 | free* (strapping, JTAG select) | | 40 | volume up |
| 4 | speaker amp enable | | 41 | free* |
| 5 | I2S MCLK | | 42 | free* |
| 6 | I2S DOUT | | 43 | UART0 TX (unused, console is USB) |
| 7 | I2S DIN | | 44 | UART0 RX (unused) |
| 8 | LCD DC | | 45 | free* (strapping, VDD_SPI) |
| 9 | LCD SCLK | | 46 | free* (strapping) |
| 10 | LCD MOSI | | 47 | charge detect |
| 11 | I2C SCL | | 48 | free* |
| 12 | I2C SDA | | | |
| 13 | LCD backlight | | | |
| 14 | LCD CS | | | |
| 15 | I2S BCLK | | | |
| 16 | I2S WS | | | |
| 17 | battery voltage (ADC2 CH6) | | | |
| 18 | LCD RST | | | |
| 19/20 | USB | | | |

\* "free" means only that **no firmware uses it**. Whether the pin reaches a pad on this PCB is
unknown; check with a multimeter before relying on it. Avoid the strapping pins 0/3/45/46 for anything
that pulls them at reset.

## Display

| Param | Value |
|---|---|
| Controller | NV3023, RGB565, natively 240 × 296 portrait |
| Logical size | **296 × 240** landscape (swap_xy = true, mirror_x = false, mirror_y = true) |
| Offset X / Y | 24 / 0 |
| Invert colour / RGB order | false / RGB |
| Init table | [`hw/nv3023_init_cmds.h`](../hw/nv3023_init_cmds.h) (25 commands from the vendor firmware) |
| Visible area | the case bezel and the rounded glass hide the outer edge. MeowBox keeps UI inside 14 px left/right and 10 px top/bottom for now. The exact inset is still to be measured: on the System tile, long-press the title to show 8 nested frames (0–28 px from the edge) |
