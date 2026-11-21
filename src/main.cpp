#include "config.h"
#include "audio.h"
#include "board.h"
#include "tilt_trigger.h"
#include "ring_gesture.h"
#include "volume_command.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <math.h>
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "mdns.h"

static const char *TAG = "main";

static uint32_t compute_lmk_signature(const uint8_t *lmk) {
    uint32_t hash = 2166136261U;
    for (int i = 0; i < 16; i++) {
        hash ^= lmk[i];
        hash *= 16777619U;
    }
    return hash;
}

static void encrypt_aes128_ecb(const uint8_t *key, const uint8_t *input, uint8_t *output) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    for (int i = 0; i < 15; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input + i * 16, output + i * 16);
    }
    mbedtls_aes_free(&aes);
}

static void decrypt_aes128_ecb(const uint8_t *key, const uint8_t *input, uint8_t *output) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    for (int i = 0; i < 15; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input + i * 16, output + i * 16);
    }
    mbedtls_aes_free(&aes);
}

// ─── ESP-NOW Packet Layout ──────────────────────────────────────────────────
// byte 0 = type: 0x01 Audio, 0x02 Pairing Request, 0x03 Pairing Ack, 0x04 Ring
// Audio:   [type][240 x u-law]                    (unpaired, 241 bytes)
//          [type][4 x key signature][240 x AES]   (paired, 245 bytes)
// Request: [type]["PAIR"][has_lmk][16 x LMK]      (22 bytes)
// Ack:     [type]["PAIR"][16 x LMK]               (21 bytes)
// Ring:    [type]["RING"][4 x key signature]      (9 bytes, signature 0 = unpaired)

// ─── Global State ────────────────────────────────────────────────────────────
static RingbufHandle_t s_audio_ringbuf = NULL;
static uint8_t s_peer_lmk[16] = {0};
static bool s_has_paired_peer = false;

static volatile bool s_pairing_mode = false;
static volatile int s_pairing_timer = 0; // In 50ms intervals
// Set by the ESP-NOW callback (WiFi task); NVS write + tones happen in control_task.
static volatile bool s_pairing_done_pending = false;
// True while tx_task is really transmitting (half-duplex mute for the receiver).
static volatile bool s_tx_active = false;
static uint8_t s_own_mac[6] = {0};
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#if BOARD_SINGLE_BUTTON
// Set by button_task once the button is held long enough to count as "talk"
static volatile bool s_ptt_held = false;
static inline bool ptt_is_pressed(void) { return s_ptt_held; }
#else
static inline bool ptt_is_pressed(void) { return gpio_get_level(PIN_PTT) == 0; }
#endif

#define AUDIO_FRAME_SAMPLES 240          // 15 ms @ 16 kHz per ESP-NOW packet
#define PLAY_PREBUFFER_MS   30           // jitter buffer filled at start of a stream
#define PLAY_UNDERRUN_MS    40           // gap after which a stream counts as ended

// ─── G.711 u-law Encoder/Decoder ─────────────────────────────────────────────
#define BIAS 0x84
#define CLIP 32635

static int16_t s_ulaw_decode_table[256];

static void ulaw_init_table(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t u_val = (uint8_t)~i;
        int exponent = (u_val >> 4) & 0x07;
        int mantissa = u_val & 0x0F;
        int sample = (((mantissa << 3) + BIAS) << exponent) - BIAS;
        s_ulaw_decode_table[i] = (int16_t)((u_val & 0x80) ? -sample : sample);
    }
}

static inline uint8_t linear2ulaw(int16_t pcm) {
    int32_t number = pcm; // 32 bit: -32768 must not overflow on negation
    int sign = 0;
    if (number < 0) {
        sign = 0x80;
        number = -number;
    }
    if (number > CLIP) {
        number = CLIP;
    }
    number += BIAS; // 132 .. 32767 -> highest set bit is bit 7 .. 14
    int exponent = 24 - __builtin_clz((uint32_t)number);
    int mantissa = (number >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

// ─── ADXL345 Driver & Logic ──────────────────────────────────────────────────
static i2c_master_dev_handle_t s_adxl_dev = NULL;
static volatile bool s_tilt_active = false;
// Ring (Klingeln): Kurbeln an einer Dose laesst alle anderen klingeln, die auf dem Kopf stehen.
static volatile bool s_upside_down = false;
static volatile bool s_ring_send_request = false;  // set by adxl_task / console
static volatile bool s_ring_rx_pending = false;    // set by the ESP-NOW callback
static volatile uint32_t s_last_ring_rx_time = 0;
// While the ring tone plays (and shortly after) the speaker shakes the can: no crank detection
static volatile bool s_ring_playing = false;
static volatile uint32_t s_ring_end_time = 0;
#define RING_CRANK_MUTE_MS  1000
#define RING_REPEAT_COUNT   3                      // ring packet is sent a few times
#ifndef RING_RX_COOLDOWN_MS
#define RING_RX_COOLDOWN_MS 10000                  // receiver: no new ring this long after one started
#endif
#ifndef RING_TONE_LEVEL
#define RING_TONE_LEVEL     3.0f                   // ring tone level relative to the other tones
#endif
static volatile uint32_t s_last_rx_audio_time = 0;

typedef enum {
    PTT_MODE_TILT = 0,
    PTT_MODE_VOX = 1
} ptt_mode_t;

#define DEFAULT_PTT_MODE (DEFAULT_PTT_MODE_VOX ? PTT_MODE_VOX : PTT_MODE_TILT)
static volatile ptt_mode_t s_ptt_mode = DEFAULT_PTT_MODE;

static const float s_tilt_thresholds[11] = {
    140.0f, // Level 0 (not used)
    120.0f, // Level 1: least sensitive
    100.0f, // Level 2
    80.0f,  // Level 3
    65.0f,  // Level 4
    50.0f,  // Level 5: default
    40.0f,  // Level 6
    30.0f,  // Level 7
    20.0f,  // Level 8
    12.0f,  // Level 9
    6.0f    // Level 10: most sensitive
};

static const int s_vox_thresholds[11] = {
    1800,  // Level 0 (not used)
    1800,  // Level 1: least sensitive
    1500,  // Level 2
    1200,  // Level 3
    900,   // Level 4
    750,   // Level 5
    600,   // Level 6: default
    450,   // Level 7
    300,   // Level 8
    200,   // Level 9
    100    // Level 10: most sensitive
};

static volatile int s_vox_sens_level = DEFAULT_VOX_SENS;
// Kept for backward-compatible NVS/settings controls; the TILT window is fixed.
static volatile int s_tilt_sens_level = 5;
static float s_adxl_x = 0.0f;
static float s_adxl_y = 0.0f;
static float s_adxl_z = 256.0f; // Initialized to flat gravity vector (approx 256 LSB at +/-2g)

static float s_adxl_offset_x = 0.0f;
static float s_adxl_offset_y = 0.0f;
static float s_adxl_offset_z = 256.0f; // Default resting Z
static TiltTrigger s_tilt_trigger;
static esp_err_t save_adxl_offsets(float ox, float oy, float oz);
static volatile bool s_tilt_calibration_requested = false;
static int s_tilt_calibration_samples = 0;
static float s_tilt_calibration_sum_x = 0.0f;
static float s_tilt_calibration_sum_y = 0.0f;
static float s_tilt_calibration_sum_z = 0.0f;

static void request_tilt_calibration(void) {
#if !TILT_ENABLE
    ESP_LOGW("tilt", "TILT ist in dieser Firmware deaktiviert (TILT_ENABLE=0 in platformio.ini).");
    return;
#endif
    s_tilt_calibration_requested = true;
    ESP_LOGI("tilt", "Kalibriere Dosenachse: Dose 1 Sekunde aufrecht auf den Boden stellen.");
}

// Addresses to look for the sensor at: the one fixed in platformio.ini, or both
// addresses the ADXL345 can have (SDO on GND = 0x53, SDO on VCC = 0x1D).
#ifdef ADXL_I2C_ADDR
[[maybe_unused]] static const uint8_t s_adxl_addresses[] = {ADXL_I2C_ADDR};
#else
[[maybe_unused]] static const uint8_t s_adxl_addresses[] = {0x53, 0x1D};
#endif

[[maybe_unused]] static bool adxl345_init(void) {
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) return false;

    if (s_adxl_dev != NULL) {
        i2c_master_bus_rm_device(s_adxl_dev);
        s_adxl_dev = NULL;
    }

    for (size_t i = 0; i < sizeof(s_adxl_addresses) / sizeof(s_adxl_addresses[0]); i++) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_adxl_addresses[i],
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &dev_config, &s_adxl_dev) != ESP_OK) {
            s_adxl_dev = NULL;
            continue;
        }

        // Check connection by reading DEVID (register 0x00)
        uint8_t devid_reg = 0x00;
        uint8_t devid = 0;
        esp_err_t err = i2c_master_transmit_receive(s_adxl_dev, &devid_reg, 1, &devid, 1, pdMS_TO_TICKS(100));
        if (err == ESP_OK && devid == 0xE5) {
            // Configure POWER_CTL 0x2D = 0x08 (Measure mode)
            uint8_t pwr_ctrl[2] = {0x2D, 0x08};
            i2c_master_transmit(s_adxl_dev, pwr_ctrl, 2, pdMS_TO_TICKS(100));

            // Configure DATA_FORMAT register 0x31 to 0x08 (Full Resolution, +/-2g)
            uint8_t data_fmt[2] = {0x31, 0x08};
            i2c_master_transmit(s_adxl_dev, data_fmt, 2, pdMS_TO_TICKS(100));

            ESP_LOGI("adxl", "ADXL345 gefunden an I2C-Adresse 0x%02X (DEVID 0x%02X)",
                     s_adxl_addresses[i], devid);
            return true;
        }

        ESP_LOGD("adxl", "Kein ADXL345 an 0x%02X (err=%s, devid=0x%02X)",
                 s_adxl_addresses[i], esp_err_to_name(err), devid);
        i2c_master_bus_rm_device(s_adxl_dev);
        s_adxl_dev = NULL;
    }
    return false;
}

[[maybe_unused]] static bool adxl345_read(int16_t *x, int16_t *y, int16_t *z) {
    if (!s_adxl_dev) return false;

    uint8_t reg = 0x32; // X0 data register
    uint8_t buf[6];
    esp_err_t err = i2c_master_transmit_receive(s_adxl_dev, &reg, 1, buf, 6, pdMS_TO_TICKS(50));
    if (err != ESP_OK) return false;

    *x = (int16_t)(buf[0] | (buf[1] << 8));
    *y = (int16_t)(buf[2] | (buf[3] << 8));
    *z = (int16_t)(buf[4] | (buf[5] << 8));
    return true;
}

[[maybe_unused]] static void adxl_filter_update(float raw_x, float raw_y, float raw_z) {
    const float alpha = 0.30f; // Fast response while still suppressing jitter
    s_adxl_x = s_adxl_x * (1.0f - alpha) + raw_x * alpha;
    s_adxl_y = s_adxl_y * (1.0f - alpha) + raw_y * alpha;
    s_adxl_z = s_adxl_z * (1.0f - alpha) + raw_z * alpha;
}

[[maybe_unused]] static void adxl_task(void *) {
    int16_t raw_x, raw_y, raw_z;
    while (1) {
        if (!s_adxl_dev) {
            if (!adxl345_init()) {
                ESP_LOGE("adxl", "ADXL345 nicht gefunden (Adresse%s). Verkabelung pruefen: SDA=GPIO%d, SCL=GPIO%d! Retry in 2s...",
#ifdef ADXL_I2C_ADDR
                         " fest eingestellt",
#else
                         "n 0x53 und 0x1D",
#endif
                         (int)PIN_I2C_SDA, (int)PIN_I2C_SCL);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        if (adxl345_read(&raw_x, &raw_y, &raw_z)) {
            adxl_filter_update((float)raw_x, (float)raw_y, (float)raw_z);

            const GravityVector gravity = {s_adxl_x, s_adxl_y, s_adxl_z};
            if (s_tilt_calibration_requested) {
                s_tilt_calibration_sum_x += gravity.x;
                s_tilt_calibration_sum_y += gravity.y;
                s_tilt_calibration_sum_z += gravity.z;
                if (++s_tilt_calibration_samples >= 50) {
                    s_adxl_offset_x = s_tilt_calibration_sum_x / s_tilt_calibration_samples;
                    s_adxl_offset_y = s_tilt_calibration_sum_y / s_tilt_calibration_samples;
                    s_adxl_offset_z = s_tilt_calibration_sum_z / s_tilt_calibration_samples;
                    s_tilt_trigger.calibrate_axis({s_adxl_offset_x, s_adxl_offset_y, s_adxl_offset_z});
                    save_adxl_offsets(s_adxl_offset_x, s_adxl_offset_y, s_adxl_offset_z);
                    s_tilt_calibration_requested = false;
                    s_tilt_calibration_samples = 0;
                    s_tilt_calibration_sum_x = s_tilt_calibration_sum_y = s_tilt_calibration_sum_z = 0.0f;
                    ESP_LOGI("tilt", "Dosenachse gespeichert: X=%.1f Y=%.1f Z=%.1f", s_adxl_offset_x, s_adxl_offset_y, s_adxl_offset_z);
                    audio_play_tone(400.0f, 100); vTaskDelay(pdMS_TO_TICKS(50));
                    audio_play_tone(700.0f, 100); vTaskDelay(pdMS_TO_TICKS(50));
                    audio_play_tone(1100.0f, 150);
                }
            }
            const uint32_t now_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
            s_tilt_active = s_tilt_trigger.update(gravity, now_ms);

            static UpsideDownDetector upside_down;
            s_upside_down = s_tilt_trigger.calibrated() &&
                            upside_down.update(s_tilt_trigger.tilt_degrees(gravity),
                                               TiltTrigger::moving(gravity), now_ms);

            static CrankDetector crank;
            const bool ring_vibration = s_ring_playing ||
                (s_ring_end_time != 0 &&
                 (xTaskGetTickCount() - s_ring_end_time) < pdMS_TO_TICKS(RING_CRANK_MUTE_MS));
            if (ring_vibration) {
                crank.reset();
            } else if (crank.update({(float)raw_x, (float)raw_y, (float)raw_z}, now_ms)) {
                ESP_LOGI("ring", "Kurbeln erkannt: andere Dosen klingeln lassen");
                s_ring_send_request = true;
            }

            // Debug only (compiled out at INFO level): once per second
            static int log_counter = 0;
            if (++log_counter >= 50) {
                log_counter = 0;
                ESP_LOGD("adxl_debug", "Raw X:%4d Y:%4d Z:%4d | Filt X:%6.1f Y:%6.1f Z:%6.1f | Tilt:%5.1f | Act:%d",
                         raw_x, raw_y, raw_z, s_adxl_x, s_adxl_y, s_adxl_z,
                         s_tilt_trigger.tilt_degrees(gravity), s_tilt_active);
            }
        } else {
            ESP_LOGW("adxl", "Lesefehler. Verbindung verloren? Re-initialisiere...");
            if (s_adxl_dev) {
                i2c_master_bus_rm_device(s_adxl_dev);
                s_adxl_dev = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz: responsive tilt detection
    }
}

// ─── NVS Storage Helper Functions ────────────────────────────────────────────
static esp_err_t load_peer_config(uint8_t *lmk) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;
    
    size_t lmk_len = 16;
    err = nvs_get_blob(my_handle, "peer_lmk", lmk, &lmk_len);
    nvs_close(my_handle);
    return err;
}

static esp_err_t save_peer_config(const uint8_t *lmk) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_blob(my_handle, "peer_lmk", lmk, 16);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return err;
}

static esp_err_t delete_peer_config(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    
    nvs_erase_key(my_handle, "peer_lmk");
    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

static esp_err_t load_volume_level(int *level) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;
    int32_t val = 5;
    err = nvs_get_i32(my_handle, "vol_level", &val);
    *level = (int)val;
    nvs_close(my_handle);
    return err;
}

static esp_err_t save_volume_level(int level) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(my_handle, "vol_level", (int32_t)level);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return err;
}

static esp_err_t load_ptt_mode(ptt_mode_t *mode) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;
    int32_t val = 0;
    err = nvs_get_i32(my_handle, "ptt_mode", &val);
    *mode = (ptt_mode_t)val;
    nvs_close(my_handle);
    return err;
}

static esp_err_t save_ptt_mode(ptt_mode_t mode) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(my_handle, "ptt_mode", (int32_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return err;
}

static esp_err_t load_sens_levels(int *vox, int *tilt) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;
    int32_t v_val = 6;
    int32_t t_val = 5;
    nvs_get_i32(my_handle, "vox_sens", &v_val);
    nvs_get_i32(my_handle, "tilt_sens", &t_val);
    // Levels index the threshold tables: never trust stored values blindly
    *vox = (v_val < 1) ? 1 : (v_val > 10) ? 10 : (int)v_val;
    *tilt = (t_val < 1) ? 1 : (t_val > 10) ? 10 : (int)t_val;
    nvs_close(my_handle);
    return err;
}

static esp_err_t save_sens_levels(int vox, int tilt) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    nvs_set_i32(my_handle, "vox_sens", (int32_t)vox);
    nvs_set_i32(my_handle, "tilt_sens", (int32_t)tilt);
    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

static esp_err_t load_adxl_offsets(float *ox, float *oy, float *oz) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;
    int32_t ix = 0, iy = 0, iz = 256;
    nvs_get_i32(my_handle, "adxl_ox", &ix);
    nvs_get_i32(my_handle, "adxl_oy", &iy);
    nvs_get_i32(my_handle, "adxl_oz", &iz);
    *ox = (float)ix;
    *oy = (float)iy;
    *oz = (float)iz;
    nvs_close(my_handle);
    return err;
}

static bool load_dose_axis_is_calibrated(void) {
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) != ESP_OK) return false;
    uint8_t version = 0;
    esp_err_t err = nvs_get_u8(my_handle, "tilt_axis_v2", &version);
    nvs_close(my_handle);
    return err == ESP_OK && version == 1;
}

static esp_err_t save_adxl_offsets(float ox, float oy, float oz) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;
    nvs_set_i32(my_handle, "adxl_ox", (int32_t)ox);
    nvs_set_i32(my_handle, "adxl_oy", (int32_t)oy);
    nvs_set_i32(my_handle, "adxl_oz", (int32_t)oz);
    nvs_set_u8(my_handle, "tilt_axis_v2", 1);
    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

// ─── ESP-NOW Helpers ──────────────────────────────────────────────────────────
static esp_err_t add_espnow_peer(const uint8_t *mac, bool encrypt, const uint8_t *lmk) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0; // 0 = current home channel (13 standalone, AP channel in OTA mode)
    peer.encrypt = encrypt;
    peer.ifidx = WIFI_IF_STA;
    if (encrypt && lmk) {
        memcpy(peer.lmk, lmk, 16);
    }
    
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fehler beim Hinzufuegen des Peers " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Peer " MACSTR " erfolgreich hinzugefuegt (encrypt=%d)", MAC2STR(mac), encrypt);
    }
    return ret;
}

// ─── ESP-NOW Callback ────────────────────────────────────────────────────────
// Runs in the WiFi task: must never block (no NVS, no tones, no delays).
static void espnow_pairing_finish(const uint8_t *lmk) {
    memcpy(s_peer_lmk, lmk, 16);
    s_has_paired_peer = true;
    s_pairing_mode = false;
    s_pairing_done_pending = true;
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t type = data[0];

    if (type == 0x01) {
        // Audio Packet
        if (!s_audio_ringbuf) return;
        // Half-duplex: do not play while we are really transmitting. A pressed PTT or
        // tilt that is still waiting for a free channel must not mute the other side.
        if (s_tx_active) return;

        if (s_has_paired_peer) {
            // Encrypted audio packet expected: len == 245
            if (len != 245) return;
            uint32_t received_sig;
            memcpy(&received_sig, data + 1, 4);
            if (received_sig != compute_lmk_signature(s_peer_lmk)) return;

            uint8_t dec_buf[AUDIO_FRAME_SAMPLES];
            decrypt_aes128_ecb(s_peer_lmk, data + 5, dec_buf);
            s_last_rx_audio_time = xTaskGetTickCount();
            xRingbufferSend(s_audio_ringbuf, dec_buf, AUDIO_FRAME_SAMPLES, 0);
        } else {
            // Unencrypted audio packet expected: len == 241
            if (len != 241) return;
            s_last_rx_audio_time = xTaskGetTickCount();
            xRingbufferSend(s_audio_ringbuf, data + 1, AUDIO_FRAME_SAMPLES, 0);
        }
    }
    else if (type == 0x04) {
        // Ring: only from cans with the same key (or both unpaired)
        if (len != 9 || memcmp(data + 1, "RING", 4) != 0) return;
        uint32_t received_sig;
        memcpy(&received_sig, data + 5, 4);
        uint32_t expected_sig = s_has_paired_peer ? compute_lmk_signature(s_peer_lmk) : 0;
        if (received_sig != expected_sig) return;
        // Ignore rings while one is pending/playing and for the cooldown after it started
        if (s_ring_rx_pending || s_ring_playing) return;
        uint32_t now = xTaskGetTickCount();
        if (s_last_ring_rx_time != 0 && (now - s_last_ring_rx_time) < pdMS_TO_TICKS(RING_RX_COOLDOWN_MS)) return;
        s_last_ring_rx_time = now;
        s_ring_rx_pending = true;
    }
    else if (type == 0x02) {
        // Pairing Request (broadcast). Both sides broadcast requests while pairing;
        // only the device with the higher MAC answers, otherwise both could answer
        // at the same moment and end up with two different keys.
        if (s_pairing_mode && len >= 5 && memcmp(data + 1, "PAIR", 4) == 0 &&
            memcmp(s_own_mac, recv_info->src_addr, 6) > 0) {
            uint8_t lmk[16];
            bool sender_has_lmk = (len >= 22) && (data[5] == 1);

            if (sender_has_lmk) {
                memcpy(lmk, data + 6, 16);       // adopt the sender's key
            } else if (s_has_paired_peer) {
                memcpy(lmk, s_peer_lmk, 16);     // pass on our own key
            } else {
                esp_fill_random(lmk, 16);        // fresh key
            }

            uint8_t ack_pkg[21];
            ack_pkg[0] = 0x03;
            memcpy(ack_pkg + 1, "PAIR", 4);
            memcpy(ack_pkg + 5, lmk, 16);
            // The ack is an unacknowledged broadcast: repeat it so a single lost
            // frame does not leave the two devices with different keys.
            for (int i = 0; i < 3; i++) {
                esp_now_send(s_broadcast_mac, ack_pkg, sizeof(ack_pkg));
            }
            espnow_pairing_finish(lmk);
        }
    }
    else if (type == 0x03) {
        // Pairing Ack
        if (s_pairing_mode && len >= 21 && memcmp(data + 1, "PAIR", 4) == 0) {
            espnow_pairing_finish(data + 5);
        }
    }
}

// ─── OTA & WiFi STA Setup ───────────────────────────────────────────────────
static bool s_ota_in_progress = false;
static httpd_handle_t s_ota_http_server = NULL;

static bool check_http_basic_auth(httpd_req_t *req) {
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        if (strncmp(auth_hdr, "Basic ", 6) == 0) {
            char *encoded = auth_hdr + 6;
            unsigned char decoded[128] = {0};
            size_t out_len = 0;
            mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len, (const unsigned char *)encoded, strlen(encoded));
            decoded[out_len] = '\0';
            char *colon = strchr((char *)decoded, ':');
            if (colon && strcmp(colon + 1, OTA_HTTP_PASSWORD) == 0) {
                return true;
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Canfunk OTA\"");
    httpd_resp_sendstr(req, "401 Unauthorized: Passwort erforderlich (Passwort: " OTA_HTTP_PASSWORD ")");
    return false;
}

static esp_err_t ota_index_html_handler(httpd_req_t *req) {
    if (!check_http_basic_auth(req)) {
        return ESP_OK;
    }
    const char *html =
        "<!DOCTYPE html>"
        "<html><head><meta charset='utf-8'><title>Canfunk OTA Update</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body{font-family:sans-serif;background:#121212;color:#fff;text-align:center;padding:40px 20px;}"
        ".card{background:#1e1e1e;max-width:480px;margin:0 auto;padding:30px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,0.5);}"
        "h1{color:#00e676;margin-bottom:10px;}"
        "p{color:#aaa;}"
        "input[type=file]{margin:20px 0;background:#2a2a2a;padding:10px;border-radius:6px;color:#fff;width:100%;box-sizing:border-box;}"
        "button{background:#00e676;color:#000;font-weight:bold;border:none;padding:12px 24px;font-size:16px;border-radius:6px;cursor:pointer;width:100%;}"
        "button:hover{background:#00c853;}"
        ".progress{width:100%;background:#333;height:20px;border-radius:10px;overflow:hidden;margin-top:20px;display:none;}"
        ".bar{width:0%;height:100%;background:#00e676;transition:width 0.2s;}"
        "#status{margin-top:15px;font-weight:bold;color:#00e676;}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>Canfunk OTA Update</h1>"
        "<p>Waehle die neue <b>firmware.bin</b> Datei aus.</p>"
        "<form id='uploadForm'>"
        "<input type='file' id='firmwareFile' accept='.bin' required>"
        "<button type='submit' id='subBtn'>Firmware Flashen</button>"
        "</form>"
        "<div class='progress' id='prg'><div class='bar' id='bar'></div></div>"
        "<div id='status'></div>"
        "</div>"
        "<script>"
        "document.getElementById('uploadForm').onsubmit = function(e) {"
        "  e.preventDefault();"
        "  var fileInput = document.getElementById('firmwareFile');"
        "  if (!fileInput.files.length) return;"
        "  var file = fileInput.files[0];"
        "  var xhr = new XMLHttpRequest();"
        "  xhr.open('POST', '/update', true);"
        "  xhr.upload.onprogress = function(e) {"
        "    if (e.lengthComputable) {"
        "      var pct = Math.round((e.loaded / e.total) * 100);"
        "      document.getElementById('prg').style.display = 'block';"
        "      document.getElementById('bar').style.width = pct + '%';"
        "      document.getElementById('status').innerText = 'Flashe: ' + pct + '%';"
        "    }"
        "  };"
        "  xhr.onload = function() {"
        "    if (xhr.status == 200) {"
        "      document.getElementById('status').innerText = 'Erfolg! Modul startet neu...';"
        "      setTimeout(function(){ location.reload(); }, 5000);"
        "    } else {"
        "      document.getElementById('status').innerText = 'Fehler beim Flashen!';"
        "    }"
        "  };"
        "  document.getElementById('subBtn').disabled = true;"
        "  xhr.send(file);"
        "};"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

static esp_err_t ota_update_post_handler(httpd_req_t *req) {
    if (!check_http_basic_auth(req)) {
        return ESP_OK;
    }
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE("ota", "Keine OTA Update-Partition gefunden!");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition available");
        return ESP_FAIL;
    }

    ESP_LOGI("ota", "Starte OTA Update auf Partition: %s", update_partition->label);
    s_ota_in_progress = true;

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE("ota", "esp_ota_begin fehlgeschlagen: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Begin failed");
        s_ota_in_progress = false;
        return ESP_FAIL;
    }

    static char buf[4096]; // static: keeps the httpd stack small, one upload at a time
    int received;
    int remaining = req->content_len;

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE("ota", "Fehler beim Empfang von Daten!");
            esp_ota_abort(ota_handle);
            s_ota_in_progress = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE("ota", "esp_ota_write fehlgeschlagen: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            s_ota_in_progress = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE("ota", "esp_ota_end fehlgeschlagen: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA End failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE("ota", "esp_ota_set_boot_partition fehlgeschlagen: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Set Boot Partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI("ota", "OTA Update ERFOLGREICH! Modul startet in 1s neu...");
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_volume_post_handler(httpd_req_t *req) {
    if (!check_http_basic_auth(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 16) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Volume must be 1-10");
        return ESP_FAIL;
    }

    char body[17] = {0};
    int received = httpd_req_recv(req, body, req->content_len);
    if (received != req->content_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete volume request");
        return ESP_FAIL;
    }

    int volume = 0;
    if (!parse_volume_command(body, (size_t)received, &volume)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Volume must be 1-10");
        return ESP_FAIL;
    }

    audio_spk_set_volume(volume);
    if (save_volume_level(volume) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save volume");
        return ESP_FAIL;
    }
    ESP_LOGI("ota", "Lautstaerke per OTA gesetzt: %d", volume);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static httpd_handle_t start_ota_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = ota_index_html_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t update_uri = {
            .uri      = "/update",
            .method   = HTTP_POST,
            .handler  = ota_update_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &update_uri);

        httpd_uri_t volume_uri = {
            .uri      = "/volume",
            .method   = HTTP_POST,
            .handler  = ota_volume_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &volume_uri);

        ESP_LOGI("ota", "OTA HTTP Web-Server gestartet auf Port 80");
    }
    return server;
}

static void start_wifi_ota_window(void) {
    ESP_LOGI("ota", "=================================================");
    ESP_LOGI("ota", "Suche 5s nach WLAN Hotspot '" OTA_WIFI_SSID "'...");
    ESP_LOGI("ota", "=================================================");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, OTA_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, OTA_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    // Check for 5 seconds (10 x 500ms) if connected
    bool connected = false;
    esp_netif_ip_info_t ip_info;
    for (int i = 0; i < 10; i++) {
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!connected) {
        ESP_LOGI("ota", "Hotspot '" OTA_WIFI_SSID "' nicht gefunden (5s Timeout). Starte ESP-NOW (Kanal %d)...", ESPNOW_CHANNEL);
        esp_wifi_disconnect();
        esp_wifi_stop();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_max_tx_power(80);
        ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
        return;
    }

    // Get STA MAC Address and construct hostname "dose-XXXXXXXXXXXX"
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char hostname[32] = {0};
    snprintf(hostname, sizeof(hostname), "dose-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Start mDNS
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_instance_name_set("Canfunk Walkie-Talkie OTA");
        ESP_LOGI("ota", "mDNS gestartet: http://%s.local", hostname);
    }

    // Start OTA Web Server
    s_ota_http_server = start_ota_web_server();

    ESP_LOGI("ota", "WLAN '" OTA_WIFI_SSID "' ERFOLGREICH VERBUNDEN! IP: " IPSTR " | Web-OTA bereit auf http://%s.local",
             IP2STR(&ip_info.ip), hostname);

    // Confirmation tones for successful hotspot connection
    audio_play_tone(800.0f, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_play_tone(1200.0f, 150);
}

// ─── Playback Task ───────────────────────────────────────────────────────────
static void play_task(void *) {
    static int16_t pcm_buf[AUDIO_FRAME_SAMPLES];
    bool streaming = false;
    while (1) {
        size_t size = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
            s_audio_ringbuf, &size,
            streaming ? pdMS_TO_TICKS(PLAY_UNDERRUN_MS) : portMAX_DELAY,
            AUDIO_FRAME_SAMPLES);
        if (!data) {
            streaming = false; // stream ended or long gap: re-buffer on the next packet
            continue;
        }
        if (!streaming) {
            // Let a few packets queue up before the first sample is played, so that
            // normal radio jitter does not drain the I2S buffer (audible gaps).
            vTaskDelay(pdMS_TO_TICKS(PLAY_PREBUFFER_MS));
            streaming = true;
        }
        // The byte buffer hands out a short chunk at its wrap-around point. u-law is
        // one byte per sample, so every chunk is played, whatever its length.
        for (size_t i = 0; i < size; i++) {
            pcm_buf[i] = s_ulaw_decode_table[data[i]];
        }
        vRingbufferReturnItem(s_audio_ringbuf, data);
        audio_spk_write(pcm_buf, size);
    }
}

// ─── Transmit Task ───────────────────────────────────────────────────────────
static int32_t mean_abs_amplitude(const int16_t *buf, size_t count) {
    int32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (buf[i] < 0) ? -buf[i] : buf[i];
    }
    return sum / (int32_t)count;
}

static void tx_task(void *) {
    static int16_t mic_buf[AUDIO_FRAME_SAMPLES];
    static uint8_t send_buf[1 + AUDIO_FRAME_SAMPLES];
    static uint8_t enc_buf[5 + AUDIO_FRAME_SAMPLES];
    int accum_count = 0;
    send_buf[0] = 0x01; // Audio Type
    uint32_t last_tx_time = 0; // tick of our last sent packet (0 = never)
    
    bool was_pressed = false;
    static int tx_count = 0;
    
#if !BOARD_SINGLE_BUTTON
    // Configure PTT button (single-button boards: handled by button_task)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PIN_PTT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
#endif

    // VOX state variables
    uint32_t vox_hold_timeout = 0;
    bool s_vox_active = false;
    const int VOX_HOLD_MS = 1000;    // Nachlaufzeit in ms (1 second)

    while (1) {
        uint32_t now = xTaskGetTickCount();
        int vox_threshold = s_vox_thresholds[s_vox_sens_level];

        // Carrier sense: Check if the other person is currently speaking
        bool is_rx_active = (s_last_rx_audio_time != 0) && 
                            ((now - s_last_rx_audio_time) < pdMS_TO_TICKS(800));
        
        // PTT is pressed
        bool ptt_pressed = ptt_is_pressed();
        
        // Handle voice level detection when in VOX mode
        if (s_ptt_mode == PTT_MODE_VOX && !s_pairing_mode) {
            if (!was_pressed) {
                // Read microphone to monitor voice level even when not transmitting
                size_t read = audio_mic_read(mic_buf, AUDIO_FRAME_SAMPLES, 10);
                if (read > 0) {
                    int avg_amp = mean_abs_amplitude(mic_buf, read);
                    // Our own end beep must not re-trigger VOX right after a transmission
                    bool own_tx_echo = (last_tx_time != 0) &&
                                       ((now - last_tx_time) < pdMS_TO_TICKS(800));

                    if (avg_amp >= vox_threshold && !is_rx_active && !own_tx_echo) {
                        s_vox_active = true;
                        vox_hold_timeout = now + pdMS_TO_TICKS(VOX_HOLD_MS);
                        ESP_LOGI(TAG, "VOX aktiv! Pegel: %d", avg_amp);
                    }
                }
            }
            
            // Check if VOX hold timer has expired
            if (s_vox_active && now >= vox_hold_timeout) {
                s_vox_active = false;
                ESP_LOGI(TAG, "VOX inaktiv (Hold-Time abgelaufen).");
            }
        } else {
            s_vox_active = false;
        }

        // Determine trigger state based on selected mode
        bool trigger_active = (s_ptt_mode == PTT_MODE_VOX) ? s_vox_active : s_tilt_active;
        bool want_to_tx = ptt_pressed || trigger_active;
        
        // Active if we want to transmit, but only if the channel is free or we are already transmitting
        bool pressed = want_to_tx && (!is_rx_active || was_pressed);
        
        if (pressed && !s_pairing_mode) {
            if (!was_pressed) {
                ESP_LOGI(TAG, "Sende Audio...");
                was_pressed = true;
                s_tx_active = true;
                accum_count = 0;
                // Play short start beep
                audio_play_tone(880.0f, 80);
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            
            // Read straight into the frame buffer until one packet is complete
            size_t read = audio_mic_read(mic_buf + accum_count, AUDIO_FRAME_SAMPLES - accum_count, 50);
            if (read > 0) {
                // In VOX mode, monitor voice amplitude of current sending audio to refresh VOX hold timer
                if (s_ptt_mode == PTT_MODE_VOX &&
                    mean_abs_amplitude(mic_buf + accum_count, read) >= vox_threshold) {
                    vox_hold_timeout = now + pdMS_TO_TICKS(VOX_HOLD_MS);
                }
                accum_count += read;

                if (accum_count >= AUDIO_FRAME_SAMPLES) {
                    accum_count = 0;
                    // Compress to G.711 u-law
                    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                        send_buf[i + 1] = linear2ulaw(mic_buf[i]);
                    }
                    // Send via ESP-NOW
                    esp_err_t send_err;
                    if (s_has_paired_peer) {
                        // Encrypted packet: type (1 byte) + signature (4 bytes) + encrypted payload (240 bytes)
                        enc_buf[0] = 0x01; // Audio type
                        uint32_t sig = compute_lmk_signature(s_peer_lmk);
                        memcpy(enc_buf + 1, &sig, 4);
                        encrypt_aes128_ecb(s_peer_lmk, send_buf + 1, enc_buf + 5);
                        send_err = esp_now_send(s_broadcast_mac, enc_buf, sizeof(enc_buf));
                    } else {
                        // Unencrypted packet: type (1 byte) + raw payload (240 bytes)
                        send_err = esp_now_send(s_broadcast_mac, send_buf, sizeof(send_buf));
                    }

                    if (send_err == ESP_ERR_ESPNOW_NO_MEM) {
                        // Queue is full. Sleep to let the WiFi driver clear pending packets.
                        vTaskDelay(pdMS_TO_TICKS(20));
                    } else if (send_err == ESP_OK) {
                        tx_count++;
                        if (tx_count % 500 == 0) {
                            ESP_LOGI("espnow_tx", "Audio-Pakete gesendet: %d", tx_count);
                        }
                        last_tx_time = xTaskGetTickCount();
                    }
                    // No extra delay here: the blocking microphone read paces the loop
                    // and lets lower-priority tasks run.
                }
            } else {
                // Yield if no samples were read to prevent CPU starvation
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            if (was_pressed) {
                ESP_LOGI(TAG, "Standby");
                was_pressed = false;
                accum_count = 0;
                // Play short end beep
                audio_play_tone(550.0f, 60);
                s_tx_active = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// ─── Ring Task ───────────────────────────────────────────────────────────────
// Sends the ring packet after a crank gesture and plays the ring tone when a ring
// arrives while this can is standing on its head.
static void play_ring_tone(void) {
    // Two short bell bursts: fast trill between two tones, ~2.4 s in total.
    // Louder than the other tones (RING_TONE_LEVEL), speech volume stays as set.
    for (int burst = 0; burst < 2; burst++) {
        for (int i = 0; i < 20; i++) {
            audio_play_tone(1000.0f, 25, RING_TONE_LEVEL);
            audio_play_tone(1300.0f, 25, RING_TONE_LEVEL);
        }
        if (burst == 0) vTaskDelay(pdMS_TO_TICKS(400));
    }
}

static void ring_task(void *) {
    uint8_t pkt[9];
    pkt[0] = 0x04;
    memcpy(pkt + 1, "RING", 4);
    while (1) {
        if (s_ring_send_request) {
            s_ring_send_request = false;
            if (!s_pairing_mode) {
                uint32_t sig = s_has_paired_peer ? compute_lmk_signature(s_peer_lmk) : 0;
                memcpy(pkt + 5, &sig, 4);
                for (int i = 0; i < RING_REPEAT_COUNT; i++) {
                    esp_now_send(s_broadcast_mac, pkt, sizeof(pkt));
                    vTaskDelay(pdMS_TO_TICKS(40));
                }
                ESP_LOGI("ring", "Klingelsignal gesendet.");
                // Local confirmation: short rising double beep
                audio_play_tone(1300.0f, 50); vTaskDelay(pdMS_TO_TICKS(40));
                audio_play_tone(1600.0f, 70);
            }
        }
        if (s_ring_rx_pending) {
            s_ring_rx_pending = false;
            if (s_upside_down && !s_tx_active) {
                ESP_LOGI("ring", "Klingelsignal empfangen: Dose steht auf dem Kopf, klingle.");
                s_ring_playing = true;
                play_ring_tone();
                s_ring_end_time = xTaskGetTickCount();
                s_ring_playing = false;
            } else {
                ESP_LOGI("ring", "Klingelsignal empfangen, Dose steht nicht auf dem Kopf: kein Klingeln.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── Control & Pairing Task ──────────────────────────────────────────────────
#if BOARD_SINGLE_BUTTON
// ─── Single-Button Control ───────────────────────────────────────────────────
// Hold = talk. Short clicks are counted and executed after a pause:
//   2 = louder, 3 = quieter, 4 = TILT/VOX, 5 = pairing, 6 = calibrate tilt axis
#define BTN_POLL_MS      10
#define BTN_DEBOUNCE_MS  20
#define BTN_HOLD_MS      250   // pressed longer than this = talk, not a click
#define BTN_GAP_MS       400   // pause that ends a click sequence

static void button_clicks_action(int clicks) {
    switch (clicks) {
        case 2:
        case 3: {
            int lv = audio_spk_get_volume() + ((clicks == 2) ? 1 : -1);
            if (lv >= 1 && lv <= 10) {
                audio_spk_set_volume(lv);
                save_volume_level(lv);
            } else {
                lv = audio_spk_get_volume(); // already at the limit: just beep
            }
            audio_play_tone(300.0f + (float)lv * 60.0f, 80);
            break;
        }
        case 4:
            if (s_ptt_mode == PTT_MODE_TILT) {
                s_ptt_mode = PTT_MODE_VOX;
                ESP_LOGI(TAG, "PTT-Modus auf VOX gewechselt.");
                audio_play_tone(800.0f, 100); vTaskDelay(pdMS_TO_TICKS(100)); audio_play_tone(1000.0f, 100);
            } else {
                s_ptt_mode = PTT_MODE_TILT;
                ESP_LOGI(TAG, "PTT-Modus auf TILT gewechselt.");
                audio_play_tone(1000.0f, 100); vTaskDelay(pdMS_TO_TICKS(100)); audio_play_tone(800.0f, 100);
            }
            save_ptt_mode(s_ptt_mode);
            break;
        case 5:
            if (!s_pairing_mode) {
                ESP_LOGI(TAG, "Pairing-Modus aktiviert!");
                s_pairing_timer = 300; // 300 * 50ms = 15 seconds timeout
                s_pairing_mode = true;
                audio_play_tone(800.0f, 100);  vTaskDelay(pdMS_TO_TICKS(50));
                audio_play_tone(1000.0f, 100); vTaskDelay(pdMS_TO_TICKS(50));
                audio_play_tone(1200.0f, 150);
            }
            break;
        case 6:
            request_tilt_calibration();
            break;
        default:
            break;
    }
}

static void button_task(void *) {
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PIN_PTT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    int clicks = 0;
    int pressed_ms = 0;
    int released_ms = 0;

    while (1) {
        if (gpio_get_level(PIN_PTT) == 0) {
            released_ms = 0;
            pressed_ms += BTN_POLL_MS;
            if (pressed_ms >= BTN_HOLD_MS) {
                s_ptt_held = true;
                clicks = 0;
            }
        } else {
            if (!s_ptt_held && pressed_ms >= BTN_DEBOUNCE_MS) {
                clicks++;
            }
            s_ptt_held = false;
            pressed_ms = 0;
            if (clicks > 0) {
                released_ms += BTN_POLL_MS;
                if (released_ms >= BTN_GAP_MS) {
                    button_clicks_action(clicks);
                    clicks = 0;
                    released_ms = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}
#endif

static void control_task(void *) {
#if !BOARD_SINGLE_BUTTON
    gpio_config_t vol_cfg = {
        .pin_bit_mask = (1ULL << PIN_VOL_UP) | (1ULL << PIN_VOL_DOWN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&vol_cfg);

    // Configure BOOT button (GPIO 0)
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
#endif

    int pairing_btn_hold_counter = 0;
    int boot_btn_hold_counter = 0;
    int boot_ptt_hold_counter = 0;
    int pairing_broadcast_counter = 0;

    bool prev_vol_u = false;
    bool prev_vol_d = false;
    bool vol_u_pending = false;
    bool vol_d_pending = false;

    while (1) {
        // Pairing finished in the ESP-NOW callback: persist the key and confirm here
        if (s_pairing_done_pending) {
            s_pairing_done_pending = false;
            if (save_peer_config(s_peer_lmk) != ESP_OK) {
                ESP_LOGE(TAG, "LMK konnte nicht gespeichert werden!");
            }
            ESP_LOGI(TAG, "Pairing erfolgreich!");
            audio_play_tone(1500.0f, 100);
            vTaskDelay(pdMS_TO_TICKS(50));
            audio_play_tone(1800.0f, 150);
        }

#if BOARD_SINGLE_BUTTON
        // No volume/mode buttons on this board: button_task handles the one button,
        // the button combinations below never trigger.
        const bool vol_u = false, vol_d = false, boot_pressed = false, ptt_pressed = false;
#else
        bool vol_u = (gpio_get_level(PIN_VOL_UP) == 0);
        bool vol_d = (gpio_get_level(PIN_VOL_DOWN) == 0);
        bool boot_pressed = (gpio_get_level(PIN_BOOT_BUTTON) == 0);
        bool ptt_pressed = ptt_is_pressed();
#endif

        // Calibrate the physical dose axis (hold BOOT + PTT for 2 seconds while upright)
        if (boot_pressed && ptt_pressed) {
            boot_btn_hold_counter = -1; // Cancel mode switch hold
            if (boot_ptt_hold_counter >= 0) {
                boot_ptt_hold_counter++;
                if (boot_ptt_hold_counter == 40) { // 40 * 50ms = 2000ms = 2 seconds
                    boot_ptt_hold_counter = -1; // Flag to ignore further counting until release
                    
                    request_tilt_calibration();
                }
            }
        } else {
            boot_ptt_hold_counter = 0;
        }

        // Handle BOOT button toggle and sensitivity
        if (boot_pressed) {
            // Sensitivity adjustment: if VOL+ or VOL- is pressed while BOOT is held
            if (vol_u && !prev_vol_u) {
                boot_btn_hold_counter = -1; // Cancel mode switch hold
                if (s_ptt_mode == PTT_MODE_VOX) {
                    if (s_vox_sens_level < 10) {
                        s_vox_sens_level++;
                        save_sens_levels(s_vox_sens_level, s_tilt_sens_level);
                        ESP_LOGI(TAG, "VOX Sensibilitaet erhoeht auf Stufe %d (Schwellenwert %d)", 
                                 s_vox_sens_level, s_vox_thresholds[s_vox_sens_level]);
                        audio_play_tone(400.0f + (float)s_vox_sens_level * 80.0f, 80);
                    }
                } else {
                    if (s_tilt_sens_level < 10) {
                        s_tilt_sens_level++;
                        save_sens_levels(s_vox_sens_level, s_tilt_sens_level);
                        ESP_LOGI(TAG, "TILT Sensibilitaet erhoeht auf Stufe %d (Schwellenwert %1.1f)", 
                                 s_tilt_sens_level, s_tilt_thresholds[s_tilt_sens_level]);
                        audio_play_tone(400.0f + (float)s_tilt_sens_level * 80.0f, 80);
                    }
                }
            }
            else if (vol_d && !prev_vol_d) {
                boot_btn_hold_counter = -1; // Cancel mode switch hold
                if (s_ptt_mode == PTT_MODE_VOX) {
                    if (s_vox_sens_level > 1) {
                        s_vox_sens_level--;
                        save_sens_levels(s_vox_sens_level, s_tilt_sens_level);
                        ESP_LOGI(TAG, "VOX Sensibilitaet verringert auf Stufe %d (Schwellenwert %d)", 
                                 s_vox_sens_level, s_vox_thresholds[s_vox_sens_level]);
                        audio_play_tone(400.0f + (float)s_vox_sens_level * 80.0f, 80);
                    }
                } else {
                    if (s_tilt_sens_level > 1) {
                        s_tilt_sens_level--;
                        save_sens_levels(s_vox_sens_level, s_tilt_sens_level);
                        ESP_LOGI(TAG, "TILT Sensibilitaet verringert auf Stufe %d (Schwellenwert %1.1f)", 
                                 s_tilt_sens_level, s_tilt_thresholds[s_tilt_sens_level]);
                        audio_play_tone(400.0f + (float)s_tilt_sens_level * 80.0f, 80);
                    }
                }
            }

            // Standard mode switch timer (5 seconds: 100 * 50ms)
            if (boot_btn_hold_counter >= 0) {
                boot_btn_hold_counter++;
                if (boot_btn_hold_counter == 100) { // 100 * 50ms = 5 seconds
                    boot_btn_hold_counter = -1; // Flag to ignore further counting until release
                    
                    if (s_ptt_mode == PTT_MODE_TILT) {
                        s_ptt_mode = PTT_MODE_VOX;
                        ESP_LOGI(TAG, "PTT-Modus auf VOX gewechselt.");
                        
                        // Play rising tone: 800Hz -> 1000Hz
                        audio_play_tone(800.0f, 100);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        audio_play_tone(1000.0f, 100);
                    } else {
                        s_ptt_mode = PTT_MODE_TILT;
                        ESP_LOGI(TAG, "PTT-Modus auf TILT gewechselt.");
                        
                        // Play falling tone: 1000Hz -> 800Hz
                        audio_play_tone(1000.0f, 100);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        audio_play_tone(800.0f, 100);
                    }
                    save_ptt_mode(s_ptt_mode);
                }
            }
        } else {
            boot_btn_hold_counter = 0;

            // Check if both volume buttons are pressed simultaneously
            if (vol_u && vol_d) {
                vol_u_pending = false;
                vol_d_pending = false;
                if (pairing_btn_hold_counter >= 0) {
                    pairing_btn_hold_counter++;
                    if (pairing_btn_hold_counter == 60) { // 60 * 50ms = 3 seconds
                        if (!s_pairing_mode) {
                            ESP_LOGI(TAG, "Pairing-Modus aktiviert!");
                            s_pairing_mode = true;
                            s_pairing_timer = 300; // 300 * 50ms = 15 seconds timeout
                            
                            // Play pairing active tone
                            audio_play_tone(800.0f, 100);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            audio_play_tone(1000.0f, 100);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            audio_play_tone(1200.0f, 150);
                        }
                    }
                    else if (pairing_btn_hold_counter >= 200) { // 200 * 50ms = 10 seconds
                        pairing_btn_hold_counter = -1; // Flag to ignore further counting until release
                        s_pairing_mode = false;
                        
                        // Reset passkey: delete configuration from NVS and clear variables
                        delete_peer_config();
                        
                        s_has_paired_peer = false;
                        memset(s_peer_lmk, 0, 16);
                        
                        // Re-register broadcast peer without encryption
                        add_espnow_peer(s_broadcast_mac, false, NULL);
                        
                        ESP_LOGI(TAG, "Passkey zurueckgesetzt (NVS geloescht).");
                        
                        // Play rapid confirmation tone: 5 fast short beeps (1200Hz, 30ms on, 30ms off)
                        for (int i = 0; i < 5; i++) {
                            audio_play_tone(1200.0f, 30);
                            vTaskDelay(pdMS_TO_TICKS(30));
                        }
                    }
                }
            } else {
                pairing_btn_hold_counter = 0;

                // Handle Vol+ release
                if (vol_u && !prev_vol_u && !vol_d) {
                    vol_u_pending = true;
                }
                if (!vol_u && prev_vol_u) {
                    if (vol_u_pending) {
                        int lv = audio_spk_get_volume();
                        if (lv < 10) {
                            lv++;
                            audio_spk_set_volume(lv);
                            save_volume_level(lv);
                            // Tone pitch increases with volume
                            audio_play_tone(300.0f + (float)lv * 60.0f, 80);
                        }
                        vol_u_pending = false;
                    }
                }

                // Handle Vol- release
                if (vol_d && !prev_vol_d && !vol_u) {
                    vol_d_pending = true;
                }
                if (!vol_d && prev_vol_d) {
                    if (vol_d_pending) {
                        int lv = audio_spk_get_volume();
                        if (lv > 1) {
                            lv--;
                            audio_spk_set_volume(lv);
                            save_volume_level(lv);
                            // Tone pitch increases with volume
                            audio_play_tone(300.0f + (float)lv * 60.0f, 80);
                        }
                        vol_d_pending = false;
                    }
                }
            }
        }

        // Manage pairing loop
        if (s_pairing_mode) {
            s_pairing_timer--;
            if (s_pairing_timer <= 0) {
                s_pairing_mode = false;
                ESP_LOGW(TAG, "Pairing-Modus abgebrochen (Timeout).");
                // Play timeout tone
                audio_play_tone(350.0f, 300);
            } else {
                // Broadcast pairing request packet every 500ms
                pairing_broadcast_counter++;
                if (pairing_broadcast_counter >= 10) { // 10 * 50ms = 500ms
                    pairing_broadcast_counter = 0;
                    
                    uint8_t pkg[22] = {0};
                    pkg[0] = 0x02; // Pairing Request
                    memcpy(pkg + 1, "PAIR", 4);

                    // Include existing LMK info for multi-device sync
                    pkg[5] = s_has_paired_peer ? 1 : 0;
                    if (s_has_paired_peer) {
                        memcpy(pkg + 6, s_peer_lmk, 16);
                    }

                    esp_now_send(s_broadcast_mac, pkg, sizeof(pkg));
                }
            }
        }

        prev_vol_u = vol_u;
        prev_vol_d = vol_d;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── Serial Console Task ─────────────────────────────────────────────────────
static void console_task(void *) {
    char buf[128];
    int pos = 0;

    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("\n=======================================================\n");
    printf("  Canfunk Walkie-Talkie Serial Console Ready!\n");
    printf("  Geben Sie 'help' oder '?' ein fuer eine Liste aller Befehle.\n");
    printf("=======================================================\n> ");
    fflush(stdout);

    while (1) {
        int c = getchar();
        if (c != EOF && c != 0xFF) {
            if (c == '\r' || c == '\n') {
                if (pos > 0) {
                    buf[pos] = '\0';
                    printf("\n");

                    // Trim leading spaces
                    char *cmd = buf;
                    while (*cmd == ' ') cmd++;

                    // Extract command and argument
                    char *arg = strchr(cmd, ' ');
                    if (arg) {
                        *arg = '\0';
                        arg++;
                        while (*arg == ' ') arg++;
                    } else {
                        arg = (char *)"";
                    }

                    // Process commands
                    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
                        printf("--- Canfunk Serielle Befehle ---\n");
                        printf("  status / info  : Aktuellen Status (Modus, Vol, Sens, ADXL) anzeigen\n");
                        printf("  cal / calibrate: Dosenachse kalibrieren (Dose aufrecht auf Boden)\n");
                        printf("  mode tilt      : Auf Neigungs-Modus umschalten\n");
                        printf("  mode vox       : Auf Sprachsteuerungs-Modus umschalten\n");
                        printf("  sens <1-10>    : Sensibilitaet einstellen (z.B. sens 8)\n");
                        printf("  vol <1-10>     : Lautstaerke einstellen (z.B. vol 3)\n");
                        printf("  pair           : Pairing-Modus starten (15s Timeout)\n");
                        printf("  ring           : Andere Dosen klingeln lassen (wie Kurbeln)\n");
                        printf("  reset          : Passkey zuruecksetzen (NVS loeschen)\n");
                        printf("  reboot         : Geraet neu starten\n");
#if BOARD_SINGLE_BUTTON
                        printf("--- Taste (BOOT) ---\n");
                        printf("  halten = sprechen | 2x lauter | 3x leiser | 4x TILT/VOX | 5x Pairing | 6x Kalibrieren\n");
#endif
                    }
                    else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "info") == 0) {
                        printf("--- Canfunk Status ---\n");
                        printf("  Board        : %s\n", BOARD_NAME);
                        printf("  PTT Modus    : %s\n", (s_ptt_mode == PTT_MODE_VOX) ? "VOX (Sprache)" :
                               TILT_ENABLE ? "TILT (Neigung)" : "Taste (TILT deaktiviert)");
                        printf("  Lautstaerke  : %d / 10\n", audio_spk_get_volume());
                        printf("  Sensibilitaet: VOX=%d (Schwellenwert %d), TILT=%d (Schwellenwert %.1f LSB)\n",
                               s_vox_sens_level, s_vox_thresholds[s_vox_sens_level],
                               s_tilt_sens_level, s_tilt_thresholds[s_tilt_sens_level]);
#if TILT_ENABLE
                        printf("  Dosenachse   : X=%.1f, Y=%.1f, Z=%.1f\n",
                               s_adxl_offset_x, s_adxl_offset_y, s_adxl_offset_z);
                        printf("  Neigung      : %.1f Grad (TILT %s)\n",
                               s_tilt_trigger.tilt_degrees({s_adxl_x, s_adxl_y, s_adxl_z}),
                               s_tilt_active ? "aktiv" : "inaktiv");
#endif
#if TILT_ENABLE
                        printf("  Lage         : %s\n", s_upside_down ? "auf dem Kopf (klingelt bei Anruf)" : "nicht auf dem Kopf");
#endif
                        printf("  Paired Key   : %s\n", s_has_paired_peer ? "JA (Verschluesselt)" : "NEIN (Broadcast)");
                    }
                    else if (strcmp(cmd, "cal") == 0 || strcmp(cmd, "calibrate") == 0) {
#if TILT_ENABLE
                        request_tilt_calibration();
                        printf("[OK] Kalibrierung gestartet: Dose 1 Sekunde aufrecht auf den Boden stellen.\n");
#else
                        printf("[ERR] TILT ist deaktiviert (TILT_ENABLE=0 in platformio.ini).\n");
#endif
                    }
                    else if (strcmp(cmd, "mode") == 0) {
                        if (strcmp(arg, "vox") == 0) {
                            s_ptt_mode = PTT_MODE_VOX;
                            save_ptt_mode(s_ptt_mode);
                            printf("[OK] PTT-Modus auf VOX gewechselt.\n");
                            audio_play_tone(800.0f, 100); vTaskDelay(pdMS_TO_TICKS(50)); audio_play_tone(1000.0f, 100);
                        } else if (strcmp(arg, "tilt") == 0) {
                            s_ptt_mode = PTT_MODE_TILT;
                            save_ptt_mode(s_ptt_mode);
                            printf("[OK] PTT-Modus auf TILT gewechselt.\n");
                            audio_play_tone(1000.0f, 100); vTaskDelay(pdMS_TO_TICKS(50)); audio_play_tone(800.0f, 100);
                        } else {
                            printf("[ERR] Ungueltiger Modus. Verwende 'mode tilt' oder 'mode vox'.\n");
                        }
                    }
                    else if (strcmp(cmd, "sens") == 0) {
                        int v = atoi(arg);
                        if (v >= 1 && v <= 10) {
                            if (s_ptt_mode == PTT_MODE_VOX) {
                                s_vox_sens_level = v;
                                printf("[OK] VOX Sensibilitaet auf Stufe %d gesetzt (Schwellenwert %d).\n",
                                       v, s_vox_thresholds[v]);
                            } else {
                                s_tilt_sens_level = v;
                                printf("[OK] TILT Sensibilitaet auf Stufe %d gesetzt (Schwellenwert %.1f LSB).\n",
                                       v, s_tilt_thresholds[v]);
                            }
                            save_sens_levels(s_vox_sens_level, s_tilt_sens_level);
                            audio_play_tone(400.0f + (float)v * 80.0f, 80);
                        } else {
                            printf("[ERR] Sensibilitaet muss zwischen 1 und 10 liegen (z.B. 'sens 7').\n");
                        }
                    }
                    else if (strcmp(cmd, "vol") == 0) {
                        int v = atoi(arg);
                        if (v >= 1 && v <= 10) {
                            audio_spk_set_volume(v);
                            save_volume_level(v);
                            printf("[OK] Lautstaerke auf Stufe %d gesetzt.\n", v);
                            audio_play_tone(300.0f + (float)v * 60.0f, 80);
                        } else {
                            printf("[ERR] Lautstaerke muss zwischen 1 und 10 liegen (z.B. 'vol 3').\n");
                        }
                    }
                    else if (strcmp(cmd, "pair") == 0) {
                        s_pairing_mode = true;
                        s_pairing_timer = 300;
                        printf("[OK] Pairing-Modus gestartet (15s Timeout)...\n");
                        audio_play_tone(800.0f, 100); vTaskDelay(pdMS_TO_TICKS(50));
                        audio_play_tone(1000.0f, 100); vTaskDelay(pdMS_TO_TICKS(50));
                        audio_play_tone(1200.0f, 150);
                    }
                    else if (strcmp(cmd, "reset") == 0) {
                        delete_peer_config();
                        s_has_paired_peer = false;
                        memset(s_peer_lmk, 0, 16);
                        add_espnow_peer(s_broadcast_mac, false, NULL);
                        printf("[OK] Passkey zurueckgesetzt (NVS geloescht).\n");
                        for (int i = 0; i < 5; i++) {
                            audio_play_tone(1200.0f, 30);
                            vTaskDelay(pdMS_TO_TICKS(30));
                        }
                    }
                    else if (strcmp(cmd, "ring") == 0) {
                        s_ring_send_request = true;
                        printf("[OK] Klingelsignal wird gesendet.\n");
                    }
                    else if (strcmp(cmd, "reboot") == 0) {
                        printf("[SYSTEM] Geraet wird neu gestartet...\n");
                        vTaskDelay(pdMS_TO_TICKS(200));
                        esp_restart();
                    }
                    else {
                        printf("[ERR] Unbekannter Befehl: '%s'. Tippe 'help' fuer Hilfe.\n", cmd);
                    }

                    pos = 0;
                    printf("> ");
                    fflush(stdout);
                }
            }
            else if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    printf("\b \b");
                    fflush(stdout);
                }
            }
            else if (pos < (int)sizeof(buf) - 1) {
                buf[pos++] = (char)c;
                putchar(c);
                fflush(stdout);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ─── Entry Point ─────────────────────────────────────────────────────────────
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Start Canfunk Walkie-Talkie auf %s...", BOARD_NAME);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Try loading saved peer config from NVS
    if (load_peer_config(s_peer_lmk) == ESP_OK) {
        s_has_paired_peer = true;
        ESP_LOGI(TAG, "Gespeicherten LMK aus NVS geladen.");
    } else {
        s_has_paired_peer = false;
        ESP_LOGI(TAG, "Kein LMK gespeichert. Verwende unverschluesselten Broadcast.");
    }

    ulaw_init_table();

    // Initialize audio drivers
    audio_init();

    // Try loading saved volume level from NVS
    int vol = DEFAULT_VOLUME;
    if (load_volume_level(&vol) == ESP_OK) {
        audio_spk_set_volume(vol);
        ESP_LOGI(TAG, "Gespeicherte Lautstarke geladen: %d", vol);
    } else {
        audio_spk_set_volume(DEFAULT_VOLUME);
        ESP_LOGI(TAG, "Keine Lautstarke gespeichert. Verwende Standardwert %d.", DEFAULT_VOLUME);
    }

    // Try loading saved PTT mode from NVS
    ptt_mode_t mode = DEFAULT_PTT_MODE;
    if (load_ptt_mode(&mode) == ESP_OK) {
        s_ptt_mode = mode;
        ESP_LOGI(TAG, "Gespeicherten PTT-Modus geladen: %s", (mode == PTT_MODE_VOX) ? "VOX" : "TILT");
    } else {
        s_ptt_mode = DEFAULT_PTT_MODE;
        ESP_LOGI(TAG, "Kein PTT-Modus gespeichert. Verwende Standardwert: %s", DEFAULT_PTT_MODE_VOX ? "VOX" : "TILT");
    }

    // Try loading saved sensitivity levels from NVS
    int vox_sens = DEFAULT_VOX_SENS;
    int tilt_sens = 5;
    if (load_sens_levels(&vox_sens, &tilt_sens) == ESP_OK) {
        s_vox_sens_level = vox_sens;
        s_tilt_sens_level = tilt_sens;
        ESP_LOGI(TAG, "Gespeicherte Sensibilitaeten geladen: VOX=%d, TILT=%d", vox_sens, tilt_sens);
    } else {
        s_vox_sens_level = DEFAULT_VOX_SENS;
        s_tilt_sens_level = 5;
        ESP_LOGI(TAG, "Keine Sensibilitaeten gespeichert. Verwende Standard: VOX=%d, TILT=5", DEFAULT_VOX_SENS);
    }

    // Try loading saved ADXL offsets from NVS
    float ox = 0.0f, oy = 0.0f, oz = 256.0f;
    if (load_adxl_offsets(&ox, &oy, &oz) == ESP_OK && load_dose_axis_is_calibrated()) {
        s_adxl_offset_x = ox;
        s_adxl_offset_y = oy;
        s_adxl_offset_z = oz;
        s_tilt_trigger.calibrate_axis({s_adxl_offset_x, s_adxl_offset_y, s_adxl_offset_z});
        ESP_LOGI(TAG, "Gespeicherte Dosenachse geladen: X=%.1f, Y=%.1f, Z=%.1f", ox, oy, oz);
    } else {
        s_adxl_offset_x = 0.0f;
        s_adxl_offset_y = 0.0f;
        s_adxl_offset_z = 256.0f;
#if TILT_ENABLE
        ESP_LOGW(TAG, "Keine Dosenachse kalibriert. TILT bleibt bis zur aufrechten Kalibrierung im Empfang.");
#endif
    }

    // Create ring buffer for incoming audio (8KB byte-buffer)
    s_audio_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
    if (s_audio_ringbuf == NULL) {
        ESP_LOGE(TAG, "Fehler beim Erstellen des Audio Ring-Buffers!");
        return;
    }

    // 5 s OTA window: join the OTA hotspot and run Web-OTA if it is there, otherwise switch to ESP-NOW (ESPNOW_CHANNEL)
    start_wifi_ota_window();

    // ESP-NOW needs the receiver permanently on: modem sleep would drop audio
    // packets (matters when the OTA hotspot connection is up).
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, s_own_mac);

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    
    // Always set PMK first before adding peers
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)"CanfunkPMKHeader1"));
    
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    // Register broadcast peer (always unencrypted)
    add_espnow_peer(s_broadcast_mac, false, NULL);

    // Play startup confirmation tone
    audio_play_tone(440.0f, 150);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_play_tone(554.37f, 150);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_play_tone(659.25f, 150);

    // Audio tasks on the second core where there is one (keeps core 0 free for WiFi)
    const BaseType_t audio_core = (portNUM_PROCESSORS > 1) ? 1 : 0;
    xTaskCreatePinnedToCore(play_task, "audio_play", 8192, NULL, 10, NULL, audio_core);
#if TILT_ENABLE
    xTaskCreatePinnedToCore(adxl_task, "adxl", 4096, NULL, 9, NULL, 0);
#else
    ESP_LOGI(TAG, "TILT deaktiviert (TILT_ENABLE=0): Sprechen per Taste oder VOX.");
#endif
    xTaskCreatePinnedToCore(tx_task, "audio_tx", 8192, NULL, 8, NULL, audio_core);
    xTaskCreatePinnedToCore(control_task, "control", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(ring_task, "ring", 3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 5, NULL, 0);
#if BOARD_SINGLE_BUTTON
    xTaskCreatePinnedToCore(button_task, "button", 4096, NULL, 6, NULL, 0);
#endif

    ESP_LOGI(TAG, "Walkie-Talkie gestartet und betriebsbereit.");
}
