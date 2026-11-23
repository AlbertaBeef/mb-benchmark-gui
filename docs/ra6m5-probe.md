# RA6M5 probe — firmware and wire protocol

A SparkFun Thing Plus RA6M5 running `firmware/mb-probe-ra6m5/` is a *smart*
telemetry probe: it owns its sensors, samples them far faster than the GUI's
1 Hz tick, and reports per-period statistics over USB CDC. The host only
parses. It sits **alongside** the FT232H/INA228 probe, which stays a dumb I2C
bridge driven from `src/Probes.cpp`.

| sensor | bus | address | status |
| --- | --- | --- | --- |
| INA228 power monitor | Qwiic I2C | 0x40-0x4F (0x40 here) | supported |
| FS3000-1005 air velocity | Qwiic I2C | 0x28 (fixed) | supported |
| EMC2101 fan controller | Qwiic I2C | 0x4C | duty set by host command `fan` (fw 0.4.0) or the on-board control loop (fw 0.5.0); tach needs the board's TACH jumper bridged |
| MAX31855K thermocouple, K-type | SPI, SCK D19 / DO D17 / CS D0 (P112) | — | supported (fw 0.3.0); junction fitted 2026-11-22 |
| MAX17048 fuel gauge | Qwiic I2C | 0x36 | on the board itself; ignored |

## Building and flashing

```bash
tools/flash-ra6m5.sh              # build + flash, hands-off
tools/flash-ra6m5.sh --build-only
```

One-time host setup (none of it lives in the repo):

- `arduino-cli` (installed to `~/.local/bin`), plus the official
  `arduino:renesas_portenta` core for its compiler and `dfu-util`.
- **The board is not in the official core.** SparkFun ships a ported core,
  `https://docs.sparkfun.com/SparkFun_Thing_Plus_RA6M5/assets/SFE-Renesas-Arduino_core.zip`;
  unpack it to `~/Arduino/hardware/sparkfun/renesas_portenta/` (a sketchbook
  platform, so the official core is left untouched). FQBN:
  `sparkfun:renesas_portenta:thingplus_ra6m5`.
- `tools/99-thingplus-ra6m5.rules` into `/etc/udev/rules.d/` (the header has
  the commands), or `dfu-util` cannot open the bootloader as a normal user.

**Flashing needs no button press.** The core implements the 1200-baud touch
(`SerialUSB.cpp`: open at 1200 baud, drop DTR → DFU bootloader `1b4f:0036`);
SparkFun's `boards.txt` merely sets `use_1200bps_touch=false`, so the script
does the touch itself. Double-tap RST only when no sketch is running — the blue
`STAT` LED then fades slowly. **Don't touch RST during an upload**: a reset at
72% killed one with `LIBUSB_ERROR_NO_DEVICE`.

## Protocol (proto=1)

Lines, `\n`-terminated, space-separated `key=value`. Text on purpose: `cat
/dev/ttyACM0` is a complete debugger.

**Host → device**

| command | effect |
| --- | --- |
| `info` | re-send the `I` block |
| `period <ms>` | report period, 100-60000 (default 1000) |
| `reset_acc` | clear the INA228 ENERGY/CHARGE accumulators |
| `rescan` | re-detect sensors, then re-send `I` — **changes the channel list** |
| `fan <percent>` | EMC2101 duty 0-100. **Refused while a control mode is active** |
| `ctl` | print one `I ctl …` status line |
| `ctl.mode off\|on` | the control loop; see below |
| `ctl.tc on\|off` | use the physical thermocouple |
| `ctl.nn on\|off` | substitute a learned estimate of it |
| `ctl.target <C>\|auto` | setpoint, -40..150, or the board's measured bound |
| `ctl.rint <C/W>` | die-estimate coefficient (default 1.709, the Hailo-8's) |
| `ctl.kp` / `ctl.ki` | PI gains (1.0 and 0.01) |
| `ctl.min` / `ctl.max` | duty limits (9.5 and 73.0) |

**Device → host**

- `I proto=1 fw=mb-probe-ra6m5 ver=… period_ms=…`, then one
  `I sensor=<name> present=0|1 …` per sensor, then one `I ch=<key> unit=<u>`
  per channel, then `I end`. The channel list is the schema: the host fixes
  its metrics (and the CSV header) from it, so only `info` at discovery
  defines it.
- `D seq=<n> t_ms=<ms> <ch>=<value> …` once per period. **Every declared
  channel appears in every line**, in declaration order; `nan` means no valid
  sample in that period. `seq` counts lines actually sent, so a gap is a lost
  line.
- `OK <cmd>` / `E <message>`.

Reports are sent only while a host holds the port open (DTR). With nobody
listening the statistics roll over every period, so a reader that connects gets
a clean first period rather than an hour of accumulated samples.

### Channels

| channel | unit | meaning |
| --- | --- | --- |
| `ina228.vbus` / `_min` / `_max` | V | mean / extremes over the period's conversions |
| `ina228.current` / `_min` / `_max` | A | **signed, as measured** — the host applies `invert` |
| `ina228.power` / `_min` / `_max` | W | |
| `ina228.dietemp` | °C | the monitor chip, not the card |
| `ina228.energy` | J | hardware accumulator, read at report time |
| `ina228.charge` | C | hardware accumulator, signed |
| `ina228.uv` | count | conversions flagged below BUVL (3.00 V) this period |
| `ina228.n` | count | conversions taken this period (~5 at 1 s) |
| `ina228.err` | count | failed I2C transactions this period |
| `fs3000.airflow` / `_min` / `_max` | m/s | 0-7.23 |
| `fs3000.raw` | count | latest raw reading, 409 = still air |
| `fs3000.n` / `.err` | count | samples / checksum-or-bus failures this period |
| `emc2101.temp_int` | °C | the EMC2101's own die, 0.25 °C steps |
| `emc2101.temp_ext` | °C | external diode on DP/DN; `nan` with a diode fault (none fitted yet) |
| `emc2101.rpm` / `_min` / `_max` | rpm | 5 400 000 / tach count; `nan` when stalled |
| `emc2101.duty` | % | readback of the fan setting (0x4C); the firmware never writes it |
| `emc2101.stall` / `.fault` / `.n` / `.err` | count | tach-stalled / diode-fault / samples / I2C failures |
| `max31855.tc` / `_min` / `_max` | °C | thermocouple junction, 0.25 °C steps, 4 Hz; `nan` while faulted |
| `max31855.cj` | °C | cold junction = the MAX31855's own die, 0.0625 °C steps |
| `max31855.fault` | count | samples with the fault bit set this period |
| `max31855.fault_bits` | mask | OR over the period: 1 open circuit, 2 short to GND, 4 short to VCC |
| `max31855.n` / `.err` | count | samples / frames failing the reserved-bit check |
| `ctl.mode` | enum | 0 off, 1 on |
| `ctl.src` | enum | **0 measured, 1 inferred, 2 none** — see below |
| `ctl.state` | enum | 0 off, 1 settling, 2 holding, 3 floor, 4 ceiling, 5 open, 6 degraded, 7 fault |
| `ctl.tc` | bool | thermocouple in use — **effective**, not requested |
| `ctl.nn` | bool | learned estimate in use — 0 while the board has no model |
| `ctl.auto` | bool | was the setpoint defaulted (1) or named (0) |
| `ctl.target` | °C | setpoint; `nan` while off |
| `ctl.in` | °C | the quantity actually regulated; `nan` in open loop |
| `ctl.die_est` | °C | the board's model of the die |
| `ctl.err` | °C | `in − target`; **positive means too hot**; `nan` in open loop |
| `ctl.duty` | % | what the loop last commanded |
| `ctl.ff` | % | the feedforward part alone |
| `ctl.integ` | % | integrator state |
| `ctl.sat` | count | steps clamped at a duty limit, this period |

### The control loop (fw ≥ 0.6.0)

Closed-loop fan control running **on the board**. Everything it needs is here —
the heatsink thermocouple, the card's rail watts and board air — so it keeps
cooling the card when the host crashes, exits or is unplugged. The host
configures it and reads it back; it never closes the loop itself.
`firmware/mb-probe-ra6m5/Control.h` carries the design and the provenance of
every constant.

**ONE MODE since 0.6.0.** 0.5.0 shipped three — off / host / smart — with a
selectable input, die or heatsink. Reading the code back against the 225-minute
four-setpoint run of 22/23 Nov showed that two of those were the same control
law with a different constant, because in die mode

    e = die_est − target = (tc + rint·W) − target = tc − (target − rint·W)

and `(target − rint·W)` is exactly the heatsink target the feedforward already
computes. **The observer is a setpoint transformation, not a feedback element**,
so "regulate the die" and "regulate the heatsink" differed only in whether that
setpoint moved with load. All three configurations shared one error path, which
is why one defect produced three different failures in that run.

So: one loop, one setpoint, always the **estimated on-die temperature**.

**Power is now mandatory and the thermocouple is substitutable** — the reverse
of 0.5.0. The regulated quantity needs watts (`die = heatsink + rint × W`) and
so does the feedforward, so without the INA228 there is nothing to estimate from
and nothing to predict with. Without the thermocouple there is still a measured
load and a measured fan curve, which is enough to run open-loop.

Three sources for one signal, resolved once per step and reported as `ctl.src`:

| `ctl.src` | `tc_` from | loop | PI |
|---|---|---|---|
| 0 measured | the physical thermocouple | closed | yes |
| 1 inferred | a learned estimate (`ctl.nn`) | closed | yes |
| 2 none | neither | **open** | no |

The control law is identical for the first two; only where `tc_` comes from
changes. That is the point of putting a soft sensor here — it is a drop-in for
one input, not a second controller. **`ctl.tc` wins when both are on**: a real
sensor is never overridden by an estimate of itself, so `ctl.nn` doubles as the
fallback for *losing* the thermocouple.

**Open loop is the feedforward alone.** `duty = invert_curve((target − rint·W −
air) / W)`, with no PI — there is no second temperature to feed back, and
running a PI against the model's own output would be a loop closed on itself.
`ctl.in` and `ctl.err` read `nan` there, because nothing is being measured and
quoting an error would invent one. `ctl.die_est` still reports the model's
prediction, and note it equals `ctl.target` by construction while the command is
unclamped — the board genuinely has no independent information about the die in
this mode. It stops being tautological exactly when the fan runs out of
authority, which is the one case it is needed.

**`ctl.src` changed meaning in 0.6.0 while keeping its column name.** It
reported die-vs-heatsink before; a log from 0.5.0 or earlier does not mean what
the table above says.

**`ctl.nn` has no model yet.** The enable, the provenance plumbing and the
experiment landed first; the model is trained on the data that experiment
produces. `ctl.nn on` is accepted and answers `(no model compiled in)`, and the
channel reports **0** with `ctl.state = 6` (degraded) — so a run configured for
the inferred source can never be mistaken in the log for one that got it.

**What the board cannot see is core clock or frame rate**: those are PCIe-side.
So the loop holds a die temperature *believed* to be below the throttle knee —
it cannot detect throttling and react. The host still logs `_CLK`, so a setpoint
can be audited after the fact. `ctl.target auto` is the measured bound for the
card the firmware was built for (**95 °C** for the Hailo-8, 0.00 % throttled
over 13 247 samples and reproduced over a further 7 192); `ctl.auto` records
whether that was defaulted or named, because the two stop meaning the same thing
the moment another card is in the slot.

**Measured on the 0.5.0 run** (225 min, four setpoints): held to **≤ 0.03 °C** of
standing error, settling in 6.5–11.8 min, **0.00 % throttled at 1372.3 ± 0.47
fps**, and the die estimate within **0.68 °C** of the card's own sensor over
27 °C of range. `ctl.rint` **stays at 1.709**: the four held setpoints measure
1.619–1.742 °C/W, so the 1.627 a hand-driven session implied is below the whole
range. R_int is not quite constant — 1.619 at 32–41 % duty against 1.742 at
58–61 %, most likely the probe being cooled by the airstream — and 1.709 sits
between the two, which is why the estimate errs in both directions and never by
much.

Four things that are load-bearing:

- **The PI may trim the feedforward by at most `kTrim` (15) duty points.** This
  is the fix for every failure the 0.5.0 run found, and they were all one
  defect: the error during an approach is "model error" plus "the plant has not
  got there yet", and nothing separated them, so the PI demanded the fan
  compensate for the plant's own time constant. Both signs were measured —
  arming from a hand-set 100 % fan preseeded the integrator to **+103.4 points**
  and pinned the fan at its ceiling for 9.4 min with the die already 10.3 °C
  *below* setpoint; and on a cold start `kp·err` subtracted 29.7 points from a
  feedforward that was already correct, flooring the fan for 40 s and
  overshooting by 3.5 °C.
- **The `ctl.*` channels are declared whenever the loop COULD run** — EMC2101
  and MAX31855 both present — never because it happens to be running, and they
  read `nan` while off. The host fixes its metric list and CSV header at
  discovery and never reconnects, so a channel appearing later would be a column
  that board can never grow. **Restart the app after flashing.**
- **`fan <percent>` is refused while the loop is on** (`E fan: control loop
  active, set ctl.mode off first`). Two writers on one register is a fault
  nobody can reproduce.
- **Failsafe is 100 %, not `ctl.max`** — a failsafe must not depend on a
  configured limit being sane. The matrix inverted in 0.6.0: losing the
  **INA228** now triggers it, and losing the **thermocouple** does not. Rule:
  fail to 100 % only when there is no way to estimate the die at all. Losing
  board air triggers it too when there is no `tc_`, since open loop needs air.

The fan register is written only when the quantised level **changes**, with
hysteresis of a quarter-level. Rounding alone is not enough: a demand that
settles on a level boundary flips on noise far below what the thermocouple can
resolve — a 60 °C heatsink target parks at 59.5234 % against a 59.5238 %
boundary, and two of every three steps wrote, forever.

**The EMC2101's writes are deliberate.** `CONFIG.ALT_TCH` at start-up makes
pin 6 the tach input. The fan setting (0x4C) is written by the host command
`fan <percent>` (0-100, 64 steps, replies `OK fan <applied>`) and, since 0.5.0,
by the control loop above — never by anything else, and never by both at once. Adafruit's `begin()` forces
100% duty; this firmware leaves the chip at its power-on 0%. A set duty is
kept until the next command or a power cycle — **not** cleared when the host
disconnects, since a cooling fan quitting with the app is the worse failure.
Direct drive relies on `FAN_CONFIG.PROG` (0x4A bit 5), the power-on default;
`set_duty()` checks it. `emc_regs` prints a read-only register dump.

Verified 2026-11-16: `fan 100` spins the fan, `fan 0` stops it.

**The tach needs the Adafruit breakout's `TACH` solder jumper bridged.** A fan
tach is open collector; that jumper is the board's only pull-up on the line and
ships **open**. Without it the tach reads 0xFFFF (stalled) while the fan turns,
with ALT_TCH correctly set (CONFIG 0x84) — the chip simply sees no edges. A
hand-held short is not good enough: it read, but noisily (per-second means
3860–6220 rpm, single samples up to 12 770 — contact bounce counted as edges).

Measured 2026-11-18 with the jumper soldered, Noctua NF-A4x10 5V PWM (green
wire on TACH), 1 s reports, settled:

| duty | rpm | spread within a report |
| --- | ---: | --- |
| 100 % | 4190 → 4290, still spinning up | < 1 % |
| 74.6 % | 4320 → 4380 (the climb from 100 % continued) | < 1 % |
| 50.8 % | 3163–3173 | < 0.3 % |
| 30.2 % | 1918–1923 | < 0.3 % |
| 0 % | `nan`, `stall` = 4/4 | — |

Above ~75 % the fan is at or near its ceiling, so the top of the range buys
little; 50 % and 30 % are clean and proportional. The fan takes well over
10 s to settle at full speed, so measure a step after it, not during.

**Reconnecting the Qwiic chain needs a `rescan`.** The firmware scans I2C at
boot only; a bus plugged back in while the board is powered stays
`present=0` (INA228, FS3000 and EMC2101 all at once) until the host sends
`rescan` or the board is replugged.

**The MAX31855K is detected by its two reserved bits** (D17, D3), which are
always 0 — an unwired MISO reads all ones or all zeros and is rejected, so
`present=1` means a real chip answered. Its K-type conversion is the chip's own
linear 41.276 uV/°C, good to about ±2 °C near room-to-a-few-hundred °C; no NIST
correction is applied. A fault (`fault_bits=1` = open, the usual cause being a
probe not seated in the screw terminal) makes `max31855.tc` `nan`, so the GUI
shows a gap rather than the cold-junction temperature. First reading,
2026-11-16: chip present, cold junction 23.76 °C (EMC2101 die 23.0 °C beside
it), thermocouple reporting open circuit — correctly, as no thermocouple wire
was fitted yet.

**A K-type junction was fitted on 2026-11-22**, bonded with Grizzly Kryonaut
between the fins at the centre of the Hailo-8's heatsink, so `max31855.tc` and
the `usb <port>_MAX31855` column carry values from that session onward. Two
things follow. **It measures the heatsink, not the die**: the Hailo's own
TS0/TS1 sit on the silicon and reached 108 °C during the 11-21 soak, and this
junction should read well below that and lag it, so the useful quantity is the
die-to-heatsink difference as a function of fan duty rather than the absolute
number. And **no `rescan` is needed when only the junction changes** — the
reserved-bit check has reported `present=1` since 11-16, with the open-circuit
fault bit set; attaching the wire clears that bit on the next conversion. A
rescan is for a chip that was not on the bus at power-up.

## Host side

`src/Probes.cpp`, namespace `ra6m5` plus two probes; the design notes are in
CLAUDE.md under **The RA6M5 probe board**. In short: the board's INA228 appears
exactly like an FT232H shunt (`INA228 - <card>`, folding onto the card), the
airflow appears as `RA6M5 AIRFLOW` on its own **Airflow (m/s)** graph and CSV
column `usb <port>_AIRFLOW`, the EMC2101 die as `RA6M5 EMC2101` and the
thermocouple as `RA6M5 MAX31855` on the Temperature graph
(`usb <port>_EMC2101`, `usb <port>_MAX31855`), and the board is mapped in `config/ina228.conf` by
USB serial number.

**The fan** (2026-11-18) is its own `fan` family — `RA6M5 FAN RPM` and
`RA6M5 FAN DUTY`, CSV `usb <port>_FAN_RPM` / `_FAN_DUTY`, real units — but is
drawn on the Airflow graph for now, scaled onto its axis (rpm / 1000, duty /
10; the section title says so, the legend shows real values). A period in
which every tach sample stalled is logged as **0 rpm, not a gap**: with the
TACH pull-up fitted a stall means a stopped fan. **Telemetry -> Fan** in the
panel sends `fan <percent>` on every change and logs `fan duty set to N %`;
it starts at the duty the board reports, because the firmware keeps a duty
across host disconnects and opening the app must not stop a running fan.
Nothing stops the fan when the app exits either. The host takes the per-period **means**; the min/max columns
are on the wire but not yet consumed.

## Things that are deliberate

- **The INA228 is configured exactly as the FT232H driver configures it**
  (15 mOhm, 5 A, `ADC_CONFIG` 0xFB6B, RSTACC, BUVL 3.00 V + ALATCH), so a rail
  read through either probe is comparable. A faster ADC setting is the obvious
  later knob; changing it here alone would make the two probes disagree.
- **The firmware is the only reader of `DIAG_ALRT`.** Reading it clears the
  latch, and it is polled every 5 ms for the conversion-ready flag, so every
  latched undervoltage trip is counted rather than only the one a 1 Hz host
  read happened to catch.
- **The INA228 is identified by `DEVICE_ID`, not merely by an ACK**, so the
  planned EMC2101 at 0x4C can never be mistaken for one during the 0x40-0x4F
  scan.
- **An FS3000 checksum failure is a gap, never a value.** SparkFun's library
  ignores the failure and returns the corrupted reading; this firmware does
  not.

## First readings (2026-11-16)

Bench check on the Strix Halo host, no card under load: VBUS 3.3245 V × |I| 0.15005 A =
0.4988 W against POWER 0.4988 W; dE/dt 0.4985 W; dQ/dt -0.150 A. 5 INA228
conversions and 8 FS3000 samples per second, zero errors. **Current reads
negative — this breakout is wired IN+/IN- reversed too**, like the four on the
EPYC rig. FS3000 raw ~490-505 (≈0.19 m/s) in near-still air.

**Cross-checked against the card's own sensor, same day.** Mapped to Hailo (the
board sits on the Strix Halo's Hailo-8 rail) and with HailoRT installed, the
shunt folds onto the real `Hailo` row: `Hailo INA228 POWER` **0.4985 W** against
the Hailo-8's on-board INA231 `Hailo POW` **0.4973 W** — agreement to 0.2%.
That confirms the rail, and that 0.50 W is this card's genuine idle here,
below the 0.81-0.85 W the mb-hailo skill records on other boards.

**Under load, through the GUI** (log `mb-benchmark-20261116-102043.csv`,
ResNet-50, Hailo async depth 4): ~1195 fps; shunt 0.50 -> **3.43-3.47 W**
within a second of Start and back to 0.51 W at Stop; the INA231 read
3.33-3.39 W, so the shunt runs 1.5-2.5% above the card's own sensor under load
and, being the max, is what fps/W uses (~347 fps/W, 2.88 mJ/frame). The rail
sagged 3.324 -> 3.254 V at ~1.06 A.
