#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t audio_init(void);
// level: extra factor on top of the volume setting (e.g. louder ring tone), clipped to full scale
void      audio_play_tone(float freq_hz, uint32_t duration_ms, float level = 1.0f);
size_t    audio_mic_read(int16_t *out, size_t samples, uint32_t timeout_ms);
void      audio_spk_write(const int16_t *samples, size_t count);
// Lautsprecher-Abtastrate temporär ändern (z.B. für Mikrofon-Loopback)
void      audio_spk_set_rate(uint32_t rate_hz);
// Lautstärke: 1 (leise) … 10 (laut), Standard 5 = 0 dB
void      audio_spk_set_volume(int level);
int       audio_spk_get_volume(void);
