#pragma once

#include "sdkconfig.h"

/*
 * ESP32-C6 retains digital output state through deep sleep with the per-pin
 * gpio_hold_en()/gpio_hold_dis() API. ESP-IDF 5.5 does not expose the older
 * global deep-sleep hold helpers for this target, so keep the existing calls
 * source-compatible while making them deliberate no-ops on C6.
 */
#if CONFIG_IDF_TARGET_ESP32C6
#define gpio_deep_sleep_hold_en()  ((void)0)
#define gpio_deep_sleep_hold_dis() ((void)0)
#endif

/* ESP Zigbee SDK 2.0.x naming used by the managed 2.0.3 component. */
#ifndef EZB_ZCL_ATTR_BASIC_SW_BUILD_ID
#define EZB_ZCL_ATTR_BASIC_SW_BUILD_ID EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID
#endif

#ifndef EZB_ZCL_ATTR_TYPE_BITMAP16
#define EZB_ZCL_ATTR_TYPE_BITMAP16 EZB_ZCL_ATTR_TYPE_MAP16
#endif
