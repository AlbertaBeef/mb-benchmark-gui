// TI INA228 on the Qwiic bus — the same configuration the host-side FT232H
// driver (`ftdi::Ina228` in src/Probes.cpp) writes, so a rail measured through
// this board and one measured through an FT232H are directly comparable:
// 15 mOhm shunt, 5 A full scale, ADC_CONFIG 0xFB6B (continuous bus + shunt +
// temperature, 1052 us per channel, AVG 64 -> ~202 ms per full conversion),
// RSTACC pulsed at start, BUVL armed at 3.00 V with ALATCH.
//
// What differs is WHO reads it. Here the firmware is the only reader of
// DIAG_ALRT, so it polls the conversion-ready flag and takes EVERY conversion
// (~5 Hz) into per-period statistics, and folds every latched undervoltage
// trip into a count. The host-side driver could only read one instant per
// second and had to take care that exactly one DIAG_ALRT read happened per
// poll; that constraint now lives here, in `service()`, and nowhere else.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Stat.h"

class Ina228 {
public:
    static constexpr double kShuntOhm = 0.015;
    static constexpr double kMaxAmps = 5.0;
    static constexpr double kUndervoltV = 3.0;  // M.2 3.3 V rail -9%

    uint8_t addr = 0x40;
    bool present = false;

    // Per-period statistics, reset by the reporter.
    Stat vbus, current, power, dietemp;
    unsigned uv_trips = 0;     // conversions flagged below BUVL this period
    unsigned i2c_errors = 0;   // failed transactions this period

    bool begin(uint8_t a) {
        addr = a;
        present = false;
        if (!ping()) return false;
        uint16_t id = 0;
        if (!read16(kDeviceId, &id) || (id >> 4) != 0x228) return false;
        current_lsb_ = kMaxAmps / 524288.0;  // 2^19
        const uint16_t cal = (uint16_t)(13107200000.0 * current_lsb_ * kShuntOhm + 0.5);
        const uint16_t adc = (0xF << 12) | (5 << 9) | (5 << 6) | (5 << 3) | 3;  // 0xFB6B
        present = write16(kConfig, kRstAcc) && write16(kConfig, 0x0000) &&
                  write16(kAdcConfig, adc) && write16(kShuntCal, cal) &&
                  write16(kBuvl, (uint16_t)(kUndervoltV / kBuvlLsbV + 0.5)) &&
                  write16(kDiagAlrt, kAlatch);
        return present;
    }

    // Clear ENERGY and CHARGE. CONFIG bit 14 is an ordinary R/W bit, so it
    // must be written back to 0 or the accumulators stay pinned at zero.
    bool reset_accumulators() {
        return present && write16(kConfig, kRstAcc) && write16(kConfig, 0x0000);
    }

    // Call often (every few ms). Reads DIAG_ALRT — which clears its latched
    // flags, so this must remain the ONLY reader — and on a completed
    // conversion takes VBUS / CURRENT / POWER / DIETEMP into the statistics.
    void service() {
        if (!present) return;
        uint16_t diag = 0;
        if (!read16(kDiagAlrt, &diag)) { ++i2c_errors; return; }
        if (diag & kBusUnderLimit) ++uv_trips;
        if (!(diag & kConversionReady)) return;

        uint32_t raw = 0;
        uint16_t t = 0;
        if (read24(kVbus, &raw)) vbus.add((raw >> 4) * 195.3125e-6);
        else ++i2c_errors;
        if (read24(kCurrent, &raw)) {
            raw >>= 4;  // 20-bit SIGNED in bits 23:4
            if (raw & (1ul << 19)) raw |= ~((1ul << 20) - 1);
            current.add((double)(int32_t)raw * current_lsb_);
        } else ++i2c_errors;
        if (read24(kPower, &raw)) power.add(raw * 3.2 * current_lsb_);
        else ++i2c_errors;
        if (read16(kDieTemp, &t)) dietemp.add((int16_t)t * 0.0078125);
        else ++i2c_errors;
    }

    // Hardware accumulators, read once per report. NAN on an I2C failure.
    double energy_j() {
        uint64_t v;
        if (!present || !read40(kEnergy, &v)) return NAN;
        return (double)v * 16.0 * 3.2 * current_lsb_;
    }
    double charge_c() {
        uint64_t v;
        if (!present || !read40(kCharge, &v)) return NAN;
        if (v & (1ull << 39)) v |= ~((1ull << 40) - 1);  // 40-bit signed
        return (double)(int64_t)v * current_lsb_;
    }

    void reset_stats() {
        vbus.reset(); current.reset(); power.reset(); dietemp.reset();
        uv_trips = 0; i2c_errors = 0;
    }

private:
    enum : uint8_t {
        kConfig = 0x00, kAdcConfig = 0x01, kShuntCal = 0x02, kVbus = 0x05,
        kDieTemp = 0x06, kCurrent = 0x07, kPower = 0x08, kEnergy = 0x09,
        kCharge = 0x0A, kDiagAlrt = 0x0B, kBuvl = 0x0F, kDeviceId = 0x3F,
    };
    static constexpr uint16_t kRstAcc = 0x4000;
    static constexpr uint16_t kAlatch = 0x8000;
    static constexpr uint16_t kBusUnderLimit = 1u << 3;
    static constexpr uint16_t kConversionReady = 1u << 1;  // CNVRF
    static constexpr double kBuvlLsbV = 3.125e-3;  // measured 2026-09-07
    double current_lsb_ = 0;

    bool ping() {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }
    bool write16(uint8_t reg, uint16_t v) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.write((uint8_t)(v >> 8));
        Wire.write((uint8_t)(v & 0xFF));
        return Wire.endTransmission() == 0;
    }
    bool read_n(uint8_t reg, uint8_t* out, uint8_t n) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return false;  // repeated start
        if (Wire.requestFrom(addr, n) != n) return false;
        for (uint8_t i = 0; i < n; ++i) out[i] = Wire.read();
        return true;
    }
    bool read16(uint8_t reg, uint16_t* v) {
        uint8_t b[2];
        if (!read_n(reg, b, 2)) return false;
        *v = (uint16_t)((b[0] << 8) | b[1]);
        return true;
    }
    bool read24(uint8_t reg, uint32_t* v) {
        uint8_t b[3];
        if (!read_n(reg, b, 3)) return false;
        *v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
        return true;
    }
    bool read40(uint8_t reg, uint64_t* v) {
        uint8_t b[5];
        if (!read_n(reg, b, 5)) return false;
        uint64_t x = 0;
        for (int i = 0; i < 5; ++i) x = (x << 8) | b[i];
        *v = x;
        return true;
    }
};
