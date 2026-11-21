#include "audio.h"
#include "config.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "audio";

static i2s_chan_handle_t s_spk = NULL;
static i2s_chan_handle_t s_mic = NULL;

// Lautstärke: Level 1-10 (Stufe 1 ist sehr leise)
static const float VOL_GAINS[11] = {
    0.0f,   // 0 (unused)
    0.02f,  // 1  (sehr leise, ~ -34 dB)
    0.05f,  // 2  (-26 dB)
    0.10f,  // 3  (-20 dB)
    0.18f,  // 4  (-15 dB)
    0.30f,  // 5  (-10.5 dB, Standard)
    0.50f,  // 6  (-6 dB)
    0.80f,  // 7  (-2 dB)
    1.20f,  // 8  (+1.6 dB)
    2.00f,  // 9  (+6 dB)
    3.20f,  // 10 (+10 dB)
};
static int   s_vol_level = 5;
static float s_vol_gain  = 0.30f;
static int32_t s_vol_gain_q12 = 1229; // s_vol_gain as Q12 fixed point for the sample loop

#if BOARD_AUDIO_ES8311
// ─── ES8311-Codec (Toy-AI-Core-C3-MINI) ──────────────────────────────────────
// Ein I2S-Bus im Vollduplex: Der Codec ist Slave, der ESP liefert MCLK (256 x fs),
// BCLK und LRCK. Übertragen wird Stereo/16 Bit; genutzt wird nur der linke Kanal.
static i2c_master_dev_handle_t s_codec = NULL;

static esp_err_t es8311_write(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_codec, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t es8311_setup(void) {
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_FAIL;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &s_codec);
    if (err != ESP_OK) return err;

    // Reset, dann Power-On im Slave-Modus
    err = es8311_write(0x00, 0x1F);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_write(0x00, 0x00);
    es8311_write(0x00, 0x80);

    // Takt: MCLK-Pin als Quelle, alle Takte an. Teiler für MCLK 4,096 MHz / 16 kHz.
    static const uint8_t regs[][2] = {
        {0x01, 0x3F},  // Clock Manager: MCLK vom Pin, alle Takte aktiv
        {0x02, 0x00},  // Vorteiler 1, Multiplikator 1
        {0x03, 0x10},  // ADC OSR
        {0x04, 0x10},  // DAC OSR
        {0x05, 0x00},  // ADC-/DAC-Taktteiler 1
        {0x06, 0x03},  // BCLK-Teiler (nur im Master-Modus relevant)
        {0x07, 0x00},  // LRCK-Teiler high
        {0x08, 0xFF},  // LRCK-Teiler low
        {0x09, 0x0C},  // Serial In (DAC):  I2S, 16 Bit
        {0x0A, 0x0C},  // Serial Out (ADC): I2S, 16 Bit
        {0x0D, 0x01},  // Analogteil einschalten
        {0x0E, 0x02},  // PGA und ADC-Modulator einschalten
        {0x12, 0x00},  // DAC einschalten
        {0x13, 0x10},  // Ausgangstreiber einschalten
        {0x1C, 0x6A},  // ADC: Equalizer aus, DC-Offset entfernen
        {0x37, 0x08},  // DAC: Equalizer aus
        {0x32, 0xBF},  // DAC-Lautstärke 0 dB (Lautstärke regelt die Software)
        {0x17, 0xC8},  // ADC-Lautstärke
        {0x14, 0x1A},  // Analoges Mikrofon an MIC1P/MIC1N, PGA an
        {0x16, ES8311_MIC_GAIN},  // Mikrofon-Verstärkung
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        err = es8311_write(regs[i][0], regs[i][1]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t audio_init(void) {
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &s_spk, &s_mic));

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_SPK),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PIN_I2S_MCLK,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_WS,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din  = (gpio_num_t)PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic));

    // Der Codec braucht den laufenden MCLK, bevor er konfiguriert wird
    esp_err_t err = es8311_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 nicht erreichbar (I2C 0x%02X): %s – kein Ton!",
                 ES8311_I2C_ADDR, esp_err_to_name(err));
    }

    // Verstärker (NS4150) einschalten
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PIN_PA_ENABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level((gpio_num_t)PIN_PA_ENABLE, 1);

    ESP_LOGI(TAG, "I2S bereit – ES8311 %d Hz, Mikrofon-Gain Stufe %d",
             AUDIO_SAMPLE_RATE_SPK, ES8311_MIC_GAIN);
    return err;
}

// Mono-Samples ausgeben: der Codec-Bus läuft in Stereo, also jedes Sample doppelt.
static void spk_write_raw(const int16_t *mono, size_t count) {
    int16_t stereo[256 * 2];
    size_t off = 0;
    while (off < count) {
        size_t n = (count - off < 256) ? (count - off) : 256;
        for (size_t i = 0; i < n; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = mono[off + i];
        }
        i2s_channel_write(s_spk, stereo, n * 2 * sizeof(int16_t), NULL, pdMS_TO_TICKS(500));
        off += n;
    }
}

#else
// ─── Getrennte I2S-Module (ESP32-S3: INMP441 + I2S-Verstärker) ───────────────
esp_err_t audio_init(void) {
    // ── Lautsprecher (TX, I2S0) ───────────────────────────────────────────────
    i2s_chan_config_t spk_ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    spk_ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&spk_ch, &s_spk, NULL));

    i2s_std_config_t spk_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_SPK),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_SPK_BCLK,
            .ws   = (gpio_num_t)PIN_SPK_LRCK,
            .dout = (gpio_num_t)PIN_SPK_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk, &spk_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk));

    // ── Mikrofon (RX, I2S1) ───────────────────────────────────────────────────
    i2s_chan_config_t mic_ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&mic_ch, NULL, &s_mic));

#if MIC_SLOT_BITS == 16
    #define MIC_BIT_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#else
    #define MIC_BIT_WIDTH I2S_DATA_BIT_WIDTH_32BIT
#endif

#if MIC_PHILIPS
    #define MIC_SLOT_CFG I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(MIC_BIT_WIDTH, I2S_SLOT_MODE_MONO)
#else
    #define MIC_SLOT_CFG I2S_STD_MSB_SLOT_DEFAULT_CONFIG(MIC_BIT_WIDTH, I2S_SLOT_MODE_MONO)
#endif

    i2s_std_config_t mic_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_MIC),
        .slot_cfg = MIC_SLOT_CFG,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_MIC_SCK,
            .ws   = (gpio_num_t)PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)PIN_MIC_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic, &mic_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic));

    ESP_LOGI(TAG, "I2S bereit – Mic %d Hz %d-Bit %s shift>>%d  Spk %d Hz",
             AUDIO_SAMPLE_RATE_MIC, MIC_SLOT_BITS,
             MIC_PHILIPS ? "Philips" : "MSB", MIC_BIT_SHIFT,
             AUDIO_SAMPLE_RATE_SPK);
    return ESP_OK;
}

static void spk_write_raw(const int16_t *mono, size_t count) {
    i2s_channel_write(s_spk, mono, count * sizeof(int16_t), NULL, pdMS_TO_TICKS(500));
}
#endif

// Sinuston auf dem Lautsprecher ausgeben.
// Rekursiver Oszillator (y[n] = k*y[n-1] - y[n-2]) statt sinf() pro Sample, kein malloc,
// 2 ms Ein-/Ausblenden gegen Knackser.
void audio_play_tone(float freq_hz, uint32_t duration_ms, float level) {
    const int sr = AUDIO_SAMPLE_RATE_SPK;
    const int n  = (sr * (int)duration_ms) / 1000;
    if (n <= 0) return;

    int fade = sr / 500;
    if (fade > n / 2) fade = n / 2;

    const float w   = 2.0f * (float)M_PI * freq_hz / (float)sr;
    const float k   = 2.0f * cosf(w);
    float amp = 8000.0f * s_vol_gain * level;
    if (amp > 30000.0f) amp = 30000.0f; // stay below int16 full scale
    float y1 = 0.0f;       // sin(0)
    float y2 = -sinf(w);   // sin(-w)

    int16_t buf[256];
    int done = 0;
    while (done < n) {
        int chunk = n - done;
        if (chunk > (int)(sizeof(buf) / sizeof(buf[0]))) chunk = sizeof(buf) / sizeof(buf[0]);
        for (int i = 0; i < chunk; i++) {
            const int idx = done + i;
            float env = amp;
            if (idx < fade)            env = amp * (float)idx / (float)fade;
            else if (idx >= n - fade)  env = amp * (float)(n - 1 - idx) / (float)fade;
            buf[i] = (int16_t)(env * y1);
            const float y0 = k * y1 - y2;
            y2 = y1;
            y1 = y0;
        }
        spk_write_raw(buf, chunk);
        done += chunk;
    }
}

void audio_spk_set_volume(int level) {
    if (level < 1) level = 1;
    if (level > 10) level = 10;
    s_vol_level = level;
    s_vol_gain  = VOL_GAINS[level];
    s_vol_gain_q12 = (int32_t)lroundf(s_vol_gain * 4096.0f);
    ESP_LOGI(TAG, "Lautstärke: %d  (gain=%.2f)", level, s_vol_gain);
}

int audio_spk_get_volume(void) { return s_vol_level; }

// Lautsprecher-Abtastrate ändern (I2S-Clock neu konfigurieren)
void audio_spk_set_rate(uint32_t rate_hz) {
    i2s_channel_disable(s_spk);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
    i2s_channel_reconfig_std_clock(s_spk, &clk);
    i2s_channel_enable(s_spk);
}

// PCM-Samples direkt auf den Lautsprecher schreiben (für Streaming-Playback).
// Wendet den Volume-Gain als Festkomma-Multiplikation an (kein Float pro Sample).
void audio_spk_write(const int16_t *samples, size_t count) {
    const size_t CHUNK = 256;
    int16_t tmp[CHUNK];
    const int32_t gain = s_vol_gain_q12;
    size_t off = 0;
    while (off < count) {
        size_t n = (count - off < CHUNK) ? (count - off) : CHUNK;
        for (size_t i = 0; i < n; i++) {
            int32_t v = ((int32_t)samples[off + i] * gain) >> 12;
            tmp[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
        }
        spk_write_raw(tmp, n);
        off += n;
    }
}

// Mikrofon-Samples lesen – liefert 16-Bit-PCM.
// Format und Bit-Shift werden über MIC_SLOT_BITS / MIC_BIT_SHIFT in config.h gesteuert.
size_t audio_mic_read(int16_t *out, size_t samples, uint32_t timeout_ms) {
    size_t got = 0;

#if BOARD_AUDIO_ES8311
    // Codec liefert Stereo/16 Bit; der ADC-Kanal liegt links (gerade Indizes).
    if (samples > 240) samples = 240;
    int16_t raw[240 * 2];

    size_t bytes_read = 0;
    i2s_channel_read(s_mic, raw, samples * 2 * sizeof(int16_t),
                     &bytes_read, pdMS_TO_TICKS(timeout_ms));
    got = (bytes_read / sizeof(int16_t)) / 2;
    for (size_t i = 0; i < got; i++) out[i] = raw[i * 2];

#elif MIC_SLOT_BITS == 16
    // Direkt 16-Bit lesen
    size_t bytes_read = 0;
    i2s_channel_read(s_mic, out, samples * sizeof(int16_t),
                     &bytes_read, pdMS_TO_TICKS(timeout_ms));
    got = bytes_read / sizeof(int16_t);
#if MIC_BIT_SHIFT != 0
    for (size_t i = 0; i < got; i++) out[i] = (int16_t)((int16_t)out[i] >> MIC_BIT_SHIFT);
#endif

#else
    // ESP32-S3 I2S Philips-MONO gibt L+R interleaved zurück (beide Slots).
    // INMP441 sitzt auf L-Kanal (gerade Indizes). Wir lesen 2× und nehmen nur L.
    if (samples > 240) samples = 240;
    int32_t raw[240 * 2];

    size_t bytes_read = 0;
    i2s_channel_read(s_mic, raw, samples * 2 * sizeof(int32_t),
                     &bytes_read, pdMS_TO_TICKS(timeout_ms));
    got = (bytes_read / sizeof(int32_t)) / 2;  // Stereo-Paare → Mono-Samples

    for (size_t i = 0; i < got; i++)
        out[i] = (int16_t)(raw[i * 2] >> MIC_BIT_SHIFT);  // nur L-Kanal (Index 0,2,4,…)

#ifdef MIC_DEBUG // Pegel-Log nur bei Bedarf: kostet sonst Zeit im Sendepfad
    static int dbg_ctr = 0;
    if (got > 0 && ++dbg_ctr % 50 == 0) {
        int16_t mx = 0;
        for (size_t i = 0; i < got; i++) {
            int16_t a = out[i] < 0 ? -out[i] : out[i];
            if (a > mx) mx = a;
        }
        ESP_LOGI("mic", "L=0x%08lX R=0x%08lX shift>>%d peak=%d",
                 (unsigned long)raw[0], (unsigned long)raw[1], MIC_BIT_SHIFT, (int)mx);
    }
#endif
#endif

    return got;
}
