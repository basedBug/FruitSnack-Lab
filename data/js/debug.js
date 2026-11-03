import { store, need } from './app.js';
import { showToast } from './controls.js';

export class Debug {
  constructor(api) {
    this.api = api;
    this.machineActive = false;

    this.els = {
      gainKp: need('gain-kp'),
      gainKi: need('gain-ki'),
      gainKd: need('gain-kd'),
      applyGains: need('apply-gains'),
      resetGains: need('reset-gains'),
      termP: need('term-p'),
      termI: need('term-i'),
      termD: need('term-d'),
      saturated: need('saturated'),
      machineMode: need('machine-mode'),
      machineOutput: need('machine-output'),
      machineSetpoint: need('machine-setpoint'),
      deviceInfo: need('device-info'),
      memoryInfo: need('memory-info'),
      logViewer: need('log-viewer'),
      stateJson: need('state-json'),
      copyState: need('copy-state'),
      rebootBtn: need('reboot-btn'),
      rebootDialog: need('reboot-dialog'),
      rebootConfirmBtn: need('reboot-confirm-btn'),
      rebootCancelBtn: need('reboot-cancel-btn'),
    };

    this.initReboot();
    this.initCopy();
    this.bindPid();

    store.subscribe('info', info => this.renderInfo(info));
    store.subscribe('state', s => this.renderState(s));
    store.subscribe('config', c => this.renderConfig(c));
    store.subscribe('connection', () => this.updateDisabled());
    store.subscribe('section', section => {
      this.machineActive = section === 'machine';
      if (this.machineActive) {
        this.refreshLogs();
        if (store.state) this.renderState(store.state);
        else this.els.stateJson.textContent = 'Waiting for state…';
      }
    });

    this.refreshLogs();
    setInterval(() => this.refreshLogs(), 3000);
  }

  updateDisabled() {
    const disabled = !store.connected || !store.state;
    this.els.gainKp.disabled = disabled;
    this.els.gainKi.disabled = disabled;
    this.els.gainKd.disabled = disabled;
    this.els.applyGains.disabled = disabled;
    this.els.resetGains.disabled = !store.config;
    this.els.rebootBtn.disabled = !store.connected;
  }

  renderInfo(info) {
    if (!info) return;
    const rows = [
      ['Firmware', info.firmware],
      ['Version', info.version],
      ['Chip', info.chip],
      ['SDK', info.sdk],
      ['Core', info.core],
      ['Flash', this.bytes(info.flashSize)],
      ['PSRAM', this.bytes(info.psramSize)],
      ['Uptime', this.duration(info.uptime)],
      ['IP', info.ip],
      ['RSSI', info.rssi != null ? info.rssi + ' dBm' : '--'],
      ['Hostname', info.hostname || (store.config && store.config.hostname) || '--'],
    ];
    this.els.deviceInfo.innerHTML = rows.map(([k, v]) => {
      return '<dt>' + this.escape(k) + '</dt><dd>' + this.escape(String(v ?? '--')) + '</dd>';
    }).join('');

    const memRows = [
      ['Heap size', this.bytes(info.heapSize)],
      ['Free heap', this.bytes(info.freeHeap)],
      ['Min free heap', this.bytes(info.minFreeHeap)],
      ['LittleFS total', this.bytes(info.littlefsTotal)],
      ['LittleFS used', this.bytes(info.littlefsUsed)],
    ];
    const usedPct = info.littlefsTotal ? (info.littlefsUsed / info.littlefsTotal * 100).toFixed(1) : '0.0';
    this.els.memoryInfo.innerHTML = memRows.map(([k, v]) => {
      return '<dt>' + this.escape(k) + '</dt><dd>' + this.escape(String(v ?? '--')) + '</dd>';
    }).join('') +
      '<dt>LittleFS usage</dt><dd><span class="usage-row"><span class="usage-bar"><div style="width:' + usedPct + '%"></div></span><span>' + usedPct + '%</span></span></dd>';
  }

  renderConfig(c) {
    if (!c) return;
    this.safeSet(this.els.gainKp, c.kp);
    this.safeSet(this.els.gainKi, c.ki);
    this.safeSet(this.els.gainKd, c.kd);
  }

  renderState(s) {
    this.updateDisabled();
    if (this.machineActive) {
      this.els.stateJson.textContent = s ? JSON.stringify(s, null, 2) : 'Waiting for state…';
    }
    if (!s) {
      this.els.termP.textContent = '--';
      this.els.termI.textContent = '--';
      this.els.termD.textContent = '--';
      this.els.saturated.hidden = true;
      this.els.machineMode.textContent = '--';
      this.els.machineOutput.textContent = '--';
      this.els.machineSetpoint.textContent = '--';
      return;
    }

    if (s.pid) {
      const pTerm = s.pid.p;
      const iTerm = s.pid.i;
      const dTerm = s.pid.d;
      this.els.termP.textContent = typeof pTerm === 'number' ? pTerm.toFixed(2) : '--';
      this.els.termI.textContent = typeof iTerm === 'number' ? iTerm.toFixed(2) : '--';
      this.els.termD.textContent = typeof dTerm === 'number' ? dTerm.toFixed(2) : '--';
      this.els.saturated.hidden = !s.pid.saturated;
      this.safeSet(this.els.gainKp, s.pid.kp);
      this.safeSet(this.els.gainKi, s.pid.ki);
      this.safeSet(this.els.gainKd, s.pid.kd);
    } else {
      this.els.termP.textContent = '--';
      this.els.termI.textContent = '--';
      this.els.termD.textContent = '--';
      this.els.saturated.hidden = true;
    }

    const setpoint = s.setpoint;
    this.els.machineMode.textContent = s.mode || '--';
    this.els.machineSetpoint.textContent = typeof setpoint === 'number' ? setpoint.toFixed(1) + ' °C' : '--';
    this.els.machineOutput.textContent = s.pwmReady && typeof s.output === 'number'
      ? Math.round(s.output) + '%'
      : '--';
  }

  bindPid() {
    this.els.applyGains.addEventListener('click', () => this.applyGains());
    this.els.resetGains.addEventListener('click', () => {
      if (store.config) this.renderConfig(store.config);
    });
  }

  applyGains() {
    const s = store.state;
    const kpRaw = this.els.gainKp.value;
    const kiRaw = this.els.gainKi.value;
    const kdRaw = this.els.gainKd.value;
    if (kpRaw === '' || kiRaw === '' || kdRaw === '') {
      showToast('All PID gains are required', 'error');
      if (s && s.pid) this.renderState(s);
      return;
    }
    const kp = clamp(parseFloat(kpRaw), 0, 1000);
    const ki = clamp(parseFloat(kiRaw), 0, 100);
    const kd = clamp(parseFloat(kdRaw), 0, 10000);
    if (isNaN(parseFloat(kpRaw)) || isNaN(parseFloat(kiRaw)) || isNaN(parseFloat(kdRaw))) {
      showToast('PID gains must be numbers', 'error');
      if (s && s.pid) this.renderState(s);
      return;
    }
    this.safeSet(this.els.gainKp, kp);
    this.safeSet(this.els.gainKi, ki);
    this.safeSet(this.els.gainKd, kd);
    this.api.postPid(kp, ki, kd)
      .then((res) => {
        if (res && res.persisted === false) {
          showToast('Applied in RAM, not saved — ' + (res.error || ''), 'warn');
        } else {
          showToast('PID gains applied', 'success');
        }
      })
      .catch(err => {
        showToast(err.serverError ?? err.message, 'error');
        if (s && s.pid) {
          this.safeSet(this.els.gainKp, s.pid.kp);
          this.safeSet(this.els.gainKi, s.pid.ki);
          this.safeSet(this.els.gainKd, s.pid.kd);
        }
      });
  }

  async refreshLogs() {
    if (!this.machineActive) return;
    try {
      const data = await this.api.getLogs(60);
      const lines = data.lines || [];
      if (!lines.length) {
        this.els.logViewer.innerHTML = '<div class="log-line"><span class="ts">--:--:--</span>No logs yet</div>';
        return;
      }
      this.els.logViewer.innerHTML = lines.map(l => {
        const ts = this.formatTs(l.ts);
        return '<div class="log-line ' + this.escape(l.level) + '"><span class="ts">' + ts + '</span><span>' + this.escape(l.text) + '</span></div>';
      }).join('');
      this.els.logViewer.scrollTop = this.els.logViewer.scrollHeight;
    } catch (e) {
      // Keep the previous lines; the stale connection pill already reports the outage.
    }
  }

  formatTs(ts) {
    if (store.bootEpoch != null && typeof ts === 'number') {
      const d = new Date(store.bootEpoch + ts);
      return d.toLocaleTimeString();
    }
    return '--:--:--';
  }

  initReboot() {
    const dialog = this.els.rebootDialog;
    this.els.rebootBtn.addEventListener('click', () => { if (dialog.showModal) dialog.showModal(); });
    this.els.rebootCancelBtn.addEventListener('click', () => { if (dialog.close) dialog.close(); });
    this.els.rebootConfirmBtn.addEventListener('click', () => {
      this.api.postReboot()
        .then(() => showToast('Rebooting…', 'success'))
        .catch(err => showToast(err.serverError ?? err.message, 'error'));
      if (dialog.close) dialog.close();
    });
  }

  initCopy() {
    this.els.copyState.addEventListener('click', () => {
      const text = this.els.stateJson.textContent;
      navigator.clipboard.writeText(text).then(() => showToast('Copied', 'success')).catch(() => {
        const ta = document.createElement('textarea');
        ta.value = text;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        showToast('Copied', 'success');
      });
    });
  }

  safeSet(el, value) {
    if (!el || document.activeElement === el) return;
    const rounded = typeof value === 'number' ? parseFloat(value.toFixed(4)) : value;
    if (el.value !== String(rounded)) el.value = rounded;
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

  escape(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
}

function clamp(v, min, max) {
  return Math.min(max, Math.max(min, isNaN(v) ? min : v));
}
