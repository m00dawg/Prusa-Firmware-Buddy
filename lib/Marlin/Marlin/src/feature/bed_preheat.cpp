#include "bed_preheat.hpp"

#include "../../Marlin.h"
#include "../../module/temperature.h"
#include "../../gcode/gcode.h"
#include "../../lcd/ultralcd.h"

#if HAS_HEATED_BED

static constexpr int16_t minimal_preheat_temp = 60;
static constexpr int16_t minimal_temp_diff = 15;

void BedPreheat::update() {
    bool temp_near_target = thermalManager.degTargetBed() && std::abs(thermalManager.degBed() - thermalManager.degTargetBed()) < minimal_temp_diff;

    bool bedlets_changed = false;
    #if ENABLED(MODULAR_HEATBED)
    if (thermalManager.getEnabledBedletMask() != last_enabled_bedlets) {
        last_enabled_bedlets = thermalManager.getEnabledBedletMask();
        bedlets_changed = true;
    }
    #endif

    if (temp_near_target && bedlets_changed == false) {
        if (heating_start_time.has_value() == false) {
            heating_start_time = millis();
        }
        can_preheat = true;
        if (remaining_preheat_time() == 0) {
            preheated = true;
        }
    } else {
        heating_start_time = std::nullopt;
        can_preheat = false;
        preheated = false;
    }
}

uint32_t BedPreheat::required_preheat_time() {
    if (thermalManager.degTargetBed() < minimal_preheat_temp) {
        return 0;
    } else {
        int32_t time = (180 + (thermalManager.degTargetBed() - 60) * (12 * 60 / 50)) * 1000;
        return time > 0 ? time : 0;
    }
}

uint32_t BedPreheat::remaining_preheat_time() {
    int32_t required = required_preheat_time();
    if (required != 0 && heating_start_time.has_value()) {
        int32_t elapsed = millis() - heating_start_time.value();
        int32_t remaining = required - elapsed;
        return remaining < 0 ? 0 : remaining;
    } else {
        return 0;
    }
}

void BedPreheat::wait_for_preheat() {
    assert(waiting == false);
    waiting = true;
    static constexpr uint32_t message_interval = 1000;
    uint32_t last_message_timestamp = millis() - message_interval;

    while (can_preheat && !preheated) {
        idle(true);

        // make sure we don't turn off the motors
        gcode.reset_stepper_timeout();

        if (millis() - last_message_timestamp > message_interval) {
            uint32_t remaining_seconds = remaining_preheat_time() / 1000;
            MarlinUI::status_printf_P(0, "Absorbing heat (%is)", remaining_seconds);
            last_message_timestamp = millis();
        }
    }

    MarlinUI::reset_status();
    waiting = false;
}

bool BedPreheat::can_skip() {
    return can_preheat && !preheated;
}

bool BedPreheat::is_waiting() {
    return waiting;
}

void BedPreheat::skip_preheat() {
    if (can_preheat) {
        preheated = true;
    }
}

BedPreheat bed_preheat;

#endif
