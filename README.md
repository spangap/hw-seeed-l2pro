# hw-seeed-l2pro — Seeed Wio Tracker L2 Pro board HAL

```
board I2C up (47/48)?     no  → nothing below runs; the board is a bare ESP32-S3
 ↓ yes
IO expander (0x21)        → all rails on, panel/touch/GNSS resets released   (onStart)
 ↓
SD mount, panel, touch    → spangap-core / spangap-lcd, on the rails just raised
 ↓
LP5814 backlight          → spangap-lcd's lamp, driven over I2C
battery (ADS1115)         → battery.millivolt / battery.percent               (every minute)
codecs (ES8311, ES7243E)  → switched with the audio engine's directions
```

**hw-seeed-l2pro** is the board-support straddle for the **Seeed Wio Tracker
L2 Pro**: a handheld touch-screen LoRa mesh node built on Seeed's **Wio-S3**
module (ESP32-S3R8, 16 MB flash, 8 MB **octal** PSRAM, a Semtech **SX1262** on
the module's own SPI bus). The case carries a 3.2" 320x240 IPS panel — an
**NV3031B** on **quad SPI** — with **GT911** capacitive touch and an **LP5814**
LED driver as its backlight, a **Quectel L76K** GNSS receiver, an **ES8311**
speaker DAC and an **ES7243E** microphone ADC, microSD, an **ADS1115** battery
ADC, two keys and a 3000 mAh li-ion cell. A TCA9535-class IO expander switches
nearly every rail and reset. Board reference:
<https://wiki.seeedstudio.com/meshtastic_wio_tracker_l2_intro/>.

It is a **non-buildable** component. A buildable adds it and inherits the
board: `spangap build reticulous/reticulous --with spangap/hw-seeed-l2pro`.

The board stages `spangap-lcd` (drop with `--no-lcd`), `gps` and `audio`, and
`iface-lora` when the buildable is a Reticulum node. There is **no hardware
keyboard**: spangap-lcd's on-screen keyboard is the text input, and the touch
screen is the pointer.

Seeed publishes no full schematic. The map below comes from Seeed's wiki and
block diagram, the Wio-S3 datasheet, Meshtastic's `seeed_wio_tracker_L2`
variant (Seeed's own port) and LovyanGFX's `Panel_NV3031B`, and every part of
it — panel, touch, backlight, radio, GNSS pins, SD, battery, keys, speaker and
microphone — has been brought up on a unit.

## Audio

One set of I2S clocks (MCLK 10, BCLK 11, WS 12) serves both codecs, so the
audio engine runs both directions on I2S0 (`CONFIG_AUDIO_DUPLEX`), 16 kHz, in
32-bit slots:

- **Speaker:** the ES8311 (0x18), configured through `esp_codec_dev`, data out on
  GPIO 16, behind an amplifier switched on expander line P1.4 while playing.
- **Microphone:** the ES7243E (0x14), data in on GPIO 15, **right slot**. Its
  supply is expander line P0.4 (Meshtastic names that line `LCD_CS`). The board
  writes the codec's init table and stops, preamp at +30 dB — no driver enable
  step after it.

`audio` on the CLI shows the last captured block per slot (peak, mean, raw
words); `l2pro i2c 14 <reg>` reads the codec live.

## Keys and standby

The **U/B key** (GPIO 0, the BOOT strap) is Home: one click → launcher, two
clicks → app switcher, hold 300 ms → standby, any press in standby → wake. The
**side key** (expander line P0.0, reaching the chip as the expander's INT on
GPIO 45) toggles standby. In standby both are light-sleep wake sources. Touch
cannot wake the device: the GT911's INT is an expander line held low for its
address select, so touch is polled.

## Hardware & pin map

### LoRa — SX1262 inside the Wio-S3 (SPI host 2, private bus)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| SCK | 4 | | RST | 7 |
| MOSI | 6 | | BUSY | 8 |
| MISO | 5 | | DIO1 | 9 |
| NSS / CS | 21 | | Power gate | none |

DIO2 drives the module's SKY13453 RF switch; DIO3 feeds the 32 MHz TCXO (and
the switch's supply). No external PA. One hardware SKU covers 863–928 MHz.

### Display — NV3031B on quad SPI (host 3, its own bus)

| Signal | Pin | | Signal | Pin |
|---|---|---|---|---|
| SCLK | 42 | | CS | 46 |
| D0 | 41 | | RST | expander P0.6 |
| D1 | 40 | | Power | expander P0.5 |
| D2 | 39 | | Backlight | LP5814 @ 0x2C |
| D3 | 38 | | TE | not connected |

Native 240x320, rotated to 320x240 landscape (90°, the NV3031B's reversed scan
base taken into account by its driver), INVOFF, 40 MHz. The
glass reads both touch axes reversed against the panel's native frame. No D/C
line: commands are
framed `02 00 <cmd> 00`, pixels `32 00 2C 00` + quad data.

### Board I2C (port 0, SDA 47 / SCL 48, 100 kHz)

| Addr | Part | Notes |
|---|---|---|
| 0x14 | ES7243E | mic ADC |
| 0x18 | ES8311 | speaker DAC |
| 0x21 | IO expander | INT → GPIO 45 |
| 0x22 | AW35615 | USB-C CC controller (unwired) |
| 0x2C | LP5814 | backlight, four channels in parallel |
| 0x34 | LP5814 | the same chip: the LP5814/5/6/7 family's broadcast address |
| 0x48 | ADS1115 | battery, AIN0 over a 1:2 divider |
| 0x5D | GT911 | touch, reset/INT on the expander |

A second I2C (SDA 13 / SCL 14) is on the Grove port, unwired here.

### IO expander lines (8 x port + bit)

| Line | Function | | Line | Function |
|---|---|---|---|---|
| 0 | side key (in, active low) | | 8 | touch reset |
| 1 | shared I2C INT (in) | | 9 | GNSS reset (active high) |
| 2 | SD card detect (in) | | 10 | user LED (unwired) |
| 3 | touch INT / address select (held low) | | 11 | USB 5 V out (held off) |
| 4 | disputed, driven high | | 12 | speaker amplifier |
| 5 | panel rail | | 13 | GNSS rail |
| 6 | panel reset | | 14 | SD rail |
| 7 | Grove rail (on) | | 15 | battery divider (on) |

### Everything else

| Peripheral | Pins |
|---|---|
| microSD (SDMMC 1-bit) | CLK 2, CMD 3, D0 1 |
| GNSS — L76K (UART1) | host RX 18 ← GPS TX, host TX 17 → GPS RX |
| Audio (one I2S controller) | MCLK 10, BCLK 11, LRCK 12, DAC data 16, ADC data 15 |
| U/B key | GPIO 0 |
| Mesh LED (unwired) | GPIO 43 (also UART0 TX) |
| USB | native USB-Serial-JTAG (GPIO 19/20) |

Download mode: hold U/B while plugging in USB, then press RST.

## Storage variables

The board publishes `sys.board` and the shared `battery.millivolt` /
`battery.percent` ephemerals. Display settings live at `s.lcd.*`
([spangap-lcd](../spangap-lcd)), LoRa at `s.lora.*`
([iface-lora](../iface-lora)), GNSS at `s.gps.*` ([gps](../gps)), audio at
`s.audio.*` ([audio](../audio)).

## Dependencies

- [spangap-core](../spangap-core) — runtime; owns the SD card.
- [spangap-lcd](../spangap-lcd) — the NV3031B panel and the GT911 touch.
- [gps](../gps) — the GNSS task; this board supplies its power switch.
- [audio](../audio) — the I2S engine, in duplex mode; this board supplies the
  codec control plane.
- [iface-lora](../iface-lora) — the SX1262 radio engine.
- `espressif/esp_codec_dev` — the ES8311 register sequences.

## Read next

- [INTERNALS.md](INTERNALS.md) — the bring-up order, the expander, detection,
  the keys' standby state machine, the codec setup and the board's pitfalls.
