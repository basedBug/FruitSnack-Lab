export class Api {
  constructor() {
    this.base = '';
    this.timeoutMs = 8000;
  }

  async _fetch(path, opts = {}) {
    const url = this.base + path;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let res;
    try {
      res = await fetch(url, {
        ...opts,
        signal: controller.signal,
        headers: {
          'Content-Type': 'application/json',
          ...(opts.headers || {}),
        },
      });
    } catch (networkErr) {
      clearTimeout(timer);
      const message = networkErr && networkErr.name === 'AbortError' ? 'Request timed out' : (networkErr && networkErr.message ? networkErr.message : 'Network error');
      const err = new Error(message);
      err.status = 0;
      err.serverError = null;
      throw err;
    }
    clearTimeout(timer);

    let body = '';
    try {
      body = await res.text();
    } catch (_) {
      body = '';
    }

    if (!res.ok) {
      let serverError = null;
      try {
        const parsed = JSON.parse(body);
        if (parsed && typeof parsed.error === 'string') serverError = parsed.error;
      } catch (_) {}
      const err = new Error(res.status + ' ' + res.statusText + ': ' + body);
      err.status = res.status;
      err.serverError = serverError;
      throw err;
    }

    if (res.status === 204) return null;
    if (!body) return null;
    try {
      return JSON.parse(body);
    } catch (_) {
      return body;
    }
  }

  getState() { return this._fetch('/api/state'); }
  getGraph(limit = 200) { return this._fetch('/api/graph?limit=' + limit); }
  getConfig() { return this._fetch('/api/config'); }
  getInfo() { return this._fetch('/api/info'); }
  getLogs(limit = 60) { return this._fetch('/api/logs?limit=' + limit); }

  postConfig(body) { return this._fetch('/api/config', { method: 'POST', body: JSON.stringify(body) }); }
  postSetpoint(value) { return this._fetch('/api/setpoint', { method: 'POST', body: JSON.stringify({ value }) }); }
  postMode(mode) { return this._fetch('/api/mode', { method: 'POST', body: JSON.stringify({ mode }) }); }
  postManual(duty) { return this._fetch('/api/manual', { method: 'POST', body: JSON.stringify({ duty }) }); }
  postPid(kp, ki, kd) { return this._fetch('/api/pid', { method: 'POST', body: JSON.stringify({ kp, ki, kd }) }); }
  postReboot() { return this._fetch('/api/reboot', { method: 'POST' }); }
}
