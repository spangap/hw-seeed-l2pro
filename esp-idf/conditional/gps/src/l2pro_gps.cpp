/**
 * l2pro_gps.cpp — the L76K's supply, for spangap/gps. Compiled only when gps is
 * staged.
 *
 * gps declares gpsBoardPower() weak and calls it where a board owns the
 * receiver's supply. Here that switch is an expander line (EXP_GNSS_PWR), not
 * a pin, which is the case CONFIG_GPS_POWER_PIN cannot express.
 */
#include "l2pro.h"

extern "C" bool gpsBoardPower(bool on)
{
    return l2proExpSet(EXP_GNSS_PWR, on);
}
