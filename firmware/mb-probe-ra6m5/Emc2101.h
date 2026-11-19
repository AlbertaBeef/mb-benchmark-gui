// Microchip EMC2101 fan controller + temperature sensor (Adafruit breakout),
// fixed I2C address 0x4C.
//
// Two writes, both deliberate:
//   - CONFIG.ALT_TCH (0x03 bit 2) at begin(), which turns pin 6 from ALERT into
//     the TACH input — without it the tach count is meaningless.
//   - the fan setting (0x4C), ONLY on the host's `fan <percent>` command
//     (fw 0.4.0). Nothing else drives the fan: begin() does not touch the duty,
//     the look-up table, the PWM frequency or the data rate (Adafruit's
//     library forces 100% duty at begin; this does not). The chip powers up at
//     0%, and a set duty is kept until the next command or power cycle — the
//     firmware does NOT stop the fan when the host disconnects, because a
//     cooling fan that quits with the app is the worse failure.
//
// Direct drive needs FAN_CONFIG.PROG (0x4A bit 5) set, which is the chip's
// power-on default (0x20): the fan-setting register then drives the PWM and
// the look-up table is idle. set_duty() checks rather than assumes it.
//
// Registers (datasheet DS20006703):
//   0x00 internal temp, signed 8-bit degC
//   0x01 external diode temp MSB (signed degC), 0x10 LSB bits 7:5 = 0.125 degC
//        — reading the MSB latches the LSB. 0x7F/0xE0 means diode fault.
//   0x02 status (bit 2 = external diode fault)
//   0x03 config (bit 2 ALT_TCH)
//   0x46/0x47 tach count LSB/MSB — LSB first, it latches the MSB.
//        RPM = 5 400 000 / count; 0xFFFF = stalled or no tach signal.
//   0x4A fan config, 0x4C fan setting (0..63 -> duty 0..100%)
//   0xFD product id (0x16 EMC2101, 0x28 EMC2101-R), 0xFE manufacturer (0x5D)
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Stat.h"

class Emc2101 {
public:
    static constexpr uint8_t kAddr = 0x4C;
    static constexpr uint32_t kSamplePeriodMs = 250;  // chip converts at 16 Hz

    bool present = false;
    uint8_t product = 0, fan_config = 0;
    Stat temp_int, temp_ext;  // degC
    Stat rpm;                 // only samples with a valid tach count
    double duty = NAN;        // %, latest readback of the fan setting
    unsigned stalls = 0;      // samples with tach 0xFFFF (stopped / no signal)
    unsigned faults = 0;      // external diode fault samples
    unsigned errors = 0;      // I2C failures

    bool begin() {
        present = false;
        uint8_t mfr = 0;
        if (!read8(0xFE, &mfr) || mfr != 0x5D) return false;
        if (!read8(0xFD, &product) || (product != 0x16 && product != 0x28)) return false;
        uint8_t cfg = 0;
        if (!read8(0x03, &cfg)) return false;
        if (!(cfg & 0x04) && !write8(0x03, cfg | 0x04)) return false;  // ALT_TCH
        read8(0x4A, &fan_config);
        present = true;
        return true;
    }

    void sample() {
        if (!present) return;
        uint8_t ti, st, hi, lo, tl, th, fs;
        if (read8(0x00, &ti)) temp_int.add((int8_t)ti); else ++errors;

        if (read8(0x02, &st) && read8(0x01, &hi) && read8(0x10, &lo)) {
            if ((st & 0x04) || (hi == 0x7F && (lo & 0xE0) == 0xE0)) ++faults;
            else temp_ext.add((int8_t)hi + ((lo >> 5) & 0x7) * 0.125);
        } else ++errors;

        if (read8(0x46, &tl) && read8(0x47, &th)) {
            const uint16_t c = (uint16_t)th << 8 | tl;
            if (c == 0xFFFF || c == 0) ++stalls;
            else rpm.add(5400000.0 / c);
        } else ++errors;

        if (read8(0x4C, &fs)) duty = (fs & 0x3F) * 100.0 / 63.0; else ++errors;
    }

    // pct 0..100 -> fan setting 0..63. Returns the duty actually written.
    bool set_duty(double pct, double* applied) {
        if (!present) return false;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        uint8_t fc = 0;
        if (!read8(0x4A, &fc)) return false;
        if (!(fc & 0x20) && !write8(0x4A, fc | 0x20)) return false;  // PROG
        fan_config = fc | 0x20;
        const uint8_t v = (uint8_t)(pct * 63.0 / 100.0 + 0.5);
        if (!write8(0x4C, v)) return false;
        *applied = v * 100.0 / 63.0;
        return true;
    }

    // Diagnostic: read one register (no side effects except the documented
    // tach LSB->MSB latch, which sample() re-does on its next read).
    bool peek(uint8_t reg, uint8_t* v) { return read8(reg, v); }

    void reset_stats() {
        temp_int.reset(); temp_ext.reset(); rpm.reset();
        stalls = faults = errors = 0;
    }

private:
    static bool read8(uint8_t reg, uint8_t* v) {
        Wire.beginTransmission(kAddr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return false;
        if (Wire.requestFrom(kAddr, (uint8_t)1) != 1) return false;
        *v = Wire.read();
        return true;
    }
    static bool write8(uint8_t reg, uint8_t v) {
        Wire.beginTransmission(kAddr);
        Wire.write(reg);
        Wire.write(v);
        return Wire.endTransmission() == 0;
    }
};
