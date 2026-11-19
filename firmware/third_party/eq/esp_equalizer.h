/*
 * hifi-receiver's own 10-band equalizer, with the calls squeezelite-esp32's
 * equalizer.c makes of Espressif's prebuilt one (libesp_processing.a), which
 * the makers' image can't link (third_party/eq/README.md)
 *
 * Copyright (c) 2026 Hive-RSI-Lab. MIT licence (third_party/eq/README.md).
 */
#ifndef _ESP_EQUALIZER_H
#define _ESP_EQUALIZER_H

#ifdef __cplusplus
extern "C" {
#endif

/* a handle for nch interleaved 16-bit channels at g_rate Hz with n_band bands
 * (at most 10, centred 31 Hz to 16 kHz an octave apart); the last argument is
 * ignored. NULL when out of memory or the arguments are out of range */
void *esp_equalizer_init(int nch, int g_rate, int n_band, int use_xmms_original_freqs);
void esp_equalizer_uninit(void *handle);
/* filters length bytes of pcm_buf in place; 0, or -1 for a bad argument */
int esp_equalizer_process(void *handle, unsigned char *pcm_buf, int length, int g_rate, int nch);
/* band index's gain on channel nch, in dB (clamped to -20..+20) */
void esp_equalizer_set_band_value(void *handle, float value, int index, int nch);
int esp_equalizer_get_band_count(void *handle);
float esp_equalizer_get_band_value(void *handle, int index, int nch);

#ifdef __cplusplus
}
#endif

#endif
