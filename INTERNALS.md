# hw-seeed-l2pro — INTERNALS

Maintainer reference for the Seeed Wio Tracker L2 Pro board HAL. For what the
straddle is and how to build with it, read [README.md](README.md) first.

```
detect_hw: 16 MB flash? → expander ACK at 0x21 (47/48)? → write its rest state
           → LP5814 0x2C + ADS1115 0x48 ACK? → SX1262 on 4/6/5/21/7/8? → hw-seeed-l2pro
L2proBoard::onStart:  bus → expander rest state → rails settle 50 ms
                      → panel + touch resets released, GNSS out of reset → 120 ms
L2proLcdInput::onStart: LP5814 up → lcdPanelSetBacklightDriver → input HAL
L2proBattery::onInit:   ADS1115 single shot, read by the next tick (1/min)
L2proAudio::onInit:     codec ops → audio engine (applied per direction)
```

## 1. The expander is the board

A TCA9535-class expander at 0x21 switches nearly every rail and reset: panel
power and reset, touch reset and address select, GNSS power and reset, SD power,
the battery divider, the speaker amplifier, the USB 5 V output and the ES7243E's
supply (P0.4). `l2pro.cpp` keeps a shadow of the two output registers and
changes lines against it (`l2proExpSet`, under a mutex — the lcd, audio and gps
slices all call it), never against a read of the chip, whose input registers
report the pins.

The expander holds its outputs across a reset of the ESP32 but not across a
power loss, after which every line is an input and every rail it switches is
off. Two consequences:

- **Detection writes it.** The LP5814 and the ADS1115 sit behind its rails, so
  after a power loss only 0x21 answers. `detect_hw` (and flashmon's copy)
  writes the same rest state `expInit` does once 0x21 has answered on 47/48,
  waits 50 ms, then probes.
- **An unpowered ES7243E drags the bus.** With P0.4 low, the codec's
  protection diodes hold SDA/SCL down and every transaction on the board bus
  fails until it is powered again. P0.4 stays high.

The GT911 takes its address from its INT line as it leaves reset, and INT is an
expander line (P0.3), driven low through the reset for 0x5D and left there. Touch
is therefore polled (`CONFIG_LCD_TOUCH_POLL_MS`) and cannot wake the device.

## 2. Display

The NV3031B is spangap-lcd's (`CONFIG_LCD_CONTROLLER_NV3031B` on a quad-SPI
bus, host 3, its own pins). Its reset and rail are expander lines handled in
`onStart`; inversion is off (`CONFIG_LCD_INVERT_COLOR=n`, against that symbol's
default) and rotation 90°. The backlight is an LP5814 (0x2C, also answering its
family's broadcast address 0x34): four channels in parallel, configured once
(chip enable, 51 mA max, DC 200, latch with 0x55 to 0x0F) and then driven by
PWM register writes from the lcd task through `lcdPanelSetBacklightDriver`.

## 3. Keys and standby

`conditional/spangap-lcd/src/l2pro_lcd.cpp`. The U/B key (GPIO 0) is the
TAP V2's Home state machine (one click → home, two → recents, press in standby
→ wake, absorbed) with the hold as the talk key: 300 ms down reports
`LCD_KEY_PTT` down through `lcd_keys.h`, the release reports it up. A press in
standby while a consumer has the talk key claimed is not absorbed: it wakes and
keys on the same edge. The side key is expander line P0.0
and reaches the chip as the expander's INT on GPIO 45, which falls on any input
change and stays low until the input ports are read: an edge means "read the
expander", and a fresh press toggles `sys.standby` (presses within 250 ms of the
last are bounce).

In standby both GPIO 0 and GPIO 45 are LOW_LEVEL light-sleep wake sources. A
level ISR fires for as long as its line is low, so each silences its own pin on
first fire and the lcd task re-enables it after handling; `l2SleepWake` re-checks
both lines on every GPIO wake in case a bounce hid the edge. A Home press still
down when standby begins (it releases the talk key) must lift before GPIO 0 can be armed, so a `PM_NO_LIGHT_SLEEP` lock holds the CPU
awake for just that window.

## 4. Audio

Both codecs share MCLK/BCLK/WS, so the audio engine runs in duplex mode on I2S0
(see the audio straddle's INTERNALS): 16 kHz, MCLK 256 × fs, 32-bit slots with
WS spanning each slot, both slots captured.

- **ES8311 (0x18), speaker.** `esp_codec_dev`, DAC mode, slave, MCLK in;
  enabled per playback with the amplifier on P1.4, suspended after. Its ADC
  output does not reach GPIO 15.
- **ES7243E (0x14), microphone.** Written directly: the codec's init table
  (`kEs7243eInit`, the sequence every published ES7243E driver opens with) and
  nothing after it — the configuration proven on this board, with the preamp
  at +30 dB (0x20/0x21 = 0x1A) and the reference select 0x17 at 0x02. The
  drivers' enable step that usually follows rewrites both (0x01, 0 dB) and is
  not used. The mic is the codec's second channel, the
  **right** slot (`CONFIG_AUDIO_IN_SLOT_RIGHT`); the left carries only the idle
  first input.

## 5. Battery

VBAT reaches the ADS1115's AIN0 through a 1:2 divider switched on P1.7 (left
on). Each one-minute tick reads the conversion the previous tick started and
starts the next (single shot, ±4.096 V, 128 SPS), so no tick waits on the ADC.
Percent comes from Meshtastic's 11-point open-circuit curve for this board,
smoothed by a light EMA.

## 6. Pitfalls

- **Strapping pins in use:** GPIO 0 (U/B key), GPIO 45 (expander INT, the
  VDD_SPI strap) and GPIO 46 (panel CS, the boot-mode strap).
- **GPIO 43 is UART0 TX and the Mesh LED**: the ROM boot log flickers it.
- **The ES8311 refuses one write** at bring-up now and then
  (`I2C_If: Fail to write to dev 30`); the speaker works regardless.
- Keep flashmon's detector copy (`flashmon/esp-idf/main/detect.c`) in step with
  `esp-idf/src/detect.cpp`.
