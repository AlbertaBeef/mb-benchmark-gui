// Microchip EMC2101 fan controller + temperature sensor (Adafruit breakout),
// fixed I2C address 0x4C.
//
// READ-ONLY BY DESIGN, with one exception. The duty cycle, the look-up table
// and the PWM setup are left exactly as the chip has them: driving the fan is
// the first thing this project would write to hardware that changes the
// physical world, and that is a separate, deliberate step. The exception is
// CONFIG.ALT_TCH (0x03 bit 2), which turns pin 6 from ALERT into the TACH
// input — without it the tach count is meaningless. It changes what the chip
// measures, not what it drives, like the INA228's ADC configuration.
//
// Unlike Adafruit's library, begin() therefore does NOT set the duty to 100%,
// the PWM frequency or the data rate.
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
