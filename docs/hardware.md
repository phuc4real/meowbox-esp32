# Hardware: OSTB xiaozhi-3st chatbot speaker

A small ESP32-S3 AI speaker sold in Vietnam. It shipped with a closed-source Vietnamese fork of
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (vendor SKU string `ostb-xiaozhi-3st`). Its audio
section is the same design as Espressif's ESP32-S3-BOX: ES8311 + ES7210. There is no schematic; everything
below was found from the device itself (boot log, disassembly, probing). See
[reverse-engineering.md](reverse-engineering.md). Pins: [pinmap.md](pinmap.md).

## Main parts

| Block | Part | Notes |
|---|---|---|
| MCU | **ESP32-S3** (QFN56) rev v0.2 | dual core 240 MHz, Wi-Fi + BLE 5 (no Bluetooth Classic, so no A2DP audio) |
| Flash | 16 MB, Boya (mfr `0x68`, dev `0x4018`) | quad (QIO), 3.3 V |
| PSRAM | 8 MB octal, AP Memory gen 3 | 80 MHz |
| Speaker DAC | **Everest ES8311** | I2C `0x18`, I2S slave, MCLK from the ESP32 |
| Speaker amp | (unknown part) | enable on GPIO4, active high |
| Mic ADC | **Everest ES7210**, 4 channels | I2C `0x40`, TDM; mics on slots 0 and 2 |
| Display | TFT, **NV3023** controller | SPI, 296 × 240 landscape, RGB565; the case bezel hides the outer edge |
| Touch | **Hynitron CST816S** | I2C `0x15`, polled (INT not wired), coordinates already in screen space |
| Backlight | PWM on GPIO13 | active high |
| Buttons | volume up GPIO40, volume down GPIO39 (active low); middle button GPIO1 (active high) | holding the middle button powers off in hardware |
| Battery | Li-ion cell with charger | charge detect GPIO47, voltage on GPIO17 (ADC2 CH6, 1:2 divider assumed) |
| USB | native USB-Serial/JTAG (VID `303A`, PID `1001`) | appears as a COM port; flashing needs no buttons |

Security eFuses are all off: no secure boot, no flash encryption, every key block empty, so **any
firmware can be flashed**. A full flash backup of the vendor state should be kept before experimenting
(`backup/`, not in git).

## Block diagram

```
                     USB-C (native USB, GPIO19/20)
                               │
                     ┌─────────┴──────────┐
 middle btn GPIO1 ───┤                    ├──── GPIO13 PWM ──► backlight
   vol+   GPIO40 ────┤                    │
   vol−   GPIO39 ────┤     ESP32-S3       ├──── SPI3: MOSI10 SCLK9 CS14 DC8 RST18 ──► NV3023 TFT
 charging GPIO47 ────┤  16 MB flash       │
 battery  GPIO17 ────┤  8 MB octal PSRAM  │
                     │                    │
                     │   I2C0 SDA12 SCL11 ├──┬──► ES8311  @0x18 (control)
                     │                    │  ├──► ES7210  @0x40 (control)
                     │                    │  └──► CST816S @0x15 (touch)
                     │                    │
                     │   I2S0  MCLK5      ├──┬──► ES8311 + ES7210 (shared clocks)
                     │         BCLK15     │  │
                     │         WS16       │  │
                     │         DOUT6 ─────┼──┴──► ES8311 ──► amp (EN = GPIO4) ──► speaker
                     │         DIN7  ◄────┼────── ES7210 ◄── mics (TDM slots 0 and 2)
                     └────────────────────┘
```

## Audio path

- I2S0 runs **full duplex** with shared clocks: TX in standard mode to the ES8311, RX in TDM mode
  (4 slots) from the ES7210. MCLK = 256 × fs.
- The vendor firmware and xiaozhi run it at 24 kHz with echo cancellation (slot 1 = speaker loopback).
  MeowBox runs it at **44.1 kHz** mono output: streams are downmixed and resampled to that rate.
- Driver stack: `esp_codec_dev` with the es8311 and es7210 drivers (see `firmware/components/audio`).

## Power

- The middle button is wired to a hardware power latch: a long press switches the speaker off, a short
  press only pulses GPIO1, which firmware can read.
- Battery voltage = calibrated ADC reading on GPIO17 × 2. Measured 4.21 V while charging. The divider
  ratio has not been checked with a multimeter yet.
- The ADC → % table (1970 = 0 % … 2430 = 100 %) comes from the vendor firmware and still needs a real
  full-discharge run to confirm.
