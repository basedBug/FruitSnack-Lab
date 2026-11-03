# FruitSnack MATLAB logger

`fruitsnack_log.m` plots and records the read-only telemetry tap on the
FruitSnack UART. It requires MATLAB R2019b or newer with Instrument Control
Toolbox (`serialport` / `readline`). No System Identification Toolbox is
required.

The complete copy-paste version of the script is in
[`MATLAB_IMPLEMENTATION.md`](MATLAB_IMPLEMENTATION.md).

## Wiring

Use the **UART USB port** on the ESP32-S3-DevKitC-1. That port is the
USB-to-UART bridge wired to GPIO43 (TX) / GPIO44 (RX); it is not the native
USB-Serial/JTAG port. The firmware prints one line every 250 ms at 921600 baud:

```text
184250,203.84,205.00,79
```

| Column | Meaning |
|---|---|
| `ms` | `millis()` since boot, not wall-clock time |
| `temp_c` | filtered hotend temperature in °C |
| `setpoint_c` | target temperature in °C |
| `duty` | raw LEDC duty, 0–255 |

Log lines from the console share the same UART. The reader ignores anything
that is not exactly four comma-separated numbers, so those lines are harmless.
The tap has **no commands**: change the setpoint, mode, or manual duty from the
web dashboard, or over the REST API.

Every received line is printed to the command window as `RX: ...` and written
to `<name>.raw.txt`. If the plot stays empty, inspect that file first.

## Usage

```matlab
% Live plot until Ctrl+C
fruitsnack_log("COM7")

% Record a 10-minute run into runs/step_200C.csv and .mat
fruitsnack_log("COM7", "Duration", 600, "Output", "runs/step_200C")

% Explicit baud
fruitsnack_log("COM7", "Baud", 921600)
```

On exit (Ctrl+C or `Duration`), the script closes the port and writes:

- `<name>.raw.txt` — every received serial line, verbatim
- `<name>.csv` — `time_s,temp_c,setpoint_c,duty`
- `<name>.mat` — table `T` plus metadata: port, baud, start time, sample
  count, nominal sample period, and the duty units

`duty / 255` is the 0–1 actuator input for system identification. The firmware
also exposes `outputPct` in `/api/state` if you prefer percent.

The logger sends ETX (`0x03`) when it opens the port, so it always returns a
firmware left in binary mode by a Simulink run to the normal ASCII stream.

## Binary probe

`serial_probe("COM9")` opens the UART port, sends STX every 0.5 s until a frame
arrives, prints up to three decoded `0xAA 0x55` frames, and sends ETX on exit.
The retry matters because opening the port can reset the ESP32, and a single
STX sent during boot would be lost. Use it to verify the firmware stream
independently of Simulink. Close any monitor or logger first.

## Live Simulink scopes

For scrolling scopes instead of a MATLAB figure, see `simulink/`. The
`build_live_scope` helper creates a model with temperature/setpoint and duty
scopes and switches the firmware to binary telemetry while it runs. Stop the
simulation, then run `fruitsnack_log("COM7")` once to restore ASCII.

## Changing the input

The stream is observation-only. Use the Cockpit controls, or `webwrite`:

```matlab
base = "http://192.168.1.50";
opts = weboptions("MediaType", "application/json", "Timeout", 5);
webwrite(base + "/api/mode",    jsonencode(struct("mode", "manual")), opts);
webwrite(base + "/api/manual",  jsonencode(struct("duty", 40)), opts);
webwrite(base + "/api/setpoint", jsonencode(struct("value", 200)), opts);
```

## Troubleshooting

- **No samples:** the wrong COM port is selected. Use `serialportlist`; the
  native USB port does not carry the telemetry tap.
- **Port busy:** close `pio device monitor` (or any other terminal) first;
  Windows allows one owner per COM port.
- **Samples stop:** the device rebooted or the USB bridge reset. `ms` restarts
  near zero; start a new recording.
- **Occasional non-numeric lines:** expected console logs; the parser skips
  them.
- **`serialport` undefined:** MATLAB is older than R2019b; use the legacy
  `serial` object or upgrade.
- **Stream rate:** 4 Hz is set by `TELEMETRY_PERIOD_MS` (250 ms) in
  `src/config.h`. It is plenty for a thermal response; raise the rate only if
  you need sub-250 ms resolution.

MATLAB is not installed in the development environment used to write this
script, so it has been syntax-reviewed but not executed here.
