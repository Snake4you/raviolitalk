#include "ring_gesture.h"

#include <cassert>
#include <cmath>

namespace {
constexpr float kGravity = 256.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kSampleMs = 20; // 50 Hz like adxl_task

// Crank: acceleration vector rotating in a circle (1.5 Hz, 0.9 g) plus gravity.
GravityVector crank_sample(int i) {
    const float t = (float)(i * kSampleMs) / 1000.0f;
    const float phase = 2.0f * kPi * 1.5f * t;
    return {cosf(phase) * 0.9f * kGravity, sinf(phase) * 0.9f * kGravity, kGravity};
}

void test_resting_can_never_rings() {
    CrankDetector crank;
    for (int i = 0; i < 500; i++) {
        const float noise = (i % 2) ? 3.0f : -3.0f;
        assert(!crank.update({noise, 0.0f, kGravity + noise}, i * kSampleMs));
    }
}

void test_short_pick_up_does_not_ring() {
    CrankDetector crank;
    int i = 0;
    for (; i < 50; i++) assert(!crank.update({0.0f, 0.0f, kGravity}, i * kSampleMs));
    // 0.3 s of strong movement (picking the can up), then still again
    for (int k = 0; k < 15; k++, i++) {
        const float jolt = (k % 2) ? 120.0f : -120.0f;
        assert(!crank.update({jolt, 0.0f, kGravity}, i * kSampleMs));
    }
    for (int k = 0; k < 100; k++, i++) assert(!crank.update({0.0f, 0.0f, kGravity}, i * kSampleMs));
}

void test_cranking_rings_once_then_cools_down() {
    CrankDetector crank;
    int triggers = 0;
    int first_trigger = -1;
    // 2 s of continuous cranking: one trigger, cooldown prevents a second
    for (int i = 0; i < 100; i++) {
        if (crank.update(crank_sample(i), i * kSampleMs)) {
            triggers++;
            if (first_trigger < 0) first_trigger = i;
        }
    }
    assert(triggers == 1);
    assert(first_trigger >= 30 && first_trigger <= 50); // after ~0.6-1 s of cranking

    // Cranking again 5 s later is still inside the 10 s cooldown: no ring
    int during = 0;
    for (int i = 250; i < 350; i++) {
        if (crank.update(crank_sample(i), i * kSampleMs)) during++;
    }
    assert(during == 0);

    // After the cooldown (12 s), cranking again rings again
    int later = 0;
    for (int i = 600; i < 700; i++) {
        if (crank.update(crank_sample(i), i * kSampleMs)) later++;
    }
    assert(later == 1);
}

void test_upside_down_needs_stable_position_and_has_hysteresis() {
    UpsideDownDetector det;
    assert(!det.update(170.0f, false, 0));
    assert(!det.update(170.0f, false, 299));
    assert(det.update(170.0f, false, 300));

    // Moving does not change the state
    assert(det.update(10.0f, true, 400));
    // 125 deg is still inside the hysteresis band
    assert(det.update(125.0f, false, 500));
    assert(det.update(125.0f, false, 1500));
    // Upright again: leaves after being stable
    assert(det.update(10.0f, false, 2000));
    assert(!det.update(10.0f, false, 2300));
}
void test_reset_discards_movement_so_far() {
    CrankDetector crank;
    for (int i = 0; i < 25; i++) assert(!crank.update(crank_sample(i), i * kSampleMs));
    crank.reset();
    assert(crank.score() == 0);
    // Needs a full crank again after the reset
    for (int i = 25; i < 50; i++) assert(!crank.update(crank_sample(i), i * kSampleMs));
}
} // namespace

int main() {
    test_resting_can_never_rings();
    test_short_pick_up_does_not_ring();
    test_cranking_rings_once_then_cools_down();
    test_reset_discards_movement_so_far();
    test_upside_down_needs_stable_position_and_has_hysteresis();
    return 0;
}
