import { store, need } from './app.js';

const PAD = { top: 18, right: 42, bottom: 26, left: 44 };

export class Chart {
  constructor() {
    this.windowS = 60;
    this.pending = false;
    this.hoverT = null;
    this.lastYRange = { min: 0, max: 300 };
    this.layout = null;

    this.slot = need('chart-slot-dashboard');
    this.canvas = need('temp-chart');
    this.ctx = this.canvas.getContext('2d');
    this.tooltip = need('chart-tooltip');
    this.windows = need('chart-windows');
    this.rangeLabel = need('chart-range');

    this.colors = {
      ink: this.readColor('--ink', '#17171a'),
      ink3: this.readColor('--ink-3', '#73737c'),
      line: this.readColor('--line', '#e4e3dd'),
      grid: '#e9e8e2',
      accent: this.readColor('--accent', '#1d4ed8'),
      warn: this.readColor('--warn', '#a85a06'),
      out: '#a3a3ab',
      outFill: 'rgba(163,163,171,0.09)',
    };

    this.setupResize();
    this.setupWindows();
    this.setupPointer();
    this.updateRangeLabel();
  }

  readColor(name, fallback) {
    const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return value || fallback;
  }

  setupWindows() {
    this.windows.querySelectorAll('button').forEach(btn => {
      btn.addEventListener('click', () => {
        this.windows.querySelectorAll('button').forEach(b => b.classList.toggle('active', b === btn));
        this.windowS = parseInt(btn.dataset.window, 10) || 60;
        this.hoverT = null;
        this.tooltip.hidden = true;
        this.updateRangeLabel();
        this.scheduleDraw();
      });
    });
  }

  updateRangeLabel() {
    if (this.windowS === 60) this.rangeLabel.textContent = 'Last 60 seconds';
    else if (this.windowS === 300) this.rangeLabel.textContent = 'Last 5 minutes · 225 s available';
    else this.rangeLabel.textContent = 'Device history · 225 seconds';
  }

  setupResize() {
    if (window.ResizeObserver) {
      this.resizeObserver = new ResizeObserver(() => this.resize());
      this.resizeObserver.observe(this.slot);
    }
    window.addEventListener('resize', () => this.resize());
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) this.scheduleDraw();
    });
  }

  setupPointer() {
    this.canvas.addEventListener('pointermove', (event) => {
      if (!this.layout) return;
      const rect = this.canvas.getBoundingClientRect();
      const x = event.clientX - rect.left;
      const y = event.clientY - rect.top;
      const { pad, gw, gh } = this.layout;
      if (x < pad.left || x > pad.left + gw || y < pad.top || y > pad.top + gh) {
        this.hoverT = null;
        this.tooltip.hidden = true;
        this.scheduleDraw();
        return;
      }
      const ratio = (x - pad.left) / gw;
      this.hoverT = this.layout.xMin + ratio * (this.layout.xMax - this.layout.xMin);
      this.scheduleDraw();
    });

    this.canvas.addEventListener('pointerleave', () => {
      this.hoverT = null;
      this.tooltip.hidden = true;
      this.scheduleDraw();
    });
  }

  resize() {
    const rect = this.slot.getBoundingClientRect();
    if (rect.width < 1 || rect.height < 1) return;
    const dpr = window.devicePixelRatio || 1;
    this.width = rect.width;
    this.height = rect.height;
    this.canvas.width = Math.max(1, Math.floor(rect.width * dpr));
    this.canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.scheduleDraw();
  }

  scheduleDraw() {
    if (this.pending || document.hidden) return;
    this.pending = true;
    requestAnimationFrame(() => {
      this.pending = false;
      this.draw();
    });
  }

  draw() {
    const ctx = this.ctx;
    const w = this.slot.clientWidth;
    const h = this.slot.clientHeight;
    if (w < 2 || h < 2) return;
    if (this.width !== w || this.height !== h) {
      const dpr = window.devicePixelRatio || 1;
      this.width = w;
      this.height = h;
      this.canvas.width = Math.max(1, Math.floor(w * dpr));
      this.canvas.height = Math.max(1, Math.floor(h * dpr));
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    ctx.clearRect(0, 0, w, h);
    const pad = { ...PAD };
    const gw = Math.max(1, w - pad.left - pad.right);
    const gh = Math.max(1, h - pad.top - pad.bottom);
    this.layout = { pad, gw, gh, xMin: 0, xMax: 0 };

    const all = store.samples || [];
    if (all.length < 2) {
      this.drawEmpty(ctx, w, h, all.length === 0 ? 'Waiting for telemetry…' : 'Collecting samples…');
      return;
    }

    const xMax = all[all.length - 1].t;
    const xMin = xMax - this.windowS * 1000;
    const samples = all.filter(s => s.t >= xMin);
    if (samples.length < 2) {
      this.layout.xMin = xMin;
      this.layout.xMax = xMax;
      this.drawEmpty(ctx, w, h, 'No samples in this window');
      return;
    }

    this.layout.xMin = xMin;
    this.layout.xMax = xMax;
    const yRange = this.computeYRange(samples);
    const xSpan = Math.max(1, xMax - xMin);

    this.drawGrid(ctx, pad, gw, gh, yRange, xMin, xMax, xSpan);

    const last = samples[samples.length - 1];
    const lastX = pad.left + ((last.t - xMin) / xSpan) * gw;

    ctx.beginPath();
    ctx.moveTo(pad.left, pad.top + gh);
    for (const s of samples) {
      const x = pad.left + ((s.t - xMin) / xSpan) * gw;
      const y = pad.top + gh - (clamp(s.out, 0, 100) / 100) * gh;
      ctx.lineTo(x, y);
    }
    ctx.lineTo(pad.left + gw, pad.top + gh);
    ctx.closePath();
    ctx.fillStyle = this.colors.outFill;
    ctx.fill();

    ctx.beginPath();
    for (let i = 0; i < samples.length; i++) {
      const s = samples[i];
      const x = pad.left + ((s.t - xMin) / xSpan) * gw;
      const y = pad.top + gh - (clamp(s.out, 0, 100) / 100) * gh;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = this.colors.out;
    ctx.lineWidth = 1;
    ctx.lineJoin = 'round';
    ctx.stroke();

    ctx.beginPath();
    ctx.setLineDash([5, 4]);
    for (let i = 0; i < samples.length; i++) {
      const s = samples[i];
      const x = pad.left + ((s.t - xMin) / xSpan) * gw;
      const y = this.yToPx(s.sp, pad, gh, yRange);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = this.colors.warn;
    ctx.lineWidth = 1.4;
    ctx.stroke();
    ctx.setLineDash([]);

    const gradient = ctx.createLinearGradient(0, pad.top, 0, pad.top + gh);
    gradient.addColorStop(0, 'rgba(29,78,216,0.10)');
    gradient.addColorStop(1, 'rgba(29,78,216,0.00)');
    ctx.beginPath();
    ctx.moveTo(pad.left + ((samples[0].t - xMin) / xSpan) * gw, pad.top + gh);
    for (const s of samples) {
      const x = pad.left + ((s.t - xMin) / xSpan) * gw;
      ctx.lineTo(x, this.yToPx(s.temp, pad, gh, yRange));
    }
    ctx.lineTo(lastX, pad.top + gh);
    ctx.closePath();
    ctx.fillStyle = gradient;
    ctx.fill();

    ctx.beginPath();
    for (let i = 0; i < samples.length; i++) {
      const s = samples[i];
      const x = pad.left + ((s.t - xMin) / xSpan) * gw;
      const y = this.yToPx(s.temp, pad, gh, yRange);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = this.colors.accent;
    ctx.lineWidth = 2.1;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    ctx.stroke();

    const lastY = this.yToPx(last.temp, pad, gh, yRange);
    ctx.beginPath();
    ctx.arc(lastX, lastY, 4, 0, Math.PI * 2);
    ctx.fillStyle = this.colors.accent;
    ctx.fill();
    ctx.lineWidth = 2;
    ctx.strokeStyle = '#ffffff';
    ctx.stroke();

    if (this.hoverT != null) {
      const nearest = this.nearestSample(samples, this.hoverT);
      if (nearest) {
        this.drawHover(ctx, nearest, pad, gw, gh, yRange, xMin, xSpan, w);
      }
    } else {
      this.tooltip.hidden = true;
    }
  }

  drawEmpty(ctx, w, h, text) {
    ctx.fillStyle = this.colors.ink3;
    ctx.font = '11px ui-monospace, monospace';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(text, w / 2, h / 2);
    this.tooltip.hidden = true;
  }

  nearestSample(samples, t) {
    let best = samples[0];
    let bestDist = Math.abs(best.t - t);
    for (let i = 1; i < samples.length; i++) {
      const dist = Math.abs(samples[i].t - t);
      if (dist < bestDist) {
        best = samples[i];
        bestDist = dist;
      }
    }
    return best;
  }

  drawHover(ctx, sample, pad, gw, gh, yRange, xMin, xSpan, w) {
    const x = pad.left + ((sample.t - xMin) / xSpan) * gw;
    const yTemp = this.yToPx(sample.temp, pad, gh, yRange);
    const ySp = this.yToPx(sample.sp, pad, gh, yRange);

    ctx.beginPath();
    ctx.moveTo(x, pad.top);
    ctx.lineTo(x, pad.top + gh);
    ctx.strokeStyle = 'rgba(23,23,26,0.18)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.beginPath();
    ctx.arc(x, yTemp, 3.2, 0, Math.PI * 2);
    ctx.fillStyle = this.colors.accent;
    ctx.fill();
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = '#ffffff';
    ctx.stroke();

    ctx.beginPath();
    ctx.arc(x, ySp, 2.8, 0, Math.PI * 2);
    ctx.fillStyle = this.colors.warn;
    ctx.fill();
    ctx.strokeStyle = '#ffffff';
    ctx.stroke();

    this.tooltip.innerHTML =
      '<span class="tooltip-time">' + this.formatOffset(sample.t - this.layout.xMax) + '</span>' +
      'Actual <b>' + fmt(sample.temp, 1) + ' °C</b><br>' +
      'Target <b>' + fmt(sample.sp, 1) + ' °C</b><br>' +
      'Output <b>' + fmt(sample.out, 0) + '%</b>';
    this.tooltip.style.left = clamp(x, 66, w - 66) + 'px';
    const above = Math.min(yTemp, ySp) > 90;
    this.tooltip.style.top = above
      ? (Math.min(yTemp, ySp) - 8) + 'px'
      : (Math.max(yTemp, ySp) + 12) + 'px';
    this.tooltip.style.transform = above ? 'translate(-50%, -100%)' : 'translate(-50%, 0)';
    this.tooltip.hidden = false;
  }

  formatOffset(ms) {
    const sec = Math.round(ms / 1000);
    if (sec <= -60) {
      const min = Math.floor(Math.abs(sec) / 60);
      const rest = Math.abs(sec) % 60;
      return '−' + min + 'm ' + rest + 's';
    }
    return '−' + Math.abs(sec) + 's';
  }

  computeYRange(samples) {
    let min = Infinity;
    let max = -Infinity;
    for (const s of samples) {
      min = Math.min(min, s.temp, s.sp);
      max = Math.max(max, s.temp, s.sp);
    }
    if (!isFinite(min)) { min = 0; max = 250; }
    const padding = Math.max(4, (max - min) * 0.1);
    let yMin = Math.max(0, Math.floor((min - padding) / 10) * 10);
    let yMax = Math.ceil((max + padding) / 10) * 10;
    if (yMax - yMin < 20) {
      const center = (min + max) / 2;
      yMin = Math.max(0, Math.round((center - 10) / 5) * 5);
      yMax = yMin + 20;
    }

    const last = this.lastYRange;
    if (yMin > last.min && yMin < last.min + 12) yMin = last.min;
    if (yMax < last.max && yMax > last.max - 12) yMax = last.max;
    this.lastYRange = { min: yMin, max: yMax };
    return { min: yMin, max: yMax };
  }

  yToPx(value, pad, gh, range) {
    const ratio = (value - range.min) / (range.max - range.min);
    return pad.top + gh - clamp(ratio, 0, 1) * gh;
  }

  drawGrid(ctx, pad, gw, gh, yRange, xMin, xMax, xSpan) {
    ctx.lineWidth = 1;
    ctx.font = '9.5px ui-monospace, monospace';
    ctx.textBaseline = 'middle';

    const ySteps = 4;
    for (let i = 0; i <= ySteps; i++) {
      const value = yRange.min + (i / ySteps) * (yRange.max - yRange.min);
      const y = pad.top + gh - (i / ySteps) * gh;
      ctx.beginPath();
      ctx.moveTo(pad.left, y);
      ctx.lineTo(pad.left + gw, y);
      ctx.strokeStyle = i === 0 ? this.colors.line : this.colors.grid;
      ctx.stroke();
      ctx.fillStyle = this.colors.ink3;
      ctx.textAlign = 'right';
      ctx.fillText(value.toFixed(0), pad.left - 7, y);
    }

    ctx.textAlign = 'left';
    ctx.fillStyle = this.colors.ink3;
    ctx.fillText('°C', 6, pad.top - 8);
    ctx.textAlign = 'right';
    ctx.fillText('%', pad.left + gw + 36, pad.top - 8);

    const outSteps = 2;
    for (let i = 0; i <= outSteps; i++) {
      const y = pad.top + gh - (i / outSteps) * gh;
      ctx.fillStyle = this.colors.ink3;
      ctx.textAlign = 'left';
      ctx.fillText((i * 50) + '%', pad.left + gw + 8, y);
    }

    const xSteps = 4;
    for (let i = 0; i <= xSteps; i++) {
      const x = pad.left + (i / xSteps) * gw;
      if (i > 0 && i < xSteps) {
        ctx.beginPath();
        ctx.moveTo(x, pad.top);
        ctx.lineTo(x, pad.top + gh);
        ctx.strokeStyle = this.colors.grid;
        ctx.stroke();
      }
      const t = xMin + (i / xSteps) * xSpan;
      ctx.fillStyle = this.colors.ink3;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      ctx.fillText(this.formatAxis(t - xMax), x, pad.top + gh + 8);
    }
    ctx.textBaseline = 'middle';
  }

  formatAxis(ms) {
    const sec = Math.round(ms / 1000);
    if (sec === 0) return 'now';
    if (Math.abs(sec) >= 60) {
      const min = Math.round(Math.abs(sec) / 60);
      return '−' + min + 'm';
    }
    return '−' + Math.abs(sec) + 's';
  }
}

function clamp(value, min, max) {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, value));
}

function fmt(value, digits) {
  return typeof value === 'number' ? value.toFixed(digits) : '--';
}
