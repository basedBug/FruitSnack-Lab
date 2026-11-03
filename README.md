# FruitSnack

ESP32-S3 firmware for a PID-controlled hotend: NTC100K/B3950 temperature sensing,
PWM heater drive, a light precision cockpit served from the chip's own
flash, and a 921600-baud serial console for debugging and fallback control.

Everything runs on the device — no cloud, no companion app, no internet connection
required once the cockpit is loaded.

## MATLAB and Simulink

- [`matlab/`](matlab/) — live UART logger, binary-frame probe, and system-identification notes.
- [`matlab/simulink/`](matlab/simulink/) — live scrolling scopes for temperature/setpoint and duty.

---

## 1. Hardware

| Function | Pin | Notes |
|---|---|---|
| Heater PWM | GPIO21 | 1 kHz, 8-bit LEDC. Drive an SSR or a MOSFET gate; **never** a heater directly. |
| NTC divider node | GPIO1 | ADC1_CH0. Must be an ADC1 pin (GPIO1–GPIO10 on the ESP32-S3). |
| UART1 TX / RX | GPIO43 / GPIO44 | 921600 baud, remapped onto the DevKitC-1 header pins. The console port. |
| USB | GPIO19 / GPIO20 | Native USB-Serial/JTAG, used by `pio device monitor`. |

Board: ESP32-S3-DevKitC-1-N8R2, Arduino core 3.3.x on ESP-IDF 5.5, PlatformIO
environment `esp32s3_custom`. The wiring is fixed by the PCB; nothing below is
selectable at runtime.

### Thermistor wiring

The NTC is on the **low side** of the divider, and the firmware is hard-wired for
exactly this arrangement:

```
3V3 ──[4.7k]──●── NTC100K (B3950) ── GND
              │
             GPIO1
```

The ADC therefore sees

```
node = 3.3 V · Rntc / (Rpull + Rntc)     Rpull = 4.7 kΩ
```

so the node voltage **falls** as the hot end heats up. The pull-up is 4.7 kΩ
(±10 %). Its value is a setting (`ntcPullupOhms`, default 4700) because it scales
the whole temperature curve — see *Sensor calibration* below.

| Hot end | NTC resistance | Node voltage |
|---|---|---|
| 25 °C | 100.0 kΩ | 3152 mV |
| 50 °C | 35.9 kΩ | 2918 mV |
| 100 °C | 7.0 kΩ | 1972 mV |
| 150 °C | 2.0 kΩ | 982 mV |
| 250 °C | 0.33 kΩ | 220 mV |

#### The ADC ceiling, and why a cold hot end reads high

The ESP32-S3's calibrated ADC at 11 dB attenuation is specified over
**0–2900 mV (±50 mV total error)**. Above that the conversion is undefined: the
raw code pins at 4095 somewhere around 3.05–3.11 V, and the reported voltage then
stops tracking the divider.

With a 4.7 kΩ pull-up the node sits at **3.152 V at 25 °C** — above that ceiling.
The ADC saturates there: the raw code pins at 4095 and the calibrated voltage
freezes near **3.08 V** (the exact saturation point varies per chip,
3.07–3.11 V), so the divider inversion sees 3.08 V instead of the true 3.152 V.
The firmware clamps the node at 3100 mV before the inversion as well, and the
`sensor` command prints the value the ADC actually reported, not the clamp. The
consequence is honest and deliberate:

* Below roughly **50 °C** the reported temperature is an **upper bound**, not a
  measurement: with the sensor at 25 °C the firmware reports about **34 °C**
  (32–35 °C depending on the chip's saturation point).
* The firmware flags this as the **`below-range`** warning. It is a **warning, not
  a fault** — a cold hot end still heats normally, and the reading is accurate
  within the ADC's specified error once the node drops into range at about 50 °C.

Above ~50 °C the node is inside the specified range and the reading is accurate:
100 °C → 1.97 V, 150 °C → 0.98 V, 250 °C → 0.22 V.

The PCB is fixed, so this cannot be corrected in hardware. The workaround is the
**sensor offset** (`sensorOffset`), set once against a reference thermometer. It is
a single additive trim on the reported temperature, so it is exact around the
temperature it was calibrated at; the error it corrects is largest at room
temperature and disappears once the node is inside the ADC's range.

### Sensor calibration

Two settings exist, and no others:

| Setting | Key | Default | Meaning |
|---|---|---|---|
| Sensor offset | `sensorOffset` | `0.0` | °C added to the reported temperature (clamped to ±50) |
| Pull-up resistor | `ntcPullupOhms` | `4700` | Ω between 3V3 and the node; **must equal the resistor on the board** |

The pull-up scales every temperature: the thermistor resistance is computed from
the configured value, so a 4.7 kΩ board resistor configured as 10 kΩ reports every
temperature far too cold. Change the resistor and the setting in the same breath.

`sensor` prints the live electrical state in four lines:

```
> sensor
node: 3080 mV (raw 4095)
thermistor: 65.8 kOhm
temperature: 34.7 C
warning: below-range
```

`warning:` prints `none` when the reading is healthy. `short`, `open` and `range`
are faults (the heater is switched off); `below-range` is the warning described
above.

Both values are editable from the dashboard's *Machine → Sensor Calibration* card,
as flat keys in `POST /api/config`, or from the console:

```
config set sensoroffset -7.3
config set ntcpullup 4700
```

### Power

The heater supply is separate from the regulator. Provide a common ground between
the heater supply and the ESP32, and size the MOSFET/SSR for the heater current.
The firmware only ever drives a logic-level gate.

---

## 2. Build, flash, monitor

```bash
pio run                      # build the firmware
pio run -t upload            # flash the application
pio run -t uploadfs          # flash the web dashboard into LittleFS
pio device monitor -b 921600 # USB-Serial/JTAG log output
```

**A blank partition is not formatted for you — run `pio run -t uploadfs`.** The
firmware mounts LittleFS *without* the format-on-fail option, so a transient
mount failure can no longer erase `config.json` and with it the WiFi credentials,
hostname, gains and calibration. The price is that an unformatted partition stays
unformatted: the boot log reports the failed mount and that "the partition is
NOT formatted", the dashboard answers **503** with a "Filesystem not mounted"
page until the image is written, and the device runs on defaults with nowhere to
persist settings. Flash the filesystem image once on every fresh board (and after
a partition-table change), then reload the page.

At the end of boot the banner says `ready`. If the control task could not be
created it says `not ready: the control loop is not running`, the failure is
logged as an error (the heater stays off), and the same is true for the net and
UART tasks — a task that cannot start is reported, never counted as a success.

`platformio.ini` pins the exact library versions that are known to build against
the pioarduino Arduino core 3.x for the ESP32-S3:

```
ESP32Async/ESPAsyncWebServer@3.12.1
ESP32Async/AsyncTCP@3.5.0
bblanchon/ArduinoJson@7.4.3
```

### Pin and limit overrides

The pins and safety limits are compile-time constants in `src/config.h` and can be
overridden without editing the file:

```bash
pio run -e esp32s3_custom --project-option="build_flags=-DPIN_HEATER_PWM=6 -DPIN_NTC=3"
```

### Partition layout

`partitions.csv` (8 MB flash) gives 3 MB per application slot and ~2 MB of
LittleFS for the dashboard:

```
nvs       0x9000    0x5000
otadata   0xe000    0x2000
app0      0x10000   0x300000
app1      0x310000  0x300000
littlefs  0x610000  0x1F0000
```

---

## 3. Networking

The device joins the configured WiFi network in station mode and announces itself
over mDNS, so no IP address has to be known:

```
http://fruitsnack.local
```

The hostname is configurable (`config set hostname …`) and is used as the device
name in the dashboard's top app bar. If no network is configured the device falls
back to a rescue access point (`FruitSnack-Setup`).

WiFi credentials, PID gains, setpoint, limits and the sensor calibration are
persisted in `/config.json` inside LittleFS and survive a reboot.

---

## 4. Web dashboard

The dashboard is a self-contained single-page app (no CDN, no build step) stored
in the LittleFS partition and served by `ESPAsyncWebServer`. The redesign is a
light, precision-instrument interface: warm off-white surfaces, hairline borders,
ink-black actions, a single cobalt accent for live data, and monospace telemetry.
It is still plain HTML, CSS and ES modules.

Routing is by hash: `#dashboard`, `#machine` and `#console` are real,
bookmarkable URLs, and an empty or unknown hash falls back to `#dashboard`. The
old `#temperature` bookmark is still supported: it redirects to `#dashboard` and
opens the sensor disclosure. All three sections live in the one document and are
shown or hidden by the router, so switching views fetches nothing.

The app bar is always visible. It carries the product mark and configured
hostname, the connection pill (Live / Polling / Reconnecting / Offline) with the
age of the last update, a heater badge that appears only when `heaterOn` is true,
and a red **Heater Off** action that returns the controller to `off` from any
mode. Faults that are not view-specific — `pwmReady:false`, an overtemp latch, a
sensor fault, an NTC warning — are raised in a global banner above the content,
whichever view is open.

| View | Contents |
|---|---|
| **Cockpit** | The hero gauge (actual temperature, target tick, delta to target, output %, raw reading, LEDC duty and sensor disclosure with node voltage, ADC code, thermistor resistance and warning), the control card (setpoint field, slider and −/+ steppers, Off / PID / Manual segmented mode, contextual manual duty, heater toggle), the temperature chart, System health (uptime, RSSI, free heap, LittleFS usage) and the live PID terms with a link to Machine. |
| **Machine** | PID gains (kp / ki / kd) with Apply and Reset, a live P / I / D readout with the saturation flag and current mode/output/setpoint, sensor calibration (offset °C, pull-up Ω) beside the live sensor figures, device information, memory and LittleFS usage, the log viewer, the collapsible raw state JSON with a copy button, and the reboot button behind a confirmation dialog. |
| **Console** | A full-height terminal with command history (↑/↓, kept in `localStorage`), quick buttons for `status` / `help` / `info`, an auto-scroll toggle, a clear button, and live device log/reply lines. |

On desktop the rail is 208 px wide and the panels are laid out on a 12-column
grid. Between 768 px and 1279 px the rail collapses to 72 px of icons; below
768 px it becomes a 62 px bottom tab bar with the safe-area inset added on
phones. Routes scroll to the top, and transitions and the section fade are
disabled under `prefers-reduced-motion`.

The cockpit is laid out as a control surface rather than a card wall: the hero
and the controls share the top row, the history chart sits under the hero, and
System health plus PID terms stack in the right rail. At 1440 × 900 the cockpit
fits without page scroll; below 900 px it becomes a single column with the two
rail cards side by side until the phone breakpoint.

The chart is drawn on a `<canvas>` with no chart library: temperature and setpoint
on the left °C axis, heater output as a soft filled percentage trace on the
right-hand axis, auto-scaling with hysteresis, device-pixel-ratio aware rendering
and redraw pausing while the tab is hidden. Hover or tap on the plot shows a
crosshair and a tooltip with time offset, actual temperature, target and output.
The 1 m and 5 m windows are real; the third button is labelled `Max · 225s`
because the firmware's fixed telemetry ring holds 900 samples at 4 Hz — about
225 s — which is all the device can serve. The websocket streams new samples
about once per second, while `GET /api/graph` serves at most the newest 200, so
the REST fallback has a much shorter history than the live stream.

### What the UI does not fake

The firmware's behaviour is unchanged; the redesign keeps the places where the
page could contradict it closed.

* **The heater badge follows `heaterOn`**, the real LEDC compare value, not the
  requested output. When `pwmReady` is false the badge cannot read ON (the duty is
  zero, so `heaterOn` is false), the output percentage and LEDC duty read `--`
  instead of a duty that never reaches the gate, the hero state chip says
  `Detached`, and the global banner explains that the output stage is not
  attached.
* **The setpoint's maximum is the device's effective `maxTemperature`** (default
  250 °C; 350 °C, `TEMPERATURE_CEILING_C`, is the firmware's hard ceiling), taken
  from the state payload rather than hard-coded in the page. The field is
  refreshed from `state.setpoint` whenever it is not being edited, so it shows the
  value the controller actually applied, not the value that was typed.
* **A refused mode change is an error.** `POST /api/mode` can answer 400 with
  `mode refused: sensor fault or overtemp latch`; the page shows that exact text
  inline and as a toast, and snaps the segmented control back, instead of
  reporting success.
* **A PID change that did not persist is a warning.** `POST /api/pid` answers
  `ok:true` with `persisted:false` when the gains were applied in RAM but
  `config.json` could not be written; the page says "Applied in RAM, not saved"
  and never claims success.
* **A corrupt `config.json` is refused**, not replaced: `POST /api/config`
  answers 500 `config unreadable`, and the calibration form shows the error and
  keeps the stored values.
* **The sensor warning is not buried.** `diag.ntcWarn` is rendered next to the
  hero temperature, inside the sensor disclosure, and in the Machine calibration
  card — and a thermistor resistance the device did not report renders as `--`,
  never as `0 Ω`.
* **An unreachable device is shown as unreachable.** While the socket is down
  every live surface is greyed out, write controls disable, and the app bar shows
  the age of the last update; after three failed REST polls it says it cannot
  reach the device.
* **Writes go over REST only.** The page posts setpoint, mode, manual duty, PID
  gains and configuration; the websocket carries console `cmd` frames and the
  `ping` heartbeat. The firmware still accepts the `setpoint` / `mode` / `manual`
  / `pid` message types for other clients.
* **Manual duty is contextual.** The duty field and slider appear only in Manual
  mode, and disappear when the controller reports another mode; there is no
  second, hidden duty writer on another page.

The page talks to the device over a websocket at `/ws` and also polls
`GET /api/state` every second as a REST fallback. It sends a `ping` every 20 s,
treats 60 s of silence as a dead socket, and reconnects with 1 s → 15 s backoff.
The log viewer on the Machine view polls `GET /api/logs` every 3 s; the Console
receives the same lines and command replies live over the socket.

## 5. APIs

### REST

| Method | Path | Body / query | Purpose |
|---|---|---|---|
| GET | `/api/state` | | full system snapshot |
| GET | `/api/graph?limit=600` | | recent telemetry samples (server caps the response at 200) |
| GET | `/api/config` | | persisted configuration (password masked) |
| POST | `/api/config` | partial config | update and persist settings |
| POST | `/api/setpoint` | `{"value":205}` | set the target temperature (clamped to the 350 °C ceiling, then to `maxTemperature`) |
| POST | `/api/mode` | `{"mode":"pid"}` | `off` / `pid` / `manual`; 400 + error when refused |
| POST | `/api/manual` | `{"duty":42}` | manual duty percent |
| POST | `/api/pid` | `{"kp":12,"ki":0.4,"kd":30}` | set the gains and persist; `ok:true` with `persisted:false` (and an `error`) when only RAM was updated |
| GET | `/api/info` | | firmware, chip, memory and storage details |
| GET | `/api/logs?limit=200` | | recent log lines (server caps the response at 60) |
| POST | `/api/reboot` | | restart the device; 500 `reboot task could not be created` if the deferred restart cannot start |

These are all the API routes. A `GET` for any other `/api/*` path is answered
with HTTP 404 and `{"ok":false,"error":"not found"}`, not with the dashboard
shell, so a mistyped endpoint is distinguishable from a missing page.

The configuration payload (`GET /api/config`, and the body accepted by
`POST /api/config`) looks like this — a partial body only changes the keys it
contains:

```json
{
  "wifiSsid": "lab-network",
  "wifiPassword": "********",
  "hostname": "fruitsnack",
  "kp": 12.0, "ki": 0.4, "kd": 30.0,
  "setpoint": 205.0,
  "maxTemperature": 250.0,
  "outputLimit": 100.0,
  "sensorOffset": 0.0,
  "ntcPullupOhms": 4700.0,
  "controlPeriodMs": 50,
  "pidEnabled": true
}
```

The stored password is never sent back in clear text, and posting the `********`
mask back does not overwrite it. Values are clamped on the way in:
`setpoint`/`maxTemperature` 0–350 °C, `outputLimit` 0–100 %, `sensorOffset`
±50 °C, `controlPeriodMs` 10–1000 ms, `ntcPullupOhms` must be > 0. The 350 °C
ceiling is a single constant, `TEMPERATURE_CEILING_C` in `src/config.h`; it is
equal to `SENSOR_MAX_VALID_C`, the top of the sensor's valid window, because a
higher limit could never be honoured — anything above it is treated as an
out-of-range sensor and faults instead of heating. The console `set` command and
`config set setpoint|maxtemp` clamp to the same value. A setpoint is clamped twice:
`POST /api/setpoint` clamps to the 350 °C ceiling and the controller then clamps to
the configured `maxTemperature`, so the reply can name a higher value than the one
that took effect — `state.setpoint` always carries the applied value.

If `config.json` is present but cannot be read (corrupt), writes are **refused**
instead of being replaced by defaults: `POST /api/config` answers HTTP 500 with
`{"ok":false,"error":"config unreadable"}` and the existing file is left exactly
as it was. Overwriting it with defaults would silently destroy the stored WiFi
credentials and hostname. A *missing* file is not an error — the defaults are
used and the new configuration is written.

`POST /api/mode` is checked, not assumed. While a sensor fault is active or the
overtemp latch is on, the controller refuses to leave `off`:

```http
HTTP/1.1 400 Bad Request
{"ok":false,"error":"mode refused: sensor fault or overtemp latch"}
```

The websocket `mode` message is answered with the same refusal in a `reply`
frame (`"ok":false`), and the console prints
`mode refused: sensor fault or overtemp latch (still off)`. The same refusal
applies to `on` and to the mode switch that `manual <duty>` performs. Returning
to `off` is always accepted.

### WebSocket (`/ws`)

Server → client, all as `{"type": …, "data": {…}}`:

| Type | Payload |
|---|---|
| `hello` | firmware, version, protocol revision, client count |
| `state` | the full snapshot, pushed ~5 Hz |
| `graph` | `{"samples":[{"t":ms,"temp":°C,"sp":°C,"out":%}]}` — new samples, pushed ~1 Hz |
| `log` | `{"level":"info","ts":ms,"text":"…"}` |
| `reply` | `{"id":n,"ok":bool,"text":"…"}` — answer to a client command |
| `pong` | `{"id":n}` — answer to `ping` |

The `state` object is:

```json
{
  "mode": "pid", "state": "heating",
  "setpoint": 205.0, "temperature": 204.74, "temperatureRaw": 204.83,
  "output": 38.2, "duty": 97, "heaterOn": true, "pwmReady": true,
  "overtemp": false, "sensorFault": false, "maxTemperature": 250.0,
  "pid": {"kp": 12.0, "ki": 0.4, "kd": 30.0,
          "p": 3.12, "i": 34.02, "d": 1.06, "saturated": false},
  "diag": {
    "adcMv": 425.0, "adcRaw": 528, "loopHz": 20,
    "ntcResistance": 696.0, "ntcWarn": ""
  }
}
```

`heaterOn` follows the **actual LEDC compare value**, not the requested output:
it is true only when `duty` is above zero. The payload also publishes
`pwmReady`, which is false when the LEDC channel could not be attached at boot —
the heater is then physically dead even though the PID or manual mode keeps
requesting a duty, and `duty` and `heaterOn` correctly stay 0. Because `heaterOn`
already tracks the real duty, no client has to interpret `pwmReady`; the key is
there for diagnostics, so a requested duty that never reaches the gate can be
told apart from a controller that simply asks for zero. `duty` is the 8-bit LEDC
count (0–255) that corresponds to `output`.

`diag` also carries `uptime` (ms), `heap`, `minHeap`, `fragmentation`, `rssi`,
`ip`, `mac` and `hostname`. `diag.ntcWarn` is an empty string when the reading is
healthy, otherwise `short`, `open`, `range` (faults) or `below-range` (warning
only). `temperature` is the filtered reading with the sensor offset applied,
`temperatureRaw` an unfiltered single sample without it.

Client → server:

```json
{"type":"cmd","data":{"id":1,"command":"set 205"}}
{"type":"setpoint","data":{"value":205}}
{"type":"mode","data":{"mode":"pid"}}
{"type":"manual","data":{"duty":42}}
{"type":"pid","data":{"kp":12,"ki":0.4,"kd":30}}
{"type":"ping"}
```

The dashboard itself sends only `cmd` and `ping`; the write message types remain
in the protocol so an existing client does not break, but new clients should use
the REST endpoints above.

---

## 6. Serial console

The console is the hardware UART on GPIO43 (TX) / GPIO44 (RX) at **921600 baud**,
so the device stays controllable if the network is down. On the
ESP32-S3-DevKitC-1, the **UART USB port** is already wired to those pins through
its USB-to-UART bridge, so a separate adapter is not needed. The native
USB-Serial/JTAG port (`Serial`) carries the same log output for
`pio device monitor`, but its baud rate is cosmetic and the firmware does not
read commands from it.

### Telemetry tap

The UART also emits a read-only telemetry line every 250 ms (4 Hz):

```text
184250,203.84,205.00,79
```

The four columns are `ms` (boot time), `temp_c`, `setpoint_c`, and `duty` (raw LEDC 0-255).
It is always on and has no command interface; use the dashboard or REST API to
change the setpoint, mode, or manual duty. Log lines share the same UART and are
ignored by a numeric parser. See `matlab/` for a ready-made live
plot/logger; it echoes each received line and saves a `.raw.txt` copy.

For live Simulink scopes, send STX (`0x02`) to switch the same UART to an
18-byte binary frame at 4 Hz: the `0xAA 0x55` sync header followed by four
little-endian `single` values (seconds, temperature, setpoint, duty). The header
lets the receiver discard a partial frame and resync. ETX (`0x03`) restores the
ASCII stream. UART log text is suppressed while binary so packets stay intact;
native USB and the dashboard keep logging. `matlab/simulink/` builds the model
with a MATLAB System block, so it does not need the Instrument Control Toolbox
Simulink serial blocks. The model enables Simulink pacing so the scopes run at
wall-clock speed, and it retries STX because opening the port can reset the
board before the first command arrives.

```
help                       show the command list
status                     print the system state
set <celsius>              set the temperature setpoint
sp                         show the setpoint
on | off                   enable / disable PID control
mode <off|pid|manual>      set the control mode
manual <0-100>             set the manual duty percent
duty                       show the current duty
pid                        show the PID gains and live terms
pid set <kp> <ki> <kd>     set the PID gains
pid save                   persist the gains
config                     show the persisted configuration
config set <key> <value>   change a config value
save                       persist the current set
clear                      clear the graph history
graph [n]                  print the last n samples as CSV
info                       print the device information
wifi                       print the link status
wifi scan                  scan for networks
wifi reconnect             reconnect now
mdns                       print the hostname
sensor                     print the thermistor diagnostics
reboot                     restart the device
```

`config set` keys: `ssid`, `password`, `hostname`, `kp`, `ki`, `kd`, `setpoint`,
`maxtemp`, `outputlimit`, `sensoroffset`, `ntcpullup` (also accepted as
`ntcpullupohms`). `controlperiodms` and `pidenabled` are shown by `config` and
accepted in the JSON payload, but have no console setter.

A session looks like this:

```
fruitsnack> set 205
setpoint = 205.00 C
fruitsnack> status
mode: pid
state: heating
temperature: 204.74 C (raw 204.83 C)
setpoint: 205.00 C
output: 38.2 %
duty: 97
pid: kp=12.000 ki=0.400 kd=30.000
pid terms: p=3.12 i=34.02 d=1.06 saturated=no
uptime: 512.4 s
sensor warning: none
adc: 3080 mV (raw 4095)
```

`sensor warning:` repeats `diag.ntcWarn`: `none` when the reading is healthy,
`below-range` when the node sits above the ADC's specified range (so the
temperature is only an upper bound, see *The ADC ceiling* above), and
`short` / `open` / `range` for a real fault. `adc:` is the calibrated node
voltage followed by the raw 12-bit code behind it — the same pair shown in the
Cockpit sensor disclosure and the Machine calibration card. When a condition is
latched, `status` appends `WARNING: over temperature` and/or `WARNING: sensor fault`.

---

## 7. Control model

* **Acquisition** — `analogReadMilliVolts()` (core-calibrated) on GPIO1 at 11 dB
  attenuation, with the node clamped to 3100 mV once it is above the ADC's usable
  range. Divider inversion `R = Rpull·a/(1−a)` with `a = node / 3.3 V`, then
  Klipper's beta model `1/T = 1/T1 + (1/β)·ln(R/R1)` (T1 = 25 °C, R1 = 100 kΩ,
  β = 3950), a 5-sample median filter, an EMA (α = 0.25) and the additive sensor
  offset.
* **PID** — positional, output in percent. The derivative acts on the measurement
  (no setpoint kick) and is EMA-filtered; the integrator is clamped and frozen
  while the output is saturated, so a cold start cannot wind up.
* **Loop** — 20 Hz control task, 4 Hz telemetry sampling, 5 Hz websocket state
  push.

### Safety

* **Overtemp** — above the configured limit the heater output is forced to zero
  and latched until the temperature drops 5 °C below the limit.
* **Sensor fault** — a shorted divider node (`short`), a node at the supply rail
  (`open`) or a computed temperature outside the valid window (`range`) forces
  the output to zero and reports `sensorFault`. The `below-range` warning is not
  a fault and does not stop heating.
* **Output limits** — the PID output is clamped to `outputLimit`, and the
  integrator is frozen while saturated.
* **Mutex timeout** — every entry point takes the control mutex with a 20 ms
  timeout. If the control task cannot get it in time it switches the heater off,
  reports `state: fault` and logs `control: mutex timeout`, instead of skipping
  the cycle (a skipped cycle would also skip the overtemp and sensor checks and
  leave the previous duty applied).
* **Ceiling** — `TEMPERATURE_CEILING_C` (350 °C, equal to `SENSOR_MAX_VALID_C`)
  bounds every user-settable setpoint and maximum temperature, from the console,
  REST and websocket alike, so a limit above the sensor's valid window can never
  be stored.
* **Fail-safe defaults** — a corrupt or out-of-range stored configuration cannot
  raise the temperature limit or the output limit beyond their legal range, and a
  corrupt file is never overwritten with defaults.

---

## 8. Project layout

```
src/
  main.cpp             task setup, logging, UART console reader
  config.h             pins, limits, timings (compile-time overridable)
  app_state.h          shared data contracts
  heater_controller.h  the control interface consumed by the web/console layers
  heater_controller.cpp control loop, safety, telemetry snapshots
  ntc_sensor.h/.cpp    divider inversion, Klipper beta model, median/EMA filtering, warnings
  heater.h/.cpp        LEDC PWM output
  pid.h/.cpp           PID controller
  telemetry.h/.cpp     fixed-size sample ring
  network_manager.h/.cpp WiFi station, mDNS, scanning, reconnect
  settings.h/.cpp      /config.json persistence
  logger.h/.cpp        levelled logging, serial/UART fan-out, ring buffer
  state_json.h/.cpp    JSON serialisation of the state/config payloads
  console.h/.cpp       shared command interpreter
  web_server.h/.cpp    AsyncWebServer, websocket, REST endpoints

data/                  the dashboard, uploaded to LittleFS
  index.html
  css/fruitsnack.css
  js/api.js, app.js, chart.js, console.js, controls.js, debug.js, sensor.js, ws.js
  favicon.svg

matlab/
  fruitsnack_log.m     UART telemetry logger
  serial_probe.m       binary-frame probe
  MATLAB_IMPLEMENTATION.md
  README.md
  simulink/
    build_live_scope.m       live-scope model builder
    FruitSnackSerialSource.m MATLAB System block source
    README.md

tools/
  sim/                 host-side simulation of the control core (Arduino shim)
  check_webui.ps1      dashboard lint (syntax, assets, external refs)
  check_protocol.py    firmware JSON <-> browser contract check
```

---

## 9. Verification

The control core is exercised on the development machine, without hardware, by
compiling the **real** firmware sources against a small Arduino shim and running
them against a synthetic thermal plant:

```bash
C:\msys64\usr\bin\bash.exe -lc 'export PATH=/ucrt64/bin:/usr/bin:$PATH; bash tools/sim/run_tests.sh'
```

This covers the NTC curve, PID convergence and stability, integrator anti-windup,
the overtemp and sensor-fault heater cut-offs and the telemetry ring buffer. A
second host test drives the console command interpreter and the settings store
end to end (clamping, rejected input, persistence, every command group).

Cockpit and contract checks:

```powershell
powershell -File tools/check_webui.ps1     # JS syntax, assets, no external resources
python tools/check_protocol.py             # firmware JSON vs browser expectations
```

`check_protocol.py` builds the real `src/state_json.cpp` for the host, extracts
the key names the firmware writes for every payload, parses the payload
handlers in `data/js/*.js` and fails if the browser reads a field the firmware
does not provide — so the two halves of the interface cannot drift apart
silently.

### Hardware validation checklist

The simulation cannot cover ADC calibration error, real thermal inertia or WiFi
behaviour. Before relying on the device:

1. Confirm the heater is off on boot and that the overtemp latch actually trips
   (lower the limit below the current temperature).
2. Compare the reading against a reference thermometer at room temperature and
   set the sensor offset accordingly — the node is above the ADC's usable range
   below ~50 °C, so an offset is expected on this board.
3. Confirm the cockpit loads over `http://fruitsnack.local` and that the graph,
   all three hash routes, the `#temperature` redirect, the console and the Machine
   view all update.
4. Confirm the UART console at 921600 baud answers `status`.

### Troubleshooting

**The reading is wrong at room temperature.**
With the 4.7 kΩ pull-up the node is above the ADC's usable range below ~50 °C, so
the reported value is an **upper bound** (about 34 °C when the sensor is at 25 °C)
and the firmware reports `below-range`. Set the sensor offset once against a
reference thermometer, and make sure `ntcPullupOhms` equals the resistor on the
board (4.7 kΩ) — a wrong pull-up rescales every temperature. `sensor` prints the
node voltage, the computed resistance and the warning.

**The heater stays off and `status` reports a sensor fault.**
`sensor` shows which code is active: `short` means the node is at 0 V (shorted
sensor or missing pull-up), `open` means it sits at the rail (thermistor
disconnected), `range` means the computed temperature is outside the valid
window. Check the divider against the wiring diagram above; the NTC belongs
between the node and GND.

**The page shows "Handler did not handle the request".**
That is the ESPAsyncWebServer's 501 fallback: a request reached a handler that
produced no response. With the current firmware the shell route checks the
filesystem first and answers **503** with an explicit page instead, so a 501 is a
sign that the LittleFS partition is not mounted at all (a missing dashboard image
is caught). The reason is logged at boot either way.

```bash
pio run -t upload      # firmware + partition table
pio run -t uploadfs    # the dashboard image
```

**The dashboard shows "Filesystem not mounted".**
The LittleFS partition could not be mounted. Either the partition table on the
device does not match `partitions.csv`, or — most often on a fresh board — the
partition has never been formatted. The firmware deliberately mounts **without**
format-on-fail (so a transient error cannot wipe `config.json`), so it will not
create the filesystem itself: flash the firmware once (`pio run -t upload`) so the
table is written, then `pio run -t uploadfs` to create and populate the filesystem.

**"Dashboard not installed" but the API works.**
`/api/info` answers and reports `"dashboard": false` — the firmware is fine, the
filesystem image is missing. Run `pio run -t uploadfs`.

**The page loads but the websocket never connects.**
Check that the browser can reach `/ws` directly and that no proxy is rewriting
the URL. The client reconnects automatically with backoff, and the connection pill
in the app bar shows the live state and the age of the last update.
