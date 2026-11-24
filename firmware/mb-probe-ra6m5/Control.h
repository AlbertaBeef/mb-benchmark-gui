// Closed-loop fan control, running on the board.
//
// WHY IT IS HERE AND NOT ON THE HOST. Every input this needs is already on the
// RA6M5 — the heatsink thermocouple, the card's rail watts and board air — so
// the loop keeps protecting the card when the host crashes, exits or is
// unplugged. That is the whole argument; see `step()`, which is deliberately
// called from outside the `if (Serial)` branch in loop().
//
// What the board CANNOT see is the accelerator's core clock and frame rate:
// those are PCIe-side, host-only. So this holds a die temperature believed to
// be below the throttle knee — it cannot detect throttling and react. The host
// still logs `_CLK`, so a setpoint can be audited after any run.
//
// ONE LOOP (0.6.0). 0.5.0 shipped three modes — off / host / smart — with a
// selectable input, die or heatsink. Reading the code back against the
// 225-minute four-setpoint run of 22 Nov showed that two of those were the same
// control law with a different constant, because in die mode
//
//     e = die_est - target = (tc + rint*W) - target = tc - (target - rint*W)
//
// and `(target - rint*W)` is exactly the heatsink target the feedforward
// already computes. The observer is a SETPOINT TRANSFORMATION, not a feedback
// element, so "regulate the die" and "regulate the heatsink" differed only in
// whether that setpoint moved with load. All three configurations shared one
// error path — which is why one defect produced three different failures in
// that run. So: one loop, one setpoint, always the estimated die.
//
// THE THERMOCOUPLE IS NOW SUBSTITUTABLE, AND POWER IS MANDATORY. This reverses
// 0.5.0, where the thermocouple was required and the INA228 optional. The
// regulated quantity is always the estimated die, and `die = heatsink +
// rint * W` needs watts; so does the feedforward. Without watts there is
// nothing to estimate from and nothing to predict with, and the only safe
// output is everything the fan has. Without the thermocouple there is still a
// measured load and a measured fan curve, which is enough to run open-loop.
//
// Three sources for one signal, resolved once per step into `src`:
//
//   kMeasured  the physical thermocouple            -> closed loop, PI active
//   kModelled  a fitted estimate of it               -> closed loop, PI active
//   kNone      neither                               -> OPEN loop, feedforward only
//
// The control law is identical for the first two; only where `tc_` comes from
// changes. That is the point of putting a soft sensor here at all — it is a
// drop-in for one input, not a second controller.
//
// ONE SELECTOR, NOT TWO FLAGS (0.7.0). `ctl.tc` was a bool and `ctl.nn` was a
// second bool, which made four combinations for three meanings and left the
// fourth ("use the real sensor and refuse to fall back") as something nobody
// would ever ask for. It is now a three-valued REQUEST:
//
//   ctl.tc off     no temperature input at all      -> open loop, by choice
//   ctl.tc real    the thermocouple                 -> model, then open, on loss
//   ctl.tc model   the fitted estimate, deliberately
//
// DEGRADATION IS AUTOMATIC, not configured: `real` falls back to the model when
// one exists and to open loop when it does not. That was 0.6.0's `tc on, nn on`
// combination, and making it a choice only invited someone to switch the
// fallback off. `model` does not fall back to the thermocouple — asking for the
// estimate while a sensor is fitted is exactly what the experiment does, and
// silently preferring the sensor would make that arm unmeasurable.
//
// `ctl.tc` therefore reports what was ASKED FOR and `ctl.src` what was GOT.
// They were both effective before, which made `ctl.tc`/`ctl.nn` a restatement
// of `ctl.src` and told a reader nothing new; now "asked for real, got none" is
// one row of the log.
//
// WHY FEEDFORWARD, which every source uses. The plant's dominant pole is 75 s
// at full fan and ~150 s with the fan off, but power changes instantly:
// measured at load-on, the rail went 0.85 -> 4.23 W in one second while the
// heatsink moved 0.30 C in two. A loop that waits for the thermocouple is two
// minutes late on every load step. Predicting the duty a given wattage needs,
// and letting the PI correct only what the prediction got wrong, is the one
// thing a board-side loop can do that watching temperature cannot. Measured on
// the 22 Nov run: the feedforward alone predicted 41.6 % where the closed loop
// settled at 41.2 %, and the seeded curve re-measures to within 1.7 %.
//
// WHY THE PI's AUTHORITY IS BOUNDED — see kTrim. This is the fix for every
// failure that run found, and they were all one defect: the error during any
// approach is "model error" plus "the plant has not got there yet", and nothing
// separated them, so the PI demanded the fan compensate for the plant's own
// time constant. Measured, both signs:
//
//   * arming from a hand-set 100 % fan preseeded the integrator to +103.4 duty
//     points. The fan held its 73 % ceiling for 9.4 minutes while the die was
//     already 10.3 C BELOW setpoint, and the step took 20.1 min to settle
//     against 6.5 for the same law at a different setpoint. It compounds on
//     itself: over-cooling shrinks the error, and the unwind rate is
//     proportional to it.
//   * on a cold start the feedforward correctly asked for 33.2 % at 4.33 W and
//     kp*err subtracted 29.7 points for being 30 C below a setpoint the plant
//     had not had time to reach. Output 2.7 %, floored at 9.5, fan idle for
//     40 s, die overshot to +3.5 C, ten minutes to recover.
//
// EVERY CONSTANT BELOW IS MEASURED, not chosen. Sources are the 22 Nov 2026
// Hailo-8 fan ladder (`docs/reports/ra6m5-thermocouple/`) and the 22/23 Nov
// four-setpoint run of this loop:
//
//   R_int  = 1.709 +/- 0.059 C/W over 14 285 full-rate samples, flat across
//            47 C of heatsink temperature and 4.2-5.3 W. `die = heatsink +
//            R_int * W` reproduces the heatsink's own signal to a 0.08 C
//            residual in quiet windows, and scored within 0.68 C of the card's
//            own die sensor at every one of 2 400 steady-state samples across
//            27 C of die. NOTE it is not quite constant: 1.619 C/W at 32-41 %
//            duty against 1.742 at 58-61 %, most likely the probe being cooled
//            by the airstream. 1.709 sits between the two, which is why the
//            estimate errs in both directions and never by much. A retune to
//            1.627 was proposed from a hand-driven session and is REFUTED —
//            that figure is below the whole steady-state range.
//   target = 95 C die gave 0.00 % throttled over 13 247 samples, reproduced at
//            1372.3 +/- 0.47 fps and 400 MHz over a further 7 192. The throttle
//            boundary is hysteretic AND stochastic — throttling seen anywhere
//            from 98.9 to 104.2 C, and two runs disagreed at the same 101.6 /
//            101.7 C die — so the margin has to absorb a boundary that moves.
//   duty   = the EMC2101 quantises to 1/63 (1.587 %), and the fan SATURATES at
//            73.0 %: the 18 levels above it hold 4 628 +/- 8 rpm and 3.35 +/-
//            0.04 m/s, i.e. they do nothing. Confirmed a third time on the
//            four-setpoint run, where 73 % and 100 % landed within 1 rpm.
//   gain   = -1.8 C per duty-point at 30 % duty falling to -0.6 at 70 %, a 3x
//            spread. invert_curve() is what linearises it, which is why kp = 1
//            works unchanged across 32-61 % duty.
//   model  = the soft sensor, fitted 2026-11-24 -- `kHeatCapacity` and the
//            refitted curve_rconv(). Jointly fitted on 26 094 samples (the
//            11-22 ladder's unthrottled region plus the 11-23 soft-sensor run)
//            to 0.51 C rms, then validated on 20 698 samples of two runs held
//            out of the fit entirely: 0.52 and 0.87 C rms on their loaded
//            windows, unbiased (-0.03 and +0.28 C). It is the SAME curve the
//            feedforward inverts, which is deliberate -- one table, so the
//            predictor and the observer cannot disagree about the plant.
//
// THE 0.7.0 REFIT ALSO CORRECTED THE FEEDFORWARD, and this is worth knowing
// before trusting any pre-0.7.0 log of a light-load step. The old curve took
// all nine nodes from the fan ladder, and the ladder's bottom three treads are
// THROTTLED: below 23.8 % duty the card sits on its thermal limit, so the
// heatsink reading there is set by the limit and not by the fan. Those nodes
// were wrong by up to 32 % -- the table claimed 16.17 C/W at the 9.5 % floor
// where the plant delivers 21.28 -- i.e. the feedforward believed the fan was
// half again as effective at its floor as it is. Above 30.2 % the ladder is
// clean and nothing moved by more than 1.1 %, so a run held at 32-61 % duty
// (which is every loop run on disk) is unaffected.
//
// SCOPE: the feedforward curve and R_int are the Hailo-8's, and kAutoTarget is
// its measured bound. Another card in the slot has its own heatsink and its own
// interface; `ctl.rint` and `ctl.target` are settable, but kFfRconv would need
// re-measuring. A run scores itself — the host logs both this model's
// `ctl.die_est` and the card's true die temperature, in the same CSV row.
#pragma once
#include <Arduino.h>
#include <math.h>
#include "Emc2101.h"
#include "Ina228.h"
#include "Max31855.h"

class Control {
public:
    enum Mode : uint8_t { kOff = 0, kOn = 1 };

    // Where the regulated signal came from. This is `ctl.src`, which in 0.5.0
    // reported die-vs-heatsink — a choice the reduction above showed was never
    // real. It now reports PROVENANCE, which is the one thing a reader of the
    // log cannot afford to guess: whether the loop was closed on a measurement
    // or on an estimate. Same column name, different meaning; an older log's
    // values do not mean this.
    enum Src : uint8_t { kMeasured = 0, kModelled = 1, kNone = 2 };

    // What the host ASKED for, reported as `ctl.tc`. Distinct from Src above,
    // which is what the step actually achieved.
    enum TcWant : uint8_t { kTcOff = 0, kTcReal = 1, kTcModel = 2 };

    // kFloor and kCeiling replace 0.5.0's single kSaturated, which was set from
    // "the output is clamped" — true at both rails and meaning opposite things
    // at each. Every idle in the four-setpoint run reported "saturated" while
    // sitting at the 9.5 % floor with a slack setpoint, which is the opposite
    // of running out of cooling authority.
    enum State : uint8_t { kIdle = 0, kSettling = 1, kHolding = 2,
                           kFloor = 3, kCeiling = 4,
                           kOpen = 5, kDegraded = 6, kFault = 7 };

    // 250 ms matches Max31855::kSamplePeriodMs and Emc2101::kSamplePeriodMs —
    // both inputs refresh at that rate, and against a 75 s pole this is already
    // 300x oversampled. Running faster would only add I2C traffic.
    static constexpr uint32_t kPeriodMs = 250;
    // How long an input may go without a fresh sample before it is a fault.
    // Must exceed one report period (1 s by default), because report() resets
    // every Stat and a mean is briefly NaN afterwards — see `freshen()`.
    static constexpr uint32_t kStaleMs = 3000;

    // ---- settings, all reachable from the host as `ctl.<key>` -------------
    Mode mode = kOff;
    TcWant tc_want = kTcReal;  // ctl.tc off|real|model — the REQUEST
    double target = kAutoTarget;
    bool target_auto = true;   // whether `target` was chosen or defaulted
    double rint = 1.709;       // C/W, measured
    double kp = 1.0;           // duty-% per C
    double ki = 0.01;          // duty-% per C per second; kp/ki = 100 s, and the
                               // plant's pole is 75-150 s, so Ti ~= tau
    double min_duty = 9.5;     // the ramp studies' first tread
    double max_duty = 73.0;    // measured ceiling; above it the fan does nothing

    // The measured zero-throttle bound for the Hailo-8. `ctl.target auto`.
    static constexpr double kAutoTarget = 95.0;

    // How far the PI may move the feedforward, in duty points. The feedforward
    // is accurate to ~1.7 % on R_conv and predicted 41.6 % against a held
    // 41.2 %, so a correct operating point is always well inside this window;
    // anything outside it is the PI trying to fix elapsed time rather than
    // model error. 15 points is ~9 EMC2101 levels, comfortably more than any
    // steady-state trim observed (the integrators held between -0.97 and -0.51).
    static constexpr double kTrim = 15.0;

    // ---- status, published verbatim as the ctl.* channels -----------------
    State state = kIdle;
    Src src = kNone;
    double in_c = NAN;       // the quantity actually regulated
    double die_est = NAN;    // the board's model of the die
    double err = NAN;        // in_c - target; POSITIVE MEANS TOO HOT
    double duty = NAN;       // what the loop last commanded
    double ff = NAN;         // the feedforward part alone
    double integ = 0;        // integrator state, in duty-%
    unsigned sat = 0;        // steps clamped at a limit, since the last report
    unsigned write_err = 0;  // EMC2101 write failures, since the last report

    // Mode changes go through here so the transfer is bumpless.
    void set_mode(Mode m) {
        if (m == mode) return;
        mode = m;
        if (m == kOff) {
            state = kIdle; src = kNone; integ = 0;
            in_c = die_est = err = duty = ff = NAN;
            model_forget();
        } else {
            rearm_ = true;   // next step() seeds the integrator — see below
        }
    }

    // `auto` and an explicit setpoint are distinguishable in the log, because
    // the moment a different card is in the slot the distinction is the whole
    // story: 95 C is the Hailo-8's bound and nothing else's.
    void set_target(double c) { target = c; target_auto = false; }
    void set_target_auto() { target = kAutoTarget; target_auto = true; }

    // ---- the soft sensor: a stand-in for the heatsink thermocouple -------
    //
    // FITTED 2026-11-24 on 26 094 samples and validated on 20 698 more that
    // were held out of the fit entirely. It is a FIRST-ORDER LAG, and the order
    // is the whole point — see below.
    //
    //   tc_ss  = air + W * R_conv(duty)          the steady state
    //   tau    = R_conv(duty) * kHeatCapacity    the pole, from the same curve
    //   d tc/dt = (tc_ss - tc) / tau
    //
    // So it adds exactly ONE parameter to what the feedforward already knew:
    // the heat capacity. `kHeatCapacity` is a lumped heatsink + thermocouple
    // capacity, fitted jointly with the curve.
    //
    // NOT NAMED `nn_*`, deliberately, and the fit vindicated that: three
    // physically interpretable numbers beat a net here, and `invert_curve` is
    // provably monotone where a net is not.
    //
    // WHY THE DYNAMICS ARE NOT OPTIONAL. The static form -- `air + W*R(duty)`,
    // evaluated fresh each step, which is what the open-loop branch's own
    // die_est does -- scores rms 7.3-9.0 C on the held-out runs with
    // excursions to 43 and 72 C, because it has no memory and a 70-180 s plant
    // settles slowly. The same curve with this one pole scores 0.52-0.87 C.
    // Do not "simplify" this back to the algebraic form.
    //
    // WHAT IT IS WORTH, measured by replaying the 11-23 arm that ran closed on
    // the real probe and substituting this for it: the loop commands a settled
    // duty of 37.29 % against the firmware's own 37.72 % -- 0.43 duty points,
    // ~0.6 C of die, against a 5 C margin. That is the cost of losing the
    // probe, and it is affordable.
    //
    // THE ERROR BUDGET, and it does NOT come out one-sided. Two terms push
    // opposite ways, so do not read either on its own:
    //
    //   model identity -- R_conv is fitted to ResNet-50, which has the HIGHEST
    //     convective resistance of the four models measured, so every other
    //     load reads HOT: +0.30 C (SCRFD-10G), +0.43 (YOLOv8m), +2.19
    //     (YOLOv8s, 4.2 % of its heatsink rise). Hot over-cools, so this term
    //     alone is in the safe direction. The board cannot observe it -- which
    //     model is running is PCIe-side.
    //   session drift -- the plant itself moves ~1-2 % in R_conv between
    //     sessions, and THAT term reads COLD. On the two held-out runs the
    //     estimate was below the probe on 15.5 % and 60.0 % of loaded samples,
    //     worst excursion -2.34 C.
    //
    // So the honest worst case is the cold one: the loop may believe the
    // heatsink is ~2.3 C cooler than it is. The throttle boundary is
    // hysteretic and stochastic between 98.9 and 104.2 C, i.e. as little as
    // 3.9 C above kAutoTarget, and -2.3 C of that is most of the margin.
    // RECOMMENDATION: regulate on the model at 90 C, not at `target auto`.
    // With the real probe 95 C measured 0.00 % throttled over 13 247 samples;
    // the estimate has not earned that setpoint and this comment is the only
    // place that distinction is written down.
    static constexpr double kHeatCapacity = 10.4;   // J/C, fitted

    // True once a curve and a capacity for the card in the slot are compiled
    // in. Kept as a predicate rather than hard-wired to `true`: both constants
    // are the Hailo-8's, and the honest answer for another card is `false`.
    bool model_ready() const { return true; }

    // The steady state this operating point implies. Pure, and the one piece
    // the open-loop branch shares, so the two can never disagree about where
    // the plant is heading.
    double model_steady(double w, double air, double duty) const {
        return air + w * rconv_at(duty);
    }
    // The pole at that duty. tau = R*C held to 0.1 % between 50.8 and 73 %
    // duty on the fit run, which is what says C is a constant and not a
    // per-duty fudge: letting it vary as (R/R73)^a made the held-out error
    // strictly worse for every a > 0.
    double model_tau(double duty) const {
        return rconv_at(duty) * kHeatCapacity;
    }
    // Advance the state by `dt_s` and return the estimate. Stateful by
    // necessity -- that state IS the model's memory of the heatsink.
    double model_estimate(double w, double air, double duty, double dt_s) {
        const double ss = model_steady(w, air, duty);
        if (isnan(ss)) return NAN;
        if (isnan(tc_hat_)) { tc_hat_ = ss; return tc_hat_; }
        const double tau = model_tau(duty);
        if (!(tau > 0) || !(dt_s > 0)) return tc_hat_;
        // Clamped so an implausibly long step cannot overshoot past the
        // steady state and ring; the explicit Euler form is stable for
        // dt < tau and this is the guard for when it is not.
        tc_hat_ += clampd(dt_s / tau, 0.0, 1.0) * (ss - tc_hat_);
        return tc_hat_;
    }
    // Bumpless handover. The case this sensor exists for is a thermocouple
    // that WAS there and went away, so the last thing it said is a far better
    // seed than the steady state -- which, mid-transient, can be tens of
    // degrees out.
    void model_seed(double tc) { tc_hat_ = tc; }
    void model_forget() { tc_hat_ = NAN; model_ms_ = 0; }

    void reset_stats() { sat = write_err = 0; }

    // ---------------------------------------------------------------- step --
    void step(Emc2101& emc, Max31855& tcx, Ina228& ina, uint32_t now) {
        // The sensors' Stat accumulators are reset by report() once a period,
        // so a mean read just after that is NaN through no fault of the sensor.
        // Hold the last good value of each and age it instead.
        freshen(tcx.present ? tcx.tc.mean() : NAN, now, &tc_, &tc_ms_);
        freshen(ina.present ? ina.power.mean() : NAN, now, &w_, &w_ms_);
        freshen(emc.present ? emc.temp_int.mean() : NAN, now, &air_, &air_ms_);

        if (mode == kOff) { state = kIdle; return; }

        const bool tc_ok = tcx.present && fresh(tc_ms_, now);
        const bool w_ok = ina.present && fresh(w_ms_, now);
        const bool air_ok = emc.present && fresh(air_ms_, now);

        // FAILSAFE, first half. Power is mandatory in 0.6.0: the regulated
        // quantity is the estimated die, and both the estimate and the
        // feedforward are functions of watts. Without them there is no way to
        // know the die at all, so the only safe output is everything the fan
        // has. 100 %, not max_duty: a failsafe must not depend on a configured
        // limit being sane, and above 73 % the two are identical anyway.
        if (!w_ok) {
            fail(emc);
            return;
        }

        // ---- resolve the one signal, and say where it came from ------------
        // `degraded` means "running, but not the way it was asked to" — a lost
        // thermocouple, a model that is not there, or a missing air reading. It
        // is deliberately distinct from kOpen, which is running open-loop
        // BY CHOICE and is not a fault at all.
        double pv_tc = NAN;
        bool degraded = false;
        if (tc_want == kTcReal && tc_ok) {
            src = kMeasured;
            pv_tc = tc_;
            // Drop any model state while the probe is answering, so a later
            // handover re-seeds from a CURRENT reading rather than from one
            // the model has been extrapolating on its own since.
            model_forget();
        } else if (tc_want != kTcOff && model_ready()) {
            src = kModelled;
            // It is a model of the duty ACTUALLY APPLIED, so it cannot run
            // before something has applied one; and it needs air, like the
            // feedforward. Either way one step of open loop covers it and
            // `ctl.src` says that is what happened.
            if (isnan(duty) || !air_ok) {
                pv_tc = NAN;
            } else {
                // Seed from the probe's last word whenever there is one, fresh
                // or not: a probe that went stale 3 s ago is still the best
                // estimate of where the heatsink was.
                if (isnan(tc_hat_) && !isnan(tc_)) model_seed(tc_);
                double dt_s = model_ms_ ? (now - model_ms_) / 1000.0 : 0.0;
                if (!(dt_s > 0) || dt_s > kStaleMs / 1000.0)
                    dt_s = kPeriodMs / 1000.0;
                model_ms_ = now;
                pv_tc = model_estimate(w_, air_, duty, dt_s);
            }
            // Asking for `model` and getting it is NOT degradation. Asking for
            // `real` and landing here is: the fallback fired.
            degraded = (tc_want == kTcReal);
            // A model that declined to answer this step: fall the rest of the
            // way to open loop rather than closing on a NaN. Tested HERE and
            // not after the if/else — `pv_tc` is NaN in the kNone branch too,
            // so a shared check made kOpen unreachable and reported every
            // open-loop step as degraded. Caught on hardware, not in review:
            // arms 2 and 3 of the thermocouple plan are told apart by exactly
            // this state, so both would have read 6.
            if (isnan(pv_tc)) { src = kNone; degraded = true; }
        } else {
            src = kNone;
            // Wanting something and not having it is degradation; wanting
            // nothing is just open-loop operation.
            degraded = (tc_want != kTcOff);
        }

        if (src == kNone) {
            open_loop(emc, air_ok, degraded);
            return;
        }

        // ---- closed loop, on a measurement or on an estimate ---------------
        die_est = pv_tc + rint * w_;
        in_c = die_est;
        err = in_c - target;

        // Feedforward: invert the measured fan curve for the convective
        // resistance this load needs. The target is a HEATSINK temperature to
        // the fan curve, so the interface drop comes off the die setpoint
        // first. Losing board air costs the feedforward but not the loop — the
        // PI can still regulate, just slowly and from scratch.
        ff = NAN;
        double base = 0.0;
        if (air_ok && w_ > 0.1) {
            ff = base = invert_curve((target - rint * w_ - air_) / w_);
        } else {
            degraded = true;
        }

        if (rearm_) {
            // Bumpless transfer: make this step's output equal the duty already
            // on the chip, so switching modes never jumps the fan. Bounded by
            // the trim window below, which is what makes it safe — in 0.5.0
            // this one line injected +103.4 points and nothing pulled it back.
            const double now_duty = isnan(emc.duty) ? 0.0 : emc.duty;
            integ = clampd(now_duty, min_duty, max_duty) - base - kp * err;
            rearm_ = false;
        }

        const double dt = kPeriodMs / 1000.0;
        integ += ki * err * dt;

        // THE PI MAY TRIM THE FEEDFORWARD BY AT MOST kTrim POINTS. Stated this
        // way rather than as conditional integration, because the failures this
        // replaces were not windup in the classical sense: the integrator was
        // unwinding correctly, from a value the loop could never use. Bounding
        // the whole correction bounds the arming case, the cold-start case and
        // windup together, instead of special-casing each.
        const double corr = clampd(kp * err + integ, -kTrim, kTrim);
        const double raw = base + corr;
        const double out = clampd(raw, min_duty, max_duty);
        // Hand back any integral the window cannot express, so it cannot hide
        // there and reappear when the error changes sign.
        integ = clampd(integ, -kTrim - kp * err, kTrim - kp * err);
        if (out != raw) ++sat;

        state = degraded          ? kDegraded
              : (raw > out)       ? kCeiling
              : (raw < out)       ? kFloor
              : fabs(err) < 1.0   ? kHolding
                                  : kSettling;
        commit(emc, out);
    }

    // `ctl` with no argument: one status line, in the I namespace like `info`.
    void print_status() const {
        Serial.print("I ctl mode=");
        Serial.print(mode == kOff ? "off" : "on");
        Serial.print(" src="); Serial.print(src_name());
        Serial.print(" state="); Serial.print(state_name());
        Serial.print(" tc="); Serial.print(tc_want_name());
        if (tc_want != kTcOff && !model_ready())
            Serial.print(tc_want == kTcModel ? " (no model compiled in)"
                                             : " (no model to fall back to)");
        Serial.print(" target="); Serial.print(target, 2);
        Serial.print(target_auto ? " (auto)" : " (set)");
        Serial.print(" in="); print_d(in_c, 2);
        Serial.print(" die_est="); print_d(die_est, 2);
        Serial.print(" err="); print_d(err, 2);
        Serial.print(" duty="); print_d(duty, 1);
        Serial.print(" ff="); print_d(ff, 1);
        Serial.print(" integ="); Serial.print(integ, 2);
        Serial.print(" rint="); Serial.print(rint, 3);
        Serial.print(" kp="); Serial.print(kp, 3);
        Serial.print(" ki="); Serial.print(ki, 4);
        Serial.print(" trim="); Serial.print(kTrim, 1);
        Serial.print(" min="); Serial.print(min_duty, 1);
        Serial.print(" max="); Serial.print(max_duty, 1);
        Serial.print(" sat="); Serial.print(sat);
        Serial.print(" write_err="); Serial.println(write_err);
    }

    // The duty whose convective resistance is `want`. Public because it is a
    // pure function of the measured curve — worth querying on its own, and the
    // host-side test exercises it directly.
    //
    // A target needing LESS resistance than the fan can deliver at its ceiling
    // is unreachable; the honest answer there is the ceiling, and the loop then
    // reports kCeiling, which is exactly the state the host should see.
    double invert_curve(double want) const {
        const double* du = curve_duty();
        const double* rc = curve_rconv();
        if (!(want > 0)) return max_duty;              // target already below air
        if (want >= rc[0]) return min_duty;
        if (want <= rc[kCurveN - 1]) return max_duty;
        for (int i = 1; i < kCurveN; ++i) {
            if (want > rc[i]) {
                const double f = (rc[i - 1] - want) / (rc[i - 1] - rc[i]);
                return clampd(du[i - 1] + f * (du[i] - du[i - 1]),
                              min_duty, max_duty);
            }
        }
        return max_duty;
    }

    // The forward direction of the same table: what convective resistance a
    // given duty delivers. Only the open-loop estimate needs this.
    double rconv_at(double d) const {
        const double* du = curve_duty();
        const double* rc = curve_rconv();
        if (d <= du[0]) return rc[0];
        if (d >= du[kCurveN - 1]) return rc[kCurveN - 1];
        for (int i = 1; i < kCurveN; ++i) {
            if (d <= du[i]) {
                const double f = (d - du[i - 1]) / (du[i] - du[i - 1]);
                return rc[i - 1] + f * (rc[i] - rc[i - 1]);
            }
        }
        return rc[kCurveN - 1];
    }

    const char* state_name() const {
        switch (state) {
            case kIdle: return "off";
            case kSettling: return "settling";
            case kHolding: return "holding";
            case kFloor: return "floor";
            case kCeiling: return "ceiling";
            case kOpen: return "open";
            case kDegraded: return "degraded";
            default: return "fault";
        }
    }

    const char* src_name() const {
        switch (src) {
            case kMeasured: return "measured";
            case kModelled: return "modelled";
            default: return "none";
        }
    }

    const char* tc_want_name() const {
        switch (tc_want) {
            case kTcReal: return "real";
            case kTcModel: return "model";
            default: return "off";
        }
    }

private:
    bool rearm_ = false;
    int last_level_ = -1;   // EMC2101 fan setting last written, 0..63
    double tc_ = NAN, w_ = NAN, air_ = NAN;
    uint32_t tc_ms_ = 0, w_ms_ = 0, air_ms_ = 0;
    double tc_hat_ = NAN;    // the soft sensor's state: its memory of the sink
    uint32_t model_ms_ = 0;  // when it was last advanced, for the real dt

    // The measured R_conv-against-duty curve. Monotonically decreasing, so
    // inverting it is a scan plus a linear interpolation — and monotone BY
    // CONSTRUCTION, which a fitted model would not be. Both this and
    // kHeatCapacity are the Hailo-8's; another card needs them re-measured.
    //
    // REFITTED 2026-11-24, and the three lowest nodes CHANGED A LOT -- up to
    // 32 %. The earlier table took all nine nodes from the 22 Nov ladder's
    // 59-tread R_conv column, and the bottom of that ladder is THROTTLED
    // DATA: with the fan at 0-23.8 % the card sits on its thermal limit, so
    // 100 % of those samples are below 400 MHz and the heatsink reading is
    // pinned by the limit rather than set by the fan. Those treads measure the
    // throttle, not the plant. The ladder is clean only from 30.2 % up, where
    // it reads 0.0 % throttled, and the nodes at and above 30.2 % moved by at
    // most 1.1 % as a result -- so this is a low-duty correction, not a
    // re-measurement of the whole curve.
    //
    // Provenance node by node:
    //   0.0        natural convection, the four fan-off brackets of the 11-23
    //              three-arm run: 22.46 / 22.19 / 21.80 / 20.55, mean 21.75.
    //              An 8.8 % spread across one afternoon -- this node drifts
    //              more than any other, and it is measured at 0.86 W only.
    //   9.5        the two settled 9.5 % blocks of the 11-22 four-setpoint run,
    //              20.84 and 21.72. Also 0.87 W only. Note it lands within the
    //              drift of the 0 node: at its 9.5 % floor the fan turns at
    //              262 rpm and buys almost nothing over still air.
    //   20.6       the one clean low-duty tread that exists -- T15 of the
    //              11-23 fit run, 0.0 % throttled at 3.83 W. n=1, and the
    //              weakest node in the table. Its three neighbours at the same
    //              duty all throttled and read 15.05-15.15, which is what a
    //              throttled tread looks like here: LOWER, not higher.
    //   30.2-73.0  the 11-22 ladder's clean region, lag-corrected by the joint
    //              fit below. Cross-checked against the fit run's own settled
    //              asymptotes at 73.0 / 50.8 / 33.3 %: 6.67 / 9.75 / 12.40
    //              against this curve's 6.73 / 9.77 / 12.40.
    //   >73.0      flat, and verified so: the ladder's 18 levels from 73 to
    //              100 % read 6.73-6.81 with no trend, agreeing with the tach.
    //
    // ALL NINE ARE AGAINST SIMULTANEOUS AIR -- the EMC2101's own reading at the
    // same duty, which is what the board has at run time. That convention is
    // load-bearing: the EMC2101 is itself cooled by the fan at -0.026 C per
    // duty point, so pricing R_conv against air measured at a DIFFERENT duty
    // folds a duty-dependent bias straight into the curve. The fit run's idles
    // all sat at 73 % while its treads ran down to 20.6 %, so that mistake was
    // available and worth 2 % at the bottom of the range.
    //
    // Function-local statics behind accessors so the header needs no
    // out-of-line definitions on a pre-C++17 core, and so the forward and
    // inverse directions cannot drift apart.
    static constexpr int kCurveN = 9;
    static const double* curve_duty() {
        static const double d[kCurveN] = {0.0, 9.5, 20.6, 30.2, 39.7,
                                          50.8, 60.3, 69.8, 73.0};
        return d;
    }
    static const double* curve_rconv() {
        static const double r[kCurveN] = {21.75, 21.28, 15.82, 12.97, 11.30,
                                          9.77, 8.31, 7.07, 6.73};
        return r;
    }

    static double clampd(double v, double lo, double hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }
    static void print_d(double v, int digits) {
        if (isnan(v)) Serial.print("nan"); else Serial.print(v, digits);
    }
    static void freshen(double v, uint32_t now, double* slot, uint32_t* stamp) {
        if (!isnan(v)) { *slot = v; *stamp = now; }
    }
    static bool fresh(uint32_t stamp, uint32_t now) {
        return stamp != 0 && now - stamp < kStaleMs;
    }

    void fail(Emc2101& emc) {
        state = kFault;
        src = kNone;
        in_c = die_est = err = ff = NAN;
        integ = 0;
        commit(emc, 100.0);
    }

    // OPEN LOOP: no thermocouple and no stand-in for it, so there is nothing to
    // feed back and the feedforward is the whole controller. Deliberately NOT a
    // PI against the model's own output, which would be a loop closed on itself.
    void open_loop(Emc2101& emc, bool air_ok, bool degraded) {
        if (!air_ok) { fail(emc); return; }   // the feedforward needs air too

        in_c = err = NAN;      // nothing is being measured, so nothing is in error
        integ = 0;

        ff = invert_curve((target - rint * w_ - air_) / w_);
        const double out = clampd(ff, min_duty, max_duty);
        if (out != ff) ++sat;

        // The model's own prediction of the die at the duty actually applied.
        // Note this equals `target` by construction while the command is
        // unclamped — the board genuinely has no independent information about
        // the die here, and `src = none` is what says so. It stops being
        // tautological exactly when the fan runs out of authority, which is the
        // one case it is needed: then the duty is pinned and this diverges.
        const double applied = isnan(duty) ? out : duty;
        die_est = air_ + w_ * (rint + rconv_at(applied));

        state = degraded   ? kDegraded
              : (ff > out) ? kCeiling
              : (ff < out) ? kFloor
                           : kOpen;
        commit(emc, out);
    }

    // Write the fan only when the QUANTISED level changes. The EMC2101 has 64
    // levels 1.587 % apart, so at 4 Hz an unconditional write would be four I2C
    // transactions a second to say the same thing. `duty` keeps the value the
    // chip actually applied, not the one asked for.
    //
    // THE HYSTERESIS IS NOT OPTIONAL, and it was measured rather than guessed.
    // Rounding alone still writes constantly whenever the demand settles ON a
    // level boundary, because the rounding then flips on noise far below what
    // the thermocouple can even resolve. With a 60 C heatsink target the demand
    // parks at 59.5234 %, and the 37/38 boundary is 59.5238: two of every three
    // steps wrote, forever. Requiring 0.75 of a level of movement costs at most
    // a quarter-level of extra steady-state error on an actuator already
    // quantised to 1.587 %, and the integrator absorbs that. Confirmed on the
    // four-setpoint run: one or two level changes per 30-minute idle, and zero
    // in the last ten minutes of three of them.
    static constexpr double kLevelHyst = 0.25;

    void commit(Emc2101& emc, double pct) {
        pct = clampd(pct, 0.0, 100.0);
        const double step = 100.0 / 63.0;        // one EMC2101 level
        int level = (int)(pct / step + 0.5);
        if (last_level_ >= 0) {
            const double lo = (last_level_ - 0.5 - kLevelHyst) * step;
            const double hi = (last_level_ + 0.5 + kLevelHyst) * step;
            if (pct > lo && pct < hi) level = last_level_;
        }
        if (level == last_level_) return;
        // Ask for the level's own percentage, so set_duty()'s rounding and the
        // decision just made cannot disagree about which level that is.
        double applied = 0;
        if (!emc.set_duty(level * step, &applied)) { ++write_err; return; }
        last_level_ = level;
        duty = applied;
    }

};
