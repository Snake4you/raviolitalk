#pragma once

#include <cmath>
#include <cstdint>

#include "tilt_trigger.h"

#ifndef RING_SEND_COOLDOWN_MS
#define RING_SEND_COOLDOWN_MS 10000  // after a crank no new ring for this long
#endif

// Kurbel-Geste: die Dose ein paar Mal im Kreis schwenken (wie die Kurbel eines
// alten Telefons). Erkannt wird anhaltend starke Aenderung der Beschleunigung
// zwischen zwei Messungen (50 Hz, ungefilterte Rohwerte, 256 LSB = 1 g).
// Kurzes Anheben oder Abstellen reicht nicht, erst ca. 1 s Kurbeln loest aus.
class CrankDetector {
public:
    static constexpr float kDeltaThreshold = 40.0f;   // LSB je Messung (~0.16 g)
    static constexpr int kTriggerScore = 30;          // Netto-Messungen mit Bewegung
    static constexpr int kMaxScore = 40;
    static constexpr uint32_t kCooldownMs = RING_SEND_COOLDOWN_MS; // danach erst wieder ausloesbar

    // Returns true once per detected crank gesture.
    bool update(const GravityVector& raw, uint32_t now_ms) {
        if (!m_have_prev) {
            m_prev = raw;
            m_have_prev = true;
            return false;
        }
        const float dx = raw.x - m_prev.x;
        const float dy = raw.y - m_prev.y;
        const float dz = raw.z - m_prev.z;
        m_prev = raw;
        m_last_delta = sqrtf(dx * dx + dy * dy + dz * dz);

        if (m_last_delta >= kDeltaThreshold) {
            if (m_score < kMaxScore) m_score++;
        } else if (m_score > 0) {
            m_score--;
        }

        const bool cooling_down = m_triggered_once && (now_ms - m_last_trigger_ms) < kCooldownMs;
        if (!cooling_down && m_score >= kTriggerScore) {
            m_triggered_once = true;
            m_last_trigger_ms = now_ms;
            m_score = 0;
            return true;
        }
        return false;
    }

    // Forget the movement so far (e.g. while the can vibrates from its own ring tone)
    void reset() {
        m_have_prev = false;
        m_score = 0;
    }

    int score() const { return m_score; }
    float last_delta() const { return m_last_delta; }

private:
    GravityVector m_prev{0.0f, 0.0f, 0.0f};
    bool m_have_prev{false};
    int m_score{0};
    float m_last_delta{0.0f};
    bool m_triggered_once{false};
    uint32_t m_last_trigger_ms{0};
};

// "Auf dem Kopf": Neigung gegenueber der kalibrierten Dosenachse ueber 135 Grad,
// ruhig fuer mindestens 300 ms. Zurueck erst unter 120 Grad (Hysterese).
class UpsideDownDetector {
public:
    static constexpr float kEnterDegrees = 135.0f;
    static constexpr float kExitDegrees = 120.0f;
    static constexpr uint32_t kStableTimeMs = 300;

    bool update(float tilt_degrees, bool moving, uint32_t now_ms) {
        if (moving) {
            m_candidate_valid = false;
            return m_active;
        }
        const bool desired = m_active ? tilt_degrees > kExitDegrees : tilt_degrees >= kEnterDegrees;
        if (desired == m_active) {
            m_candidate_valid = false;
            return m_active;
        }
        if (!m_candidate_valid) {
            m_candidate_valid = true;
            m_candidate_since_ms = now_ms;
            return m_active;
        }
        if (now_ms - m_candidate_since_ms >= kStableTimeMs) {
            m_active = desired;
            m_candidate_valid = false;
        }
        return m_active;
    }

    bool active() const { return m_active; }

private:
    bool m_active{false};
    bool m_candidate_valid{false};
    uint32_t m_candidate_since_ms{0};
};
