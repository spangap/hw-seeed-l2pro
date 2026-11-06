# hw-seeed-l2pro board HAL — an ordinary spangap component (not a buildable).
#
# Compiles the Seeed Wio Tracker L2 Pro board code: the board I2C bus, the IO
# expander, rails and resets, and the battery ADC (l2pro.cpp). The conditional/
# slices compile only when their straddle is staged: the backlight driver and
# the two keys (spangap-lcd), the audio codecs (audio) and the GNSS supply
# (gps). The panel, touch, radio, SD card and I2S engine are the generic
# straddles' — this board only supplies their pins, in straddle.yaml.
#
# app_main is NOT here — it's generated into the reticulous/reticulous buildable
# that adds this board with `--with`. This component just exports the board's
# Service classes that the generated dispatcher constructs and walks.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/l2pro.cpp"
        # This board's one self-assertion (detect_hw), called by
        # spangapInit() before any bus is claimed.
        "src/detect.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_timer
)
