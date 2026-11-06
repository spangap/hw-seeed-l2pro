/**
 * detect.cpp — is the hardware under this firmware a Seeed Wio Tracker L2 Pro?
 *
 * The board's one self-assertion. It answers about THIS board only: its own
 * name when the three always-powered parts on its I2C bus answer and an SX1262
 * answers on the Wio-S3 module's radio pins, and NULL otherwise. Nothing here
 * enumerates other boards — that comparison belongs to whoever calls it.
 *
 * Two callers, one body:
 *
 *   * spangap-core, at the top of spangapInit(), before any bus is claimed. A
 *     board straddle is staged because the image was built for that board, so a
 *     NULL here means the image is on the wrong hardware and the platform halts
 *     rather than driving someone else's pins for the rest of the boot.
 *   * flashmon's standalone detector, which carries a copy of this function
 *     renamed `detect_hw_seeed_l2pro` and calls it alongside every other
 *     board's, to identify a chip whose firmware is unknown.
 *
 * The copy is manual and deliberately so — see detect_probe.h. Change this,
 * change flashmon/esp-idf/main/detect.c.
 *
 * This probe WRITES the expander once it has answered: the backlight driver and
 * the battery ADC sit behind its rails, and after a power loss it holds every
 * line as an input. The write is the rest state the board's own onStart writes
 * moments later, so it is a scratch write in detect_probe.h's sense. On a board
 * this is not, nothing at 0x21 on 47/48 answered and nothing is written. The
 * radio has no power gate.
 */
#include "detect_probe.h"
#include "l2pro.h"

/* LoRa pins (the Wio-S3 module's SX1262). They live in straddle.yaml as
 * CONFIG_LORA* — but those symbols only exist when iface-lora is staged, and
 * this probe has to work in an image built without a radio stack at all, so
 * they are written out here. */
#define DETECT_LORA_SCK    4
#define DETECT_LORA_MOSI   6
#define DETECT_LORA_MISO   5
#define DETECT_LORA_CS    21
#define DETECT_LORA_RST    7
#define DETECT_LORA_BUSY   8

/* Expander registers (output and direction, per port) and the rest state
 * l2pro.cpp's expInit writes: every rail on, the resets released, the three
 * input lines left as inputs. */
#define DETECT_EXP_OUT0   0x02
#define DETECT_EXP_OUT1   0x03
#define DETECT_EXP_CFG0   0x06
#define DETECT_EXP_CFG1   0x07
#define DETECT_EXP_REST0  0xF7
#define DETECT_EXP_REST1  0xE1
#define DETECT_EXP_IN0    0x07

extern "C" const char* detect_hw(void)
{
    /* 16 MB flash or it is not an L2 Pro — cheapest possible rejection, and it
     * touches no pin at all. */
    if (!detect_flash_mb(16)) return NULL;

    detect_i2c_t h;
    if (!detect_i2c_open(&h, BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN)) return NULL;
    bool exp = detect_i2c_ack(&h, BOARD_EXP_ADDR);
    if (exp) {
        /* The backlight driver and the ADC sit behind expander rails, which
         * come out of a power loss as inputs. Put the outputs at the rest
         * state L2proBoard::onStart sets, then make them outputs. */
        detect_i2c_wr(&h, BOARD_EXP_ADDR, DETECT_EXP_OUT0, DETECT_EXP_REST0);
        detect_i2c_wr(&h, BOARD_EXP_ADDR, DETECT_EXP_OUT1, DETECT_EXP_REST1);
        detect_i2c_wr(&h, BOARD_EXP_ADDR, DETECT_EXP_CFG0, DETECT_EXP_IN0);
        detect_i2c_wr(&h, BOARD_EXP_ADDR, DETECT_EXP_CFG1, 0x00);
        vTaskDelay(pdMS_TO_TICKS(50));      /* rails settle */
    }
    bool bl  = exp && detect_i2c_ack(&h, BOARD_LP5814_ADDR);
    bool adc = bl  && detect_i2c_ack(&h, BOARD_ADS1115_ADDR);
    detect_i2c_close(&h);
    if (!adc) {
        detect_miss("expander=%d backlight=%d adc=%d — not a Wio Tracker L2 Pro",
                    exp, bl, adc);
        return NULL;
    }

    /* Confirm with the radio, on the module's private bus. */
    if (!detect_radio_is(DETECT_LORA_SCK, DETECT_LORA_MOSI, DETECT_LORA_MISO,
                         DETECT_LORA_CS, DETECT_LORA_RST, DETECT_LORA_BUSY, "sx1262")) {
        detect_miss("I2C parts answered but no SX1262 — not a Wio Tracker L2 Pro");
        return NULL;
    }

    detect_found("hw_seeed_l2pro");
    return "hw-seeed-l2pro";
}
