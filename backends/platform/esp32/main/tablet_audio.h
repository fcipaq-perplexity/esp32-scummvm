/*
 * Audio output of the tablet boards: I2S, 16 bit stereo, into the NS4168 speaker and headphone
 * amplifiers (they share the I2S lines, one enable pin each). Volume is applied in software.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP_ERR_NOT_SUPPORTED if the board has no audio output */
esp_err_t tablet_audio_init(int sample_rate);
bool tablet_audio_available(void);

/* Writes interleaved 16 bit stereo samples, blocks until the I2S driver takes them. */
esp_err_t tablet_audio_write(void *buf, size_t bytes);

void tablet_audio_set_volume(int percent);
void tablet_audio_set_headphones(bool headphones);
bool tablet_audio_headphones(void);

/* Mutes and switches both amplifiers off (before power off) */
void tablet_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
