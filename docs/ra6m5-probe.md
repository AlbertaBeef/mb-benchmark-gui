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
| EMC2101 fan controller | Qwiic I2C | 0x4C | duty set by host command `fan` (fw 0.4.0); tach not reading yet |
| MAX31855K thermocouple, K-type | SPI, SCK D19 / DO D17 / CS D0 (P112) | — | supported (fw 0.3.0) |
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

**The EMC2101 has two writes, both deliberate.** `CONFIG.ALT_TCH` at start-up
makes pin 6 the tach input. The fan setting (0x4C) is written **only** by the
host command `fan <percent>` (0-100, 64 steps, replies `OK fan <applied>`);
nothing in the firmware drives the fan on its own. Adafruit's `begin()` forces
100% duty; this firmware leaves the chip at its power-on 0%. A set duty is
kept until the next command or a power cycle — **not** cleared when the host
disconnects, since a cooling fan quitting with the app is the worse failure.
Direct drive relies on `FAN_CONFIG.PROG` (0x4A bit 5), the power-on default;
`set_duty()` checks it. `emc_regs` prints a read-only register dump.

Verified 2026-11-16: `fan 100` spins the fan, `fan 0` stops it. **The tach
reads 0xFFFF (stalled) while the fan turns**, with ALT_TCH set (CONFIG 0x84):
the chip sees no edges, so the cause is on the wire — a fan tach is open
collector and needs a pull-up to 3.3 V on the TACH line. Until that is fixed
`emc2101.rpm` is `nan` and `emc2101.stall` counts every sample.

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

## Host side

`src/Probes.cpp`, namespace `ra6m5` plus two probes; the design notes are in
CLAUDE.md under **The RA6M5 probe board**. In short: the board's INA228 appears
exactly like an FT232H shunt (`INA228 - <card>`, folding onto the card), the
airflow appears as `RA6M5 AIRFLOW` on its own **Airflow (m/s)** graph and CSV
column `usb <port>_AIRFLOW`, the EMC2101 die as `RA6M5 EMC2101` and the
thermocouple as `RA6M5 MAX31855` on the Temperature graph
(`usb <port>_EMC2101`, `usb <port>_MAX31855`), and the board is mapped in `config/ina228.conf` by
USB serial number. The host takes the per-period **means**; the min/max columns
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
