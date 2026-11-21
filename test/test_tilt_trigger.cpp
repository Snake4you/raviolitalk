#include "tilt_trigger.h"

#include <cassert>
#include <cmath>

namespace {
constexpr float kGravity = 256.0f;
constexpr float kPi = 3.14159265358979323846f;

GravityVector tilted_about_x(float degrees, float magnitude = kGravity) {
    const float radians = degrees * kPi / 180.0f;
    return {sinf(radians) * magnitude, 0.0f, cosf(radians) * magnitude};
}

GravityVector tilted_about_y(float degrees, float magnitude = kGravity) {
    const float radians = degrees * kPi / 180.0f;
    return {0.0f, sinf(radians) * magnitude, cosf(radians) * magnitude};
}

void test_transmit_window_requires_stability() {
    TiltTrigger trigger;
    trigger.calibrate_axis({0.0f, 0.0f, kGravity});

    assert(!trigger.update(tilted_about_x(0.0f), 0));
    assert(!trigger.update(tilted_about_x(40.0f), 50));
    assert(!trigger.update(tilted_about_x(40.0f), 149));
    assert(trigger.update(tilted_about_x(40.0f), 150));
}

void test_hysteresis_keeps_transmitting_until_exit_is_stable() {
    TiltTrigger trigger;
    trigger.calibrate_axis({0.0f, 0.0f, kGravity});
    assert(!trigger.update(tilted_about_x(40.0f), 0));
    assert(trigger.update(tilted_about_x(40.0f), 100));

    assert(trigger.update(tilted_about_x(30.0f), 300));
    assert(trigger.update(tilted_about_x(20.0f), 200));
    assert(trigger.update(tilted_about_x(20.0f), 299));
    assert(!trigger.update(tilted_about_x(20.0f), 300));

    assert(!trigger.update(tilted_about_x(40.0f), 400));
    assert(trigger.update(tilted_about_x(40.0f), 500));
    // 72 deg: inside the 70..75 hysteresis band, keeps transmitting
    assert(trigger.update(tilted_about_x(72.0f), 550));
    assert(trigger.update(tilted_about_x(72.0f), 1000));
    assert(trigger.update(tilted_about_x(78.0f), 1100));
    assert(trigger.update(tilted_about_x(78.0f), 1199));
    assert(!trigger.update(tilted_about_x(78.0f), 1200));
}

void test_upper_start_limit_is_70_degrees() {
    TiltTrigger trigger;
    trigger.calibrate_axis({0.0f, 0.0f, kGravity});
    // 72 deg is above the start limit: does not start
    assert(!trigger.update(tilted_about_x(72.0f), 0));
    assert(!trigger.update(tilted_about_x(72.0f), 500));
    // 68 deg starts
    assert(!trigger.update(tilted_about_x(68.0f), 600));
    assert(trigger.update(tilted_about_x(68.0f), 700));
}

void test_rotation_around_the_dose_axis_does_not_change_trigger() {
    TiltTrigger trigger;
    trigger.calibrate_axis({0.0f, 0.0f, kGravity});

    assert(fabsf(trigger.tilt_degrees(tilted_about_x(40.0f)) - 40.0f) < 0.1f);
    assert(fabsf(trigger.tilt_degrees(tilted_about_y(40.0f)) - 40.0f) < 0.1f);
    assert(!trigger.update(tilted_about_x(40.0f), 0));
    assert(trigger.update(tilted_about_y(40.0f), 200));
}

void test_motion_does_not_change_state() {
    TiltTrigger trigger;
    trigger.calibrate_axis({0.0f, 0.0f, kGravity});

    assert(!trigger.update(tilted_about_x(40.0f, 400.0f), 0));
    assert(!trigger.update(tilted_about_x(40.0f, 400.0f), 500));
    assert(!trigger.update(tilted_about_x(40.0f), 600));
    assert(trigger.update(tilted_about_x(40.0f), 700));
}
}  // namespace

int main() {
    test_transmit_window_requires_stability();
    test_upper_start_limit_is_70_degrees();
    test_hysteresis_keeps_transmitting_until_exit_is_stable();
    test_rotation_around_the_dose_axis_does_not_change_trigger();
    test_motion_does_not_change_state();
}
