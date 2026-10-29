/*
 * lite-main's pins and parts, from boards/lite/README.md (the board's own
 * pin map; agreed with its designer). Squeezelite-esp32 owns the codec's
 * I2S, the buttons, IR and the LED through the Kconfig defaults in
 * firmware/patches; this component owns the power path and the amp.
 */
#pragma once

#define HIFI_MODEL "hifi-lite"
#define HIFI_BOARD "lite-main"
#define HIFI_FW_VERSION "0.1.0"
#define HIFI_UPSTREAM "squeezelite-esp32 master-v4.3 ee1720c"

#define PIN_PD_PG 4      /* CH224A PG, open drain, low = contract good */
#define PIN_PVDD_EN 18   /* high closes the PVDD switch (ANDed with PG in hardware) */
#define PIN_AMP_SDZ 21   /* TPA3118 SDZ through 100k, high = run */
#define PIN_AMP_FAULTZ 23 /* TPA3118 FAULTZ, open drain, low = fault */
#define ADC_VBUS ADC1_CHANNEL_6 /* GPIO34, VBUS / 11 (100k over 10k) */
#define VBUS_DIVIDER 11.0f

#define I2C_PORT 0       /* shared with squeezelite's codec driver */
#define I2C_SDA 33
#define I2C_SCL 32
#define ES8388_ADDR 0x10

/* ES8388 registers this component touches (datasheet rev 5.0) */
#define ES8388_ADCPOWER 0x03 /* bit7/6 power down the analog inputs */
#define ES8388_DACPOWER 0x04 /* bits 5..2: LOUT1 ROUT1 LOUT2 ROUT2 on */
#define ES8388_DACCONTROL17 0x27 /* left mixer: bit7 DAC, bit6 LIN, bits5:3 LIN gain */
#define ES8388_DACCONTROL20 0x2A /* right mixer, same layout */
#define ES8388_LOUT2VOL 0x30 /* sub out L: 0 = -45 dB, 30 = 0 dB, 33 = +4.5 dB */
#define ES8388_ROUT2VOL 0x31

#define TONE_RATE 48000
