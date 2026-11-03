const ARC_LENGTH = 2 * Math.PI * 84 * 0.75;

function clamp(value, min, max) {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, value));
}

/**
 * Thin 270-degree temperature arc.
 * The circles share a dash pattern; only dash offsets move.
 */
export class Gauge {
  constructor(progress, target, svg) {
    this.progress = progress;
    this.target = target;
    this.svg = svg;
  }

  render(temperature, setpoint, maxTemperature, live) {
    const max = Number.isFinite(maxTemperature) && maxTemperature > 0 ? maxTemperature : 350;
    const temp = Number.isFinite(temperature) ? temperature : 0;
    const sp = Number.isFinite(setpoint) ? setpoint : 0;
    const tempFrac = clamp(temp / max, 0, 1);
    const spFrac = clamp(sp / max, 0, 1);

    this.progress.style.strokeDashoffset = String(ARC_LENGTH * (1 - tempFrac));
    this.target.style.strokeDashoffset = String(-ARC_LENGTH * spFrac);

    if (this.svg) {
      const label = live
        ? 'Hotend at ' + temp.toFixed(1) + ' degrees Celsius, target ' + sp.toFixed(1)
        : 'Hotend temperature unavailable';
      this.svg.setAttribute('aria-label', label);
    }
  }
}
