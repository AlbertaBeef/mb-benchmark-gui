// MAX31855K thermocouple-to-digital converter (Adafruit breakout), on SPI.
//
// Read-only by construction: the chip has no registers and no input pin
// besides CS/SCK — pulling CS low freezes the latest conversion and 32 bits
// come out on MISO. Nothing is ever written to it.
//
// Wiring: SCK on D19 (SCK0), DO on D17 (POCI0), CS on D0 — the pin the board
// variant itself labels CS0 (PIN_SPI_CS). Change kCsPin if it is wired
// elsewhere.
//
// Frame (datasheet 19-5793):
//   D31..D18  thermocouple temp, signed 14-bit, 0.25 degC/LSB
//   D17       reserved, always 0
//   D16       fault (any of D2..D0)
//   D15..D4   cold-junction (die) temp, signed 12-bit, 0.0625 degC/LSB
//   D3        reserved, always 0
//   D2 SCV (short to VCC), D1 SCG (short to GND), D0 OC (open circuit)
//
// The two reserved bits are what make presence detectable: an unconnected
// MISO reads all ones (or all zeros), and either way the frame is rejected.
//
// Conversion takes ~100 ms and reading with CS low suspends it, so sampling
// at 4 Hz leaves the chip converting most of the time. The K-type conversion
// is the chip's own linear 41.276 uV/degC, accurate to about +-2 degC up to a
// few hundred degrees — fine for a heatsink, not a calibration standard.
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "Stat.h"

class Max31855 {
public:
    static constexpr int kCsPin = PIN_SPI_CS;          // D0, "CS0"
    static constexpr uint32_t kSamplePeriodMs = 250;

    bool present = false;
    Stat tc, cj;              // degC: thermocouple, cold junction
    unsigned faults = 0;      // samples with D16 set (no tc reading)
    uint8_t fault_bits = 0;   // OR of D2..D0 over the period: 1 OC, 2 SCG, 4 SCV
    unsigned errors = 0;      // frames failing the reserved-bit check

    bool begin() {
        present = false;
        pinMode(kCsPin, OUTPUT);
        digitalWrite(kCsPin, HIGH);
        SPI.begin();
        // Power-up to first valid conversion is up to 200 ms; a few tries
        // also cover a rescan landing mid-conversion.
        for (int i = 0; i < 4 && !present; ++i) {
            uint32_t w;
            if (valid(w = read32())) present = true;
            else delay(100);
        }
        return present;
    }

    void sample() {
        if (!present) return;
        const uint32_t w = read32();
        if (!valid(w)) { ++errors; return; }
        cj.add(((int16_t)(w & 0xFFF0)) / 16 * 0.0625);  // sign-extend bits 15..4
        if (w & 0x00010000) {
            ++faults;
            fault_bits |= w & 0x7;
        } else {
            tc.add(((int32_t)(w & 0xFFFC0000)) / 262144 * 0.25);  // bits 31..18
        }
    }

    void reset_stats() {
        tc.reset(); cj.reset();
        faults = errors = 0;
        fault_bits = 0;
    }

private:
    static bool valid(uint32_t w) {
        return w != 0 && w != 0xFFFFFFFF && !(w & 0x00020008);
    }
    static uint32_t read32() {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(kCsPin, LOW);
        delayMicroseconds(1);
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i) w = (w << 8) | SPI.transfer(0);
        digitalWrite(kCsPin, HIGH);
        SPI.endTransaction();
        return w;
    }
};
