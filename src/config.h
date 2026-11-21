#pragma once
#include "driver/gpio.h"

// Board selection: set -DBOARD_TOY_AI_CORE_C3 in platformio.ini for the SpotPear
// Toy-AI-Core-C3-MINI. Without a flag the original ESP32-S3 breadboard is built.
//
// Every value in this file is only a default: it can be overridden per environment
// with build_flags in platformio.ini (see the option list there), e.g.
//   build_flags = -DTILT_ENABLE=1 -DADXL_I2C_ADDR=0x1D

#if defined(BOARD_TOY_AI_CORE_C3)
// ═══ SpotPear Toy-AI-Core-C3-MINI (ESP32-C3, ES8311 codec, NS4150 amplifier) ═══
#define BOARD_NAME              "SpotPear Toy-AI-Core-C3-MINI"

// One codec on one I2S bus (full duplex) instead of separate mic/amp modules
#define BOARD_AUDIO_ES8311      1
// BOOT is the only user button: hold = talk, multi-click = settings
#define BOARD_SINGLE_BUTTON     1
// The accelerometer is not on the board: tilt is off unless enabled in
// platformio.ini. A sensor can be wired to the I2C pads of the display connector
// (FPC pin 17 = SCL, pin 18 = SDA), where it shares the bus with the codec.
#ifndef TILT_ENABLE
#define TILT_ENABLE             0
#endif

#ifndef PIN_BOOT_BUTTON
#define PIN_BOOT_BUTTON     GPIO_NUM_9   // BOOT button, also on the BOOT/wake connectors
#endif
#ifndef PIN_PTT
#define PIN_PTT             GPIO_NUM_9   // same button
#endif
#ifndef PIN_I2S_MCLK
#define PIN_I2S_MCLK        GPIO_NUM_10  // ES8311 MCLK
#endif
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK        GPIO_NUM_8   // ES8311 SCLK
#endif
#ifndef PIN_I2S_WS
#define PIN_I2S_WS          GPIO_NUM_6   // ES8311 LRCK
#endif
#ifndef PIN_I2S_DOUT
#define PIN_I2S_DOUT        GPIO_NUM_5   // ESP -> ES8311 DSDIN (speaker)
#endif
#ifndef PIN_I2S_DIN
#define PIN_I2S_DIN         GPIO_NUM_7   // ES8311 ASDOUT -> ESP (microphone)
#endif
#ifndef PIN_PA_ENABLE
#define PIN_PA_ENABLE       GPIO_NUM_11  // NS4150 amplifier enable (high = on)
#endif
#ifndef PIN_LED
#define PIN_LED             GPIO_NUM_2   // WS2812B (unused)
#endif
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA         GPIO_NUM_3   // ES8311 control (+ optional ADXL345)
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL         GPIO_NUM_4
#endif

#ifndef ES8311_I2C_ADDR
#define ES8311_I2C_ADDR     0x18         // CE pin pulled low
#endif
// Microphone PGA gain, register 0x16: 0..7 = 0..42 dB in 6 dB steps
#ifndef ES8311_MIC_GAIN
#define ES8311_MIC_GAIN     4            // 24 dB
#endif

#else
// ═══ ESP32-S3 bread-compact-wifi board (INMP441 + I2S amplifier + ADXL345) ═════
#define BOARD_NAME              "ESP32-S3 bread-compact-wifi"

#ifndef PIN_BOOT_BUTTON
#define PIN_BOOT_BUTTON     GPIO_NUM_0   // Boot / Flash button
#endif
#ifndef PIN_WIFI_RESET
#define PIN_WIFI_RESET      GPIO_NUM_1   // WiFi/Reset button (used for PTT)
#endif
#ifndef PIN_PTT
#define PIN_PTT             PIN_WIFI_RESET
#endif
#ifndef PIN_HOOK_SWITCH
#define PIN_HOOK_SWITCH     GPIO_NUM_2   // Hook switch
#endif
#ifndef PIN_MIC_WS
#define PIN_MIC_WS          GPIO_NUM_4   // I2S Mic Word Select
#endif
#ifndef PIN_MIC_SCK
#define PIN_MIC_SCK         GPIO_NUM_5   // I2S Mic Bit Clock
#endif
#ifndef PIN_MIC_DIN
#define PIN_MIC_DIN         GPIO_NUM_6   // I2S Mic Data In
#endif
#ifndef PIN_SPK_DOUT
#define PIN_SPK_DOUT        GPIO_NUM_7   // I2S Spk Data Out
#endif
#ifndef PIN_TOUCH_BUTTON
#define PIN_TOUCH_BUTTON    GPIO_NUM_47  // Touch button
#endif
#ifndef PIN_LED
#define PIN_LED             GPIO_NUM_48  // Status LED
#endif
#ifndef PIN_VOL_DOWN
#define PIN_VOL_DOWN        GPIO_NUM_39  // Volume - button
#endif
#ifndef PIN_VOL_UP
#define PIN_VOL_UP          GPIO_NUM_40  // Volume + button
#endif
#ifndef PIN_SPK_BCLK
#define PIN_SPK_BCLK        GPIO_NUM_15  // I2S Spk Bit Clock
#endif
#ifndef PIN_SPK_LRCK
#define PIN_SPK_LRCK        GPIO_NUM_16  // I2S Spk LR Clock
#endif

// ─── Microphone Settings (INMP441) ──────────────────────────────────────────
#ifndef MIC_SLOT_BITS
#define MIC_SLOT_BITS              32  // 32-bit slot for INMP441
#endif
#ifndef MIC_PHILIPS
#define MIC_PHILIPS                 1  // I2S Philips format
#endif
// INMP441 supplies signed 24-bit samples left-aligned in 32-bit I2S words.
// Keep the top 16 bits.  A shift of 14 amplified speech by 4x and could wrap
// during conversion to int16_t, producing harsh distortion at the receiver.
#ifndef MIC_BIT_SHIFT
#define MIC_BIT_SHIFT              16
#endif

// ─── ADXL345 Accelerometer (I2C) ────────────────────────────────────────────
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA         GPIO_NUM_41
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL         GPIO_NUM_42
#endif
#endif

#ifndef BOARD_AUDIO_ES8311
#define BOARD_AUDIO_ES8311      0
#endif
#ifndef BOARD_SINGLE_BUTTON
#define BOARD_SINGLE_BUTTON     0
#endif

// ─── Tilt trigger (ADXL345) ─────────────────────────────────────────────────
// TILT_ENABLE 1 = accelerometer is fitted, tilting the can starts transmitting.
// TILT_ENABLE 0 = no sensor: nothing is probed on I2C, talk via button or VOX.
#ifndef TILT_ENABLE
#define TILT_ENABLE             1
#endif
// ADXL_I2C_ADDR: fixed I2C address of the sensor (0x53 = SDO on GND, 0x1D = SDO
// on VCC). Left undefined, both addresses are tried.

// ─── Defaults for a device without stored settings ──────────────────────────
#ifndef DEFAULT_VOLUME
#define DEFAULT_VOLUME          5      // 1..10
#endif
#ifndef DEFAULT_VOX_SENS
#define DEFAULT_VOX_SENS        6      // 1..10
#endif
#ifndef DEFAULT_PTT_MODE_VOX
#define DEFAULT_PTT_MODE_VOX    0      // 0 = TILT/button, 1 = VOX
#endif

// ─── Radio / OTA ────────────────────────────────────────────────────────────
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL          13     // WiFi channel when no OTA hotspot is found
#endif
// Placeholders only: set your own values in platformio_local.ini (see README).
#ifndef OTA_WIFI_SSID
#define OTA_WIFI_SSID           "Canfunk-OTA"
#endif
#ifndef OTA_WIFI_PASSWORD
#define OTA_WIFI_PASSWORD       "canfunk-ota"  // WPA2: at least 8 characters
#endif
#ifndef OTA_HTTP_PASSWORD
#define OTA_HTTP_PASSWORD       "canfunk"
#define OTA_HTTP_PASSWORD_IS_DEFAULT 1
#endif
#if defined(OTA_HTTP_PASSWORD_IS_DEFAULT)
#warning "OTA_HTTP_PASSWORD ist der oeffentliche Standardwert: eigenes Passwort in platformio_local.ini setzen"
#endif

// ─── NVS ────────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE       "canfunk"

// ─── Audio ──────────────────────────────────────────────────────────────────
#ifndef AUDIO_SAMPLE_RATE_SPK
#define AUDIO_SAMPLE_RATE_SPK   16000  // 16kHz for matching microphone
#endif
#ifndef AUDIO_SAMPLE_RATE_MIC
#define AUDIO_SAMPLE_RATE_MIC   16000  // 16kHz
#endif

// ─── Sanity checks for values set in platformio.ini ─────────────────────────
#if DEFAULT_VOLUME < 1 || DEFAULT_VOLUME > 10
#error "DEFAULT_VOLUME muss zwischen 1 und 10 liegen"
#endif
#if DEFAULT_VOX_SENS < 1 || DEFAULT_VOX_SENS > 10
#error "DEFAULT_VOX_SENS muss zwischen 1 und 10 liegen"
#endif
#if ESPNOW_CHANNEL < 1 || ESPNOW_CHANNEL > 13
#error "ESPNOW_CHANNEL muss zwischen 1 und 13 liegen"
#endif
#if defined(ADXL_I2C_ADDR) && (ADXL_I2C_ADDR < 0x08 || ADXL_I2C_ADDR > 0x77)
#error "ADXL_I2C_ADDR ist keine gueltige 7-Bit-I2C-Adresse (ueblich: 0x53 oder 0x1D)"
#endif
