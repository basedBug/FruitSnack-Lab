import { Api } from './api.js';
import { Ws } from './ws.js';
import { Chart } from './chart.js';
import { Controls } from './controls.js';
import { Console } from './console.js';
import { Debug } from './debug.js';
import { Sensor } from './sensor.js';

export const store = {
  state: null,
  config: null,
  info: null,
  samples: [],
  maxSamples: 3600,
  connected: false,
  wsConnected: false,
  restReachable: false,
  subs: new Map(),
  cmdId: 1,
  bootEpoch: null,
};

store.subscribe = (key, fn) => {
  if (!store.subs.has(key)) store.subs.set(key, new Set());
  store.subs.get(key).add(fn);
  return () => store.subs.get(key).delete(fn);
};

store.emit = (key, data) => {
  if (!store.subs.has(key)) return;
  for (const fn of store.subs.get(key)) {
    try {
      fn(data);
    } catch (e) {
      const text = 'subscriber error (' + key + '): ' + (e && e.message ? e.message : String(e));
      if (window.console && console.error) console.error(text);
      if (store.subs.has('console-error')) {
        for (const errFn of store.subs.get('console-error')) {
          try { errFn(text); } catch (_) {}
        }
      }
    }
  }
};

store.setState = (state) => {
  store.state = state;
  lastStateMs = Date.now();
  if (state && state.diag && typeof state.diag.uptime === 'number') {
    store.bootEpoch = Date.now() - state.diag.uptime;
  }
  store.emit('state', state);
};

store.setConfig = (config) => {
  store.config = config;
  store.emit('config', config);
};

store.setInfo = (info) => {
  store.info = info;
  store.emit('info', info);
};

store.replaceSamples = (batch) => {
  if (!Array.isArray(batch)) return;
  store.samples = batch.slice(-store.maxSamples);
  store.emit('samples', store.samples);
};

store.pushSamples = (batch) => {
  if (!Array.isArray(batch) || batch.length === 0) return;
  const current = store.samples;
  const lastT = current.length > 0 ? current[current.length - 1].t : -1;
  const newestT = batch[batch.length - 1].t;
  if (newestT < lastT) {
    store.samples = batch.slice(-store.maxSamples);
  } else {
    for (const s of batch) {
      if (s.t > lastT) current.push(s);
    }
    if (current.length > store.maxSamples) {
      current.splice(0, current.length - store.maxSamples);
    }
  }
  store.emit('samples', store.samples);
};

export function need(id) {
  const el = document.getElementById(id);
  if (!el) throw new Error('missing #' + id);
  return el;
}

const api = new Api();
const ws = new Ws();

let pollTimer = null;
let fetchFailures = 0;
let lastStateMs = 0;
let stateFetching = false;
let lastFetchErrorReported = false;

function init() {
  setupConnection();

  const chart = new Chart();
  const controls = new Controls(api);
  const terminal = new Console(ws, api);
  const debug = new Debug(api);
  const sensor = new Sensor(api);

  ws.on('open', () => {
    store.wsConnected = true;
    updateConnection(false);
    api.getInfo().then(i => store.setInfo(i)).catch(err => logFetchError('info', err));
    api.getConfig().then(c => store.setConfig(c)).catch(err => logFetchError('config', err));
    api.getGraph(200).then(g => store.replaceSamples(g.samples || [])).catch(err => logFetchError('graph', err));
  });

  ws.on('close', () => {
    store.wsConnected = false;
    updateConnection(false);
  });

  ws.on('reconnecting', () => {
    store.wsConnected = false;
    updateConnection(true);
  });

  ws.on('message', (msg) => handleMessage(msg, terminal));

  store.subscribe('samples', () => chart.scheduleDraw());
  store.subscribe('state', () => chart.scheduleDraw());
  store.subscribe('section', () => chart.scheduleDraw());

  setupRouting();

  api.getState().then(s => { store.restReachable = true; store.setState(s); })
    .catch(err => { store.restReachable = false; logFetchError('state', err); });
  api.getInfo().then(i => store.setInfo(i)).catch(err => logFetchError('info', err));
  api.getConfig().then(c => store.setConfig(c)).catch(err => logFetchError('config', err));
  api.getGraph(200).then(g => store.replaceSamples(g.samples || [])).catch(err => logFetchError('graph', err));
}

function logFetchError(resource, err) {
  if (window.console && console.error) console.error('Fetch failed (' + resource + '): ' + (err.serverError ?? err.message));
}

function handleMessage(msg, terminal) {
  try {
    switch (msg.type) {
      case 'hello':
        store.hello = msg.data;
        store.emit('hello', msg.data);
        break;
      case 'state':
        store.setState(msg.data);
        break;
      case 'graph':
        if (msg.data && Array.isArray(msg.data.samples)) {
          store.pushSamples(msg.data.samples);
        }
        break;
      case 'log':
        terminal.addLog(msg.data);
        break;
      case 'reply':
        terminal.addReply(msg.data);
        break;
      default:
        break;
    }
  } catch (e) {
    terminal.addLog({ level: 'error', ts: Date.now() - (store.bootEpoch || 0), text: 'Malformed message: ' + (e && e.message ? e.message : String(e)) });
  }
}

function setupRouting() {
  window.addEventListener('hashchange', () => showSection(location.hash));
  showSection(location.hash);
}

function showSection(hash) {
  const raw = (hash || '').replace(/^#/, '');
  const valid = ['dashboard', 'machine', 'console'];
  const legacyTemperature = raw === 'temperature';
  const section = valid.includes(raw) ? raw : 'dashboard';
  document.querySelectorAll('.section').forEach(s => {
    s.classList.toggle('active', s.id === section);
  });
  document.querySelectorAll('.nav-item').forEach(b => {
    const active = b.dataset.section === section;
    b.classList.toggle('active', active);
    b.setAttribute('aria-selected', active ? 'true' : 'false');
  });
  if (legacyTemperature) {
    const disclosure = document.getElementById('sensor-disclosure');
    if (disclosure) disclosure.open = true;
  }
  if (location.hash !== '#' + section) {
    history.replaceState(null, '', '#' + section);
  }
  window.scrollTo(0, 0);
  requestAnimationFrame(() => window.scrollTo(0, 0));
  store.emit('section', section);
}

function setupConnection() {
  document.body.dataset.conn = 'offline';
  setInterval(() => {
    if (stateFetching) return;
    if (store.wsConnected) {
      lastStateMs = Date.now();
      fetchFailures = 0;
      lastFetchErrorReported = false;
    }
    stateFetching = true;
    api.getState()
      .then(s => {
        fetchFailures = 0;
        lastStateMs = Date.now();
        store.restReachable = true;
        store.setState(s);
        updateConnection(false);
        lastFetchErrorReported = false;
      })
      .catch(err => {
        fetchFailures++;
        store.restReachable = false;
        logFetchError('state', err);
        updateConnection(false);
      })
      .finally(() => {
        stateFetching = false;
      });
  }, 1000);
}

function updateConnection(reconnecting) {
  const body = document.body;
  const online = store.restReachable;
  store.connected = online;
  if (online) {
    body.dataset.conn = 'online';
    body.classList.remove('disconnected');
  } else if (reconnecting) {
    body.dataset.conn = 'reconnecting';
    body.classList.add('disconnected');
  } else {
    body.dataset.conn = 'offline';
    body.classList.add('disconnected');
  }
  const staleS = lastStateMs ? Math.max(0, Math.round((Date.now() - lastStateMs) / 1000)) : null;
  store.emit('connection', { online, wsOnline: store.wsConnected, reconnecting, fetchFailures, staleS });
}

init();
