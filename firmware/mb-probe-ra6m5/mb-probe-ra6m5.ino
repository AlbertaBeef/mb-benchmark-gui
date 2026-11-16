// mb-probe-ra6m5 — telemetry firmware for the SparkFun Thing Plus RA6M5.
//
// The board samples its sensors far faster than the host's 1 Hz tick and
// reports, once per period, what happened INSIDE that period: mean / min / max
// and a sample count per quantity. The host (`RA6M5Probe` in src/Probes.cpp)
// only parses. See docs/ra6m5-probe.md for the wire protocol; the summary is:
//
//   device -> host, one line each, '\n'-terminated, space-separated key=value:
//     I ...          identity / sensor / channel declarations, ends "I end"
//     D seq=.. t_ms=.. <channel>=<value> ...   one per period, every declared
//                    channel every time, "nan" when there was no valid sample
//     OK <cmd> | E <message>
//   host -> device:
//     info | period <ms> | reset_acc | rescan
//
// Reboot into the DFU bootloader with the 1200-baud touch (open the port at
// 1200 baud, drop DTR) — the core implements it; tools/flash-ra6m5.sh uses it.
//
// Sensors today: INA228 (0x40-0x4F), FS3000-1005 (0x28). Planned: EMC2101 fan
// controller (0x4C), MAX31855K thermocouple on SPI.

#include <Arduino.h>
#include <Wire.h>
#include "Ina228.h"
#include "Fs3000.h"

static const char* kFwName = "mb-probe-ra6m5";
static const char* kFwVersion = "0.1.0";
static const int kProtocol = 1;

static Ina228 ina;
static Fs3000 fs;

static uint32_t period_ms = 1000;
static uint32_t seq = 0;
static uint32_t last_report = 0, last_ina = 0, last_fs = 0;
static bool led = false;

// ---------------------------------------------------------------- output ---
static void kv(const char* key, double v, int digits) {
    Serial.print(' ');
    Serial.print(key);
    Serial.print('=');
    if (isnan(v)) Serial.print("nan");
    else Serial.print(v, digits);
}
static void kv(const char* key, long v) {
    Serial.print(' ');
    Serial.print(key);
    Serial.print('=');
    Serial.print(v);
}

// One channel declaration. Every key printed in a D line must appear here, in
// the same order, so the host can fix its metric list (and the CSV header)
// before the first data line arrives.
static void declare(const char* ch, const char* unit) {
    Serial.print("I ch=");
    Serial.print(ch);
    Serial.print(" unit=");
    Serial.println(unit);
}

static void print_info() {
    Serial.print("I proto=");
    Serial.print(kProtocol);
    Serial.print(" fw=");
    Serial.print(kFwName);
    Serial.print(" ver=");
    Serial.print(kFwVersion);
    Serial.print(" period_ms=");
    Serial.println(period_ms);

    Serial.print("I sensor=ina228 present=");
    Serial.print(ina.present ? 1 : 0);
    Serial.print(" addr=0x");
    Serial.print(ina.addr, HEX);
    Serial.print(" shunt_ohm=");
    Serial.print(Ina228::kShuntOhm, 4);
    Serial.print(" imax_a=");
    Serial.print(Ina228::kMaxAmps, 1);
    Serial.print(" buvl_v=");
    Serial.println(Ina228::kUndervoltV, 3);

    Serial.print("I sensor=fs3000 present=");
    Serial.print(fs.present ? 1 : 0);
    Serial.println(" addr=0x28 model=1005 range_mps=7.23");

    if (ina.present) {
        declare("ina228.vbus", "V");
        declare("ina228.vbus_min", "V");
        declare("ina228.vbus_max", "V");
        declare("ina228.current", "A");
        declare("ina228.current_min", "A");
        declare("ina228.current_max", "A");
        declare("ina228.power", "W");
        declare("ina228.power_min", "W");
        declare("ina228.power_max", "W");
        declare("ina228.dietemp", "C");
        declare("ina228.energy", "J");
        declare("ina228.charge", "Coulomb");
        declare("ina228.uv", "count");
        declare("ina228.n", "count");
        declare("ina228.err", "count");
    }
    if (fs.present) {
        declare("fs3000.airflow", "m/s");
        declare("fs3000.airflow_min", "m/s");
        declare("fs3000.airflow_max", "m/s");
        declare("fs3000.raw", "count");
        declare("fs3000.n", "count");
        declare("fs3000.err", "count");
    }
    Serial.println("I end");
}

static void report(uint32_t now) {
    Serial.print("D");
    kv("seq", (long)seq++);
    kv("t_ms", (long)now);
    if (ina.present) {
        kv("ina228.vbus", ina.vbus.mean(), 6);
        kv("ina228.vbus_min", ina.vbus.lo, 6);
        kv("ina228.vbus_max", ina.vbus.hi, 6);
        kv("ina228.current", ina.current.mean(), 6);
        kv("ina228.current_min", ina.current.lo, 6);
        kv("ina228.current_max", ina.current.hi, 6);
        kv("ina228.power", ina.power.mean(), 6);
        kv("ina228.power_min", ina.power.lo, 6);
        kv("ina228.power_max", ina.power.hi, 6);
        kv("ina228.dietemp", ina.dietemp.mean(), 3);
        kv("ina228.energy", ina.energy_j(), 6);
        kv("ina228.charge", ina.charge_c(), 6);
        kv("ina228.uv", (long)ina.uv_trips);
        kv("ina228.n", (long)ina.vbus.n);
        kv("ina228.err", (long)ina.i2c_errors);
        ina.reset_stats();
    }
    if (fs.present) {
        kv("fs3000.airflow", fs.airflow.mean(), 3);
        kv("fs3000.airflow_min", fs.airflow.lo, 3);
        kv("fs3000.airflow_max", fs.airflow.hi, 3);
        kv("fs3000.raw", (long)fs.last_raw);
        kv("fs3000.n", (long)fs.airflow.n);
        kv("fs3000.err", (long)fs.errors);
        fs.reset_stats();
    }
    Serial.println();
}

// ----------------------------------------------------------------- input ---
static void scan_sensors() {
    // INA228 strap addresses span 0x40-0x4F; take the first that identifies
    // itself as an INA228 (DEVICE_ID), so an EMC2101 at 0x4C is never mistaken
    // for one.
    ina.present = false;
    for (uint8_t a = 0x40; a <= 0x4F && !ina.present; ++a) ina.begin(a);
    fs.begin();
}

static void handle(char* line) {
    char* arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    if (!strcmp(line, "info")) {
        print_info();
    } else if (!strcmp(line, "period")) {
        const long v = arg ? atol(arg) : 0;
        if (v < 100 || v > 60000) { Serial.println("E period must be 100..60000 ms"); return; }
        period_ms = (uint32_t)v;
        Serial.println("OK period");
    } else if (!strcmp(line, "reset_acc")) {
        Serial.println(ina.reset_accumulators() ? "OK reset_acc" : "E reset_acc failed");
    } else if (!strcmp(line, "rescan")) {
        scan_sensors();
        Serial.println("OK rescan");
        print_info();  // the channel list may have changed
    } else if (line[0]) {
        Serial.print("E unknown command: ");
        Serial.println(line);
    }
}

static void poll_input() {
    static char buf[64];
    static size_t len = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = '\0'; handle(buf); len = 0; }
        else if (len < sizeof(buf) - 1) buf[len++] = c;
    }
}

// ------------------------------------------------------------------ main ---
void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);  // USB CDC: the rate is nominal
    Wire.begin();
    Wire.setClock(400000);
    scan_sensors();
    last_report = last_ina = last_fs = millis();
}

void loop() {
    const uint32_t now = millis();
    poll_input();
    if (now - last_ina >= 5) { last_ina = now; ina.service(); }
    if (now - last_fs >= Fs3000::kSamplePeriodMs) { last_fs = now; fs.sample(); }
    if (now - last_report >= period_ms) {
        last_report += period_ms;
        if (now - last_report >= period_ms) last_report = now;  // fell behind: re-origin
        // Only talk when a host holds the port open (DTR); otherwise the
        // statistics just roll over, so a reader that connects gets a clean
        // first period rather than an hour of accumulated samples.
        if (Serial) report(now);
        else { ina.reset_stats(); fs.reset_stats(); }
        digitalWrite(LED_BUILTIN, led = !led);  // STAT heartbeat
    }
}
