/**
 * l2pro_lcd.h — the Wio Tracker L2 Pro's on-device display glue, as a Service.
 *
 * L2proLcdInput::onStart (start band, before lcdInit()) brings up the LP5814
 * backlight driver and hands it to spangap-lcd, and registers the input HAL:
 * the Home button and the side wake key. when:-gated on spangap-lcd via the
 * straddle.yaml `services:` entry, so this slice compiles only in an LCD build.
 */
#pragma once

#include "service.h"

class L2proLcdInput : public Service {
public:
    void onStart() override;
};
