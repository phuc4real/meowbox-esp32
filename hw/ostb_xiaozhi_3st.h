/*
 * OSTB xiaozhi-3st speaker board — pin & bus definitions
 *
 * Framework-agnostic: plain integers, usable from ESP-IDF (cast to gpio_num_t),
 * Arduino-ESP32, ESPHome (copy the numbers) or MicroPython.
 * Source: reverse-engineered from vendor firmware v2.4.0, then checked on the real board with the
 * MeowBox firmware (sound, picture, touch, buttons, battery)
 * (2026-11-22). See docs/pinmap.md for evidence and verification status.
 *
 * Items marked [UNVERIFIED] were inferred and not yet tested on hardware.
 */
#pragma once

/* ---------- MCU ---------- */
/* ESP32-S3 (QFN56) rev v0.2, 16 MB quad flash (Boya), 8 MB octal PSRAM (AP, 3V3)
 * GPIO 26-37 are taken by flash/PSRAM. GPIO 19/20 = native USB (console + JTAG). */

/* ---------- I2C bus 0 (shared: codec control + touch) ---------- */
#define OSTB_I2C_PORT            0
#define OSTB_I2C_SDA             12
#define OSTB_I2C_SCL             11
#define OSTB_I2C_INTERNAL_PULLUP 1       /* vendor firmware enables internal pull-ups */
#define OSTB_I2C_FREQ_HZ         400000

#define OSTB_I2C_ADDR_ES8311     0x18    /* DAC / speaker codec   (8-bit: 0x30) */
#define OSTB_I2C_ADDR_ES7210     0x40    /* 4-ch mic ADC          (8-bit: 0x80) */
#define OSTB_I2C_ADDR_CST816S    0x15    /* capacitive touch, chip id 0xB8 (184) */

/* ---------- I2S 0 (full duplex, ES8311 out / ES7210 in, TDM on input) ---------- */
#define OSTB_I2S_MCLK            5
#define OSTB_I2S_BCLK            15
#define OSTB_I2S_WS              16      /* LRCK */
#define OSTB_I2S_DOUT            6       /* ESP32 -> ES8311 DSDIN */
#define OSTB_I2S_DIN             7       /* ES7210 SDOUT -> ESP32 */
#define OSTB_AUDIO_SAMPLE_RATE   24000   /* what the vendor/xiaozhi firmware used; MeowBox runs 44100, both work */
#define OSTB_AUDIO_MCLK_MULTIPLE 256
#define OSTB_AUDIO_INPUT_REFERENCE 1     /* one ES7210 channel = speaker loopback for AEC */

#define OSTB_PA_ENABLE           4       /* speaker amplifier enable, active high */

/* ---------- Display: NV3023 TFT on SPI3 (write-only) ---------- */
#define OSTB_LCD_SPI_HOST        3       /* SPI3_HOST (enum value 2 in ESP-IDF) */
#define OSTB_LCD_MOSI            10
#define OSTB_LCD_SCLK            9
#define OSTB_LCD_MISO            (-1)
#define OSTB_LCD_CS              14
#define OSTB_LCD_DC              8
#define OSTB_LCD_RST             18
#define OSTB_LCD_PCLK_HZ         40000000
#define OSTB_LCD_SPI_MODE        0
#define OSTB_LCD_BACKLIGHT       13      /* PWM, active high, 25 kHz */
#define OSTB_LCD_BACKLIGHT_INVERT 0

/* Panel geometry as used by the vendor firmware (landscape) */
#define OSTB_LCD_WIDTH           296
#define OSTB_LCD_HEIGHT          240
#define OSTB_LCD_OFFSET_X        24
#define OSTB_LCD_OFFSET_Y        0
#define OSTB_LCD_SWAP_XY         1
#define OSTB_LCD_MIRROR_X        0
#define OSTB_LCD_MIRROR_Y        1
#define OSTB_LCD_INVERT_COLOR    0
#define OSTB_LCD_BITS_PER_PIXEL  16      /* RGB565 */
/* Init command table: hw/nv3023_init_cmds.h */

/* ---------- Touch ---------- */
#define OSTB_TOUCH_INT           (-1)    /* not wired / not used by vendor firmware -> poll */
#define OSTB_TOUCH_RST           (-1)
/* Verified 2026-11-22: polled reads give x 0..295, y 0..239 directly in display coords (no swap/mirror). */

/* ---------- Buttons (active low) ---------- */
#define OSTB_BTN_BOOT            0       /* strapping pin; no button reaches it from outside the case */
/* The middle button is a hardware power switch (long press = off) and does NOT reach any ESP32 GPIO: re-checked
 * 2026-11-25 by logging every readable GPIO while pressing it (the 2026-11-22 "GPIO1 pulse" was wrong). */
#define OSTB_BTN_VOL_UP          40      /* verified 2026-11-22 */
#define OSTB_BTN_VOL_DOWN        39

/* ---------- Power ---------- */
#define OSTB_CHARGE_DETECT       47      /* high = charging (vendor PowerManager) */
#define OSTB_BATTERY_ADC_GPIO    17      /* ADC2 CH6, 12 dB. Verified: 2461 on USB, 2430 on battery (full) */
#define OSTB_BATTERY_ADC_UNIT    2
#define OSTB_BATTERY_ADC_CHANNEL 6
#define OSTB_BATTERY_DIVIDER     2       /* [UNVERIFIED] assumed 1:2 resistor divider (raw 2430 ~ full 4.1-4.2 V) */
/* ADC raw -> % table used by the firmware: 1970=0% 2062=20% 2154=40% 2246=60% 2338=80% 2430=100% */

/* ---------- Verified on hardware 2026-11-22 (MeowBox P0 test) ----------
 * - Display orientation/colours correct with the values above (RGB order, no inversion).
 * - The case bezel and rounded glass hide the outer edge of the 296x240 area: keep UI inside a
 *   safe margin (see docs/pinmap.md "Visible area"); corners are rounded.
 * - Mics: TDM slots 0 and 2 carry the microphones (noise floor ~35-40 dB on our scale),
 *   slots 1 and 3 are near-silent (slot 1 = playback reference used for AEC).
 * - ADC probe on free pins: GPIO1 ~0, GPIO2 = 4095 (tied high?), GPIO3 ~0.
 */
