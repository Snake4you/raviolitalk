#pragma once

#include <cmath>
#include <cstdint>

struct GravityVector {
    float x;
    float y;
    float z;
};

class TiltTrigger {
public:
    static constexpr float kEnterDegrees = 35.0f;
    static constexpr float kEnterHighDegrees = 70.0f;   // start only up to here
    static constexpr float kExitLowDegrees = 25.0f;
    static constexpr float kExitHighDegrees = 75.0f;    // stop above this
    static constexpr uint32_t kStableTimeMs = 100;
    static constexpr float kGravityMagnitude = 256.0f;
    static constexpr float kMotionTolerance = 0.25f;

    void calibrate_axis(const GravityVector& gravity_down_when_upright) {
        m_dose_axis_down = normalize(gravity_down_when_upright);
        m_calibrated = magnitude(m_dose_axis_down) > 0.0f;
        m_active = false;
        m_candidate_valid = false;
    }

    bool update(const GravityVector& gravity, uint32_t now_ms) {
        if (!m_calibrated || is_moving(gravity)) {
            m_candidate_valid = false;
            return m_active;
        }

        const float angle = tilt_degrees(gravity);
        const bool desired = m_active
            ? angle > kExitLowDegrees && angle < kExitHighDegrees
            : angle >= kEnterDegrees && angle <= kEnterHighDegrees;

        if (desired == m_active) {
            m_candidate_valid = false;
            return m_active;
        }

        if (!m_candidate_valid || m_candidate_state != desired) {
            m_candidate_valid = true;
            m_candidate_state = desired;
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
    bool calibrated() const { return m_calibrated; }
    static bool moving(const GravityVector& gravity) { return is_moving(gravity); }

    float tilt_degrees(const GravityVector& gravity) const {
        return angle_degrees(normalize(gravity), m_dose_axis_down);
    }

private:
    static float magnitude(const GravityVector& v) {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    static GravityVector normalize(const GravityVector& v) {
        const float length = magnitude(v);
        if (length < 0.001f) return {0.0f, 0.0f, 0.0f};
        return {v.x / length, v.y / length, v.z / length};
    }

    static float angle_degrees(const GravityVector& a, const GravityVector& b) {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z;
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        return acosf(dot) * 57.2957795f;
    }

    static bool is_moving(const GravityVector& gravity) {
        const float length = magnitude(gravity);
        const float lower = kGravityMagnitude * (1.0f - kMotionTolerance);
        const float upper = kGravityMagnitude * (1.0f + kMotionTolerance);
        return length < lower || length > upper;
    }

    GravityVector m_dose_axis_down{0.0f, 0.0f, 1.0f};
    bool m_calibrated{false};
    bool m_active{false};
    bool m_candidate_valid{false};
    bool m_candidate_state{false};
    uint32_t m_candidate_since_ms{0};
};
