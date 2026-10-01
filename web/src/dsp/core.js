// Guitar Pedalboard DSP, ported line for line from the C++ plugin (Source/DSP/*.h).
// Runs inside the AudioWorklet (effects) and on the page (tuner, analyser, EQ curve).
// web/test/compare.mjs checks it against reference renders from the C++ build.
//
// core.js: the building blocks every effect uses (Source/DSP/DspUtils.h).

export const PI = Math.PI;
const FLT_EPS = 1.1920929e-7, FLT_MIN = 1.17549435e-38;

export const dbToGain = (db) => Math.pow(10, db * 0.05);
export const gainToDb = (g) => (g > 1e-9 ? 20 * Math.log10(g) : -180);
export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ============================================================================
// juce::SmoothedValue (linear or multiplicative), same stepping rules
export class Smoothed {
  constructor(initial = 0, multiplicative = false) {
    this.current = this.target = initial;
    this.countdown = 0;
    this.stepsToTarget = 0;
    this.step = 0;
    this.multiplicative = multiplicative;
  }
  reset(sampleRate, seconds) { this.stepsToTarget = Math.floor(seconds * sampleRate); this.setCurrentAndTarget(this.target); }
  setCurrentAndTarget(v) { this.target = this.current = v; this.countdown = 0; }
  setTarget(v) {
    const d = Math.abs(v - this.target);
    if (d <= FLT_MIN || d <= FLT_EPS * Math.max(Math.abs(v), Math.abs(this.target))) return;
    if (this.stepsToTarget <= 0) { this.setCurrentAndTarget(v); return; }
    this.target = v;
    this.countdown = this.stepsToTarget;
    this.step = this.multiplicative
      ? Math.exp((Math.log(Math.abs(this.target)) - Math.log(Math.abs(this.current))) / this.countdown)
      : (this.target - this.current) / this.countdown;
  }
  isSmoothing() { return this.countdown > 0; }
  next() {
    if (this.countdown <= 0) return this.target;
    if (--this.countdown > 0) {
      if (this.multiplicative) this.current *= this.step; else this.current += this.step;
    } else this.current = this.target;
    return this.current;
  }
  skip(n) {
    if (n >= this.countdown) { this.setCurrentAndTarget(this.target); return this.target; }
    if (this.multiplicative) this.current *= Math.pow(this.step, n); else this.current += this.step * n;
    this.countdown -= n;
    return this.current;
  }
}

// ============================================================================
// RBJ biquad, transposed direct form II (coefficients rounded to float like the C++)
export const f32 = Math.fround;
export class Biquad {
  constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.z1 = 0; this.z2 = 0; }
  reset() { this.z1 = this.z2 = 0; }
  process(x) {
    const y = this.b0 * x + this.z1;
    this.z1 = this.b1 * x - this.a1 * y + this.z2;
    this.z2 = this.b2 * x - this.a2 * y;
    return y;
  }
  static prewarp(fs, fc, q) {
    const w0 = (2 * PI * clamp(fc, 1, 0.49 * fs)) / fs;
    return [Math.cos(w0), Math.sin(w0) / (2 * q)];
  }
  set(b0, b1, b2, a0, a1, a2) {
    this.b0 = f32(b0 / a0); this.b1 = f32(b1 / a0); this.b2 = f32(b2 / a0);
    this.a1 = f32(a1 / a0); this.a2 = f32(a2 / a0);
  }
  setLowPass(fs, fc, q) {
    const [c, a] = Biquad.prewarp(fs, fc, q);
    this.set((1 - c) * 0.5, 1 - c, (1 - c) * 0.5, 1 + a, -2 * c, 1 - a);
  }
  setHighPass(fs, fc, q) {
    const [c, a] = Biquad.prewarp(fs, fc, q);
    this.set((1 + c) * 0.5, -(1 + c), (1 + c) * 0.5, 1 + a, -2 * c, 1 - a);
  }
  setPeak(fs, fc, q, gainDb) {
    const [c, a] = Biquad.prewarp(fs, fc, q);
    const A = Math.pow(10, gainDb / 40);
    this.set(1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A);
  }
  setLowShelf(fs, fc, gainDb) {
    const [c, a] = Biquad.prewarp(fs, fc, 0.70710678);
    const A = Math.pow(10, gainDb / 40), k = 2 * Math.sqrt(A) * a;
    this.set(A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - k),
             (A + 1) + (A - 1) * c + k, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k);
  }
  setHighShelf(fs, fc, gainDb) {
    const [c, a] = Biquad.prewarp(fs, fc, 0.70710678);
    const A = Math.pow(10, gainDb / 40), k = 2 * Math.sqrt(A) * a;
    this.set(A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - k),
             (A + 1) - (A - 1) * c + k, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k);
  }
  /** Linear magnitude at `freq` (for drawing EQ curves). */
  getMagnitude(freq, fs) {
    const w = (-2 * PI * freq) / fs;
    const c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
    const nr = this.b0 + this.b1 * c1 + this.b2 * c2, ni = this.b1 * s1 + this.b2 * s2;
    const dr = 1 + this.a1 * c1 + this.a2 * c2, di = this.a1 * s1 + this.a2 * s2;
    return Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
  }
}

// Topology-preserving one-pole filter
export class OnePole {
  constructor() { this.G = 0; this.s = 0; }
  setCutoff(fs, fc) { const g = Math.tan((PI * clamp(fc, 1, 0.49 * fs)) / fs); this.G = f32(g / (1 + g)); }
  reset() { this.s = 0; }
  lowPass(x) { const v = (x - this.s) * this.G; const lp = v + this.s; this.s = lp + v; return lp; }
  highPass(x) { return x - this.lowPass(x); }
}

export class DcBlocker {
  constructor() { this.r = 0.999; this.x1 = 0; this.y1 = 0; }
  prepare(fs) { this.r = f32(1 - (2 * PI * 10) / fs); this.reset(); }
  reset() { this.x1 = this.y1 = 0; }
  process(x) { const y = x - this.x1 + this.r * this.y1; this.x1 = x; this.y1 = y; return y; }
}

// Circular buffer with 4-point Hermite interpolation; read(d) = sample pushed d samples ago (d >= 1)
export class DelayLine {
  constructor() { this.buffer = new Float32Array(8); this.writePos = 0; }
  prepare(maxDelaySamples) { this.buffer = new Float32Array(maxDelaySamples + 4); this.writePos = 0; }
  reset() { this.buffer.fill(0); this.writePos = 0; }
  getMaxDelay() { return this.buffer.length - 4; }
  push(x) { this.buffer[this.writePos] = x; if (++this.writePos === this.buffer.length) this.writePos = 0; }
  read(delay) {
    delay = clamp(delay, 1, this.getMaxDelay());
    const b = this.buffer, size = b.length, di = Math.floor(delay), frac = delay - di;
    const at = (d) => { let i = this.writePos - d; while (i < 0) i += size; return b[i]; };
    const xm1 = at(di - 1 < 1 ? 1 : di - 1), x0 = at(di), x1 = at(di + 1), x2 = at(di + 2);
    const c1 = 0.5 * (x1 - xm1);
    const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * frac + c2) * frac + c1) * frac + x0;
  }
}

// ============================================================================
// juce::dsp::Oversampling (2 stages of polyphase IIR half-band = 4x), same structure.
// Coefficients come from JUCE's FilterDesign (exported by the C++ test).
export const OVERSAMPLER_COEFFS = {
  up: [[0.0457281470, 0.332501113, 0.663202047, 0.933855832, 0.168087542, 0.504485726, 0.803780854],
       [0.0542307794, 0.398796976, 0.862917840, 0.199699581, 0.621096849]],
  down: [[0.0542175248, 0.383087337, 0.748720944, 0.196797967, 0.573136389, 0.914293706],
         [0.0707659498, 0.513167560, 0.257853091, 0.817317367]],
};

class PolyphaseStage {
  constructor(up, down) {
    this.up = Float32Array.from(up); this.down = Float32Array.from(down);
    this.v1Up = new Float32Array(this.up.length); this.v1Down = new Float32Array(this.down.length);
    this.delayDown = 0;
  }
  reset() { this.v1Up.fill(0); this.v1Down.fill(0); this.delayDown = 0; }
  processUp(input, n, out) {
    const c = this.up, v = this.v1Up, stages = c.length, direct = stages - (stages >> 1);
    for (let i = 0; i < n; ++i) {
      let x = input[i];
      for (let k = 0; k < direct; ++k) { const y = c[k] * x + v[k]; v[k] = x - c[k] * y; x = y; }
      out[i << 1] = x;
      x = input[i];
      for (let k = direct; k < stages; ++k) { const y = c[k] * x + v[k]; v[k] = x - c[k] * y; x = y; }
      out[(i << 1) + 1] = x;
    }
  }
  processDown(buffer, n, out) {
    const c = this.down, v = this.v1Down, stages = c.length, direct = stages - (stages >> 1);
    let delay = this.delayDown;
    for (let i = 0; i < n; ++i) {
      let x = buffer[i << 1];
      for (let k = 0; k < direct; ++k) { const y = c[k] * x + v[k]; v[k] = x - c[k] * y; x = y; }
      const directOut = x;
      x = buffer[(i << 1) + 1];
      for (let k = direct; k < stages; ++k) { const y = c[k] * x + v[k]; v[k] = x - c[k] * y; x = y; }
      out[i] = (delay + directOut) * 0.5;
      delay = x;
    }
    this.delayDown = delay;
  }
}

export class Oversampler4x {
  constructor(maxBlock) {
    const k = OVERSAMPLER_COEFFS;
    this.stages = [new PolyphaseStage(k.up[0], k.down[0]), new PolyphaseStage(k.up[1], k.down[1])];
    this.buf2 = new Float32Array(maxBlock * 2);
    this.buf4 = new Float32Array(maxBlock * 4);
  }
  reset() { for (const s of this.stages) s.reset(); }
  /** Returns the 4x buffer (process it in place, then call down()). */
  up(input, n) {
    this.stages[0].processUp(input, n, this.buf2);
    this.stages[1].processUp(this.buf2, n * 2, this.buf4);
    return this.buf4;
  }
  down(output, n) {
    this.stages[1].processDown(this.buf4, n * 2, this.buf2);
    this.stages[0].processDown(this.buf2, n, output);
  }
}

