/**
 * l2pro.cpp — Seeed Wio Tracker L2 Pro board support, end to end.
 *
 * Single owner of the L2 Pro's bring-up. See l2pro.h for the pin map and
 * README.md for the hardware reference. Layout:
 *
 *   1. The board I2C bus and the IO expander.
 *   2. Rails and resets (L2proBoard::onStart).
 *   3. Battery monitor (L2proBattery).
 *
 * Nearly everything on this board is switched by the expander: the panel, SD
 * and GNSS rails, the panel, touch and GNSS resets, the battery divider and the
 * speaker amplifier. So the bus and the expander come first, and onStart runs
 * whole before spangapInit() mounts the card or spangap-lcd wakes the panel.
 */
#include "l2pro.h"

#include "cli.h"
#include "i2c_helper.h"     /* SPANGAP_I2C_PULLUP */
#include "log.h"
#include "storage.h"        /* battery.* ephemerals, sys.board */

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

/* =========================================================================
 * 1. The board I2C bus and the IO expander
 * ========================================================================= */

namespace {

constexpr uint8_t EXP_REG_INPUT  = 0x00;   /* port 0, then port 1 */
constexpr uint8_t EXP_REG_OUTPUT = 0x02;
constexpr uint8_t EXP_REG_POLAR  = 0x04;
constexpr uint8_t EXP_REG_CONFIG = 0x06;   /* 1 = input */

constexpr uint16_t EXP_INPUTS = (1u << EXP_WAKE_KEY) | (1u << EXP_I2C_INT) |
                                (1u << EXP_SD_DETECT);

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_exp = nullptr;
SemaphoreHandle_t       s_expLock = nullptr;
/* The output registers as last written. Lines are changed against this, never
 * against a read of the chip, whose input registers report the pins. */
uint16_t                s_expOut = 0;

bool expWrite16(uint8_t reg, uint16_t v) {
    const uint8_t buf[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    return s_exp && i2c_master_transmit(s_exp, buf, sizeof(buf), 100) == ESP_OK;
}

bool busInit() {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port          = BOARD_I2C_PORT;
    bus.sda_io_num        = (gpio_num_t)BOARD_I2C_SDA_PIN;
    bus.scl_io_num        = (gpio_num_t)BOARD_I2C_SCL_PIN;
    bus.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        err("board i2c bus would not open (SDA=%d SCL=%d)\n",
            BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
        s_bus = nullptr;
        return false;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = BOARD_EXP_ADDR;
    dev.scl_speed_hz    = BOARD_I2C_HZ;
    if (i2c_master_bus_add_device(s_bus, &dev, &s_exp) != ESP_OK) {
        err("no IO expander at 0x%02X\n", BOARD_EXP_ADDR);
        s_exp = nullptr;
        return false;
    }
    return true;
}

}  // namespace

i2c_master_bus_handle_t l2proBus(void) { return s_bus; }

bool l2proExpSet(int line, bool high) {
    if (!s_exp || !s_expLock) return false;
    xSemaphoreTake(s_expLock, portMAX_DELAY);
    uint16_t next = high ? (uint16_t)(s_expOut | (1u << line))
                         : (uint16_t)(s_expOut & ~(1u << line));
    bool ok = expWrite16(EXP_REG_OUTPUT, next);
    if (ok) s_expOut = next;
    xSemaphoreGive(s_expLock);
    return ok;
}

bool l2proExpInputs(uint16_t* in) {
    if (!s_exp) return false;
    uint8_t reg = EXP_REG_INPUT, buf[2];
    if (i2c_master_transmit_receive(s_exp, &reg, 1, buf, 2, 100) != ESP_OK) return false;
    *in = (uint16_t)(buf[0] | (buf[1] << 8));
    return true;
}

/* =========================================================================
 * 2. Rails and resets
 *
 * One write puts every output at rest, then the direction registers make them
 * outputs: the panel, SD, GNSS and Grove rails on, the battery divider on, the
 * amplifier and the USB 5 V output off, and the GNSS receiver held in reset.
 * The panel's and the touch controller's resets are then pulsed together once
 * their rail has settled.
 *
 * The GT911 picks its address from its INT line as it leaves reset — low gives
 * 0x5D — and INT here is an expander line, not a GPIO, so it stays driven low:
 * touch is polled (CONFIG_LCD_TOUCH_POLL_MS), never interrupt-driven.
 * ========================================================================= */

static void expInit(void)
{
    s_expOut = EXP_INPUTS |
               (1u << EXP_P04)      | (1u << EXP_LCD_PWR)  | (1u << EXP_LCD_RST) |
               (1u << EXP_GROVE_PWR)| (1u << EXP_GNSS_RST) | (1u << EXP_GNSS_PWR) |
               (1u << EXP_SD_PWR)   | (1u << EXP_BAT_ADC_EN);
    expWrite16(EXP_REG_OUTPUT, s_expOut);
    expWrite16(EXP_REG_POLAR, 0x0000);
    expWrite16(EXP_REG_CONFIG, EXP_INPUTS);
    vTaskDelay(pdMS_TO_TICKS(50));          /* rails settle */

    l2proExpSet(EXP_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    l2proExpSet(EXP_LCD_RST, true);
    l2proExpSet(EXP_TP_RST, true);          /* GT911 latches 0x5D on this edge */
    l2proExpSet(EXP_GNSS_RST, false);       /* receiver runs */
    vTaskDelay(pdMS_TO_TICKS(120));         /* panel out of reset, GT911 booted */

    uint16_t in;
    l2proExpInputs(&in);                    /* release any INT power-up latched */
}

void L2proBoard::onStart() {
    s_expLock = xSemaphoreCreateMutex();
    if (!busInit()) return;     /* nothing on this board works without the bus */
    expInit();
}

/* `l2pro` — the expander's lines and any register on the board bus, by hand.
 *   l2pro                          expander outputs and inputs
 *   l2pro exp <line> <0|1>         drive one expander line
 *   l2pro i2c <addr> <reg> [<val>] read (or write) one register byte */
static void l2proCli(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s expander lines, board I2C registers\n", CLI_HELP_COL, "l2pro");
        if (strcmp(args, "help") != 0) {
            cliPrintf("  l2pro                          expander outputs and inputs\n");
            cliPrintf("  l2pro exp <line> <0|1>         drive one expander line (0..15)\n");
            cliPrintf("  l2pro i2c <addr> <reg> [<val>] read or write one register (hex)\n");
        }
        return;
    }
    char* end;
    if (!strncmp(args, "exp ", 4)) {
        long line = strtol(args + 4, &end, 0);
        long val  = strtol(end, nullptr, 0);
        if (line < 0 || line > 15) { cliPrintf("line 0..15\n"); return; }
        cliPrintf("%s\n", l2proExpSet((int)line, val != 0) ? "ok" : "write failed");
        return;
    }
    if (!strncmp(args, "i2c ", 4)) {
        long addr = strtol(args + 4, &end, 16);
        long reg  = strtol(end, &end, 16);
        while (*end == ' ') end++;
        bool write = *end != 0;
        long val   = write ? strtol(end, nullptr, 16) : 0;
        if (!s_bus || addr < 0x08 || addr > 0x77 || reg < 0 || reg > 0xFF) {
            cliPrintf("usage: l2pro i2c <addr> <reg> [<val>] (hex)\n");
            return;
        }
        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address  = (uint16_t)addr;
        dev.scl_speed_hz    = BOARD_I2C_HZ;
        i2c_master_dev_handle_t h = nullptr;
        if (i2c_master_bus_add_device(s_bus, &dev, &h) != ESP_OK) { cliPrintf("no handle\n"); return; }
        uint8_t r = (uint8_t)reg, v = 0;
        esp_err_t e;
        if (write) {
            const uint8_t buf[2] = { r, (uint8_t)val };
            e = i2c_master_transmit(h, buf, 2, 100);
        } else {
            e = i2c_master_transmit_receive(h, &r, 1, &v, 1, 100);
        }
        i2c_master_bus_rm_device(h);
        if (e != ESP_OK) cliPrintf("0x%02lX: %s\n", addr, esp_err_to_name(e));
        else if (write)  cliPrintf("0x%02lX[%02X] <- %02lX\n", addr, r, val);
        else             cliPrintf("0x%02lX[%02X] = %02X\n", addr, r, v);
        return;
    }
    uint16_t in = 0;
    bool ok = l2proExpInputs(&in);
    cliPrintf("expander out %04X  in %s%04X\n", (unsigned)s_expOut, ok ? "" : "(read failed) ",
              (unsigned)in);
}

/* onInit — the board says what it is, once storage exists to say it into. */
void L2proBoard::onInit() {
    storageSet("sys.board", BOARD_NAME);
    cliRegisterCmd("l2pro", l2proCli);
}

/* =========================================================================
 * 3. Battery monitor
 *
 * VBAT reaches the ADS1115's AIN0 through a 1:2 divider (switched on by
 * EXP_BAT_ADC_EN, left on). Each one-minute tick reads the conversion the
 * previous tick started and starts the next, so no tick waits on the ADC.
 * ========================================================================= */

namespace {

constexpr uint8_t  ADS_REG_CONV   = 0x00;
constexpr uint8_t  ADS_REG_CONFIG = 0x01;
/* Single shot, AIN0 to GND, +-4.096 V, 128 SPS, comparator off. */
constexpr uint16_t ADS_CONFIG     = 0xC383;
constexpr int64_t  BAT_PERIOD_US  = 60LL * 1000000;
constexpr int64_t  BAT_FIRST_US   = 20LL * 1000;

/* Open-circuit voltage at 100 %, 90 %, … 0 % (the board's Meshtastic curve). */
const uint16_t s_ocvMv[11] = {
    4180, 4040, 3864, 3800, 3745, 3710, 3687, 3663, 3623, 3482, 3300,
};

i2c_master_dev_handle_t s_ads     = nullptr;
bool                    s_started = false;
uint32_t                s_mvEma   = 0;

uint8_t batteryPercent(uint16_t mv) {
    if (mv >= s_ocvMv[0])  return 100;
    if (mv <= s_ocvMv[10]) return 0;
    for (int i = 1; i <= 10; i++) {
        if (mv >= s_ocvMv[i]) {
            uint16_t hi = s_ocvMv[i - 1], lo = s_ocvMv[i];
            int pctLo = 100 - i * 10;
            return (uint8_t)(pctLo + (uint32_t)(mv - lo) * 10 / (hi - lo));
        }
    }
    return 0;
}

bool adsStart() {
    const uint8_t buf[3] = { ADS_REG_CONFIG, (uint8_t)(ADS_CONFIG >> 8), (uint8_t)ADS_CONFIG };
    return i2c_master_transmit(s_ads, buf, sizeof(buf), 100) == ESP_OK;
}

bool adsRead(int16_t* raw) {
    uint8_t reg = ADS_REG_CONV, buf[2];
    if (i2c_master_transmit_receive(s_ads, &reg, 1, buf, 2, 100) != ESP_OK) return false;
    *raw = (int16_t)((buf[0] << 8) | buf[1]);
    return true;
}

/* esp_timer task. */
void batteryTick(void*) {
    int16_t raw;
    if (s_started && adsRead(&raw) && raw > 0) {
        uint32_t mv = (uint32_t)raw / 4;    /* 125 uV/LSB at +-4.096 V, x2 divider */
        s_mvEma = s_mvEma ? (s_mvEma * 3 + mv) / 4 : mv;
        uint16_t outMv = (uint16_t)s_mvEma;
        storageBegin();
        storageSet("battery.millivolt", (int)outMv);
        storageSet("battery.percent",   (int)batteryPercent(outMv));
        storageEnd();
    }
    s_started = adsStart();
}

}  // namespace

void L2proBattery::onInit() {
    if (!s_bus) return;
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = BOARD_ADS1115_ADDR;
    dev.scl_speed_hz    = BOARD_I2C_HZ;
    if (i2c_master_bus_add_device(s_bus, &dev, &s_ads) != ESP_OK) {
        warn("battery: no ADS1115 at 0x%02X\n", BOARD_ADS1115_ADDR);
        return;
    }
    s_started = adsStart();

    esp_timer_create_args_t targs = { .callback = batteryTick, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "battery", .skip_unhandled_events = true };
    esp_timer_handle_t first = nullptr, every = nullptr;
    if (esp_timer_create(&targs, &first) == ESP_OK) esp_timer_start_once(first, BAT_FIRST_US);
    targs.name = "battery-1m";
    if (esp_timer_create(&targs, &every) == ESP_OK) esp_timer_start_periodic(every, BAT_PERIOD_US);
    else warn("battery: timer create failed\n");
}
