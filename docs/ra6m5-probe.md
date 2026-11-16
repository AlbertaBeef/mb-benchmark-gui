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
| EMC2101 fan controller | Qwiic I2C | 0x4C | planned |
| MAX31855K thermocouple | SPI (CS = D0/P112) | — | planned |
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
