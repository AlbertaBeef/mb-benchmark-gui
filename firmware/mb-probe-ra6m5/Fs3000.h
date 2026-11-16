// Renesas FS3000-1005 air-velocity sensor (SparkFun breakout), fixed I2C
// address 0x28, range 0-7.23 m/s.
//
// A read is 5 bytes: [0] checksum, [1] data high (low nibble valid),
// [2] data low, [3..4] padding. The frame is valid when all five bytes sum to
// 0 mod 256. The raw 12-bit count spans 409 (still air) .. 3686 (full scale)
// and maps to m/s through the datasheet's piecewise-linear curve — the table
// below is the one SparkFun's library uses for the -1005 part.
//
// Deliberately unlike that library, a checksum failure yields NAN rather than
// a value: a corrupted reading is a gap, never a number.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Stat.h"

class Fs3000 {
public:
    static constexpr uint8_t kAddr = 0x28;
    // The part's response time is 125 ms; sampling faster only re-reads the
    // same value.
    static constexpr uint32_t kSamplePeriodMs = 125;

    bool present = false;
    Stat airflow;              // m/s
    int last_raw = -1;         // latest raw count, for calibration checks
    unsigned errors = 0;       // checksum or bus failures this period

    bool begin() {
        Wire.beginTransmission(kAddr);
        present = Wire.endTransmission() == 0;
        return present;
    }

    void sample() {
        if (!present) return;
        uint8_t b[5];
        if (Wire.requestFrom(kAddr, (uint8_t)5) != 5) { ++errors; return; }
        uint8_t sum = 0;
        for (int i = 0; i < 5; ++i) { b[i] = Wire.read(); sum += b[i]; }
        if (sum != 0) { ++errors; return; }
        const int raw = ((b[1] & 0x0F) << 8) | b[2];
        last_raw = raw;
        airflow.add(to_mps(raw));
    }

    void reset_stats() { airflow.reset(); errors = 0; }

    static double to_mps(int raw) {
        static const int kRaw[] = {409, 915, 1522, 2066, 2523, 2908, 3256, 3572, 3686};
        static const double kMps[] = {0, 1.07, 2.01, 3.00, 3.97, 4.96, 5.98, 6.99, 7.23};
        constexpr int n = sizeof(kRaw) / sizeof(kRaw[0]);
        if (raw <= kRaw[0]) return 0.0;
        if (raw >= kRaw[n - 1]) return kMps[n - 1];
        int i = 0;
        while (raw > kRaw[i + 1]) ++i;
        return kMps[i] + (kMps[i + 1] - kMps[i]) *
                             (double)(raw - kRaw[i]) / (kRaw[i + 1] - kRaw[i]);
    }
};
