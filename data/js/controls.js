import { store, need } from './app.js';
import { Gauge } from './gauge.js';

// WARN_TEXT is duplicated in controls.js and sensor.js by design; keep both copies in sync.
const WARN_TEXT = {
  short: 'Node is at 0 V: the sensor is shorted, or the pull-up resistor is missing.',
  open: 'Node is at the supply rail: the thermistor is disconnected.',
  'below-range': 'Node voltage is above the ADC\'s 2.9 V limit: the reading is an upper bound below ~50 °C. Apply an offset to match a reference thermometer.',
  range: 'The computed temperature is outside the valid window: the sensor or the pull-up value is wrong.',
};

const MODE_LABEL = { off: 'Off', pid: 'PID', manual: 'Manual' };
const STATE_LABEL = {
  heating: 'Heating',
  cooling: 'Cooling',
  idle: 'Idle',
  overtemp: 'Overtemp',
  fault: 'Fault',
};

function warnText(code) {
  return WARN_TEXT[code] || ('Sensor warning: ' + code);
}

export class Controls {
  constructor(api) {
    this.api = api;
    this.lastState = null;
    this.connected = false;
    this.modePending = false;
    this.modeOverride = null;
    this.modeTimer = null;

    this.els = {
      appTitle: need('app-title'),
      appHost: need('app-host'),
      connPill: need('conn-pill'),
      connLabel: need('conn-pill').querySelector('.label'),
      lastUpdate: need('last-update'),
      appBadge: need('app-badge'),
      heaterOffBtn: need('heater-off-btn'),
      globalAlert: need('global-alert'),
      alertText: need('alert-text'),
      navFw: need('nav-fw'),
      navIp: need('nav-ip'),
      dashNote: need('dash-note'),

      stateChip: need('state-chip'),
      stateDot: need('state-dot'),
      stateLabel: need('state-label'),
      gaugeSvg: need('gauge-svg'),
      gaugeProgress: need('gauge-progress'),
      gaugeTarget: need('gauge-target'),
      dashTemp: need('dash-temp'),
      heroTarget: need('hero-target'),
      heroDelta: need('hero-delta'),
      dashSetpoint: need('dash-setpoint'),
      dashOutput: need('dash-output'),
      dashRaw: need('dash-raw'),
      dashDuty: need('dash-duty'),
      dashPrimaryWarning: need('dash-primary-warning'),
      sensorSummary: need('sensor-summary'),

      writeStatus: need('write-status'),
      spInput: need('sp-input'),
      spSlider: need('sp-slider'),
      spMinus: need('sp-minus'),
      spPlus: need('sp-plus'),
      spMaxLabel: need('sp-max-label'),
      modeGroup: need('mode-group'),
      modeButtons: Array.from(need('mode-group').querySelectorAll('button')),
      modeNote: need('mode-note'),
      modeError: need('mode-error'),
      manualBlock: need('manual-block'),
      manualInput: need('manual-input'),
      manualSlider: need('manual-slider'),
      heaterToggle: need('heater-toggle'),
      controlHint: need('control-hint'),

      sysUptime: need('sys-uptime'),
      sysRssi: need('sys-rssi'),
      sysHeap: need('sys-heap'),
      sysLittlefs: need('sys-littlefs'),
      sysStorageFill: need('sys-storage-fill'),

      loopP: need('loop-p'),
      loopI: need('loop-i'),
      loopD: need('loop-d'),
      loopSaturated: need('loop-saturated'),
    };

    this.gauge = new Gauge(this.els.gaugeProgress, this.els.gaugeTarget, this.els.gaugeSvg);

    this.bindAppBar();
    this.bindControls();
    this.bindManualDuty();

    store.subscribe('state', s => this.onState(s));
    store.subscribe('config', c => this.onConfig(c));
    store.subscribe('hello', h => this.onHello(h));
    store.subscribe('connection', d => this.onConnection(d));
    store.subscribe('info', info => this.onInfo(info));
  }

  onHello(h) {
    if (h && h.fw) {
      this.els.navFw.textContent = h.fw + (h.version ? ' ' + h.version : '');
    }
  }

  onConfig(c) {
    if (!c) return;
    if (c.hostname) {
      this.els.appHost.textContent = c.hostname;
      if (this.connected) this.els.dashNote.textContent = 'Connected to ' + c.hostname + '.';
    }
  }

  onInfo(info) {
    if (!info) return;
    if (info.firmware) {
      this.els.navFw.textContent = info.firmware + (info.version ? ' ' + info.version : '');
    }
    if (info.ip) this.els.navIp.textContent = info.ip;
    if (info.hostname) this.els.appHost.textContent = info.hostname;

    const used = info.littlefsUsed;
    const total = info.littlefsTotal;
    this.els.sysLittlefs.textContent = this.bytes(used) + ' / ' + this.bytes(total);
    const pct = total > 0 ? Math.max(0, Math.min(100, (used / total) * 100)) : 0;
    this.els.sysStorageFill.style.width = pct.toFixed(1) + '%';
  }

  onState(s) {
    this.lastState = s;
    const fmt = (n, d = 1) => (typeof n === 'number' ? n.toFixed(d) : '--');
    const maxTemp = typeof s.maxTemperature === 'number' ? s.maxTemperature : 350;
    const mode = s.mode || 'off';

    this.els.dashTemp.textContent = fmt(s.temperature, 1);
    this.els.dashSetpoint.textContent = fmt(s.setpoint, 1);
    this.els.dashRaw.textContent = fmt(s.temperatureRaw, 1);
    this.els.heroTarget.textContent = (s.setpoint > 0)
      ? 'Target ' + fmt(s.setpoint, 1) + ' °C'
      : 'Target off';

    this.gauge.render(s.temperature, s.setpoint, maxTemp, this.connected);

    if (s.pwmReady && this.connected && typeof s.output === 'number') {
      this.els.dashOutput.textContent = String(Math.round(s.output));
    } else {
      this.els.dashOutput.textContent = '--';
    }

    if (s.pwmReady && this.connected && typeof s.duty === 'number') {
      this.els.dashDuty.textContent = String(Math.round(s.duty));
    } else {
      this.els.dashDuty.textContent = '--';
    }

    this.renderStateChip(s, mode);
    this.renderDelta(s, mode, maxTemp);

    if (this.connected && s.heaterOn) {
      this.els.appBadge.hidden = false;
      this.els.appBadge.textContent = 'Heater on';
    } else {
      this.els.appBadge.hidden = true;
    }

    this.els.spInput.max = maxTemp;
    this.els.spSlider.max = maxTemp;
    this.els.spMaxLabel.textContent = fmt(maxTemp, 0) + ' °C';
    this.safeSet(this.els.spInput, s.setpoint);
    this.safeSet(this.els.spSlider, s.setpoint);

    const uiMode = this.modeOverride || mode;
    if (this.modeOverride && s.mode === this.modeOverride) {
      this.modeOverride = null;
    }
    this.applyMode(uiMode);
    this.els.modeNote.textContent = MODE_LABEL[uiMode] || uiMode;

    const inManual = uiMode === 'manual';
    this.els.manualBlock.hidden = !inManual;
    if (inManual) {
      this.safeSet(this.els.manualInput, s.output);
      this.safeSet(this.els.manualSlider, s.output);
    }

    if (s.pid) {
      this.els.loopP.textContent = fmt(s.pid.p, 2);
      this.els.loopI.textContent = fmt(s.pid.i, 2);
      this.els.loopD.textContent = fmt(s.pid.d, 2);
      this.els.loopSaturated.hidden = !s.pid.saturated;
    } else {
      this.els.loopP.textContent = '--';
      this.els.loopI.textContent = '--';
      this.els.loopD.textContent = '--';
      this.els.loopSaturated.hidden = true;
    }

    if (s.diag) {      this.els.sysUptime.textContent = this.duration(s.diag.uptime);
      this.els.sysRssi.textContent = s.diag.rssi != null ? s.diag.rssi + ' dBm' : '--';
      this.els.sysHeap.textContent = this.bytes(s.diag.heap);
    }

    this.updateAlert(s);
    this.updateDisabled();
  }

  renderStateChip(s, mode) {
    let label = STATE_LABEL[s.state] || 'Idle';
    let tone = 'neutral';

    if (!this.connected) {
      label = 'Offline';
      tone = 'neutral';
    } else if (!s.pwmReady) {
      label = 'Detached';
      tone = 'fault';
    } else if (s.overtemp || s.sensorFault) {
      label = 'Fault';
      tone = 'fault';
    } else if (mode === 'off') {
      label = 'Heater off';
      tone = 'neutral';
    } else if (s.state === 'heating') {
      tone = 'heating';
    } else if (s.state === 'cooling') {
      tone = 'live';
    } else if (s.state === 'idle') {
      tone = 'target';
    }

    this.els.stateLabel.textContent = label;
    this.els.stateChip.dataset.tone = tone;
  }

  renderDelta(s, mode, maxTemp) {
    let text = 'Waiting for telemetry';
    let tone = 'neutral';

    if (!this.connected) {
      text = 'Waiting for connection';
    } else if (!s.pwmReady) {
      text = 'Output stage unavailable';
      tone = 'danger';
    } else if (typeof s.temperature !== 'number' || typeof s.setpoint !== 'number') {
      text = 'Waiting for telemetry';
    } else if (mode === 'off') {
      text = s.setpoint > 0
        ? 'Heater off · target ' + s.setpoint.toFixed(1) + ' °C'
        : 'Heater off · no target';
    } else {
      const diff = s.setpoint - s.temperature;
      if (Math.abs(diff) <= 1.5) {
        text = 'At target · within 1.5 °C';
        tone = 'target';
      } else if (diff > 0) {
        text = '+' + diff.toFixed(1) + ' °C to target';
        tone = 'heating';
      } else {
        text = '+' + Math.abs(diff).toFixed(1) + ' °C above target';
        tone = 'heating';
      }
    }

    if (maxTemp > 0 && s.temperature > maxTemp) {
      text = 'Above configured ceiling';
      tone = 'danger';
    }

    this.els.heroDelta.textContent = text;
    this.els.heroDelta.dataset.tone = tone;
  }

  updateAlert(s) {
    const messages = [];
    let danger = false;

    if (!s.pwmReady) {
      messages.push('Heater output stage not attached — commands have no effect.');
    }
    if (s.overtemp) {
      messages.push('Overtemperature latch active.');
      danger = true;
    }
    if (s.sensorFault) {
      messages.push('Sensor fault — heating is blocked.');
      danger = true;
    }
    if (s.diag && s.diag.ntcWarn) {
      messages.push(warnText(s.diag.ntcWarn));
    }

    if (messages.length > 0) {
      this.els.globalAlert.hidden = false;
      this.els.globalAlert.dataset.tone = danger ? 'danger' : 'warn';
      this.els.alertText.textContent = messages.join(' · ');
    } else {
      this.els.globalAlert.hidden = true;
      this.els.alertText.textContent = '';
    }

    if (s.diag && s.diag.ntcWarn) {
      this.els.dashPrimaryWarning.textContent = warnText(s.diag.ntcWarn);
      this.els.dashPrimaryWarning.hidden = false;
    } else {
      this.els.dashPrimaryWarning.hidden = true;
      this.els.dashPrimaryWarning.textContent = '';
    }
  }

  onConnection(d) {
    this.connected = d.online;

    const pill = this.els.connPill;
    const label = this.els.connLabel;
    if (d.online && d.wsOnline) {
      pill.dataset.state = 'online';
      label.textContent = 'Live';
    } else if (d.online) {
      pill.dataset.state = 'online';
      label.textContent = 'Polling';
    } else if (d.reconnecting) {
      pill.dataset.state = 'reconnecting';
      label.textContent = 'Reconnecting';
    } else {
      pill.dataset.state = 'offline';
      label.textContent = 'Offline';
    }

    if (d.fetchFailures >= 3) {
      this.els.lastUpdate.textContent = 'Cannot reach device';
    } else if (d.staleS != null) {
      this.els.lastUpdate.textContent = 'last update ' + d.staleS + ' s ago';
    } else {
      this.els.lastUpdate.textContent = '';
    }

    this.els.writeStatus.textContent = this.connected ? 'Connected' : 'Read-only';
    this.els.writeStatus.dataset.tone = this.connected ? 'ready' : 'blocked';
    this.els.dashNote.textContent = this.connected
      ? (store.config && store.config.hostname ? 'Connected to ' + store.config.hostname + '.' : 'Live telemetry over REST.')
      : (d.fetchFailures >= 3 ? 'Cannot reach device.' : 'Waiting for telemetry.');

    if (this.lastState) this.onState(this.lastState);
    this.updateDisabled();
  }

  applyMode(mode) {
    this.els.modeButtons.forEach(btn => {
      btn.classList.toggle('active', btn.dataset.mode === mode);
    });
    const active = mode !== 'off';
    this.els.heaterToggle.textContent = active ? 'Turn off heater' : 'Start PID';
    this.els.heaterToggle.classList.toggle('btn-primary', !active);
    this.els.heaterToggle.classList.toggle('btn-danger', active);
    this.els.controlHint.textContent = active
      ? 'Heater commands apply immediately.'
      : 'Select a mode to enable the heater.';
  }

  updateDisabled() {
    const base = !this.connected || !this.lastState;
    this.els.spInput.disabled = base;
    this.els.spSlider.disabled = base;
    this.els.spMinus.disabled = base;
    this.els.spPlus.disabled = base;
    this.els.heaterToggle.disabled = base;
    this.els.heaterOffBtn.disabled = base;
    this.els.modeButtons.forEach(btn => { btn.disabled = base || this.modePending; });

    const manual = !base && (this.modeOverride || (this.lastState && this.lastState.mode)) === 'manual';
    this.els.manualInput.disabled = !manual;
    this.els.manualSlider.disabled = !manual;
  }

  bindAppBar() {
    this.els.heaterOffBtn.addEventListener('click', () => {
      this.requestMode('off', true);
    });
  }

  bindControls() {
    const { spInput, spSlider, modeGroup, heaterToggle } = this.els;

    this.sendSp = debounce((v) => {
      if (!this.lastState) return;
      const raw = parseFloat(v);
      if (isNaN(raw)) {
        showToast('Setpoint is required', 'error');
        this.forceSet(spInput, this.lastState.setpoint);
        this.forceSet(spSlider, this.lastState.setpoint);
        return;
      }
      const max = typeof this.lastState.maxTemperature === 'number' ? this.lastState.maxTemperature : 350;
      const val = clamp(raw, 0, max);
      this.forceSet(spInput, val);
      this.forceSet(spSlider, val);
      this.api.postSetpoint(val).catch(err => {
        showToast(err.serverError ?? err.message, 'error');
        if (this.lastState) {
          this.forceSet(spInput, this.lastState.setpoint);
          this.forceSet(spSlider, this.lastState.setpoint);
        }
      });
    }, 200);

    spSlider.addEventListener('input', () => {
      spInput.value = spSlider.value;
      this.sendSp(spSlider.value);
    });

    spInput.addEventListener('change', () => {
      spSlider.value = spInput.value;
      this.sendSp(spInput.value);
    });

    this.els.spMinus.addEventListener('click', () => this.stepSetpoint(-1));
    this.els.spPlus.addEventListener('click', () => this.stepSetpoint(1));

    modeGroup.querySelectorAll('button').forEach(btn => {
      btn.addEventListener('click', () => {
        this.requestMode(btn.dataset.mode, false);
      });
    });

    heaterToggle.addEventListener('click', () => {
      if (!this.lastState) return;
      const next = (this.modeOverride || this.lastState.mode) === 'off' ? 'pid' : 'off';
      this.requestMode(next, false);
    });
  }

  stepSetpoint(delta) {
    if (!this.lastState) return;
    const max = typeof this.lastState.maxTemperature === 'number' ? this.lastState.maxTemperature : 350;
    const current = parseFloat(this.els.spInput.value);
    const base = isNaN(current) ? this.lastState.setpoint : current;
    const val = clamp(base + delta, 0, max);
    this.forceSet(this.els.spInput, val);
    this.forceSet(this.els.spSlider, val);
    this.sendSp(val);
  }

  requestMode(next, force) {
    if (!this.lastState || !this.connected) return;
    if (!force && next === (this.modeOverride || this.lastState.mode)) return;

    const previous = this.lastState.mode || 'off';
    this.modePending = true;
    this.modeOverride = next;
    this.els.modeError.hidden = true;
    this.applyMode(next);
    this.els.modeNote.textContent = MODE_LABEL[next] || next;
    this.updateDisabled();

    this.api.postMode(next)
      .then(() => {
        this.modePending = false;
        this.updateDisabled();
        clearTimeout(this.modeTimer);
        this.modeTimer = setTimeout(() => {
          if (this.modeOverride === next) {
            this.modeOverride = null;
            if (this.lastState) this.applyMode(this.lastState.mode || 'off');
            this.updateDisabled();
          }
        }, 1600);
      })
      .catch(err => {
        this.modePending = false;
        this.modeOverride = null;
        this.applyMode(previous);
        this.els.modeNote.textContent = MODE_LABEL[previous] || previous;
        const message = err.serverError ?? err.message;
        this.els.modeError.textContent = message;
        this.els.modeError.hidden = false;
        showToast(message, 'error');
        this.updateDisabled();
      });
  }

  bindManualDuty() {
    const { manualInput, manualSlider } = this.els;

    const sendDuty = debounce((v) => {
      if (!this.lastState) return;
      const raw = parseFloat(v);
      if (isNaN(raw)) {
        showToast('Duty is required', 'error');
        this.forceSet(manualInput, this.lastState.output);
        this.forceSet(manualSlider, this.lastState.output);
        return;
      }
      const val = clamp(raw, 0, 100);
      this.forceSet(manualInput, val);
      this.forceSet(manualSlider, val);
      this.api.postManual(val).catch(err => {
        showToast(err.serverError ?? err.message, 'error');
        if (this.lastState) {
          this.forceSet(manualInput, this.lastState.output);
          this.forceSet(manualSlider, this.lastState.output);
        }
      });
    }, 200);

    manualSlider.addEventListener('input', () => {
      manualInput.value = manualSlider.value;
      sendDuty(manualSlider.value);
    });
    manualInput.addEventListener('change', () => {
      manualSlider.value = manualInput.value;
      sendDuty(manualInput.value);
    });
  }

  safeSet(el, value) {
    if (!el || document.activeElement === el) return;
    this.forceSet(el, value);
  }

  forceSet(el, value) {
    if (!el) return;
    if (el.type === 'number') {
      const rounded = typeof value === 'number' ? parseFloat(value.toFixed(4)) : value;
      if (el.value !== String(rounded)) el.value = rounded;
    } else {
      if (el.value !== String(value)) el.value = value;
    }
  }

  bytes(n) {
    if (n == null) return '--';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / (1024 * 1024)).toFixed(2) + ' MB';
  }

  duration(ms) {
    if (ms == null) return '--';
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    const d = Math.floor(h / 24);
    if (d > 0) return d + 'd ' + h % 24 + 'h';
    if (h > 0) return h + 'h ' + m % 60 + 'm';
    return m + 'm ' + s % 60 + 's';
  }
}

export function clamp(v, min, max) {
  return Math.min(max, Math.max(min, isNaN(v) ? min : v));
}

export function round(v, d) {
  const m = Math.pow(10, d);
  return Math.round(v * m) / m;
}

function debounce(fn, ms) {
  let t;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

export function showToast(text, type = 'info') {
  const stack = document.getElementById('toast-stack');
  if (!stack) return;
  const toast = document.createElement('div');
  toast.className = 'toast ' + type;
  toast.textContent = text;
  stack.appendChild(toast);
  setTimeout(() => toast.remove(), 4000);
}
