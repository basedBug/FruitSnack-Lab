# FruitSnack Frontend Redesign — Binding Implementation Contract

## 1. Purpose + Scope

This contract defines the three-view web UI for the FruitSnack ESP32-S3 hotend controller. It is the binding specification against which the implementation and the review lane will be measured.

**Built:** **Cockpit**, **Machine**, **Console**. Persistent app bar, global fault banner, left nav rail / bottom tab bar, inline SVG sprite, hash routing, a temperature gauge, one shared chart on the Cockpit, REST-first writes, WebSocket retained only for console commands and heartbeat.

**Cockpit** is the primary view: a hero temperature gauge with target, delta, output, raw reading, LEDC duty and sensor disclosure; the setpoint and mode controls; the temperature history chart; system health and live PID terms.

**Machine** contains PID gains, live loop terms, sensor calibration, device information, memory/storage, live logs, raw state and reboot. **Console** contains the terminal and command history.

**Explicitly not built:** temperature presets, a Settings page, WiFi credential editing, print job / G-code / queue, macros, webcam, bed mesh, fans, general output pins, multi-tool / bed / chamber panels, toolboards, file storage, update/upload UI.

## 2. Frozen Contract

The following file names, handler names, parameter names, and payload-property access patterns are frozen. Renaming any of them will silently void `tools/check_protocol.py`.

| File | Handler | Parameter | Rule for reading payload properties |
|------|---------|-----------|-------------------------------------|
| `controls.js` | `onState` | `s` | plain dotted access on `s` only: `s.mode`, `s.state`, `s.setpoint`, `s.temperature`, `s.temperatureRaw`, `s.output`, `s.duty`, `s.heaterOn`, `s.pwmReady`, `s.overtemp`, `s.sensorFault`, `s.maxTemperature`, `s.pid.*`, `s.diag.*` |
| `controls.js` | `onConfig` | `c` | plain dotted access on `c` only: `c.hostname`, `c.kp`, `c.ki`, `c.kd`, `c.setpoint`, `c.maxTemperature`, `c.outputLimit` |
| `controls.js` | `onHello` | `h` | plain dotted access on `h` only: `h.fw`, `h.version`, `h.protocol`, `h.clients` |
| `sensor.js` | `render` | `s`, `diag` | exactly one `render(` in `sensor.js`; plain dotted access on `s` and `diag`: `s.temperature`, `diag.adcMv`, `diag.adcRaw`, `diag.ntcResistance`, `diag.ntcWarn` |
| `sensor.js` | `renderConfig` | `c` | plain dotted access on `c`: `c.sensorOffset`, `c.ntcPullupOhms` |
| `debug.js` | `renderState` | `s` | plain dotted access on `s`; may pass `s.diag` fields to `sensor.render(s, …)` |
| `debug.js` | `renderConfig` | `c` | plain dotted access on `c`; may pass `c.sensorOffset` / `c.ntcPullupOhms` to `sensor.renderConfig(...)` |
| `debug.js` | `renderInfo` | `info` | plain dotted access on `info`: `info.firmware`, `info.version`, `info.chip`, `info.sdk`, `info.core`, `info.heapSize`, `info.freeHeap`, `info.minFreeHeap`, `info.flashSize`, `info.psramSize`, `info.littlefsTotal`, `info.littlefsUsed`, `info.uptime`, `info.ip`, `info.rssi`, `info.hostname`, `info.dashboard` |
| `console.js` | `addLog` | `entry` | plain dotted access on `entry`: `entry.level`, `entry.ts`, `entry.text` |
| `console.js` | `addReply` | `reply` | plain dotted access on `reply`: `reply.ok`, `reply.id`, `reply.text` |

`chart.js` must continue reading sample fields exactly as: `t`, `temp`, `sp`, `out`.

Inside the handler bodies: no optional chaining (`?.`), no destructuring, no aliasing, no helper functions that receive the payload. The gate's regex must see the dotted chain directly on the declared parameter name.

`gauge.js` is a presentation helper. It must not read firmware payloads; `controls.js` passes numeric values into it.

## 3. DOM id Inventory

### App shell ids

`app`, `app-bar`, `app-title`, `app-host`, `app-state`, `app-badge`, `conn-pill`, `last-update`, `heater-off-btn`, `nav`, `nav-fw`, `nav-ip`, `global-alert`, `alert-text`, `main`, `toast-stack`.

### Cockpit page ids

`dashboard`, `dash-title`, `dash-note`, `state-chip`, `state-dot`, `state-label`, `gauge-svg`, `gauge-progress`, `gauge-target`, `dash-temp`, `hero-target`, `hero-delta`, `dash-setpoint`, `dash-output`, `dash-raw`, `dash-duty`, `dash-primary-warning`, `sensor-disclosure`, `sensor-summary`, `detail-adc-mv`, `detail-adc-raw`, `detail-ntc-resistance`, `detail-warning`, `write-status`, `sp-input`, `sp-slider`, `sp-minus`, `sp-plus`, `sp-max-label`, `mode-group`, `mode-note`, `mode-error`, `manual-block`, `manual-input`, `manual-slider`, `heater-toggle`, `control-hint`, `chart-slot-dashboard`, `temp-chart`, `chart-tooltip`, `chart-windows`, `chart-range`, `sys-uptime`, `sys-rssi`, `sys-heap`, `sys-littlefs`, `sys-storage-fill`, `loop-p`, `loop-i`, `loop-d`, `loop-saturated`.

### Machine page ids

`machine`, `machine-title`, `gain-kp`, `gain-ki`, `gain-kd`, `apply-gains`, `reset-gains`, `gain-note`, `term-p`, `term-i`, `term-d`, `saturated`, `machine-mode`, `machine-output`, `machine-setpoint`, `sensor-warn-machine`, `sensor-diag`, `sensor-offset`, `ntc-pullup`, `save-cal`, `device-info`, `memory-info`, `log-viewer`, `state-json`, `copy-state`, `reboot-btn`, `reboot-dialog`, `reboot-confirm-btn`, `reboot-cancel-btn`.

### Console page ids

`console`, `console-title`, `console-conn`, `terminal`, `terminal-lines`, `console-form`, `console-input`, `autoscroll`, `console-clear`, quick-command buttons `cmd-status`, `cmd-help`, `cmd-info`.

### Removed / redirected ids and sections

- Removed: `app-temp`, `app-setpoint`, `mode-select`, `temp-duty-input`, `temp-duty-slider`, `chart-slot-temperature`, `sensor-warn-temp`, the `temperature` section and `temperature-title`.
- `#temperature` is a legacy route. It redirects to `#dashboard` and opens the sensor disclosure so old bookmarks still reveal the sensor detail.

## 4. Fact Table

| Item | Value | Notes |
|------|-------|-------|
| State push rate | 5 Hz | `WS_BROADCAST_PERIOD_MS = 200` |
| Telemetry rate | 4 Hz | `TELEMETRY_PERIOD_MS = 250` |
| Device sample ring | 900 samples | equals 225 s of history |
| `GET /api/graph` cap | 200 samples | equals 50 s max |
| `GET /api/logs` cap | 60 lines | server limits response |
| Default effective ceiling | 250 °C | `state.maxTemperature` / `AppSettings::maxTemperatureC` |
| Hard ceiling | 350 °C | `TEMPERATURE_CEILING_C`; server clamps requests here |
| Mode refusal string | `mode refused: sensor fault or overtemp latch` | exact server text |
| Log timestamps | device uptime milliseconds | use `bootEpoch = Date.now() - s.diag.uptime` |
| `s.duty` | LEDC count (0–255) | never rendered as a percentage |
| `s.output` | percent (0–100) | the only heater output percentage shown |
| `s.heaterOn` | `duty > 0` | badge truth source |
| `s.pwmReady` | LEDC attached flag | `false` suppresses output % and heating claims |
| Gauge scale | 0 → `s.maxTemperature` | fall back to 350 before the first state |
| Chart windows | 60 s / 300 s / 225 s | `Max · 225s` is the device ring limit |

## 5. Honesty Rules

- Never render `s.output` or `s.duty` as live values when `s.pwmReady === false` or the device is offline; show `--`, set the state chip to `Detached`, and raise the global banner stating the output stage is not attached.
- Never render `s.duty` as a percentage; render it only as a raw LEDC count.
- The app-bar heater badge follows `s.heaterOn` and nothing else. It is hidden when the heater is off; on the mobile app bar it collapses to a small amber dot beside the power action.
- The setpoint input/slider `max` must come from `s.maxTemperature` (350 before the first state frame); reconcile the displayed setpoint from `state.setpoint`, never from a POST reply `value`.
- Treat `persisted:false` as a warning toast, never a success toast.
- All writes are REST-first; delete every `.catch(() => ws.X())` fallback. The WebSocket is retained only for `cmd` and `ping`.
- Empty numeric input is a validation error, not `0`; write the server's clamped value back into the field.
- The fault banner lives in the app shell in its own grid row above `<main>` and is visible on every page; it must never overlap or tint the page content.
- The `below-range` warning (`diag.ntcWarn`) must be shown near the primary temperature in the hero, in the sensor disclosure, and on the Machine calibration card.
- Render log timestamps as uptime-derived offsets; never use `new Date(ts)`.
- Replace the sample buffer on every REST refetch, append only when `t` advances from WebSocket frames, and clear on `t` regression (device reboot).
- Chart x-axis is anchored to the selected window: `xMin = xMax - windowS * 1000`; an empty range draws no traces.
- The Max window is `225 s`, the device ring; never label it as a larger duration.
- Use exactly one Chart instance, mounted on the Cockpit. It must not be cloned or recreated on route changes.
- Bind each write control in exactly one place; manual duty exists only on the Cockpit and is visible only in Manual mode.
- Disable every write control until the first `state` frame and again while `body[data-conn]` indicates a lost connection.
- After 3 consecutive fetch failures show "Cannot reach device" instead of freezing stale values.
- The app-bar action is "Heater Off"; it sends `mode: off` unconditionally with no confirmation and rolls back on transport failure.
- The reboot action keeps a confirmation dialog.
- Every element lookup goes through one `need(id)` helper that throws `missing #<id>`.
- `localStorage` is guarded and degrades to in-memory when unavailable.
- Subscriber exceptions are isolated and reported to the Console as an error line.
- The mode control is a segmented button group, not a `<select>`. It updates optimistically, rolls back on failure, and shows the exact server refusal text inline.
- Numeric inputs keep the focus guard. The focus guard applies to the 5 Hz state push only; rollbacks and clamp write-backs must always land, so they use `forceSet`.
- The `#temperature` route redirects to `#dashboard` and opens the sensor disclosure; it must not leave a dead section in the DOM.

## 6. Interaction Contract

| Control | Sends | Commit trigger | Validation / clamping | Rollback | Failure message |
|---------|-------|----------------|----------------------|----------|-----------------|
| Setpoint number | `POST /api/setpoint {value}` | `change` | non-empty required; clamp 0–`s.maxTemperature` | restore last known `state.setpoint` | `err.serverError ?? err.message` |
| Setpoint slider | `POST /api/setpoint {value}` | `input` debounced 200 ms | clamp 0–`s.maxTemperature` | restore last known `state.setpoint` | `err.serverError ?? err.message` |
| Setpoint −/+ steppers | `POST /api/setpoint {value}` | click | clamp 0–`s.maxTemperature`, step 1 °C | restore last known `state.setpoint` | `err.serverError ?? err.message` |
| Mode segmented group | `POST /api/mode {mode}` | click | `off`/`pid`/`manual`; optimistic active state | restore last known `state.mode` | `err.serverError ?? err.message`; if refused, show `mode refused: sensor fault or overtemp latch` inline and as a toast |
| Heater Off (app bar) | `POST /api/mode {mode:"off"}` | click | unconditional | restore previous mode on transport failure | `err.serverError ?? err.message` |
| Heater toggle (Cockpit) | `POST /api/mode {mode}` | click | toggles between `off` and `pid` | restore previous mode | `err.serverError ?? err.message` |
| Manual duty (Cockpit) | `POST /api/manual {duty}` | number `change`, slider `input` debounced 200 ms | non-empty; clamp 0–100; visible only in Manual | restore last known `s.output` | `err.serverError ?? err.message` |
| PID Apply | `POST /api/pid {kp,ki,kd}` | click | all non-empty; `kp` 0–1000, `ki` 0–100, `kd` 0–10000; write clamped values back | restore `state.pid.kp/ki/kd` into fields | `err.serverError ?? err.message`; on `persisted:false` show warning: "Applied in RAM, not saved" |
| PID Reset | none | click | restores fields from `store.config` | n/a | n/a |
| Sensor calibration Save | `POST /api/config {sensorOffset, ntcPullupOhms}` | click | non-empty; offset ±50, pull-up > 0 | restore `config.sensorOffset` / `config.ntcPullupOhms` | `err.serverError ?? err.message` |
| Copy raw state | none | click | clipboard API with textarea fallback | n/a | n/a |
| Reboot | `POST /api/reboot` | click after dialog confirm | none | n/a | `err.serverError ?? err.message` |
| Console command | `ws {type:"cmd", data:{id, command}}` | Enter / Send | non-empty | n/a | "Not connected — command not sent" when socket down |
| Sensor disclosure | none | `<summary>` toggle | n/a | n/a | n/a |

## 7. Layout Contract

**Color palette (light precision):** background `#f4f4f1`, surface `#ffffff`, subtle surface `#fafaf7`, sunken surface `#efefea`, text `#17171a` / `#4d4d55` / `#73737c`, borders `#e4e3dd` / `#d2d1ca`, accent `#1d4ed8`, success `#137a3b`, warning `#a85a06`, danger `#b42318`.

**Typography:** system UI stack for labels and prose, `ui-monospace` for all live numbers. 14 px body, 11 px uppercase overlines, 23 px page title, up to 56 px gauge display. Accent color is reserved for live data, links, active state and focus; primary actions are ink-black.

**Spacing scale:** 4–48 px; 12 px card radius, 9 px control radius, hairline borders, one 1 px card shadow.

**Breakpoints:**
- Desktop: ≥1280 px — 208 px nav rail, 12-column grid, 16 px gutters, 24 px page padding.
- Tablet: 768–1279 px — 72 px icon rail, 12-column grid, 16 px gutters.
- Narrow: <900 px — Cockpit and Machine collapse to one column; the rail becomes two cards side by side.
- Mobile: <768 px — 62 px bottom tab bar plus `env(safe-area-inset-bottom)`, 16 px page padding, single-column cards.
- Small mobile: <460 px — two-column stat grid, single-column gains form, compact page head.

**Panel order / grid spans:**
- **Cockpit:** global alert (full width); hero (span 8) + controls (span 4); chart (span 8) + rail (span 4: System health, PID terms). Below 900 px, all cockpit cards are single-column and the two rail cards sit side by side until 767 px.
- **Machine:** PID gains (span 7) + live terms (span 5); sensor calibration (span 7) + device (span 5); live logs (span 7) + memory/storage (span 5); raw state (span 7) + reboot (span 5). Single column below 900 px.
- **Console:** terminal card fills the viewport below the app bar, page head and tab bar, with a minimum height.

**Navigation:** persistent top app bar; nav order **Cockpit → Machine → Console**. Hash routing: `#dashboard`, `#machine`, `#console`. Unknown or empty hash defaults to `#dashboard`; `#temperature` redirects to `#dashboard` and opens the sensor disclosure. Route changes scroll to the top.

**Fault banner layout:** the global alert occupies its own grid row (`grid-area: alert`) above `<main>` on all breakpoints; it must not share a grid cell with `<main>`.

## 8. File Map

| File | Responsibility | Page owner |
|------|----------------|------------|
| `data/index.html` | Shell, nav rail, three `<section>` pages, inline SVG sprite, top-level reboot `<dialog>`, `<main>` + toast stack | shell |
| `data/css/fruitsnack.css` | Light theme variables, layout grid, gauge/chart/card components, responsive breakpoints, reduced-motion block | shell |
| `data/js/api.js` | REST helpers; returns parsed JSON; rejected fetch exposes `{message, status, serverError}` | shared |
| `data/js/ws.js` | WebSocket connection, reconnect, `ping`, `cmd` send only | shared |
| `data/js/app.js` | Store, pub/sub, section/hash routing, REST state polling, sample buffer lifecycle, orchestrates modules | shared |
| `data/js/gauge.js` | Thin 270° temperature arc and target tick; values passed in by Controls | Cockpit |
| `data/js/chart.js` | Single canvas chart; reads `t`/`temp`/`sp`/`out`; hover crosshair/tooltip; window selector; mounted on Cockpit | Cockpit |
| `data/js/controls.js` | App bar, connection/fault banner logic, Cockpit hero + setpoint/mode/heater/manual controls, system/PID snapshots, toasts | Cockpit |
| `data/js/sensor.js` | Sensor disclosure values, Machine calibration form, `render(s, diag)` | Cockpit + Machine |
| `data/js/debug.js` | Machine page: PID gains, live terms, device/memory info, live logs, raw state, reboot | Machine |
| `data/js/console.js` | Terminal, command history, quick commands, autoscroll, log/reply rendering | Console |

## 9. Verification

**Gate scripts:**
- `tools/check_webui.ps1` — JS syntax, flat module layout, asset presence, no external URLs, module imports resolve, DOM ids, payload size.
- `tools/check_protocol.py` — firmware JSON keys match UI handler reads; chart sample fields match.

**Per-step manual checks:**
1. Cold load at `#dashboard`: app bar renders, gauge at zero, all readouts show `--`, controls disabled, chart shows "Waiting for telemetry…".
2. First state frame: gauge arc and target tick move, numbers populate, state chip updates, controls enable, setpoint slider max equals `s.maxTemperature`.
3. Set setpoint to 260 with default `maxTemperature=250`: field snaps to 250; −/+ steppers move by 1 and clamp; server reply does not override the display.
4. Switch mode Off / PID / Manual: the segmented active state follows; Manual reveals duty; a refused change shows the exact text inline and rolls back.
5. Heater Off from the app bar works from any view and scroll position, with no confirmation.
6. Disconnect WiFi: body gets `data-conn="offline"`, live surfaces grey/freeze, write controls disable, "last update N s ago" appears; after 3 fetch failures show "Cannot reach device".
7. Reconnect: state resumes, banner clears, controls re-enable.
8. Induce `pwmReady=false`: output % and duty become `--`, state chip reads Detached, global banner appears, heater badge hidden.
9. Induce sensor fault / overtemp: mode change returns refusal text verbatim; fault banner visible on all pages; the NTC warning appears near the gauge.
10. Change PID gains with unreadable `config.json`: warning toast, fields revert; verify clamped values land in the fields.
11. Chart: 1m / 5m / Max · 225s switch the window anchor; hover/tap shows time, actual, target and output; empty windows draw no traces.
12. Direct load `#machine`: logs poll immediately, gains populate, raw state expands and copies; reboot opens the confirmation dialog.
13. Console: `status`, `help`, `info` quick commands work; ↑/↓ history, auto-scroll toggle, clear; timestamps are uptime-relative.
14. Load `#temperature`: it redirects to `#dashboard` and opens the sensor disclosure.
15. Open and close the sensor disclosure on Cockpit; values match the Machine calibration card.

**Browser matrix:**
- Desktop 1440 × 900: online, offline, reconnecting, fault, reboot; cockpit fits without page scroll.
- Tablet 1024 × 768: icon-only rail, online, offline; chart and controls remain legible.
- Mobile 390 × 844: bottom tab bar, safe-area inset, online, offline, reconnecting; cockpit and console usable.
- Additional check at 1920 × 1080 and 360 px width.

## 10. Open Items

None.
