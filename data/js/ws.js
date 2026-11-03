export class Ws {
  constructor() {
    this.ws = null;
    this.listeners = { open: new Set(), close: new Set(), message: new Set(), reconnecting: new Set() };
    this.reconnectDelay = 1000;
    this.maxReconnectDelay = 15000;
    this.heartbeatInterval = 20000;
    this.deadTimeout = 60000;
    this.heartbeatTimer = null;
    this.deadTimer = null;
    this.intentionalClose = false;
    this.url = this._wsUrl();
    this.connect();
  }

  _wsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${location.host}/ws`;
  }

  connect() {
    if (this.ws) return;
    try {
      this.ws = new WebSocket(this.url);
      this.ws.addEventListener('open', () => this._onOpen());
      this.ws.addEventListener('close', (e) => this._onClose(e));
      this.ws.addEventListener('error', () => {});
      this.ws.addEventListener('message', (e) => this._onMessage(e));
    } catch (e) {
      this._scheduleReconnect();
    }
  }

  _onOpen() {
    this.reconnectDelay = 1000;
    this._startHeartbeat();
    for (const fn of this.listeners.open) fn();
  }

  _onClose(e) {
    this.ws = null;
    this._stopHeartbeat();
    for (const fn of this.listeners.close) fn(e);
    if (!this.intentionalClose) this._scheduleReconnect();
  }

  close() {
    this.intentionalClose = true;
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this._stopHeartbeat();
  }

  _onMessage(e) {
    this._touch();
    let msg;
    try { msg = JSON.parse(e.data); } catch (err) { return; }
    for (const fn of this.listeners.message) fn(msg);
  }

  _scheduleReconnect() {
    for (const fn of this.listeners.reconnecting) fn();
    setTimeout(() => {
      this.reconnectDelay = Math.min(this.reconnectDelay * 1.5, this.maxReconnectDelay);
      this.connect();
    }, this.reconnectDelay);
  }

  _startHeartbeat() {
    this._touch();
    this.heartbeatTimer = setInterval(() => this.send({ type: 'ping' }), this.heartbeatInterval);
  }

  _stopHeartbeat() {
    clearInterval(this.heartbeatTimer);
    clearTimeout(this.deadTimer);
  }

  _touch() {
    clearTimeout(this.deadTimer);
    this.deadTimer = setTimeout(() => {
      this.intentionalClose = true;
      if (this.ws) { this.ws.close(); this.ws = null; }
      this.intentionalClose = false;
      this._scheduleReconnect();
    }, this.deadTimeout);
  }

  send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj));
      return true;
    }
    return false;
  }

  cmd(command) {
    return this.send({ type: 'cmd', data: { id: this._nextId(), command } });
  }

  ping() { return this.send({ type: 'ping' }); }

  _nextId() {
    if (typeof window !== 'undefined' && window.__wsCmdId) return ++window.__wsCmdId;
    window.__wsCmdId = 1;
    return 1;
  }

  on(event, fn) {
    if (this.listeners[event]) this.listeners[event].add(fn);
  }
}
