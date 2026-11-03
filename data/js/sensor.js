import { store, need } from './app.js';
import { showToast } from './controls.js';

// WARN_TEXT is duplicated byte-identically in controls.js and sensor.js; keep both copies in sync.
const WARN_TEXT = {
  short: 'Node is at 0 V: the sensor is shorted, or the pull-up resistor is missing.',
  open: 'Node is at the supply rail: the thermistor is disconnected.',
  'below-range': 'Node voltage is above the ADC\'s 2.9 V limit: the reading is an upper bound below ~50 °C. Apply an offset to match a reference thermometer.',
  range: 'The computed temperature is outside the valid window: the sensor or the pull-up value is wrong.',
};

function warnText(code) {
  return WARN_TEXT[code] || ('Sensor warning: ' + code);
}

export class Sensor {
  constructor(api) {
    this.api = api;

    this.els = {
      detailAdcMv: need('detail-adc-mv'),
      detailAdcRaw: need('detail-adc-raw'),
      detailNtcResistance: need('detail-ntc-resistance'),
      detailWarning: need('detail-warning'),
      sensorSummary: need('sensor-summary'),
      sensorWarnMachine: need('sensor-warn-machine'),
      sensorDiag: need('sensor-diag'),
      offset: need('sensor-offset'),
      pullup: need('ntc-pullup'),
      saveCal: need('save-cal'),
    };

    this.bindSave();

    store.subscribe('state', s => this.renderState(s));
    store.subscribe('config', c => this.renderConfig(c));
    store.subscribe('connection', d => this.onConnection(d));
  }

  onConnection() {
    const disabled = !store.connected || !store.state;
    this.els.offset.disabled = disabled;
    this.els.pullup.disabled = disabled;
    this.els.saveCal.disabled = disabled;
  }

  renderState(s) {
    this.onConnection();
    if (!s || !s.diag) {
      this.render(s, null);
      return;
    }
    this.render(s, s.diag);
  }

  render(s, diag) {
    if (!diag) {
      this.els.sensorDiag.innerHTML = '<dt>Status</dt><dd>Waiting for state</dd>';
      this.els.sensorWarnMachine.hidden = true;
      this.els.detailAdcMv.textContent = '--';
      this.els.detailAdcRaw.textContent = '--';
      this.els.detailNtcResistance.textContent = '--';
      this.els.detailWarning.textContent = 'none';
      this.els.sensorSummary.textContent = 'Waiting';
      return;
    }

    const nodeMv = this.formatNumber(diag.adcMv, 0);
    const raw = diag.adcRaw != null ? String(diag.adcRaw) : '--';
    const resistance = this.formatResistance(diag.ntcResistance);
    const temperature = this.formatNumber(s && typeof s.temperature === 'number' ? s.temperature : null, 1);

    this.els.detailAdcMv.textContent = nodeMv + ' mV';
    this.els.detailAdcRaw.textContent = raw;
    this.els.detailNtcResistance.textContent = resistance;
    this.els.sensorSummary.textContent = resistance + ' · ' + nodeMv + ' mV';

    const rows = [
      ['Node voltage', nodeMv + ' mV'],
      ['Raw ADC', raw],
      ['Thermistor', resistance],
      ['Temperature', temperature + ' °C'],
    ];
    this.els.sensorDiag.innerHTML = rows.map(([k, v]) => {
      return '<dt>' + this.escape(k) + '</dt><dd>' + this.escape(v) + '</dd>';
    }).join('');

    const warn = diag.ntcWarn && warnText(diag.ntcWarn);
    this.els.detailWarning.textContent = warn ? diag.ntcWarn : 'none';
    if (warn) {
      this.els.sensorWarnMachine.textContent = warn;
      this.els.sensorWarnMachine.hidden = false;
    } else {
      this.els.sensorWarnMachine.hidden = true;
      this.els.sensorWarnMachine.textContent = '';
    }
  }

  renderConfig(c) {
    if (!c) return;
    this.safeSet(this.els.offset, c.sensorOffset);
    this.safeSet(this.els.pullup, c.ntcPullupOhms);
  }

  bindSave() {
    this.els.saveCal.addEventListener('click', () => {
      const offsetRaw = this.els.offset.value;
      const pullupRaw = this.els.pullup.value;
      if (offsetRaw === '' || pullupRaw === '') {
        showToast('Calibration values are required', 'error');
        return;
      }
      const sensorOffset = parseFloat(offsetRaw);
      const ntcPullupOhms = parseFloat(pullupRaw);
      if (isNaN(sensorOffset) || isNaN(ntcPullupOhms)) {
        showToast('Calibration values must be numbers', 'error');
        return;
      }
      if (sensorOffset < -50 || sensorOffset > 50) {
        showToast('Offset must be between -50 and 50 °C', 'error');
        return;
      }
      if (ntcPullupOhms <= 0) {
        showToast('Pull-up resistance must be greater than 0 Ω', 'error');
        return;
      }
      const payload = { sensorOffset, ntcPullupOhms };
      this.api.postConfig(payload)
        .then(() => {
          showToast('Calibration saved', 'success');
          this.api.getConfig().then(c => store.setConfig(c)).catch(() => {});
        })
        .catch(err => {
          showToast(err.serverError ?? err.message, 'error');
          const c = store.config;
          if (c) this.renderConfig(c);
        });
    });
  }

  safeSet(el, value) {
    if (!el || document.activeElement === el) return;
    if (value == null) {
      if (el.value !== '') el.value = '';
      return;
    }
    const rounded = typeof value === 'number' ? parseFloat(value.toFixed(4)) : value;
    if (el.value !== String(rounded)) el.value = rounded;
  }

  formatNumber(n, digits) {
    if (n == null || typeof n !== 'number' || isNaN(n)) return '--';
    return n.toFixed(digits);
  }

  formatResistance(ohms) {
    if (ohms == null || typeof ohms !== 'number' || isNaN(ohms) || ohms === 0) return '--';
    if (ohms >= 1e6) return (ohms / 1e6).toFixed(2) + ' MΩ';
    if (ohms >= 1e3) return (ohms / 1e3).toFixed(1) + ' kΩ';
    return ohms.toFixed(1) + ' Ω';
  }

  escape(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
}
