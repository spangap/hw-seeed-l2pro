/**
 * l2pro.h — Seeed Wio Tracker L2 Pro board support.
 *
 * What this module provides:
 *   - The board's own pins and I2C addresses: the board I2C bus, the IO
 *     expander and what each of its sixteen lines does, the backlight driver,
 *     the battery ADC, the audio codecs and the Home button.
 *   - The expander and the bus, to the board's conditional slices (lcd, audio,
 *     gps), which each switch a line of it.
 *   - The bring-up Services (L2proBoard / L2proBattery).
 *
 * The NV3031B panel and the GT911 touch are spangap-lcd's (CONFIG_LCD_* /
 * CONFIG_LCD_TOUCH_*), the radio iface-lora's (CONFIG_LORA*), the SD card
 * spangap-core's (CONFIG_SPANGAP_SDCARD_*), the GNSS UART gps's (CONFIG_GPS_*)
 * and the I2S lines audio's (CONFIG_AUDIO_*) — all supplied by straddle.yaml.
 */
#pragma once

#include "sdkconfig.h"
#include "service.h"

#include "driver/i2c_master.h"

#include <cstdint>

#define BOARD_NAME              "Seeed Wio Tracker L2 Pro"

/* The board I2C bus. Created on a named controller because spangap-lcd's touch
 * adopts it by number (CONFIG_LCD_TOUCH_I2C_ADOPT). Everything below that has
 * an address is on these two wires. */
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_PIN       47
#define BOARD_I2C_SCL_PIN       48
#define BOARD_I2C_HZ            100000

/* IO expander (TCA9535-class, 0x21). Its INT is open-drain, active low, held
 * until the input ports are read. */
#define BOARD_EXP_ADDR          0x21
#define BOARD_EXP_INT_PIN       45

/* Expander lines, numbered 8 x port + bit. Inputs: */
#define EXP_WAKE_KEY            0    /* side key, active low */
#define EXP_I2C_INT             1
#define EXP_SD_DETECT           2
/* Outputs: */
#define EXP_TP_INT              3    /* GT911 address select: low through reset -> 0x5D */
#define EXP_P04                 4    /* named LCD_CS by Meshtastic, CODEC_EN elsewhere; driven high */
#define EXP_LCD_PWR             5
#define EXP_LCD_RST             6
#define EXP_GROVE_PWR           7
#define EXP_TP_RST              8
#define EXP_GNSS_RST            9    /* active high */
#define EXP_LED_USER            10
#define EXP_OTG_EN              11   /* USB-C 5 V out; kept off */
#define EXP_PA_EN               12   /* speaker amplifier */
#define EXP_GNSS_PWR            13
#define EXP_SD_PWR              14
#define EXP_BAT_ADC_EN          15   /* battery divider */

/* The rest of the bus. */
#define BOARD_LP5814_ADDR       0x2C /* backlight LED driver (also answers the family broadcast, 0x34) */
#define BOARD_ADS1115_ADDR      0x48 /* battery ADC, AIN0 over a 1:2 divider */
#define BOARD_ES8311_ADDR       0x18 /* speaker DAC */
#define BOARD_ES7243E_ADDR      0x14 /* mic ADC */

/* Home button: GPIO 0, the BOOT strap (the case's U/B key), pulled-up
 * active-low. */
#define BOARD_HOME_BTN_PIN      0

/* Present but unwired — the platform has no engine for it: the Mesh LED. */
#define BOARD_MESH_LED_PIN      43

/** The board I2C bus, or null before L2proBoard::onStart made it. */
i2c_master_bus_handle_t l2proBus(void);

/** Drive one expander output line. Any task. False if the write failed. */
bool l2proExpSet(int line, bool high);

/** Read both expander input ports (line n is bit n), which also releases its
 *  INT. Any task. False if the read failed. */
bool l2proExpInputs(uint16_t* in);

/**
 * Board bring-up. onStart runs before spangapInit(): the bus, the expander,
 * every rail it switches and the resets it holds — the SD card's rail before
 * spangapInit()'s fs_mount_sd(), the panel's and the touch controller's before
 * spangap-lcd brings them up. onInit publishes sys.board.
 */
class L2proBoard : public Service {
public:
    void onStart() override;
    void onInit()  override;
};

/**
 * Battery monitor (onInit): the ADS1115 sampled once a minute, publishing
 * battery.millivolt / battery.percent.
 */
class L2proBattery : public Service {
public:
    void onInit() override;
};
