/**
 * l2pro_audio.cpp — Wio Tracker L2 Pro audio codec control. Compiled only when
 * spangap/audio is staged.
 *
 * Two codecs on the board I2C bus and one set of I2S clocks, which the audio
 * engine drives from a single controller (CONFIG_AUDIO_DUPLEX), the ESP32-S3
 * as master and both codecs as slaves:
 *
 *   ES8311  (0x18)  DAC -> speaker amplifier (switched on EXP_PA_EN)
 *   ES7243E (0x14)  ADC <- microphone, data out on GPIO 15
 *
 * The ES8311 goes through esp_codec_dev. The ES7243E is written directly: the
 * init table only, without the enable step a driver follows it with.
 */
#include "l2pro.h"
#include "l2pro_audio.h"

#include "audio.h"
#include "log.h"

#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

namespace {

const audio_codec_if_t* s_dac = nullptr;
i2c_master_dev_handle_t s_adc = nullptr;

const uint8_t kEs7243eInit[][2] = {
    {0x01, 0x3A}, {0x00, 0x80}, {0xF9, 0x00}, {0x04, 0x02}, {0x04, 0x01}, {0xF9, 0x01},
    {0x00, 0x1E}, {0x01, 0x00}, {0x02, 0x00}, {0x03, 0x20}, {0x04, 0x01}, {0x0D, 0x00},
    {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF}, {0x09, 0xCA}, {0x0A, 0x85},
    {0x0B, 0x00}, {0x0E, 0xBF}, {0x0F, 0x80}, {0x14, 0x0C}, {0x15, 0x0C}, {0x17, 0x02},
    {0x18, 0x26}, {0x19, 0x77}, {0x1A, 0xF4}, {0x1B, 0x66}, {0x1C, 0x44}, {0x1E, 0x00},
    {0x1F, 0x0C}, {0x20, 0x1A}, {0x21, 0x1A}, {0x00, 0x80}, {0x01, 0x3A}, {0x16, 0x3F},
    {0x16, 0x00},
};

const audio_codec_if_t* dac() {
    if (s_dac) return s_dac;
    audio_codec_i2c_cfg_t i2c = {};
    i2c.port       = BOARD_I2C_PORT;
    i2c.addr       = (uint8_t)(BOARD_ES8311_ADDR << 1);   /* esp_codec_dev takes 8-bit addresses */
    i2c.bus_handle = l2proBus();
    if (!i2c.bus_handle) return nullptr;
    es8311_codec_cfg_t cfg = {};
    cfg.ctrl_if     = audio_codec_new_i2c_ctrl(&i2c);
    cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
    cfg.pa_pin      = -1;                     /* the amplifier is an expander line */
    cfg.master_mode = false;
    cfg.use_mclk    = true;                   /* MCLK = 256 x fs, the I2S driver's default */
    cfg.hw_gain.pa_voltage        = 5.0f;
    cfg.hw_gain.codec_dac_voltage = 3.3f;
    if (cfg.ctrl_if) s_dac = es8311_codec_new(&cfg);
    if (!s_dac) warn("audio: no ES8311 at 0x%02X\n", BOARD_ES8311_ADDR);
    return s_dac;
}

bool adcWrite(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = { reg, val };
    for (int i = 0; i < 3; i++)
        if (i2c_master_transmit(s_adc, buf, sizeof(buf), 50) == ESP_OK) return true;
    return false;
}

uint32_t outInit(uint32_t rate) {
    const audio_codec_if_t* c = dac();
    if (!c) return 0;
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = 1;
    fs.sample_rate     = rate;
    fs.mclk_multiple   = 256;
    if (c->set_fs)  c->set_fs(c, &fs);
    if (c->enable)  c->enable(c, true);
    if (c->set_vol) c->set_vol(c, 0.0f);
    l2proExpSet(EXP_PA_EN, true);
    return rate;
}

void outDeinit(void) {
    l2proExpSet(EXP_PA_EN, false);
    if (s_dac && s_dac->enable) s_dac->enable(s_dac, false);
}

uint32_t inInit(uint32_t rate) {
    if (!s_adc) {
        i2c_master_bus_handle_t bus = l2proBus();
        if (!bus) return 0;
        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address  = BOARD_ES7243E_ADDR;
        dev.scl_speed_hz    = BOARD_I2C_HZ;
        if (i2c_master_bus_add_device(bus, &dev, &s_adc) != ESP_OK) { s_adc = nullptr; return 0; }
    }
    for (const auto& w : kEs7243eInit) {
        if (!adcWrite(w[0], w[1])) {
            warn("audio: ES7243E write failed at reg 0x%02X\n", w[0]);
            return 0;
        }
    }
    return rate;
}

void inDeinit(void) {}

const audio_codec_ops_t s_ops = {
    .in_init    = inInit,
    .in_deinit  = inDeinit,
    .out_init   = outInit,
    .out_deinit = outDeinit,
};

}  // namespace

void L2proAudio::onInit() {
    audioRegisterCodec(&s_ops);
}
