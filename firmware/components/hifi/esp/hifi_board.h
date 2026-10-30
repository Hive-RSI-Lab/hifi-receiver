/*
 * The board this build is for, from the Kconfig board choice
 * (firmware/patches): Lite's lite-main, or the core board Plus and Ultra
 * share. Each header has the board's pins and parts; hifi_bsp.h is the code
 * each board gives.
 */
#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_HIFI_PLUS) || defined(CONFIG_HIFI_ULTRA)
#include "hifi_board_core.h"
#else
#include "hifi_board_lite.h"
#endif
