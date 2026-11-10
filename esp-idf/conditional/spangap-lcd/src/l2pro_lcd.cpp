/**
 * l2pro_lcd.cpp — Wio Tracker L2 Pro on-device-UI glue: the backlight and the
 * two keys. Lives under esp-idf/conditional/spangap-lcd/, so it is compiled
 * ONLY when spangap-lcd is staged.
 *
 * The panel and the GT911 are the lcd component's (CONFIG_LCD_* /
 * CONFIG_LCD_TOUCH_*). The board has no hardware keyboard and no pointer: the
 * touch screen is the pointer and lcdSetHasKeyboard() is never called, so the
 * on-screen keyboard stays active for all text input.
 *
 * Backlight: an LP5814 LED driver on the board I2C bus, its four channels in
 * parallel, so the screen's level is a PWM register write rather than a pin
 * (lcdPanelSetBacklightDriver).
 *
 * The Home button (GPIO 0, active-low):
 *   - hold 300 ms                -> the talk key (LCD_KEY_PTT) down until release
 *   - one click                  -> the launcher (lcdGoHome)
 *   - two clicks                 -> the running-app switcher (lcdShowRecents)
 *   - any press while in standby -> wake (clear sys.standby), fully absorbed
 *                                   unless the talk key is claimed, then it
 *                                   is a fresh press
 * Awake, the key bounces: each edge restarts a settle timer and the state
 * machine acts on the level only once it has held still for kSettleMs.
 *
 * The side key (expander line EXP_WAKE_KEY): each press toggles standby. It
 * reaches us as the expander's INT on GPIO 45, which falls on any input change
 * and stays low until the input ports are read — so an edge means "read the
 * expander", and in standby the line is a level the chip can wake on.
 */
#include "l2pro.h"
#include "l2pro_lcd.h"

#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_input.h"
#include "lcd_keys.h"
#include "lcd_panel.h"
#include "lcd.h"
#include "storage.h"
#include "log.h"
#include "pm.h"

#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include <cstdint>
#include <cstdlib>
#include "lvgl.h"

/* ---- Backlight: LP5814 ---- */

static i2c_master_dev_handle_t s_lp = nullptr;

static bool lpWrite(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = { reg, val };
    return s_lp && i2c_master_transmit(s_lp, buf, sizeof(buf), 50) == ESP_OK;
}

/* lcd task: every level the screen goes to. */
static void lpSet(uint8_t level) {
    for (uint8_t i = 0; i < 4; i++) lpWrite(0x18 + i, level);   /* LED0..3 PWM */
}

static void lpInit(void) {
    i2c_master_bus_handle_t bus = l2proBus();
    if (!bus) return;
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = BOARD_LP5814_ADDR;
    dev.scl_speed_hz    = BOARD_I2C_HZ;
    if (i2c_master_bus_add_device(bus, &dev, &s_lp) != ESP_OK) {
        s_lp = nullptr;
        warn("backlight: no LP5814 at 0x%02X\n", BOARD_LP5814_ADDR);
        return;
    }
    bool ok = lpWrite(0x00, 0x01)        /* chip enable */
           && lpWrite(0x01, 0x01)        /* 51 mA max current */
           && lpWrite(0x02, 0x00)        /* outputs off while configuring */
           && lpWrite(0x04, 0x4E)        /* dimming mode */
           && lpWrite(0x05, 0xF0);       /* engine mode */
    for (uint8_t i = 0; ok && i < 4; i++) ok = lpWrite(0x14 + i, 200);   /* LED0..3 DC */
    ok = ok && lpWrite(0x02, 0x0F)       /* all four channels on */
            && lpWrite(0x0F, 0x55);      /* latch */
    lpSet(0);                            /* dark until the boot reveal */
    if (!ok) warn("backlight: LP5814 setup failed\n");
}

/* ---- Keys ---- */

static void l2InputInit(void);
static bool l2ClickRead(void);
static void l2Standby(bool on);
static void btnSettledCb(lv_timer_t*);

static constexpr uint32_t kHoldMs        = 300;   /* hold this long and it is the talk key */
static constexpr uint32_t kMulticlickMs  = 250;   /* a further click has to land within this */
static constexpr uint32_t kSettleMs      = 20;    /* the key's level has to hold this long to count */
static constexpr int64_t  kSideRepeatUs  = 250 * 1000;   /* side-key presses closer than this are bounce */

static lv_timer_t*    s_settleTimer  = nullptr;
static lv_timer_t*    s_holdTimer    = nullptr;
static lv_timer_t*    s_clickTimer   = nullptr;
static int            s_clicks       = 0;
static bool           s_btnHeld      = false;
static bool           s_btnConsumed  = false;
static bool           s_pttDown      = false;     /* this press reported LCD_KEY_PTT down */
static bool           s_wakeAbsorb   = false;
static volatile bool  s_standby      = false;     /* mirror of sys.standby (set on the lcd task) */

static volatile bool  s_expIrq       = false;     /* the expander's INT fell: read it */
static bool           s_sideDown     = false;
static int64_t        s_sideLastUs   = 0;

/* Standby wake: both keys become light-sleep wake sources at LOW_LEVEL (edges
 * are invisible while the GPIO clock is gated). A level ISR re-fires for as
 * long as the line is low, so each silences its pin on first fire and the lcd
 * task re-enables it after handling. Standby can be entered with a Home press
 * still down, and GPIO 0 can only be armed once it lifts: s_standbyLock holds
 * light sleep off for that window. */
static pm_lock_handle_t s_standbyLock = nullptr;
static bool             s_wakeArmed   = false;
static volatile bool    s_wakePending = false;   /* a Home press woke us, even if since lifted */

static void IRAM_ATTR expISR(void* arg) {
    s_expIrq = true;
    lcdInputISR(arg);
}

static void IRAM_ATTR homeStandbyISR(void* arg) {
    gpio_ll_intr_disable(&GPIO, (gpio_num_t)BOARD_HOME_BTN_PIN);
    s_wakePending = true;
    lcdInputISR(arg);
}

static void IRAM_ATTR expStandbyISR(void* arg) {
    gpio_ll_intr_disable(&GPIO, (gpio_num_t)BOARD_EXP_INT_PIN);
    s_expIrq = true;
    lcdInputISR(arg);
}

static void l2WakeArm(void) {
    gpio_isr_handler_remove((gpio_num_t)BOARD_HOME_BTN_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, homeStandbyISR, nullptr);
    pmGpioWakeEnable(BOARD_HOME_BTN_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_isr_handler_remove((gpio_num_t)BOARD_EXP_INT_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_EXP_INT_PIN, expStandbyISR, nullptr);
    pmGpioWakeEnable(BOARD_EXP_INT_PIN, GPIO_INTR_LOW_LEVEL);
    s_wakeArmed   = true;
    s_wakePending = false;
    pmLockRelease(s_standbyLock);
}

static void l2WakeDisarm(void) {
    s_wakeArmed = false;
    pmGpioWakeDisable(BOARD_HOME_BTN_PIN);
    gpio_set_intr_type((gpio_num_t)BOARD_HOME_BTN_PIN, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_remove((gpio_num_t)BOARD_HOME_BTN_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, lcdInputISR, nullptr);
    pmGpioWakeDisable(BOARD_EXP_INT_PIN);
    gpio_set_intr_type((gpio_num_t)BOARD_EXP_INT_PIN, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_remove((gpio_num_t)BOARD_EXP_INT_PIN);
    gpio_isr_handler_add((gpio_num_t)BOARD_EXP_INT_PIN, expISR, nullptr);
    gpio_intr_enable((gpio_num_t)BOARD_EXP_INT_PIN);
}

/* Light-sleep wake backstop (IDLE task). A press can bounce through the
 * sleep-exit window so the level ISR never fires; re-check both lines on every
 * GPIO wake while armed. */
static void l2SleepWake(int cause) {
    if (cause != ESP_SLEEP_WAKEUP_GPIO) return;
    if (!s_standby || !s_wakeArmed) return;
    bool hit = false;
    if (gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0) { s_wakePending = true; hit = true; }
    if (gpio_get_level((gpio_num_t)BOARD_EXP_INT_PIN) == 0)  { s_expIrq = true;      hit = true; }
    if (hit) lcdInputSignal();
}

static void l2KeysInit(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_HOME_BTN_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.intr_type    = GPIO_INTR_ANYEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)BOARD_HOME_BTN_PIN, lcdInputISR, nullptr);

    io.pin_bit_mask = 1ULL << BOARD_EXP_INT_PIN;
    io.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)BOARD_EXP_INT_PIN, expISR, nullptr);
    s_expIrq = true;                         /* read once now: an INT already low makes no edge */

    pmOnLightSleepWake(l2SleepWake);
}

/* Side key: read the expander, act on a fresh press. */
static void sidePoll(void) {
    if (!s_expIrq) return;
    s_expIrq = false;
    uint16_t in;
    bool ok = l2proExpInputs(&in);
    gpio_intr_enable((gpio_num_t)BOARD_EXP_INT_PIN);   /* a standby ISR silenced it */
    if (!ok) return;
    bool down = !(in & (1u << EXP_WAKE_KEY));
    if (down && !s_sideDown) {
        int64_t now = esp_timer_get_time();
        if (now - s_sideLastUs > kSideRepeatUs) {
            s_sideLastUs = now;
            storageSet("sys.standby", s_standby ? 0 : 1);
        }
    }
    s_sideDown = down;
}

static void cancelHoldTimer(void) {
    if (s_holdTimer) { lv_timer_delete(s_holdTimer); s_holdTimer = nullptr; }
}

static void pttDown(void) {
    s_btnConsumed = true;
    s_pttDown     = true;
    lcdKeyReport(LCD_KEY_PTT, true);
}

static void pttUp(void) {
    if (!s_pttDown) return;
    s_pttDown = false;
    lcdKeyReport(LCD_KEY_PTT, false);
}

static void cancelClickBurst(void) {
    if (s_clickTimer) { lv_timer_delete(s_clickTimer); s_clickTimer = nullptr; }
    s_clicks = 0;
}

static void btnClickCb(lv_timer_t*) {
    s_clickTimer = nullptr;
    int n = s_clicks;
    s_clicks = 0;
    if (n == 1) lcdGoHome();
}

static void armClickWindow(void) {
    if (s_clickTimer) lv_timer_delete(s_clickTimer);
    s_clickTimer = lv_timer_create(btnClickCb, kMulticlickMs, nullptr);
    lv_timer_set_repeat_count(s_clickTimer, 1);
}

static void btnHoldCb(lv_timer_t*) {
    s_holdTimer = nullptr;
    cancelClickBurst();
    pttDown();
}

/* lcd_input.h click_read (lcd task). Never asserts a click: there is no
 * pointer to click with, so a press is navigation. */
static bool l2ClickRead(void) {
    sidePoll();

    bool down = gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0;

    if (s_wakeAbsorb) {
        if (!down) {
            s_wakeAbsorb = false;
            if (s_standby && !s_wakeArmed) l2WakeArm();
        }
        return false;
    }
    if (s_standby) {
        if (s_pttDown) {
            if (!down) { pttUp(); s_btnHeld = false; }
            return false;
        }
        if (!s_wakeArmed) {
            if (!down) l2WakeArm();
            return false;
        }
        if (down || s_wakePending) {
            s_wakePending = false;
            storageSet("sys.standby", 0);
            if (down && lcdKeyClaimed(LCD_KEY_PTT)) { s_btnHeld = true; pttDown(); }
            else if (down) s_wakeAbsorb = true;
        }
        else gpio_intr_enable((gpio_num_t)BOARD_HOME_BTN_PIN);
        return false;
    }

    if (s_settleTimer) lv_timer_reset(s_settleTimer);
    else {
        s_settleTimer = lv_timer_create(btnSettledCb, kSettleMs, nullptr);
        lv_timer_set_repeat_count(s_settleTimer, 1);
    }
    return false;
}

static void btnSettledCb(lv_timer_t*) {
    s_settleTimer = nullptr;
    if (s_standby || s_wakeAbsorb) return;
    bool down = gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) == 0;

    if (down) {
        if (!s_btnHeld) {
            s_btnHeld     = true;
            s_btnConsumed = false;
            if (s_clickTimer) { lv_timer_delete(s_clickTimer); s_clickTimer = nullptr; }
            s_holdTimer = lv_timer_create(btnHoldCb, kHoldMs, nullptr);
            lv_timer_set_repeat_count(s_holdTimer, 1);
        }
        return;
    }

    cancelHoldTimer();
    pttUp();
    bool released = (s_btnHeld && !s_btnConsumed);
    s_btnHeld = false;
    if (!released) return;

    if (++s_clicks >= 2) { cancelClickBurst(); lcdShowRecents(); return; }
    armClickWindow();
}

/* lcd_input.h init hook — lcd task, once the panel and the shared GPIO ISR
 * service are up. */
static void l2InputInit(void) {
    l2KeysInit();
    lcdKeySetLabel(LCD_KEY_PTT, "U/B");
    lcdKeyDeclare(LCD_KEY_PTT);
    storageSubscribeChanges("sys.standby", ON_CHANGE { l2Standby(atoi(val) != 0); });
}

/* sys.standby subscription target (lcd task): display off, keys become the
 * wake sources — and back. */
static void l2Standby(bool on) {
    if (on == s_standby) return;
    s_standby = on;
    if (on) {
        cancelClickBurst();
        cancelHoldTimer();
        if (s_settleTimer) { lv_timer_delete(s_settleTimer); s_settleTimer = nullptr; }
        pttUp();
        s_btnHeld = false;
        lcdScreenSleep();
        if (!s_standbyLock) pmLockCreate(PM_NO_LIGHT_SLEEP, "standby", &s_standbyLock);
        pmLockAcquire(s_standbyLock);
        if (gpio_get_level((gpio_num_t)BOARD_HOME_BTN_PIN) != 0) l2WakeArm();
        info("standby\n");
    } else {
        if (s_wakeArmed) l2WakeDisarm();
        else             pmLockRelease(s_standbyLock);
        lcdScreenWake();
        info("wake\n");
    }
}

void L2proLcdInput::onStart() {
    lpInit();
    lcdPanelSetBacklightDriver(lpSet);
    static const lcd_input_t ops = {
        .init         = l2InputInit,
        .touch_read   = nullptr,
        .pointer_read = nullptr,
        .click_read   = l2ClickRead,
    };
    lcdSetInput(&ops);
}
