// Per-report-period statistics for one quantity.
//
// The firmware samples far faster than the host's 1 Hz tick, so each report
// carries what happened INSIDE the period — mean, min, max, sample count —
// rather than whichever instant the host happened to land on. That is the
// point of moving the sampling onto the board.
#pragma once
#include <math.h>

struct Stat {
    double sum = 0, lo = NAN, hi = NAN;
    unsigned n = 0;
    void add(double v) {
        if (isnan(v)) return;
        sum += v;
        if (n == 0 || v < lo) lo = v;
        if (n == 0 || v > hi) hi = v;
        ++n;
    }
    double mean() const { return n ? sum / n : NAN; }
    void reset() { sum = 0; lo = hi = NAN; n = 0; }
};
