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
//     info | period <ms> | reset_acc | rescan | fan <percent>
//     ctl | ctl.mode off|on | ctl.tc on|off | ctl.nn on|off
//     ctl.target <C>|auto | ctl.rint <C/W> | ctl.kp <v> | ctl.ki <v>
//     ctl.min <%> | ctl.max <%>
//
// Reboot into the DFU bootloader with the 1200-baud touch (open the port at
// 1200 baud, drop DTR) — the core implements it; tools/flash-ra6m5.sh uses it.
//
// Sensors today: INA228 (0x40-0x4F), FS3000-1005 (0x28), EMC2101 fan
// controller (0x4C), MAX31855K thermocouple on SPI (CS = D0).
//
// Since 0.5.0 the fan can also be driven by a closed loop running HERE rather
// than on the host — see Control.h for why, and for the provenance of every
// constant in it. While the loop is on the host's `fan` command is refused:
// two writers on one register is a bug to design out, not to debug later.
//
// 0.6.0 collapses that loop to ONE mode. The setpoint is always the estimated
// on-die temperature; power is mandatory and the thermocouple is substitutable,
// with `ctl.src` reporting which of the three sources the loop actually used.
// Control.h has the reduction that showed the old modes were one control law.

#include <Arduino.h>
#include <Wire.h>
#include "Ina228.h"
#include "Fs3000.h"
#include "Emc2101.h"
#include "Max31855.h"
#include "Control.h"

static const char* kFwName = "mb-probe-ra6m5";
// 0.5.0 added the on-board control loop and its ctl.* channels, additively.
// 0.6.0 bumps kProtocol to 2: no channel is removed, but `ctl.src` now reports
// the regulated signal's PROVENANCE (measured / inferred / none) where it used
// to report die-vs-heatsink, and `ctl.mode` has two values rather than three.
// A host reading an 0.6.0 board with 0.5.0 assumptions would misread both, so
// the version has to say so.
static const char* kFwVersion = "0.6.0";
static const int kProtocol = 2;

static Ina228 ina;
static Fs3000 fs;
static Emc2101 emc;
static Max31855 tcx;
static Control ctl;

static uint32_t period_ms = 1000;
static uint32_t seq = 0;
static uint32_t last_report = 0, last_ina = 0, last_fs = 0, last_emc = 0, last_tc = 0;
static uint32_t last_ctl = 0;
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

    Serial.print("I sensor=emc2101 present=");
    Serial.print(emc.present ? 1 : 0);
    Serial.print(" addr=0x4C product=0x");
    Serial.print(emc.product, HEX);
    Serial.print(" fan_config=0x");
    Serial.print(emc.fan_config, HEX);
    Serial.println(" control=host");

    Serial.print("I sensor=max31855 present=");
    Serial.print(tcx.present ? 1 : 0);
    Serial.print(" bus=spi cs=D");
    Serial.print(Max31855::kCsPin);
    Serial.println(" type=K");

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
    if (emc.present) {
        declare("emc2101.temp_int", "C");
        declare("emc2101.temp_ext", "C");
        declare("emc2101.rpm", "rpm");
        declare("emc2101.rpm_min", "rpm");
        declare("emc2101.rpm_max", "rpm");
        declare("emc2101.duty", "%");
        declare("emc2101.stall", "count");
        declare("emc2101.fault", "count");
        declare("emc2101.n", "count");
        declare("emc2101.err", "count");
    }
    if (tcx.present) {
        declare("max31855.tc", "C");
        declare("max31855.tc_min", "C");
        declare("max31855.tc_max", "C");
        declare("max31855.cj", "C");
        declare("max31855.fault", "count");
        declare("max31855.fault_bits", "mask");
        declare("max31855.n", "count");
        declare("max31855.err", "count");
    }
    // The control channels are declared whenever the loop COULD run, never
    // because it happens to be running. The host fixes its metric list and its
    // CSV header from this handshake and never reconnects, so a channel that
    // appeared later — when someone switched the loop on — would be a column
    // that board can never grow. They read `nan` while the mode is off.
    if (emc.present && tcx.present) {
        declare("ctl.mode", "enum");    // 0 off, 1 on
        declare("ctl.src", "enum");     // 0 measured, 1 inferred, 2 none
        declare("ctl.state", "enum");   // 0 off 1 settling 2 holding 3 floor
                                        // 4 ceiling 5 open 6 degraded 7 fault
        declare("ctl.tc", "bool");      // EFFECTIVE, not requested
        declare("ctl.nn", "bool");      // EFFECTIVE: 0 while no model is loaded
        declare("ctl.auto", "bool");    // was the setpoint defaulted or chosen
        declare("ctl.target", "C");
        declare("ctl.in", "C");         // the quantity actually regulated
        declare("ctl.die_est", "C");    // the board's model of the die
        declare("ctl.err", "C");        // in - target; positive means too hot
        declare("ctl.duty", "%");
        declare("ctl.ff", "%");         // feedforward alone
        declare("ctl.integ", "%");
        declare("ctl.sat", "count");
    }
    if (emc.present && tcx.present) ctl.print_status();
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
    if (emc.present) {
        kv("emc2101.temp_int", emc.temp_int.mean(), 2);
        kv("emc2101.temp_ext", emc.temp_ext.mean(), 3);
        kv("emc2101.rpm", emc.rpm.mean(), 1);
        kv("emc2101.rpm_min", emc.rpm.lo, 1);
        kv("emc2101.rpm_max", emc.rpm.hi, 1);
        kv("emc2101.duty", emc.duty, 1);
        kv("emc2101.stall", (long)emc.stalls);
        kv("emc2101.fault", (long)emc.faults);
        kv("emc2101.n", (long)emc.temp_int.n);
        kv("emc2101.err", (long)emc.errors);
        emc.reset_stats();
    }
    if (tcx.present) {
        kv("max31855.tc", tcx.tc.mean(), 2);
        kv("max31855.tc_min", tcx.tc.lo, 2);
        kv("max31855.tc_max", tcx.tc.hi, 2);
        kv("max31855.cj", tcx.cj.mean(), 3);
        kv("max31855.fault", (long)tcx.faults);
        kv("max31855.fault_bits", (long)tcx.fault_bits);
        kv("max31855.n", (long)(tcx.tc.n + tcx.faults));
        kv("max31855.err", (long)tcx.errors);
        tcx.reset_stats();
    }
    // Declared whenever the loop could run, so emitted on exactly the same
    // condition — every declared key must appear in every D line.
    if (emc.present && tcx.present) {
        kv("ctl.mode", (long)ctl.mode);
        kv("ctl.src", (long)ctl.src);
        kv("ctl.state", (long)ctl.state);
        // The EFFECTIVE flags, not what was asked for. `ctl.nn` reads 0 while
        // no model is compiled in, so a run configured for the inferred source
        // cannot be mistaken in the log for one that got it — the state says
        // `degraded` and this says 0.
        kv("ctl.tc", (long)(ctl.src == Control::kMeasured));
        kv("ctl.nn", (long)(ctl.src == Control::kInferred));
        kv("ctl.auto", (long)ctl.target_auto);
        kv("ctl.target", ctl.mode == Control::kOff ? NAN : ctl.target, 2);
        kv("ctl.in", ctl.in_c, 3);
        kv("ctl.die_est", ctl.die_est, 3);
        kv("ctl.err", ctl.err, 3);
        kv("ctl.duty", ctl.duty, 1);
        kv("ctl.ff", ctl.ff, 1);
        kv("ctl.integ", ctl.mode == Control::kOff ? NAN : ctl.integ, 2);
        kv("ctl.sat", (long)ctl.sat);
        ctl.reset_stats();
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
    emc.begin();
    tcx.begin();
}

// `ctl.<key> <value>`. Dotted rather than `ctl <key> <value>` so the existing
// single-space split does the work, and so the names match the automation
// plan's `fan.<knob>` keys one for one.
static void handle_ctl(const char* key, const char* arg) {
    if (!emc.present || !tcx.present) {
        Serial.println("E ctl: needs both an EMC2101 and a MAX31855");
        return;
    }
    const bool has = arg && *arg;
    if (!strcmp(key, "mode")) {
        if (!has) { Serial.println("E ctl.mode needs off|on"); return; }
        if (!strcmp(arg, "off")) ctl.set_mode(Control::kOff);
        else if (!strcmp(arg, "on")) ctl.set_mode(Control::kOn);
        else { Serial.println("E ctl.mode must be off|on"); return; }
        Serial.print("OK ctl.mode ");
        Serial.println(arg);
        return;
    }
    // The two sources for the regulated signal. Both may be on: a real sensor
    // is never overridden by an estimate of itself, so `tc` wins and `nn`
    // becomes the fallback for losing it. `ctl.src` reports which was used.
    if (!strcmp(key, "tc") || !strcmp(key, "nn")) {
        const bool is_nn = key[0] == 'n';
        if (!has) {
            Serial.print("E ctl."); Serial.print(key); Serial.println(" needs on|off");
            return;
        }
        bool v;
        if (!strcmp(arg, "on")) v = true;
        else if (!strcmp(arg, "off")) v = false;
        else {
            Serial.print("E ctl."); Serial.print(key); Serial.println(" must be on|off");
            return;
        }
        if (is_nn) ctl.nn_want = v; else ctl.tc_want = v;
        Serial.print("OK ctl."); Serial.print(key);
        Serial.print(' '); Serial.print(arg);
        // Accepted either way, but say so when it will not take effect — the
        // host greys its own control from this, and a plan that asked for the
        // inferred source deserves to know it is not getting it.
        if (is_nn && v && !ctl.nn_ready()) Serial.print(" (no model compiled in)");
        Serial.println();
        return;
    }
    // `auto` restores the measured per-card bound; a number pins it and is
    // recorded as chosen, because the difference is the whole story the moment
    // a card other than the Hailo-8 is in the slot.
    if (!strcmp(key, "target")) {
        if (!has) { Serial.println("E ctl.target needs a temperature or 'auto'"); return; }
        if (!strcmp(arg, "auto")) {
            ctl.set_target_auto();
            Serial.print("OK ctl.target auto ");
            Serial.println(ctl.target, 2);
            return;
        }
        const double v = atof(arg);
        if (!(v >= -40.0 && v <= 150.0)) {
            Serial.println("E ctl.target must be -40.0..150.0 or 'auto'");
            return;
        }
        ctl.set_target(v);
        Serial.print("OK ctl.target ");
        Serial.println(v, 4);
        return;
    }
    // The numeric settings. Each carries its own range; a value outside it is
    // refused rather than clamped, because a plan asking for something the
    // board quietly changed is a plan that cannot be read back later.
    struct Num { const char* k; double* p; double lo, hi; };
    const Num nums[] = {
        {"rint",   &ctl.rint,     0.0,  20.0},
        {"kp",     &ctl.kp,       0.0, 100.0},
        {"ki",     &ctl.ki,       0.0,  10.0},
        {"min",    &ctl.min_duty, 0.0, 100.0},
        {"max",    &ctl.max_duty, 0.0, 100.0},
    };
    for (const Num& n : nums) {
        if (strcmp(key, n.k)) continue;
        if (!has) { Serial.print("E ctl."); Serial.print(key); Serial.println(" needs a value"); return; }
        const double v = atof(arg);
        if (!(v >= n.lo && v <= n.hi)) {
            Serial.print("E ctl."); Serial.print(key);
            Serial.print(" must be "); Serial.print(n.lo, 1);
            Serial.print(".."); Serial.println(n.hi, 1);
            return;
        }
        *n.p = v;
        if (ctl.min_duty > ctl.max_duty) { const double t = ctl.min_duty; ctl.min_duty = ctl.max_duty; ctl.max_duty = t; }
        Serial.print("OK ctl."); Serial.print(key);
        Serial.print(' '); Serial.println(v, 4);
        return;
    }
    Serial.print("E unknown ctl setting: ");
    Serial.println(key);
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
    } else if (!strcmp(line, "fan")) {
        // EMC2101 fan duty, 0..100 %, in 1/63 steps. Kept until changed.
        double applied = 0;
        if (!arg || !*arg) { Serial.println("E fan needs a percentage 0..100"); return; }
        // One writer at a time. Silently losing a manual duty to the next loop
        // step, or vice versa, is the kind of fault nobody can reproduce.
        if (ctl.mode != Control::kOff) {
            Serial.println("E fan: control loop active, set ctl.mode off first");
            return;
        }
        if (!emc.set_duty(atof(arg), &applied)) { Serial.println("E fan: no EMC2101 or I2C failure"); return; }
        Serial.print("OK fan ");
        Serial.println(applied, 1);
    } else if (!strcmp(line, "ctl")) {
        ctl.print_status();
    } else if (!strncmp(line, "ctl.", 4)) {
        handle_ctl(line + 4, arg);
    } else if (!strcmp(line, "emc_regs")) {
        // Read-only register dump for bring-up: "I emc <reg>=<val> ...".
        static const uint8_t regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x10,
                                       0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
                                       0x4C, 0x4D, 0x4E, 0x4F, 0xFD, 0xFE};
        Serial.print("I emc");
        for (uint8_t r : regs) {
            uint8_t v = 0;
            Serial.print(' ');
            Serial.print(r, HEX);
            Serial.print('=');
            if (emc.peek(r, &v)) Serial.print(v, HEX); else Serial.print("err");
        }
        Serial.println();
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
    last_report = last_ina = last_fs = last_emc = last_tc = last_ctl = millis();
}

void loop() {
    const uint32_t now = millis();
    poll_input();
    if (now - last_ina >= 5) { last_ina = now; ina.service(); }
    if (now - last_fs >= Fs3000::kSamplePeriodMs) { last_fs = now; fs.sample(); }
    if (now - last_emc >= Emc2101::kSamplePeriodMs) { last_emc = now; emc.sample(); }
    if (now - last_tc >= Max31855::kSamplePeriodMs) { last_tc = now; tcx.sample(); }
    // DELIBERATELY OUTSIDE the `if (Serial)` branch below: the loop must keep
    // cooling the card when the host has crashed, exited or been unplugged.
    // That is the whole reason it lives on the board rather than on the host.
    if (now - last_ctl >= Control::kPeriodMs) {
        last_ctl = now;
        ctl.step(emc, tcx, ina, now);
    }
    if (now - last_report >= period_ms) {
        last_report += period_ms;
        if (now - last_report >= period_ms) last_report = now;  // fell behind: re-origin
        // Only talk when a host holds the port open (DTR); otherwise the
        // statistics just roll over, so a reader that connects gets a clean
        // first period rather than an hour of accumulated samples.
        if (Serial) report(now);
        else { ina.reset_stats(); fs.reset_stats(); emc.reset_stats(); tcx.reset_stats();
               ctl.reset_stats(); }
        digitalWrite(LED_BUILTIN, led = !led);  // STAT heartbeat
    }
}
