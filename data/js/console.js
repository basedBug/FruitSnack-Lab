import { store, need } from './app.js';
import { showToast } from './controls.js';

function getStorage() {
  try {
    const key = '__fs_storage_test__';
    localStorage.setItem(key, '1');
    localStorage.removeItem(key);
    return localStorage;
  } catch (e) {
    return null;
  }
}

export class Console {
  constructor(ws, api) {
    this.ws = ws;
    this.api = api;
    this.storage = getStorage();
    this.seeded = false;

    this.els = {
      lines: need('terminal-lines'),
      terminal: need('terminal'),
      input: need('console-input'),
      form: need('console-form'),
      clear: need('console-clear'),
      conn: need('console-conn'),
      autoscroll: need('autoscroll'),
      cmdStatus: need('cmd-status'),
      cmdHelp: need('cmd-help'),
      cmdInfo: need('cmd-info'),
    };

    if (this.storage) {
      try {
        this.history = JSON.parse(this.storage.getItem('fs_console_history') || '[]');
      } catch (e) {
        this.history = [];
      }
    } else {
      this.history = [];
    }
    this.historyIndex = -1;
    this.draft = '';

    this.form = this.els.form;
    this.input = this.els.input;

    this.form.addEventListener('submit', (e) => this.onSubmit(e));
    this.input.addEventListener('keydown', (e) => this.onKey(e));
    this.els.clear.addEventListener('click', () => this.clear());
    this.els.cmdStatus.addEventListener('click', () => this.run('status'));
    this.els.cmdHelp.addEventListener('click', () => this.run('help'));
    this.els.cmdInfo.addEventListener('click', () => this.run('info'));

    store.subscribe('console-error', text => this.addLog({ level: 'error', ts: this.nowTs(), text: text }, 'error'));
    store.subscribe('connection', d => this.renderConnection(d));
    store.subscribe('state', s => {
      if (!this.seeded && s) {
        this.seeded = true;
        this.seedLogs();
      }
    });
    this.renderConnection({ online: store.connected, wsOnline: store.wsConnected });
  }

  async seedLogs() {
    try {
      const data = await this.api.getLogs(60);
      const lines = data.lines || [];
      for (const line of lines) this.addLog(line);
    } catch (e) {
      // The empty terminal is still usable without a history snapshot.
    }
    this.addLog({ level: 'info', ts: null, text: 'Console ready — type help for commands.' }, 'info');
  }

  renderConnection(d) {
    const live = d && d.wsOnline;
    this.els.conn.dataset.state = live ? 'online' : 'offline';
    this.els.conn.innerHTML = '<span class="dot"></span>' + (live ? 'WebSocket live' : 'Socket offline');
  }

  nowTs() {
    if (store.bootEpoch != null) return Date.now() - store.bootEpoch;
    return 0;
  }

  onSubmit(e) {
    e.preventDefault();
    const cmd = this.input.value.trim();
    if (!cmd) return;
    this.addLog({ level: 'info', ts: this.nowTs(), text: '$ ' + cmd }, 'cmd');
    this.pushHistory(cmd);
    if (!this.ws.cmd(cmd)) {
      showToast('Not connected — command not sent', 'warn');
    }
    this.input.value = '';
    this.historyIndex = -1;
  }

  onKey(e) {
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (this.historyIndex === -1) this.draft = this.input.value;
      if (this.historyIndex < this.history.length - 1) {
        this.historyIndex++;
        this.input.value = this.history[this.historyIndex];
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (this.historyIndex > 0) {
        this.historyIndex--;
        this.input.value = this.history[this.historyIndex];
      } else if (this.historyIndex === 0) {
        this.historyIndex = -1;
        this.input.value = this.draft;
      }
    }
  }

  pushHistory(cmd) {
    this.history.unshift(cmd);
    if (this.history.length > 50) this.history.length = 50;
    if (this.storage) {
      try {
        this.storage.setItem('fs_console_history', JSON.stringify(this.history));
      } catch (e) {}
    }
  }

  addLog(entry, forceClass) {
    const line = document.createElement('div');
    line.className = 'line ' + (forceClass || entry.level || 'info');
    const ts = entry.ts == null ? '' : '<span class="ts">' + this.formatTs(entry.ts) + '</span>';
    line.innerHTML = ts + '<span>' + this.escape(entry.text || '') + '</span>';
    this.els.lines.appendChild(line);
    this.trim();
    this.scroll();
  }

  addReply(reply) {
    const text = reply.ok ? (reply.text || 'ok') : ('error: ' + (reply.text || ''));
    this.addLog({ level: reply.ok ? 'info' : 'error', ts: this.nowTs(), text: text }, 'reply');
  }

  formatTs(ts) {
    if (store.bootEpoch != null && typeof ts === 'number') {
      const d = new Date(store.bootEpoch + ts);
      return d.toLocaleTimeString();
    }
    return '--:--:--';
  }

  trim() {
    while (this.els.lines.children.length > 500) {
      this.els.lines.removeChild(this.els.lines.firstChild);
    }
  }

  scroll() {
    if (!this.els.autoscroll.checked) return;
    this.els.terminal.scrollTop = this.els.terminal.scrollHeight;
  }

  clear() {
    this.els.lines.innerHTML = '';
  }

  run(cmd) {
    this.input.value = cmd;
    this.form.dispatchEvent(new Event('submit', { cancelable: true }));
  }

  escape(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
}
