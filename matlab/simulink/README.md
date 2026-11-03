# Live Simulink scopes

This folder builds a Simulink model that reads the FruitSnack UART telemetry
and displays two live, scrolling scopes:

- **Temperature** — process value and setpoint, in °C, with a legend and
  auto-scaling axes
- **Duty** — raw heater duty, 0–255, with a legend and auto-scaling axes

It uses a standard **MATLAB System** block with `serialport` from Instrument
Control Toolbox. It does **not** use the `instrumentblks` Simulink serial
blocks, so it works even when that library is not installed.

The model runs with **simulation pacing** enabled, so simulation time tracks
wall time and the scopes scroll at the 4 Hz telemetry rate.

## Requirements

- MATLAB R2022a or newer
- Simulink
- Instrument Control Toolbox (`serialport`, `read`, `write`, `serialportlist`)

## Wiring

Use the ESP32-S3-DevKitC-1 **UART USB port** (the USB-to-UART bridge on
GPIO43/44), not the native USB/JTAG port. Baud is **921600**. Close PlatformIO's
serial monitor, the MATLAB logger, or anything else that owns the COM port
before starting the model.

If you are not sure which COM port is the UART one, run
`serialportlist("available")` with both cables connected, then unplug the UART
cable and run it again. The port that disappears is the UART port. The native
USB port only carries log lines; it does not carry the telemetry tap and does
not accept STX.

## Protocol

The model sends STX (`0x02`) at simulation start and retries it until the first
frame arrives. This matters because opening a serial port can reset the ESP32
through the DTR/RTS auto-reset circuit; the board then boots in ASCII mode, so
a single STX sent during boot is lost. The retry also recovers a device that
was reset by a cable or power cycle.

The firmware then sends one 18-byte frame every 250 ms:

| Bytes | Type | Meaning |
|---|---|---|
| 0–1 | `uint8` | sync header `0xAA 0x55` |
| 2–5 | `single` | seconds since boot |
| 6–9 | `single` | temperature, °C |
| 10–13 | `single` | setpoint, °C |
| 14–17 | `single` | raw duty, 0–255 |

All floats are little-endian. The receiver scans for the header, so a partial
first frame or a dropped byte is discarded rather than permanently shifting the
stream. ETX (`0x03`) is sent when the simulation stops, returning the firmware
to the ASCII telemetry used by `fruitsnack_log`.

## Quick start

```matlab
cd matlab/simulink
clear classes          % run this if you have run an older version in this MATLAB session
build_live_scope("COM9")
```

Use your UART COM port. The builder:

- verifies the port exists with `serialportlist`
- prints the `FruitSnackSerialSource.m` file MATLAB loaded
- sets the Source block to **Interpreted execution**
- verifies the Source block's actual `Port`
- enables simulation pacing at 1x real time
- creates `fruitsnack_live_scope` and opens it

Press **Run**. The scopes open automatically. The Command Window prints the
first frame, then one status line every 40 frames (10 s):

```text
FruitSnackSerialSource: first frame: t=184.25 s, temp=203.84 C, setpoint=205.00 C, duty=79
```

The **Latest** Display block shows the four current values; a vector of `NaN`
means no frame has arrived yet.

To keep the model:

```matlab
build_live_scope("COM9", "Save", true)
```

If the builder reports that the port is not available, fix the port name or
the cable. It no longer silently falls back to a default port.

## Scope legends and axes

The builder names the signals feeding the scopes, so the legends read:

- **Temperature** — `Temperature` and `Setpoint`
- **Duty** — `Duty`

It also configures both scopes with:

| Setting | Temperature | Duty |
|---|---|---|
| `YLimits` initial | `[0 300]` °C | `[0 255]` |
| `AxesScaling` | `Auto` | `Auto` |
| `YLabel` | `Temperature (C)` | `Duty (0-255)` |
| `ShowLegend` | on | on |
| `TimeUnits` | seconds | seconds |

Because `AxesScaling` is `Auto`, the y-axis fits the incoming data during and
after the run — you do **not** need to press **Scale Y-axis Limits**. The
initial limits show the full expected range before the first frame arrives.

If you prefer stable, fixed axes, set `AxesScaling` to `Manual` on the Scope
block after building; the initial limits above are already sensible.

The legend labels come from the Simulink signal names, which is why the builder
sets them on the `Demux → Scope` lines. R2022a's Scope block has no separate
`LegendLabels` property.

## Troubleshooting

**The model runs far too fast and the scopes stay empty**

The Source block is non-blocking, so without pacing Simulink executes the
fixed-step loop as fast as MATLAB can run it. Simulation time races ahead of
wall time, and 120 s of scope window can pass in a fraction of a second.
The builder enables pacing, but if you are using an older model, run:

```matlab
set_param('fruitsnack_live_scope','EnablePacing','on')
set_param('fruitsnack_live_scope','PacingRate','1')
```

You can confirm the model was racing by checking the number of logged samples.
The To Workspace block logs a 4x1 frame per step, so `fs_log.Data` is 4x1xN:

```matlab
d = squeeze(fs_log.Data).';   % N x 4: seconds, temp, setpoint, duty
size(d, 1)                    % samples; millions for a short run means racing
d(1:5, :)
fs_log.Time(1:5)              % simulation time for those samples
```

Regenerate with the current `build_live_scope` to get the paced model.

**Trace is off-screen or the axes stay at -10 to 10**

The builder sets the initial limits and enables auto-scaling. If you are using
an older model, apply them to the open scopes:

```matlab
cfg = get_param('fruitsnack_live_scope/Temperature','ScopeConfiguration');
cfg.YLimits = [0 300];
cfg.AxesScaling = 'Auto';
cfg.ShowLegend = true;

cfg = get_param('fruitsnack_live_scope/Duty','ScopeConfiguration');
cfg.YLimits = [0 255];
cfg.AxesScaling = 'Auto';
cfg.ShowLegend = true;
```

Set `YLimits` before `AxesScaling`: assigning manual limits after `Auto` puts
the scope back into `Manual` mode.

**Scopes blank, all values NaN**

- Confirm the Source block `Port` is the **UART** COM port, not native USB.
- Confirm nothing else owns the port (`pio device monitor`, MATLAB logger,
  PuTTY, another MATLAB session).
- Confirm the 18-byte-header firmware is flashed. The older 16-byte frame has no
  `0xAA 0x55` header.
- Run `serial_probe` to separate firmware from Simulink.
- Watch the Command Window for the one-time warning. It distinguishes “no
  bytes at all” from “bytes are arriving but no `0xAA 0x55` header.”

**MATLAB is loading the old class**

Run:

```matlab
which -all FruitSnackSerialSource
which -all build_live_scope
```

Both should point at this folder. If they point somewhere else (for example
an older checkout), MATLAB is running a stale copy.
Close the model, fix the path, then run `clear classes` or restart MATLAB.
MATLAB caches class definitions, so replacing the file alone is not enough.

**Simulink freezes or the Stop button takes forever**

Regenerate the model with the current `build_live_scope`; older versions did a
blocking read with a multi-second timeout. The current Source block never
blocks. If it still freezes, the serial port is probably valid but the firmware
is not sending binary frames; use the probe below.

**Verify the device independently**

Run this before Simulink, with every monitor and logger closed:

```matlab
addpath("..")
serial_probe("COM9")
```

It opens the port, sends STX every 0.5 s (so a boot reset cannot lose it),
prints up to three decoded frames, and sends ETX on exit. If it prints values,
the firmware and port are fine and the remaining problem is the Simulink model.

**"serialport is not supported for code generation"**

Select the **Source** block → **Simulate using: Interpreted execution**. The
builder sets this for newly generated models.

**Port busy**

Close every other program using the COM port. Windows allows one owner.

**Simulation errors when opening the port**

- Check the baud rate is 921600.
- Check the COM port still exists; USB bridges renumber after replugging.
- Check TX/RX wiring if you are not using the DevKitC-1 UART USB port.
- If the dashboard becomes unreachable while the model runs, the port open is
  resetting or holding the board; the Source block now deasserts DTR/RTS and
  retries STX, but an old model will not.

**Want ASCII logging again**

Stop the simulation (this sends ETX), then run the MATLAB logger. If MATLAB
crashed before `releaseImpl` ran, run `fruitsnack_log("COM7")` or send `0x03`
from a terminal; the logger sends ETX automatically when it opens the port.

**`instrumentblks` error**

This model does not use that library. If you see the error, you are opening an
older model; recreate it with `build_live_scope`.
