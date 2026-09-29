const DSP = (() => {
// Guitar Pedalboard DSP, ported line for line from the C++ plugin (Source/DSP/*.h).
// Runs inside the AudioWorklet (effects) and on the page (tuner, analyser, EQ curve).
// web/test/compare.mjs checks it against reference renders from the C++ build.

const PI = Math.PI;
const FLT_EPS = 1.1920929e-7, FLT_MIN = 1.17549435e-38;

const dbToGain = (db) => Math.pow(10, db * 0.05);
const gainToDb = (g) => (g > 1e-9 ? 20 * Math.log10(g) : -180);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ============================================================================
// juce::SmoothedValue (linear or multiplicative), same stepping rules
class Smoothed {
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
const f32 = Math.fround;
class Biquad {
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
class OnePole {
  constructor() { this.G = 0; this.s = 0; }
  setCutoff(fs, fc) { const g = Math.tan((PI * clamp(fc, 1, 0.49 * fs)) / fs); this.G = f32(g / (1 + g)); }
  reset() { this.s = 0; }
  lowPass(x) { const v = (x - this.s) * this.G; const lp = v + this.s; this.s = lp + v; return lp; }
  highPass(x) { return x - this.lowPass(x); }
}

class DcBlocker {
  constructor() { this.r = 0.999; this.x1 = 0; this.y1 = 0; }
  prepare(fs) { this.r = f32(1 - (2 * PI * 10) / fs); this.reset(); }
  reset() { this.x1 = this.y1 = 0; }
  process(x) { const y = x - this.x1 + this.r * this.y1; this.x1 = x; this.y1 = y; return y; }
}

// Circular buffer with 4-point Hermite interpolation; read(d) = sample pushed d samples ago (d >= 1)
class DelayLine {
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
class NoiseGate {
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.envDecay = Math.exp(-1 / (0.03 * this.fs));
    this.attackCoef = Math.exp(-1 / (0.0005 * this.fs));
    this.holdSamples = Math.floor(0.04 * this.fs);
    this.setParameters(-65, 60);
    this.reset();
  }
  reset() { this.env = 0; this.gain = 0; this.holdCounter = 0; this.isOpen = false; }
  setParameters(thresholdDb, releaseMs) {
    this.openThreshold = dbToGain(thresholdDb);
    this.closeThreshold = this.openThreshold * 0.5;
    this.releaseCoef = Math.exp(-1 / (Math.max(1, releaseMs) * 0.001 * this.fs));
  }
  process(data, n) {
    for (let i = 0; i < n; ++i) {
      const x = data[i], a = Math.abs(x);
      this.env = a > this.env ? a : this.env * this.envDecay;
      if (this.env >= this.openThreshold) { this.isOpen = true; this.holdCounter = this.holdSamples; }
      else if (this.isOpen) {
        if (this.env >= this.closeThreshold) this.holdCounter = this.holdSamples;
        else if (this.holdCounter > 0) --this.holdCounter;
        else this.isOpen = false;
      }
      const target = this.isOpen ? 1 : 0;
      const coef = target > this.gain ? this.attackCoef : this.releaseCoef;
      this.gain = target + coef * (this.gain - target);
      data[i] = x * this.gain;
    }
  }
}

// ============================================================================
// juce::dsp::Oversampling (2 stages of polyphase IIR half-band = 4x), same structure.
// Coefficients come from JUCE's FilterDesign (exported by the C++ test).
const OVERSAMPLER_COEFFS = {
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

class Oversampler4x {
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

// ============================================================================
// Distortion: the POD HD500X's 15 distortion models (Source/DSP/Distortion.h)
const shape = {
  taper: (x01, k) => (Math.exp(k * x01) - 1) / (Math.exp(k) - 1),
  diode: (x) => x / Math.pow(1 + Math.pow(Math.abs(x), 2.5), 0.4),
  asym: (x, bias) => Math.tanh(x + bias) - Math.tanh(bias),
  tube: (x) => (x >= 0 ? Math.tanh(x) : 0.7 * Math.tanh(x / 0.7)),
  eqDb: (percent, range) => (percent - 50) * (range / 50),
};
const NO_TRIM = [0, 0, 0, 0, 0];

class DriveModel {
  prepare(baseRate, overRate) {
    this.fs = baseRate; this.os = overRate;
    this.gain = this.gain || new Smoothed(1, true);
    this.level = this.level || new Smoothed(1);
    this.gain.reset(this.os, 0.05);
    this.level.reset(this.fs, 0.05);
    this.dc = new DcBlocker(); this.dc.prepare(this.fs);
    this.eqBass = new Biquad(); this.eqMid = new Biquad(); this.eqTreble = new Biquad();
    this.loudnessTrim = this.loudnessTrim || NO_TRIM;
  }
  reset() {
    this.gain.setCurrentAndTarget(this.gain.target);
    this.level.setCurrentAndTarget(this.level.target);
    this.dc.reset(); this.eqBass.reset(); this.eqMid.reset(); this.eqTreble.reset();
  }
  static drive01(k) { return f32(k[0] / 100); }
  setEqAndOutput(drivePercent, bassPercent, midPercent, treblePercent, outputDb, makeup) {
    this.eqBass.setLowShelf(this.fs, 120, shape.eqDb(bassPercent, 12));
    this.eqMid.setPeak(this.fs, 800, 0.8, shape.eqDb(midPercent, 12));
    this.eqTreble.setHighShelf(this.fs, 3000, shape.eqDb(treblePercent, 12));
    const d = clamp(drivePercent / 25, 0, 4), i = Math.min(Math.floor(d), 3), t = this.loudnessTrim;
    const trimDb = t[i] + (t[i + 1] - t[i]) * (d - i);
    this.level.setTarget(f32(f32(dbToGain(outputDb + trimDb)) * makeup));
  }
  finish(x, n) {
    const dc = this.dc, b = this.eqBass, m = this.eqMid, t = this.eqTreble, level = this.level;
    for (let i = 0; i < n; ++i) x[i] = t.process(m.process(b.process(dc.process(x[i])))) * level.next();
  }
}
const onePoles = (count) => Array.from({ length: count }, () => new OnePole());

class TubeDriveModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.coupling, this.bandLimit, this.post] = this.f ||= onePoles(4);
    this.gain.setTarget(f32(2 + 90 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 110); this.coupling.setCutoff(this.os, 40);
    this.bandLimit.setCutoff(this.os, 7000); this.post.setCutoff(this.fs, 6500);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.16);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, c = this.coupling, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const s1 = shape.tube(g.next() * hp.highPass(x[i]));
      x[i] = bl.lowPass(shape.tube(2.2 * c.highPass(s1)));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

class ScreamerModel extends DriveModel {
  setParameters(k) {
    [this.clipHp, this.feedbackCap, this.toneLp] = this.f ||= onePoles(3);
    const rd = 500e3 * f32(shape.taper(DriveModel.drive01(k), 3));
    this.gain.setTarget(f32(1 + (51e3 + rd) / 4.7e3));
    this.clipHp.setCutoff(this.os, 720);
    this.feedbackCap.setCutoff(this.os, 1 / (2 * PI * (51e3 + rd) * 51e-12));
    this.toneLp.setCutoff(this.fs, 723);
    const t = f32(k[2] / 100);
    this.treble = f32(0.05 + 1.6 * t * t);
    this.setEqAndOutput(k[0], k[1], 50, k[3], k[4], 0.32);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.clipHp, cap = this.feedbackCap;
    for (let i = 0; i < n; ++i) {
      const v = cap.lowPass(g.next() * hp.highPass(x[i]));
      x[i] += 0.6 * Math.tanh(v * (1 / 0.6));
    }
  }
  processBase(x, n) {
    const lp = this.toneLp, treble = this.treble;
    for (let i = 0; i < n; ++i) { const low = lp.lowPass(x[i]); x[i] = low + treble * (x[i] - low); }
    this.finish(x, n);
  }
}

class OverdriveModel extends DriveModel {
  setParameters(k) {
    [this.gainHp, this.opAmp, this.bandLimit, this.post] = this.f ||= onePoles(4);
    const g = f32(1 + (1e6 * f32(shape.taper(DriveModel.drive01(k), 3.5))) / 4.7e3);
    this.gain.setTarget(g);
    this.gainHp.setCutoff(this.os, 720);
    this.opAmp.setCutoff(this.os, Math.min(20000, 1e6 / g));
    this.bandLimit.setCutoff(this.os, 12000);
    this.post.setCutoff(this.fs, 7000);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.23);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.gainHp, op = this.opAmp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const v = op.lowPass(x[i] + (g.next() - 1) * hp.highPass(x[i]));
      x[i] = bl.lowPass(0.55 * shape.diode(v * (1 / 0.55)));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

class ClassicDistModel extends DriveModel {
  setParameters(k) {
    [this.leg1, this.leg2, this.opAmp, this.filter] = this.f ||= onePoles(4);
    const rd = f32(100e3 * f32(shape.taper(DriveModel.drive01(k), 4)));
    this.gain.setTarget(f32(rd / 1000 + 0.001));
    this.leg1.setCutoff(this.os, 60.5);
    this.leg2.setCutoff(this.os, 1539);
    this.opAmp.setCutoff(this.os, Math.min(20000, 1e6 / (1 + rd / 43.4)));
    const rf = 100e3 * f32(shape.taper(f32(k[2] / 100), 3));
    this.filter.setCutoff(this.fs, 1 / (2 * PI * (1.5e3 + rf) * 3.3e-9));
    this.setEqAndOutput(k[0], k[1], 50, k[3], k[4], 0.19);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, l1 = this.leg1, l2 = this.leg2, op = this.opAmp;
    for (let i = 0; i < n; ++i) {
      const rd = 1000 * (g.next() - 0.001);
      const v = x[i] + rd * (l1.highPass(x[i]) * (1 / 560) + l2.highPass(x[i]) * (1 / 47));
      x[i] = 0.55 * shape.diode(op.lowPass(v) * (1 / 0.55));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.filter.lowPass(x[i]); this.finish(x, n); }
}

class HeavyDistModel extends DriveModel {
  setParameters(k) {
    [this.tight, this.interHp, this.interLp, this.bandLimit] = this.f ||= onePoles(4);
    [this.preMid, this.fizz, this.scoop] = this.b ||= [new Biquad(), new Biquad(), new Biquad()];
    this.gain.setTarget(f32(15 + 600 * shape.taper(DriveModel.drive01(k), 3)));
    this.tight.setCutoff(this.os, 140);
    this.preMid.setPeak(this.os, 900, 0.8, 8);
    this.interHp.setCutoff(this.os, 220);
    this.interLp.setCutoff(this.os, 6000);
    this.bandLimit.setCutoff(this.os, 9000);
    this.fizz.setLowPass(this.fs, 7000, 0.707);
    this.scoop.setPeak(this.fs, 650, 0.9, -5);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.1);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); for (const b of this.b) b.reset(); }
  processOversampled(x, n) {
    const g = this.gain, tight = this.tight, pre = this.preMid, ih = this.interHp, il = this.interLp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const s1 = Math.tanh(g.next() * pre.process(tight.highPass(x[i])));
      x[i] = bl.lowPass(shape.diode(4 * il.lowPass(ih.highPass(s1))));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.scoop.process(this.fizz.process(x[i])); this.finish(x, n); }
}

class ColorDriveModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.bandLimit, this.post] = this.f ||= onePoles(3);
    this.gain.setTarget(f32(2 + 300 * shape.taper(DriveModel.drive01(k), 3.5)));
    this.inputHp.setCutoff(this.os, 45); this.bandLimit.setCutoff(this.os, 8000); this.post.setCutoff(this.fs, 7500);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.12);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) x[i] = bl.lowPass(Math.tanh(1.8 * shape.asym(g.next() * hp.highPass(x[i]), 0.35)));
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

class BuzzSawModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.bandLimit, this.post] = this.f ||= onePoles(3);
    this.horn ||= new Biquad();
    this.gain.setTarget(f32(20 + 500 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 300); this.bandLimit.setCutoff(this.os, 6000);
    this.horn.setPeak(this.fs, 1300, 1.5, 6); this.post.setCutoff(this.fs, 4500);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.092);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); this.horn.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const v = g.next() * hp.highPass(x[i]);
      const gated = v > 0.25 ? v - 0.25 : v < -0.25 ? v + 0.25 : 0;
      const y = gated >= 0 ? Math.tanh(2.5 * gated) : 0.6 * Math.tanh(gated * (2.5 / 0.6));
      x[i] = bl.lowPass(y);
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(this.horn.process(x[i])); this.finish(x, n); }
}

class FacialFuzzModel extends DriveModel {
  setParameters(k) {
    [this.pickupLoad, this.inputHp, this.bandLimit, this.post] = this.f ||= onePoles(4);
    this.gain.setTarget(f32(3 + 250 * shape.taper(DriveModel.drive01(k), 2.5)));
    this.pickupLoad.setCutoff(this.os, 4500); this.inputHp.setCutoff(this.os, 70); this.bandLimit.setCutoff(this.os, 7000);
    this.attack = f32(Math.exp(-1 / (0.002 * this.os)));
    this.release = f32(Math.exp(-1 / (0.1 * this.os)));
    this.post.setCutoff(this.fs, 5000);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.12);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); this.env = 0; }
  processOversampled(x, n) {
    const g = this.gain, load = this.pickupLoad, hp = this.inputHp, bl = this.bandLimit, attack = this.attack, release = this.release;
    let env = this.env;
    for (let i = 0; i < n; ++i) {
      const v = g.next() * hp.highPass(load.lowPass(x[i]));
      const a = Math.abs(v);
      env = f32(a + (a > env ? attack : release) * (env - a));
      const bias = 0.15 + (0.25 * env) / (1 + env);
      x[i] = bl.lowPass(Math.tanh(1.6 * shape.asym(v, bias)));
    }
    this.env = env;
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

class JumboFuzzModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.bandLimit, this.dark, this.bright] = this.f ||= onePoles(4);
    this.gain.setTarget(f32(10 + 900 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 120); this.bandLimit.setCutoff(this.os, 8000);
    this.dark.setCutoff(this.fs, 700); this.bright.setCutoff(this.fs, 1100);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.17);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) x[i] = bl.lowPass(Math.tanh(3 * shape.asym(g.next() * hp.highPass(x[i]), 0.3)));
  }
  processBase(x, n) {
    for (let i = 0; i < n; ++i) x[i] = 0.45 * this.dark.lowPass(x[i]) + 0.55 * 1.3 * this.bright.highPass(x[i]);
    this.finish(x, n);
  }
}

class FuzzPiModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.stage1Lp, this.interHp, this.stage2Lp, this.toneLp, this.toneHp] = this.f ||= onePoles(6);
    this.gain.setTarget(f32(1 + 80 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 90); this.stage1Lp.setCutoff(this.os, 2800);
    this.interHp.setCutoff(this.os, 120); this.stage2Lp.setCutoff(this.os, 3200);
    this.toneLp.setCutoff(this.fs, 723); this.toneHp.setCutoff(this.fs, 1850);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.25);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, l1 = this.stage1Lp, ih = this.interHp, l2 = this.stage2Lp;
    for (let i = 0; i < n; ++i) {
      const s1 = Math.tanh(g.next() * 4 * hp.highPass(x[i]));
      x[i] = l2.lowPass(Math.tanh(18 * ih.highPass(l1.lowPass(s1))));
    }
  }
  processBase(x, n) {
    for (let i = 0; i < n; ++i) x[i] = 0.5 * this.toneLp.lowPass(x[i]) + 0.5 * this.toneHp.highPass(x[i]);
    this.finish(x, n);
  }
}

class JetFuzzModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.bandLimit, this.tone] = this.f ||= onePoles(3);
    this.stages ||= new Float32Array(6);
    this.gain.setTarget(f32(10 + 500 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 100); this.bandLimit.setCutoff(this.os, 6000);
    this.feedback = f32((0.85 * k[1]) / 100);
    this.tone.setCutoff(this.fs, 800 * Math.pow(12, k[2] / 100));
    this.rate = k[3];
    this.setEqAndOutput(k[0], 50, 50, 50, k[4], 0.15);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); this.stages.fill(0); this.last = 0; this.phase = 0; }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) x[i] = bl.lowPass(shape.diode(g.next() * hp.highPass(x[i])));
  }
  processBase(x, n) {
    const increment = this.rate / this.fs, st = this.stages, fb = this.feedback, tone = this.tone;
    for (let i = 0; i < n; ++i) {
      const lfo = Math.sin(2 * PI * this.phase);
      this.phase += increment;
      if (this.phase >= 1) this.phase -= 1;
      const fc = 1000 * Math.pow(4, 0.8 * lfo), t = Math.tan((PI * fc) / this.fs), a = f32((t - 1) / (t + 1));
      let v = f32(x[i] + fb * this.last);
      for (let s = 0; s < 6; ++s) { const y = f32(a * v + st[s]); st[s] = v - a * y; v = y; }
      this.last = v;
      x[i] = tone.lowPass(0.5 * (x[i] + v));
    }
    this.finish(x, n);
  }
}

class Line6DriveModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.midPush, this.bandLimit] = this.f ||= onePoles(3);
    this.gain.setTarget(f32(3 + 250 * shape.taper(DriveModel.drive01(k), 3)));
    const m = f32(k[2] / 100);
    this.fuzzWeight = f32(Math.max(0, 1 - 2 * m));
    this.modernWeight = f32(1 - Math.abs(2 * m - 1));
    this.gritWeight = f32(Math.max(0, 2 * m - 1));
    this.inputHp.setCutoff(this.os, 80); this.midPush.setCutoff(this.os, 600); this.bandLimit.setCutoff(this.os, 7500);
    this.setEqAndOutput(k[0], k[1], 50, k[3], k[4], 0.12);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const gs = this.gain, hp = this.inputHp, mp = this.midPush, bl = this.bandLimit;
    const wf = this.fuzzWeight, wm = this.modernWeight, wg = this.gritWeight, d25 = shape.diode(0.25);
    for (let i = 0; i < n; ++i) {
      const g = gs.next();
      const v = hp.highPass(x[i]) + wm * 1.5 * mp.highPass(x[i]);
      const fuzz = Math.tanh(2.5 * shape.asym(4 * g * v, 0.3));
      const modern = shape.diode(1.5 * g * v);
      const grit = shape.diode(2 * g * v + 0.25) - d25;
      x[i] = bl.lowPass(0.9 * wf * fuzz + wm * modern + wg * grit);
    }
  }
  processBase(x, n) { this.finish(x, n); }
}

class Line6DistModel extends DriveModel {
  setParameters(k) {
    [this.tight, this.interLp, this.bandLimit] = this.f ||= onePoles(3);
    [this.body, this.fizz] = this.b ||= [new Biquad(), new Biquad()];
    this.gain.setTarget(f32(20 + 1200 * shape.taper(DriveModel.drive01(k), 3)));
    this.tight.setCutoff(this.os, 250); this.interLp.setCutoff(this.os, 7000); this.bandLimit.setCutoff(this.os, 10000);
    this.body.setLowShelf(this.fs, 110, 6); this.fizz.setLowPass(this.fs, 6500, 0.707);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.094);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); for (const b of this.b) b.reset(); }
  processOversampled(x, n) {
    const g = this.gain, tight = this.tight, il = this.interLp, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const s1 = shape.asym(g.next() * tight.highPass(x[i]), 0.2);
      x[i] = bl.lowPass(shape.diode(3 * il.lowPass(s1)));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.fizz.process(this.body.process(x[i])); this.finish(x, n); }
}

class SubOctaveFuzzModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.track1, this.track2, this.subLp, this.bandLimit, this.post] = this.f ||= onePoles(6);
    this.gain.setTarget(f32(5 + 200 * shape.taper(DriveModel.drive01(k), 3)));
    this.inputHp.setCutoff(this.os, 90); this.track1.setCutoff(this.os, 700); this.track2.setCutoff(this.os, 700);
    this.subLp.setCutoff(this.os, 2500); this.bandLimit.setCutoff(this.os, 7000);
    this.envAttack = f32(Math.exp(-1 / (0.003 * this.os)));
    this.envRelease = f32(Math.exp(-1 / (0.06 * this.os)));
    this.sub = f32(k[2] / 100);
    this.post.setCutoff(this.fs, 6000);
    this.setEqAndOutput(k[0], k[1], 50, k[3], k[4], 0.17);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); this.env = 0; this.high = false; this.flip1 = this.flip2 = 1; }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, t1 = this.track1, t2 = this.track2, sl = this.subLp, bl = this.bandLimit;
    const attack = this.envAttack, release = this.envRelease, sub = this.sub;
    let env = this.env;
    for (let i = 0; i < n; ++i) {
      const inp = hp.highPass(x[i]);
      const a = Math.abs(inp);
      env = f32(a + (a > env ? attack : release) * (env - a));
      const tracked = t2.lowPass(t1.lowPass(inp));
      const threshold = f32(0.1 * env);
      if (!this.high && tracked > threshold) {
        this.high = true;
        this.flip1 = -this.flip1;
        if (this.flip1 > 0) this.flip2 = -this.flip2;
      } else if (this.high && tracked < -threshold) this.high = false;
      const octaves = sl.lowPass(2 * env * (0.7 * this.flip1 + 0.5 * this.flip2));
      const fuzz = Math.tanh(g.next() * inp);
      x[i] = bl.lowPass(fuzz + sub * octaves);
    }
    this.env = env;
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

class OctaveFuzzModel extends DriveModel {
  setParameters(k) {
    [this.inputHp, this.transformer, this.bandLimit, this.post] = this.f ||= onePoles(4);
    this.gain.setTarget(f32(2 + 40 * shape.taper(DriveModel.drive01(k), 2.5)));
    this.inputHp.setCutoff(this.os, 150); this.transformer.setCutoff(this.os, 30);
    this.bandLimit.setCutoff(this.os, 6000); this.post.setCutoff(this.fs, 5000);
    this.setEqAndOutput(k[0], k[1], k[2], k[3], k[4], 0.14);
  }
  reset() { super.reset(); for (const f of this.f) f.reset(); }
  processOversampled(x, n) {
    const g = this.gain, hp = this.inputHp, tr = this.transformer, bl = this.bandLimit;
    for (let i = 0; i < n; ++i) {
      const v = Math.tanh(g.next() * hp.highPass(x[i]));
      x[i] = bl.lowPass(Math.tanh(6 * tr.highPass(Math.abs(v))));
    }
  }
  processBase(x, n) { for (let i = 0; i < n; ++i) x[i] = this.post.lowPass(x[i]); this.finish(x, n); }
}

/** dB added at 0, 25, 50, 75, 100 % drive, per model (same table as the C++). */
const LOUDNESS_TRIMS = [
  [8.1, 2.4, 0.0, -1.2, -1.8], [2.5, 1.2, -0.1, -1.1, -2.0], [14.1, 3.2, 0.1, -1.2, -1.9], [16.1, 1.4, 0.1, -0.3, -0.4],
  [2.2, 1.3, 1.0, 0.9, 0.9], [11.3, 2.4, 0.2, -0.7, -1.1], [3.3, 1.2, 0.3, -0.2, -0.3], [9.9, 2.0, 0.4, -0.3, -0.6],
  [3.0, 0.3, -0.2, -0.3, -0.4], [2.0, 0.5, 0.2, 0.1, 0.1], [7.4, 2.9, 1.4, 0.6, 0.4], [4.6, -0.5, -1.7, -2.3, -2.5],
  [1.6, -0.1, -0.4, -0.5, -0.5], [5.9, 0.6, -1.9, -3.2, -3.9], [7.0, 3.0, 0.8, -0.4, -0.8],
].map((row) => row.map(f32));
const DRIVE_MODEL_CLASSES = [TubeDriveModel, ScreamerModel, OverdriveModel, ClassicDistModel, HeavyDistModel, ColorDriveModel,
  BuzzSawModel, FacialFuzzModel, JumboFuzzModel, FuzzPiModel, JetFuzzModel, Line6DriveModel, Line6DistModel,
  SubOctaveFuzzModel, OctaveFuzzModel];

/** A slot's distortion engine: every model plus the 4x oversampler they share. */
class Distortion {
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.oversampler = new Oversampler4x(maxBlock);
    const defaults = [50, 50, 50, 50, 0, 0, 0, 0];
    this.models = DRIVE_MODEL_CLASSES.map((Model, i) => {
      const m = new Model();
      m.loudnessTrim = LOUDNESS_TRIMS[i];
      m.prepare(this.fs, this.fs * 4);
      m.setParameters(defaults);
      m.reset();
      return m;
    });
    this.current = 0;
    this.reset();
  }
  reset() { this.oversampler.reset(); this.models[this.current].reset(); }
  setModel(variant) { this.current = clamp(variant | 0, 0, this.models.length - 1); }
  setParameters(knobs) { this.models[this.current].setParameters(knobs); }
  process(data, n) {
    const model = this.models[this.current];
    const up = this.oversampler.up(data, n);
    model.processOversampled(up, n * 4);
    this.oversampler.down(data, n);
    model.processBase(data, n);
  }
}

// ============================================================================
class CabSim {
  prepare(fs) {
    this.f = [new Biquad(), new Biquad(), new Biquad(), new Biquad(), new Biquad(), new Biquad()];
    this.f[0].setHighPass(fs, 75, 0.707);
    this.f[1].setPeak(fs, 120, 1.4, 3);
    this.f[2].setPeak(fs, 450, 1.0, -3.5);
    this.f[3].setPeak(fs, 2300, 1.3, 4);
    this.f[4].setLowPass(fs, 5000, 0.6);
    this.f[5].setLowPass(fs, 6500, 0.9);
  }
  reset() { for (const f of this.f) f.reset(); }
  process(data, n) {
    const [a, b, c, d, e, g] = this.f;
    for (let i = 0; i < n; ++i) data[i] = g.process(e.process(d.process(c.process(b.process(a.process(data[i]))))));
  }
}

// ============================================================================
const EQ_BANDS = ["lowCut", "bass", "lowMid", "highMid", "treble", "highCut"];
const defaultEq = () => ({
  lowCutHz: 20, bassHz: 100, bassDb: 0, lowMidHz: 400, lowMidDb: 0, lowMidQ: 1,
  highMidHz: 2000, highMidDb: 0, highMidQ: 1, trebleHz: 5000, trebleDb: 0, highCutHz: 20000,
});

class Equalizer {
  /** Shared by the audio path and the on-screen curve. */
  static design(s, fs, filters) {
    filters[0].setHighPass(fs, s.lowCutHz, 0.707);
    filters[1].setLowShelf(fs, s.bassHz, s.bassDb);
    filters[2].setPeak(fs, s.lowMidHz, s.lowMidQ, s.lowMidDb);
    filters[3].setPeak(fs, s.highMidHz, s.highMidQ, s.highMidDb);
    filters[4].setHighShelf(fs, s.trebleHz, s.trebleDb);
    filters[5].setLowPass(fs, s.highCutHz, 0.707);
  }
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.filters = Array.from({ length: 6 }, () => new Biquad());
    this.smoothed = Array.from({ length: 12 }, () => new Smoothed(0));
    const d = defaultEq();
    this.setParameters(d);
    for (const v of this.smoothed) v.reset(this.fs, 0.04);
    this.reset();
  }
  reset() {
    for (const f of this.filters) f.reset();
    for (const v of this.smoothed) v.setCurrentAndTarget(v.target);
    Equalizer.design(this.currentSettings(), this.fs, this.filters);
  }
  setParameters(s) {
    const t = [Math.log2(s.lowCutHz), Math.log2(s.bassHz), s.bassDb, Math.log2(s.lowMidHz), s.lowMidDb, Math.log2(s.lowMidQ),
               Math.log2(s.highMidHz), s.highMidDb, Math.log2(s.highMidQ), Math.log2(s.trebleHz), s.trebleDb, Math.log2(s.highCutHz)];
    for (let i = 0; i < 12; ++i) this.smoothed[i].setTarget(f32(t[i]));
  }
  currentSettings() {
    const v = (i) => this.smoothed[i].current, hz = (i) => Math.pow(2, v(i));
    return { lowCutHz: hz(0), bassHz: hz(1), bassDb: v(2), lowMidHz: hz(3), lowMidDb: v(4), lowMidQ: hz(5),
             highMidHz: hz(6), highMidDb: v(7), highMidQ: hz(8), trebleHz: hz(9), trebleDb: v(10), highCutHz: hz(11) };
  }
  process(data, n) {
    const f = this.filters;
    for (let start = 0; start < n; start += 16) {
      const len = Math.min(16, n - start);
      if (this.smoothed.some((v) => v.isSmoothing())) {
        for (const v of this.smoothed) v.skip(len);
        Equalizer.design(this.currentSettings(), this.fs, f);
      }
      for (let i = start; i < start + len; ++i)
        data[i] = f[5].process(f[4].process(f[3].process(f[2].process(f[1].process(f[0].process(data[i]))))));
    }
  }
}

// ============================================================================
const MOD_TYPES = ["Chorus", "Flanger", "Phaser", "Tremolo"];

class Modulation {
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.lines = [new DelayLine(), new DelayLine()];
    this.flangerLines = [new DelayLine(), new DelayLine()];
    for (const l of [...this.lines, ...this.flangerLines]) l.prepare(Math.floor(0.03 * this.fs) + 8);
    this.depth = new Smoothed(0.5); this.mix = new Smoothed(0.5);
    this.depth.reset(this.fs, 0.05); this.mix.reset(this.fs, 0.05);
    this.allpass = [new Float64Array(6), new Float64Array(6)];
    this.phaserFb = [0, 0];
    this.type = 0; this.rate = 1; this.phase = 0;
    this.reset();
  }
  reset() {
    for (const l of [...this.lines, ...this.flangerLines]) l.reset();
    this.restart();
  }
  /** LFO and phaser start over, the delay lines (kept fed) stay. */
  restart() {
    this.resetPhaser();
    this.phase = 0;
    this.depth.setCurrentAndTarget(this.depth.target);
    this.mix.setCurrentAndTarget(this.mix.target);
  }
  resetPhaser() { this.allpass[0].fill(0); this.allpass[1].fill(0); this.phaserFb[0] = this.phaserFb[1] = 0; }
  feed(left, right, n) {
    const [l0, l1] = this.lines, [f0, f1] = this.flangerLines;
    for (let i = 0; i < n; ++i) { l0.push(left[i]); l1.push(right[i]); f0.push(left[i]); f1.push(right[i]); }
  }
  setParameters(type, rateHz, depth01, mix01) {
    type = clamp(type | 0, 0, 3);
    if (type !== this.type) { this.type = type; this.resetPhaser(); }
    this.rate = rateHz;
    this.depth.setTarget(f32(depth01));
    this.mix.setTarget(f32(mix01));
  }
  process(left, right, n) {
    const io = [left, right], inc = this.rate / this.fs, fs = this.fs;
    for (let i = 0; i < n; ++i) {
      const d = this.depth.next(), m = this.mix.next();
      const lfo0 = Math.sin(2 * PI * this.phase), lfo1 = Math.sin(2 * PI * (this.phase + 0.25));
      this.phase += inc;
      if (this.phase >= 1) this.phase -= 1;
      for (let ch = 0; ch < 2; ++ch) {
        const x = io[ch][i], lfo = ch === 0 ? lfo0 : lfo1;
        switch (this.type) {
          case 0: {
            const wet = this.lines[ch].read((12 + 6 * d * lfo) * 0.001 * fs);
            this.lines[ch].push(x);
            this.flangerLines[ch].push(x);
            io[ch][i] = x * (1 - m) + wet * m;
            break;
          }
          case 1: {
            const wet = this.flangerLines[ch].read((0.25 + 4 * d * 0.5 * (lfo + 1)) * 0.001 * fs);
            this.flangerLines[ch].push(x + 0.7 * wet);
            this.lines[ch].push(x);
            io[ch][i] = x * (1 - m) + wet * m;
            break;
          }
          case 2: {
            const fc = 1000 * Math.pow(2, 2.3 * d * lfo);
            const t = Math.tan((PI * fc) / fs), a = (t - 1) / (t + 1);
            const st = this.allpass[ch];
            let v = x + 0.4 * this.phaserFb[ch];
            for (let k = 0; k < 6; ++k) { const y = a * v + st[k]; st[k] = v - a * y; v = y; }
            this.phaserFb[ch] = v;
            this.lines[ch].push(x);
            this.flangerLines[ch].push(x);
            io[ch][i] = x * (1 - m) + v * m;
            break;
          }
          default: {
            this.lines[ch].push(x);
            this.flangerLines[ch].push(x);
            const k = 0.5 + 9 * m;
            const shaped = Math.tanh(k * lfo0) / Math.tanh(k);
            io[ch][i] = x * (1 - d * 0.5 * (shaped + 1));
          }
        }
      }
    }
  }
}

// ============================================================================
class Delay {
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.lines = [new DelayLine(), new DelayLine()];
    for (const l of this.lines) l.prepare(Math.floor(2 * this.fs) + 8);
    this.lowPass = [new OnePole(), new OnePole()];
    this.highPass = [new OnePole(), new OnePole()];
    for (const f of this.highPass) f.setCutoff(this.fs, 70);
    this.time = new Smoothed(1); this.feedback = new Smoothed(0); this.mix = new Smoothed(0); this.send = new Smoothed(0);
    this.time.reset(this.fs, 0.3); this.feedback.reset(this.fs, 0.05); this.mix.reset(this.fs, 0.05); this.send.reset(this.fs, 0.03);
    this.reset();
  }
  reset() {
    for (const l of this.lines) l.reset();
    for (const f of this.lowPass) f.reset();
    for (const f of this.highPass) f.reset();
    this.active = false;
    this.silentSamples = 0;
  }
  setParameters(enabled, timeMs, feedback01, mix01, tone01) {
    const delaySamples = f32(clamp(timeMs * 0.001 * this.fs, 1, this.lines[0].getMaxDelay()));
    if (enabled && !this.active) { this.reset(); this.active = true; this.time.setCurrentAndTarget(delaySamples); }
    this.send.setTarget(enabled ? 1 : 0);
    this.time.setTarget(delaySamples);
    this.feedback.setTarget(f32(feedback01));
    this.mix.setTarget(f32(mix01));
    for (const f of this.lowPass) f.setCutoff(this.fs, 1000 * Math.pow(12, tone01));
  }
  process(left, right, n) {
    if (!this.active) return;
    const io = [left, right];
    let peak = 0;
    for (let i = 0; i < n; ++i) {
      const d = this.time.next(), fb = this.feedback.next(), m = this.mix.next(), s = this.send.next();
      for (let ch = 0; ch < 2; ++ch) {
        let wet = this.lines[ch].read(d);
        wet = this.highPass[ch].highPass(this.lowPass[ch].lowPass(wet));
        this.lines[ch].push(1.5 * Math.tanh((io[ch][i] * s + fb * wet) / 1.5));
        io[ch][i] += m * wet;
        peak = Math.max(peak, Math.abs(wet));
      }
    }
    if (this.send.target === 0 && !this.send.isSmoothing()) {
      this.silentSamples = peak < 1e-5 ? this.silentSamples + n : 0;
      if (this.silentSamples > Math.floor(this.time.target) + Math.floor(0.1 * this.fs)) this.reset();
    } else this.silentSamples = 0;
  }
}

// ============================================================================
// juce::Reverb (Freeverb) port
const UNDENORM = f32(0.1);
const undenormalise = (x) => f32(f32(f32(x) + UNDENORM) - UNDENORM); // JUCE_UNDENORMALISE on x86

class Comb {
  setSize(size) { this.buffer = new Float32Array(size); this.index = 0; this.last = 0; }
  clear() { this.buffer.fill(0); this.last = 0; }
  process(input, damp, feedback) {
    const out = this.buffer[this.index];
    this.last = undenormalise(out * (1 - damp) + this.last * damp);
    this.buffer[this.index] = undenormalise(input + this.last * feedback);
    if (++this.index === this.buffer.length) this.index = 0;
    return out;
  }
}
class AllPass {
  setSize(size) { this.buffer = new Float32Array(size); this.index = 0; }
  clear() { this.buffer.fill(0); }
  process(input) {
    const buffered = this.buffer[this.index];
    this.buffer[this.index] = undenormalise(input + buffered * 0.5);
    if (++this.index === this.buffer.length) this.index = 0;
    return buffered - input;
  }
}

class Freeverb {
  constructor() {
    this.comb = [Array.from({ length: 8 }, () => new Comb()), Array.from({ length: 8 }, () => new Comb())];
    this.allPass = [Array.from({ length: 4 }, () => new AllPass()), Array.from({ length: 4 }, () => new AllPass())];
    this.damping = new Smoothed(0); this.feedback = new Smoothed(0);
    this.dryGain = new Smoothed(0); this.wetGain1 = new Smoothed(0); this.wetGain2 = new Smoothed(0);
    this.setParameters({ roomSize: 0.5, damping: 0.5, wetLevel: 0.33, dryLevel: 0.4, width: 1 });
    this.setSampleRate(44100);
  }
  setParameters(p) {
    const wet = f32(p.wetLevel * 3);
    this.dryGain.setTarget(f32(p.dryLevel * 2));
    this.wetGain1.setTarget(f32(0.5 * wet * (1 + p.width)));
    this.wetGain2.setTarget(f32(0.5 * wet * (1 - p.width)));
    this.gain = f32(0.015);
    this.damping.setTarget(f32(p.damping * 0.4));
    this.feedback.setTarget(f32(p.roomSize * 0.28 + 0.7));
  }
  setSampleRate(sampleRate) {
    const combTunings = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617], allPassTunings = [556, 441, 341, 225];
    const sr = sampleRate | 0, spread = 23;
    for (let i = 0; i < 8; ++i) {
      this.comb[0][i].setSize(Math.floor((sr * combTunings[i]) / 44100));
      this.comb[1][i].setSize(Math.floor((sr * (combTunings[i] + spread)) / 44100));
    }
    for (let i = 0; i < 4; ++i) {
      this.allPass[0][i].setSize(Math.floor((sr * allPassTunings[i]) / 44100));
      this.allPass[1][i].setSize(Math.floor((sr * (allPassTunings[i] + spread)) / 44100));
    }
    for (const s of [this.damping, this.feedback, this.dryGain, this.wetGain1, this.wetGain2]) s.reset(sampleRate, 0.01);
  }
  reset() {
    for (const ch of this.comb) for (const c of ch) c.clear();
    for (const ch of this.allPass) for (const a of ch) a.clear();
  }
  processStereo(left, right, n) {
    const [cL, cR] = this.comb, [aL, aR] = this.allPass;
    for (let i = 0; i < n; ++i) {
      const input = (left[i] + right[i]) * this.gain;
      const damp = this.damping.next(), fb = this.feedback.next();
      let outL = 0, outR = 0;
      for (let j = 0; j < 8; ++j) { outL += cL[j].process(input, damp, fb); outR += cR[j].process(input, damp, fb); }
      for (let j = 0; j < 4; ++j) { outL = aL[j].process(outL); outR = aR[j].process(outR); }
      const dry = this.dryGain.next(), wet1 = this.wetGain1.next(), wet2 = this.wetGain2.next();
      const l = left[i], r = right[i];
      left[i] = outL * wet1 + outR * wet2 + l * dry;
      right[i] = outR * wet1 + outL * wet2 + r * dry;
    }
  }
}

class ReverbFx {
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.reverb = new Freeverb();
    this.reverb.setSampleRate(this.fs);
    this.wetL = new Float32Array(maxBlock); this.wetR = new Float32Array(maxBlock);
    this.pre = [new DelayLine(), new DelayLine()];
    for (const l of this.pre) l.prepare(Math.floor(0.5 * this.fs) + 8);
    this.send = new Smoothed(0); this.preDelay = new Smoothed(0);
    this.send.reset(this.fs, 0.03); this.preDelay.reset(this.fs, 0.2);
    this.reset();
  }
  reset() {
    this.reverb.reset();
    for (const l of this.pre) l.reset();
    this.preDelay.setCurrentAndTarget(this.preDelay.target);
    this.active = false;
    this.silentSamples = 0;
  }
  setParameters(enabled, size01, damping01, mix01, preDelayMs = 0) {
    const preSamples = f32(clamp(preDelayMs * 0.001 * this.fs, 0, 0.5 * this.fs));
    const starting = enabled && !this.active;
    if (starting) { for (const l of this.pre) l.reset(); this.preDelay.setCurrentAndTarget(preSamples); this.active = true; }
    this.send.setTarget(enabled ? 1 : 0);
    this.preDelay.setTarget(preSamples);
    this.reverb.setParameters({ roomSize: f32(0.3 + 0.68 * size01), damping: damping01, wetLevel: f32(0.35 * mix01), dryLevel: 0, width: 1 });
    if (starting) this.reverb.setSampleRate(this.fs);
  }
  process(left, right, n) {
    if (!this.active) return;
    const wl = this.wetL, wr = this.wetR;
    for (let i = 0; i < n; ++i) {
      const s = this.send.next(), d = this.preDelay.next() + 1;
      this.pre[0].push(left[i] * s); this.pre[1].push(right[i] * s);
      wl[i] = this.pre[0].read(d); wr[i] = this.pre[1].read(d);
    }
    this.reverb.processStereo(wl, wr, n);
    let peak = 0;
    for (let i = 0; i < n; ++i) {
      left[i] += wl[i]; right[i] += wr[i];
      peak = Math.max(peak, Math.abs(wl[i]), Math.abs(wr[i]));
    }
    if (this.send.target === 0 && !this.send.isSmoothing()) {
      this.silentSamples = peak < 1e-5 ? this.silentSamples + n : 0;
      if (this.silentSamples > Math.floor(0.8 * this.fs)) this.reset();
    } else this.silentSamples = 0;
  }
}

// ============================================================================
// Models (Source/DSP/Models.h): the board is an HD500X-style chain of eight FX slots plus the amp/cab block
const NUM_SLOTS = 8;
const MAX_KNOBS = 8;
const CATEGORY = { none: 0, dynamics: 1, distortion: 2, modulation: 3, delay: 4, reverb: 5 };
const CATEGORY_NAMES = ["Empty", "Dynamics", "Distortion", "Modulation", "Delay", "Reverb"];
const ENGINE = { none: 0, gate: 1, distortion: 2, modulation: 3, delay: 4, reverb: 5 };
const UNIT = { knob: 0, percent: 1, db: 2, ms: 3, hz: 4, choice: 5 };

const knobSpec = (name, min, max, def, centre, step, unit, choices) => ({ name, min, max, def, centre, step, unit, choices: choices || [] });
const percent = (name, def, max = 100) => knobSpec(name, 0, max, def, 0, 1, UNIT.percent);
const decibels = (name, min, max, def, step = 0.1) => knobSpec(name, min, max, def, 0, step, UNIT.db);
const millis = (name, min, max, def, centre) => knobSpec(name, min, max, def, centre, 1, UNIT.ms);
const hertz = (name, min, max, def, centre) => knobSpec(name, min, max, def, centre, 0.01, UNIT.hz);
const choice = (name, choices, def) => knobSpec(name, 0, choices.length - 1, def, 0, 1, UNIT.choice, choices);

const DELAY_NOTE_NAMES = ["ms", "1/4", "1/8.", "1/8", "1/8T", "1/16"];
const DELAY_NOTE_BEATS = [0, 1, 0.75, 0.5, 1 / 3, 0.25];
const REVERB_NOTE_NAMES = ["ms", "1/32", "1/16", "1/8", "1/4"];
const REVERB_NOTE_BEATS = [0, 0.125, 0.25, 0.5, 1];

/** Every model, in a fixed order (append only). Same list as the plugin; compare.mjs checks it. */
const MODELS = (() => {
  const m = [];
  const add = (key, name, category, engine, variant, basedOn, knobs, extra = {}) =>
    m.push({ key, name, category, engine, variant, basedOn, knobs, timeKnob: -1, noteKnob: -1, noteBeats: null, stereo: false, trails: false, ...extra });
  add("empty", "Empty", CATEGORY.none, ENGINE.none, 0, "", []);
  add("noise_gate", "Noise Gate", CATEGORY.dynamics, ENGINE.gate, 0, "Noise suppressor with hysteresis",
    [decibels("Threshold", -90, -20, -65, 0.5), millis("Decay", 5, 500, 60, 80)]);

  const drive = (key, name, variant, basedOn, third, driveDefault) =>
    add(key, name, CATEGORY.distortion, ENGINE.distortion, variant, basedOn,
      [percent("Drive", driveDefault), percent("Bass", 50), percent(third, 50), percent("Treble", 50), decibels("Output", -30, 12, 0)]);
  drive("tube_drive", "Tube Drive", 0, "Chandler Tube Driver", "Mid", 50);
  drive("screamer", "Screamer", 1, "Ibanez TS808 Tube Screamer", "Tone", 50);
  drive("overdrive", "Overdrive", 2, "DOD Overdrive/Preamp 250", "Mid", 50);
  drive("classic_dist", "Classic Dist", 3, "Pro Co RAT", "Filter", 50);
  drive("heavy_dist", "Heavy Dist", 4, "BOSS MT-2 Metal Zone", "Mid", 60);
  drive("color_drive", "Color Drive", 5, "Colorsound Overdriver", "Mid", 50);
  drive("buzz_saw", "Buzz Saw", 6, "Maestro Fuzz-Tone FZ-1", "Mid", 60);
  drive("facial_fuzz", "Facial Fuzz", 7, "Arbiter Fuzz Face", "Mid", 60);
  drive("jumbo_fuzz", "Jumbo Fuzz", 8, "Vox Tone Bender", "Mid", 60);
  drive("fuzz_pi", "Fuzz Pi", 9, "Electro-Harmonix Big Muff Pi", "Mid", 60);
  add("jet_fuzz", "Jet Fuzz", CATEGORY.distortion, ENGINE.distortion, 10, "Roland AP-7 Jet Phaser",
    [percent("Drive", 60), percent("Fdbk", 50), percent("Tone", 50), hertz("Speed", 0.05, 8, 0.4, 1), decibels("Output", -30, 12, 0)]);
  drive("line6_drive", "Line 6 Drive", 11, "Line 6 original: Mid morphs '70s fuzz > modern high gain > Tone Bender grit", "Mid", 50);
  drive("line6_dist", "Line 6 Distortion", 12, "Line 6 original: massive, over-the-top gain", "Mid", 60);
  drive("sub_oct_fuzz", "Sub Octave Fuzz", 13, "PAiA Roctave Divider", "Sub", 60);
  drive("octave_fuzz", "Octave Fuzz", 14, "Tycobrahe Octavia", "Mid", 60);

  const mod = (key, name, variant, basedOn, third) =>
    add(key, name, CATEGORY.modulation, ENGINE.modulation, variant, basedOn,
      [hertz("Speed", 0.05, 10, 0.8, 1), percent("Depth", 50), percent(third, 50)], { stereo: true });
  mod("chorus", "Chorus", 0, "Stereo chorus", "Mix");
  mod("flanger", "Flanger", 1, "Stereo flanger", "Mix");
  mod("phaser", "Phaser", 2, "6-stage phaser", "Mix");
  mod("tremolo", "Tremolo", 3, "Tremolo, sine to square", "Shape");

  add("analog_delay", "Analog Delay", CATEGORY.delay, ENGINE.delay, 0, "Stereo delay, darker repeats",
    [millis("Time", 20, 2000, 500, 400), choice("Note", DELAY_NOTE_NAMES, 1), percent("Feedback", 35, 95), percent("Mix", 35),
     knobSpec("Tone", 0, 10, 6, 0, 0.1, UNIT.knob)],
    { timeKnob: 0, noteKnob: 1, noteBeats: DELAY_NOTE_BEATS, stereo: true, trails: true });
  add("room_reverb", "Room Reverb", CATEGORY.reverb, ENGINE.reverb, 0, "Freeverb room / hall",
    [percent("Size", 55), percent("Damp", 45), percent("Mix", 25), millis("Pre-Delay", 0, 500, 0, 120), choice("Note", REVERB_NOTE_NAMES, 0)],
    { timeKnob: 3, noteKnob: 4, noteBeats: REVERB_NOTE_BEATS, stereo: true, trails: true });
  return m;
})();
const MODEL_INDEX = new Map(MODELS.map((m, i) => [m.key, i]));
const modelIndex = (key) => (typeof key === "number" ? clamp(key | 0, 0, MODELS.length - 1) : MODEL_INDEX.get(key) ?? 0);
const modelInfo = (key) => MODELS[modelIndex(key)];

// knob travel (0..1) <-> value, juce::NormalisableRange style
function knobSkew(spec) { return spec.centre > 0 ? Math.log(0.5) / Math.log((spec.centre - spec.min) / (spec.max - spec.min)) : 1; }
function knobFromNorm(spec, n) {
  let p = clamp(n, 0, 1);
  if (spec.centre > 0 && p > 0) p = Math.exp(Math.log(p) / knobSkew(spec));
  let v = spec.min + (spec.max - spec.min) * p;
  if (spec.step > 0) v = spec.min + spec.step * Math.round((v - spec.min) / spec.step);
  return clamp(v, spec.min, spec.max);
}
function knobToNorm(spec, v) {
  const p = clamp((v - spec.min) / (spec.max - spec.min), 0, 1);
  return spec.centre > 0 ? Math.pow(p, knobSkew(spec)) : p;
}
function knobText(spec, v) {
  switch (spec.unit) {
    case UNIT.percent: return `${Math.round(v)} %`;
    case UNIT.db: return `${v.toFixed(1)} dB`;
    case UNIT.ms: return `${Math.round(v)} ms`;
    case UNIT.hz: return `${v.toFixed(2)} Hz`;
    case UNIT.choice: return spec.choices[clamp(Math.round(v), 0, spec.choices.length - 1)];
    default: return v.toFixed(1);
  }
}

/** A slot holding `key` with every knob at its default. */
function makeSlot(key, on) {
  const knobs = new Array(MAX_KNOBS).fill(0);
  modelInfo(key).knobs.forEach((k, i) => (knobs[i] = k.def));
  return { on, model: MODELS[modelIndex(key)].key, knobs };
}
/** Tempo sync: when the note knob is not "ms", the time knob follows the tempo. */
function resolveTempo(slot, bpm) {
  const m = modelInfo(slot.model);
  if (m.timeKnob < 0 || m.noteKnob < 0 || !(bpm > 0)) return slot;
  const note = Math.round(slot.knobs[m.noteKnob]);
  if (note > 0) {
    const spec = m.knobs[m.timeKnob];
    slot.knobs[m.timeKnob] = f32(clamp((60000 / bpm) * m.noteBeats[note], spec.min, spec.max));
  }
  return slot;
}

/** The board as it first opens: Noise Gate > Screamer > cab > Chorus (off) > Analog Delay (off) > Room Reverb. */
function defaultBoard() {
  const slots = [makeSlot("noise_gate", true), makeSlot("screamer", true), makeSlot("chorus", false),
                 makeSlot("analog_delay", false), makeSlot("room_reverb", true), makeSlot("empty", false),
                 makeSlot("empty", false), makeSlot("empty", false)];
  return { inputGainDb: 0, outputGainDb: 0, mute: false, cabOn: true, ampPosition: 2, eqOn: true, eq: defaultEq(), slots };
}
const defaultParams = defaultBoard;

class Fade {
  constructor(fs) { this.amount = new Smoothed(0); this.amount.reset(fs, 0.03); }
  set(on) { this.amount.setTarget(on ? 1 : 0); }
  isOff() { return !this.amount.isSmoothing() && this.amount.current <= 0; }
  isFullyOn() { return !this.amount.isSmoothing() && this.amount.current >= 1; }
}

/** One FX slot: an instance of every engine; picking another model fades the old one out, then the new one in. */
class Slot {
  prepare(sampleRate, maxBlock) {
    this.gate = new NoiseGate(); this.gate.prepare(sampleRate);
    this.distortion = new Distortion(); this.distortion.prepare(sampleRate, maxBlock);
    this.modulation = new Modulation(); this.modulation.prepare(sampleRate);
    this.delay = new Delay(); this.delay.prepare(sampleRate);
    this.reverb = new ReverbFx(); this.reverb.prepare(sampleRate, maxBlock);
    this.fade = new Fade(sampleRate);
    this.active = this.requested = 0;
    this.on = false; this.needsReset = false;
    this.knobs = new Float32Array(MAX_KNOBS); this.pendingKnobs = new Float32Array(MAX_KNOBS);
    this.reset();
  }
  reset() {
    for (const e of [this.gate, this.distortion, this.modulation, this.delay, this.reverb]) e.reset();
    if (this.active !== this.requested) this.knobs.set(this.pendingKnobs);
    this.active = this.requested;
    this.needsReset = false;
    this.configure(); this.resetEngine(); this.configure();
    const info = MODELS[this.active];
    this.fade.amount.setCurrentAndTarget(info.engine !== ENGINE.none && (this.on || info.trails) ? 1 : 0);
  }
  setParameters(p) {
    this.requested = modelIndex(p.model);
    this.on = !!p.on;
    if (this.requested === this.active) this.knobs.set(p.knobs); else this.pendingKnobs.set(p.knobs);
    if (MODELS[this.active].engine === ENGINE.none) this.fade.amount.setCurrentAndTarget(0);
    if (this.requested !== this.active && this.fade.isOff()) this.start();
    this.configure();
    const info = MODELS[this.active];
    this.fade.set(this.requested === this.active && info.engine !== ENGINE.none && (this.on || info.trails));
  }
  start() {
    this.active = this.requested;
    this.knobs.set(this.pendingKnobs);
    this.needsReset = false;
    this.configure(); this.resetEngine(); this.configure();
  }
  configure() {
    const info = MODELS[this.active], k = this.knobs, enabled = this.on && this.active === this.requested;
    switch (info.engine) {
      case ENGINE.gate: this.gate.setParameters(k[0], k[1]); break;
      case ENGINE.distortion: this.distortion.setModel(info.variant); this.distortion.setParameters(k); break;
      case ENGINE.modulation: this.modulation.setParameters(info.variant, k[0], f32(k[1] / 100), f32(k[2] / 100)); break;
      case ENGINE.delay: this.delay.setParameters(enabled, k[0], f32(k[2] / 100), f32(k[3] / 100), f32(k[4] / 10)); break;
      case ENGINE.reverb: this.reverb.setParameters(enabled, f32(k[0] / 100), f32(k[1] / 100), f32(k[2] / 100), k[3]); break;
      default: break;
    }
  }
  resetEngine() {
    switch (MODELS[this.active].engine) {
      case ENGINE.gate: this.gate.reset(); break;
      case ENGINE.distortion: this.distortion.reset(); break;
      case ENGINE.modulation: this.modulation.restart(); break;
      case ENGINE.delay: this.delay.reset(); break;
      case ENGINE.reverb: this.reverb.reset(); break;
      default: break;
    }
  }
  process(left, right, n, scratch) {
    const info = MODELS[this.active];
    // the chorus / flanger lines always hold the slot's recent input (see the C++)
    if (info.engine !== ENGINE.modulation || this.fade.isOff()) this.modulation.feed(left, right, n);
    if (info.engine === ENGINE.none) return;
    if (this.fade.isOff()) { this.needsReset = true; return; }
    if (this.needsReset) { if (info.engine !== ENGINE.modulation) this.resetEngine(); this.needsReset = false; }

    const full = this.fade.isFullyOn(), dl = scratch.dryLeft, dr = scratch.dryRight;
    if (!full) { dl.set(left.subarray(0, n)); dr.set(right.subarray(0, n)); }
    if (info.stereo) {
      if (info.engine === ENGINE.modulation) this.modulation.process(left, right, n);
      else if (info.engine === ENGINE.delay) this.delay.process(left, right, n);
      else this.reverb.process(left, right, n);
    } else {
      const m = scratch.mid;
      for (let i = 0; i < n; ++i) m[i] = 0.5 * (left[i] + right[i]);
      if (info.engine === ENGINE.gate) this.gate.process(m, n); else this.distortion.process(m, n);
      for (let i = 0; i < n; ++i) left[i] = right[i] = m[i];
    }
    if (!full) {
      const a = this.fade.amount;
      for (let i = 0; i < n; ++i) {
        const g = a.next();
        left[i] = dl[i] + g * (left[i] - dl[i]);
        right[i] = dr[i] + g * (right[i] - dr[i]);
      }
    }
  }
}

/** input -> 8 slots, with the amp/cab block between them -> global EQ -> output. Mono in, stereo out. */
class FxChain {
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.maxBlock = Math.max(1, maxBlock);
    this.slots = Array.from({ length: NUM_SLOTS }, () => { const s = new Slot(); s.prepare(sampleRate, this.maxBlock); return s; });
    this.ampPosition = this.requestedAmpPosition = 2;
    this.cabWanted = true;
    this.cab = new CabSim(); this.cab.prepare(sampleRate);
    this.eqLeft = new Equalizer(); this.eqLeft.prepare(sampleRate);
    this.eqRight = new Equalizer(); this.eqRight.prepare(sampleRate);
    this.cabFade = new Fade(sampleRate); this.eqFade = new Fade(sampleRate);
    this.cabNeedsReset = this.eqNeedsReset = true;
    this.inputGain = new Smoothed(1); this.outputGain = new Smoothed(1);
    this.inputGain.reset(sampleRate, 0.05); this.outputGain.reset(sampleRate, 0.05);
    this.mono = new Float32Array(this.maxBlock);
    this.scratch = { dryLeft: new Float32Array(this.maxBlock), dryRight: new Float32Array(this.maxBlock), mid: new Float32Array(this.maxBlock) };
    this.spareR = new Float32Array(this.maxBlock);
    this.analyzerTap = null;
    this.reset();
  }
  reset() {
    for (const s of this.slots) s.reset();
    this.cab.reset(); this.eqLeft.reset(); this.eqRight.reset();
    this.cabNeedsReset = this.eqNeedsReset = false;
    this.ampPosition = this.requestedAmpPosition;
    this.cabFade.amount.setCurrentAndTarget(this.cabWanted ? 1 : 0);
    this.eqFade.amount.setCurrentAndTarget(this.eqFade.amount.target);
    this.inputGain.setCurrentAndTarget(this.inputGain.target);
    this.outputGain.setCurrentAndTarget(this.outputGain.target);
    this.inputPeak = this.outputPeak = 0;
  }
  setParameters(p) {
    this.inputGain.setTarget(f32(dbToGain(p.inputGainDb)));
    this.outputGain.setTarget(p.mute ? 0 : f32(dbToGain(p.outputGainDb)));
    for (let s = 0; s < NUM_SLOTS; ++s) this.slots[s].setParameters(p.slots[s]);
    this.requestedAmpPosition = clamp(p.ampPosition | 0, 0, NUM_SLOTS);
    this.cabWanted = !!p.cabOn;
    if (this.requestedAmpPosition !== this.ampPosition && this.cabFade.isOff()) this.ampPosition = this.requestedAmpPosition;
    this.cabFade.set(this.cabWanted && this.requestedAmpPosition === this.ampPosition);
    this.eqFade.set(p.eqOn);
    this.eqLeft.setParameters(p.eq);
    this.eqRight.setParameters(p.eq);
  }
  getActiveModel(slot) { return MODELS[this.slots[slot].active].key; }
  /** Mono in, stereo out. outRight may be null for a mono mix. */
  process(input, outLeft, outRight, n) {
    this.inputPeak = this.outputPeak = 0;
    for (let offset = 0; offset < n; offset += this.maxBlock) {
      const len = Math.min(this.maxBlock, n - offset);
      const L = outLeft.subarray(offset, offset + len);
      const R = outRight ? outRight.subarray(offset, offset + len) : this.spareR.subarray(0, len);
      this.processChunk(input.subarray(offset, offset + len), L, R, len);
      if (!outRight) for (let i = 0; i < len; ++i) L[i] = 0.5 * (L[i] + R[i]);
    }
  }
  processChunk(input, left, right, n) {
    const m = this.mono;
    for (let i = 0; i < n; ++i) {
      m[i] = input[i] * this.inputGain.next();
      this.inputPeak = Math.max(this.inputPeak, Math.abs(m[i]));
    }
    for (let i = 0; i < n; ++i) left[i] = right[i] = m[i];
    for (let s = 0; s < NUM_SLOTS; ++s) {
      if (s === this.ampPosition) this.runCab(left, right, n);
      this.slots[s].process(left, right, n, this.scratch);
    }
    if (this.ampPosition >= NUM_SLOTS) this.runCab(left, right, n);
    this.runEq(left, right, n);
    if (this.analyzerTap) {
      const mid = this.scratch.mid;
      for (let i = 0; i < n; ++i) mid[i] = 0.5 * (left[i] + right[i]);
      this.analyzerTap(mid, n);
    }
    for (let i = 0; i < n; ++i) {
      const g = this.outputGain.next();
      left[i] = clamp(left[i] * g, -2, 2);
      right[i] = clamp(right[i] * g, -2, 2);
      this.outputPeak = Math.max(this.outputPeak, Math.abs(left[i]), Math.abs(right[i]));
    }
  }
  runCab(left, right, n) {
    if (this.cabFade.isOff()) { this.cabNeedsReset = true; return; }
    if (this.cabNeedsReset) { this.cab.reset(); this.cabNeedsReset = false; }
    const m = this.scratch.mid;
    for (let i = 0; i < n; ++i) m[i] = 0.5 * (left[i] + right[i]);
    this.cab.process(m, n);
    if (this.cabFade.isFullyOn()) { for (let i = 0; i < n; ++i) left[i] = right[i] = m[i]; return; }
    for (let i = 0; i < n; ++i) {
      const a = this.cabFade.amount.next();
      left[i] += a * (m[i] - left[i]);
      right[i] += a * (m[i] - right[i]);
    }
  }
  runEq(left, right, n) {
    if (this.eqFade.isOff()) { this.eqNeedsReset = true; return; }
    if (this.eqNeedsReset) { this.eqLeft.reset(); this.eqRight.reset(); this.eqNeedsReset = false; }
    if (this.eqFade.isFullyOn()) { this.eqLeft.process(left, n); this.eqRight.process(right, n); return; }
    const dl = this.scratch.dryLeft, dr = this.scratch.dryRight;
    dl.set(left.subarray(0, n)); dr.set(right.subarray(0, n));
    this.eqLeft.process(left, n); this.eqRight.process(right, n);
    for (let i = 0; i < n; ++i) {
      const a = this.eqFade.amount.next();
      left[i] = dl[i] + a * (left[i] - dl[i]);
      right[i] = dr[i] + a * (right[i] - dr[i]);
    }
  }
}

// ============================================================================
// Tuner: YIN on input decimated to ~24 kHz
class PitchDetector {
  prepare(inputRate) {
    this.decimation = Math.max(1, Math.round(inputRate / 24000));
    this.rate = inputRate / this.decimation;
    this.maxTau = Math.ceil(this.rate / 50) + 2;
    this.minTau = Math.max(2, Math.floor(this.rate / 1500));
    this.window = Math.ceil(this.rate * 0.04);
    this.ring = new Float32Array(this.window + this.maxTau + 2);
    this.frame = new Float32Array(this.ring.length);
    this.diff = new Float64Array(this.maxTau + 2);
    this.cmnd = new Float64Array(this.maxTau + 2);
    this.writePos = 0; this.acc = 0; this.accCount = 0;
  }
  pushSamples(data, n = data.length) {
    for (let i = 0; i < n; ++i) {
      this.acc += data[i];
      if (++this.accCount === this.decimation) {
        this.ring[this.writePos] = this.acc / this.decimation;
        this.writePos = (this.writePos + 1) % this.ring.length;
        this.acc = 0; this.accCount = 0;
      }
    }
  }
  detect() {
    const size = this.ring.length, x = this.frame;
    for (let i = 0; i < size; ++i) x[i] = this.ring[(this.writePos + i) % size];
    let energy = 0;
    for (let j = 0; j < size; ++j) energy += x[j] * x[j];
    if (Math.sqrt(energy / size) < 0.002) return 0;
    const { diff, cmnd, maxTau, minTau, window } = this;
    for (let tau = 1; tau <= maxTau; ++tau) {
      let sum = 0;
      for (let j = 0; j < window; ++j) { const d = x[j] - x[j + tau]; sum += d * d; }
      diff[tau] = sum;
    }
    let running = 0;
    cmnd[0] = 1;
    for (let tau = 1; tau <= maxTau; ++tau) { running += diff[tau]; cmnd[tau] = running > 0 ? (diff[tau] * tau) / running : 1; }
    let tau = -1;
    for (let t = minTau; t < maxTau; ++t) {
      if (cmnd[t] < 0.15) { while (t + 1 < maxTau && cmnd[t + 1] < cmnd[t]) ++t; tau = t; break; }
    }
    if (tau < 1) return 0;
    const a = diff[tau - 1], b = diff[tau], c = diff[tau + 1], denom = a + c - 2 * b;
    const shift = Math.abs(denom) > 1e-12 ? clamp((0.5 * (a - c)) / denom, -1, 1) : 0;
    return this.rate / (tau + shift);
  }
}

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
function frequencyToNote(hz, a4 = 440) {
  const midi = 69 + 12 * Math.log2(hz / a4), nearest = Math.round(midi);
  return { midi: nearest, cents: (midi - nearest) * 100, name: NOTE_NAMES[((nearest % 12) + 12) % 12], octave: Math.floor(nearest / 12) - 1 };
}

// ============================================================================
// FFT analyser -> 240 log-spaced columns (20 Hz .. 20 kHz) in dBFS
class SpectrumAnalyzer {
  static fftSize = 4096;
  static numColumns = 240;
  static minHz = 20;
  static maxHz = 20000;
  static floorDb = -120;
  static columnFrequency(c) {
    const S = SpectrumAnalyzer;
    return S.minHz * Math.pow(S.maxHz / S.minHz, clamp(c / (S.numColumns - 1), 0, 1));
  }
  constructor() {
    const N = SpectrumAnalyzer.fftSize;
    this.fs = 48000;
    this.ring = new Float32Array(N); this.ringPos = 0; this.newSamples = 0;
    this.re = new Float64Array(N); this.im = new Float64Array(N); this.mag = new Float64Array(N / 2 + 1);
    this.window = new Float64Array(N);
    for (let i = 0; i < N; ++i) this.window[i] = 0.5 - 0.5 * Math.cos((2 * PI * i) / (N - 1));
    this.columns = new Float32Array(SpectrumAnalyzer.numColumns).fill(SpectrumAnalyzer.floorDb);
    // bit reversal table
    this.rev = new Uint32Array(N);
    const bits = Math.log2(N);
    for (let i = 0; i < N; ++i) { let r = 0; for (let b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b); this.rev[i] = r; }
  }
  setSampleRate(fs) { if (fs > 0) this.fs = fs; }
  clear() { this.columns.fill(SpectrumAnalyzer.floorDb); }
  push(data, n = data.length) {
    const N = SpectrumAnalyzer.fftSize;
    for (let i = 0; i < n; ++i) { this.ring[this.ringPos] = data[i]; this.ringPos = (this.ringPos + 1) % N; }
    this.newSamples += n;
  }
  fft() {
    const N = SpectrumAnalyzer.fftSize, re = this.re, im = this.im;
    for (let i = 0; i < N; ++i) { const j = this.rev[i]; if (j > i) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (let size = 2; size <= N; size <<= 1) {
      const half = size >> 1, step = (-2 * PI) / size;
      for (let start = 0; start < N; start += size) {
        for (let k = 0; k < half; ++k) {
          const wr = Math.cos(step * k), wi = Math.sin(step * k);
          const a = start + k, b = a + half;
          const tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
        }
      }
    }
  }
  process() {
    const S = SpectrumAnalyzer, N = S.fftSize, fall = 1.6;
    if (this.newSamples === 0) { for (let c = 0; c < S.numColumns; ++c) this.columns[c] = Math.max(S.floorDb, this.columns[c] - fall); return; }
    this.newSamples = 0;
    for (let i = 0; i < N; ++i) { this.re[i] = this.ring[(this.ringPos + i) % N] * this.window[i]; this.im[i] = 0; }
    this.fft();
    for (let k = 0; k <= N / 2; ++k) this.mag[k] = Math.hypot(this.re[k], this.im[k]);
    const norm = 4 / N, maxBin = N / 2;
    for (let c = 0; c < S.numColumns; ++c) {
      const lo = (S.columnFrequency(c - 0.5) * N) / this.fs, hi = (S.columnFrequency(c + 0.5) * N) / this.fs;
      let m = 0;
      if (hi - lo < 1) {
        const bin = clamp(0.5 * (lo + hi), 0, maxBin - 1), i0 = Math.floor(bin), t = bin - i0;
        m = this.mag[i0] + t * (this.mag[i0 + 1] - this.mag[i0]);
      } else {
        for (let b = Math.max(0, Math.ceil(lo)); b <= Math.min(maxBin, Math.floor(hi)); ++b) m = Math.max(m, this.mag[b]);
      }
      const db = Math.max(S.floorDb, gainToDb(m * norm));
      this.columns[c] = db > this.columns[c] ? db : Math.max(db, this.columns[c] - fall);
    }
  }
}

// ============================================================================
// Tap tempo

class TapTempo {
  constructor() { this.intervals = [0, 0, 0, 0]; this.count = 0; this.next = 0; this.lastTap = 0; this.hasLastTap = false; }
  average() {
    if (this.count === 0) return 0;
    let sum = 0;
    for (let i = 0; i < this.count; ++i) sum += this.intervals[(this.next - 1 - i + 16) % 4];
    return sum / this.count;
  }
  /** Returns the averaged beat length in ms, or 0 until there are two taps. */
  tap(nowMs) {
    const interval = nowMs - this.lastTap;
    if (this.hasLastTap && interval < 100) return this.average();
    if (!this.hasLastTap || interval > 2000) this.count = 0;
    else {
      if (this.count > 0 && Math.abs(interval - this.average()) > 0.35 * this.average()) this.count = 0;
      this.intervals[this.next++ % 4] = interval;
      this.count = Math.min(this.count + 1, 4);
    }
    this.hasLastTap = true;
    this.lastTap = nowMs;
    return this.average();
  }
  getLastTapMs() { return this.hasLastTap ? this.lastTap : 0; }
}

// ============================================================================
// Test signals: Karplus-Strong strings and the demo riff
const OPEN_STRINGS = [82.41, 110.0, 146.83, 196.0, 246.94, 329.63];
const fretHz = (string, fret) => OPEN_STRINGS[string] * Math.pow(2, fret / 12);

/** How CHORD buttons play. */
const CHORD_PATTERNS = { arpeggio: 0, strumLoop: 1, single: 2 };
const CHORD_PATTERN_NAMES = ["Arpeggio", "Strum loop", "Single"];

/** Open-position chord shapes, frets from low E to high e; -1 = string not played (muted). */
const CHORDS = [
  { name: "C", shape: "x32010", frets: [-1, 3, 2, 0, 1, 0] },
  { name: "D", shape: "xx0232", frets: [-1, -1, 0, 2, 3, 2] },
  { name: "E", shape: "022100", frets: [0, 2, 2, 1, 0, 0] },
  { name: "F", shape: "133211", frets: [1, 3, 3, 2, 1, 1] },
  { name: "G", shape: "320003", frets: [3, 2, 0, 0, 0, 3] },
  { name: "A", shape: "x02220", frets: [-1, 0, 2, 2, 2, 0] },
  { name: "Am", shape: "x02210", frets: [-1, 0, 2, 2, 1, 0] },
  { name: "Dm", shape: "xx0231", frets: [-1, -1, 0, 2, 3, 1] },
  { name: "Em", shape: "022000", frets: [0, 2, 2, 0, 0, 0] },
];

/** Small deterministic PRNG (mulberry32) so the demo riff is the same every time. */
function makeRng(seed) {
  let a = seed >>> 0;
  return {
    nextFloat() {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    },
  };
}

class PluckedString {
  prepare(sampleRate, lowestHz = 40) { this.fs = sampleRate; this.buffer = new Float32Array(Math.floor(this.fs / lowestHz) + 4); this.active = false; }
  pluck(hz, velocity, t60, brightness, rng) {
    const period = this.fs / hz;
    this.length = clamp(Math.floor(period - 0.6), 2, this.buffer.length);
    const fraction = period - 0.5 - this.length;
    this.apCoef = (1 - fraction) / (1 + fraction);
    this.loopGain = Math.pow(10, (-3 * period) / (this.fs * t60));
    const pick = new OnePole();
    pick.setCutoff(this.fs, 700 + 9000 * brightness);
    let mean = 0;
    for (let i = 0; i < this.length; ++i) { this.buffer[i] = pick.lowPass(rng.nextFloat() * 2 - 1); mean += this.buffer[i]; }
    mean /= this.length;
    let peak = 1e-6;
    for (let i = 0; i < this.length; ++i) { this.buffer[i] -= mean; peak = Math.max(peak, Math.abs(this.buffer[i])); }
    for (let i = 0; i < this.length; ++i) this.buffer[i] *= velocity / peak;
    this.pos = 0; this.previous = this.apIn = this.apOut = 0;
    this.remaining = Math.floor(this.fs * t60 * 1.2);
    this.active = true;
  }
  /** Mutes a ringing string over ~60 ms, like resting a finger on it (no click). */
  damp() {
    if (!this.active) return;
    this.loopGain = Math.pow(10, (-3 * this.length) / (this.fs * 0.06));
    this.remaining = Math.min(this.remaining, Math.floor(this.fs * 0.1));
  }
  next() {
    if (!this.active) return 0;
    const out = this.buffer[this.pos];
    const averaged = 0.5 * (out + this.previous) * this.loopGain;
    this.previous = out;
    const y = this.apCoef * averaged + this.apIn - this.apCoef * this.apOut;
    this.apIn = averaged; this.apOut = y;
    this.buffer[this.pos] = y;
    if (++this.pos >= this.length) this.pos = 0;
    if (--this.remaining <= 0) this.active = false;
    return out;
  }
}

/** ~10.8 s riff at 100 BPM; loops seamlessly; peaks at -8 dBFS like a DI'd guitar. */
function renderDemoRiff(fs) {
  const eighth = 0.3, loopSeconds = 32 * eighth + 1.2;
  const notes = [];
  const chord = (at, hz, muted, length, velocity = 0.33) => {
    hz.forEach((f, string) => notes.push({
      at: at * eighth + 0.009 * string, hz: f, velocity: velocity * (1 - 0.04 * string),
      t60: muted ? 0.16 : 3.5, brightness: muted ? 0.35 : 0.8, length: muted ? eighth : length * eighth,
    }));
  };
  const e5 = [82.41, 123.47, 164.81], g5 = [98.0, 146.83, 196.0], a5 = [110.0, 164.81, 220.0], d5 = [146.83, 220.0, 293.66];
  const eMajor = [82.41, 123.47, 164.81, 207.65, 246.94, 329.63];
  for (const t of [0, 1, 2, 3]) chord(t, e5, true, 1);
  chord(4, e5, false, 2); chord(6, g5, false, 2);
  for (const t of [8, 9, 10]) chord(t, e5, true, 1);
  chord(11, a5, false, 2); chord(13, g5, false, 1); chord(14, d5, false, 2);
  const lick = [329.63, 392.0, 440.0, 493.88, 440.0, 392.0, 329.63, 293.66];
  lick.forEach((hz, i) => notes.push({ at: (16 + i) * eighth, hz, velocity: 0.38, t60: 2.5, brightness: 0.85, length: (i === 6 ? 1.5 : 1) * eighth }));
  chord(24, eMajor, false, 12, 0.3);

  const loopLength = Math.floor(loopSeconds * fs);
  const out = new Float32Array(loopLength + Math.floor(6 * fs));
  const fade = Math.floor(0.025 * fs);
  const rng = makeRng(2024), string = new PluckedString();
  string.prepare(fs);
  for (const n of notes) {
    string.pluck(n.hz, n.velocity, n.t60, n.brightness, rng);
    const start = Math.floor(n.at * fs), held = Math.floor(n.length * fs);
    for (let i = 0; i < held + fade && start + i < out.length && string.active; ++i) {
      const env = i < held ? 1 : 1 - (i - held) / fade;
      out[start + i] += env * string.next();
    }
  }
  for (let i = loopLength; i < out.length; ++i) out[i - loopLength] += out[i];
  const riff = out.slice(0, loopLength);
  let peak = 1e-6;
  for (const s of riff) peak = Math.max(peak, Math.abs(s));
  for (let i = 0; i < riff.length; ++i) riff[i] *= 0.4 / peak;
  return riff;
}

/** What feeds the board: live input, the demo riff or a looped file, plus plucked open strings. */
class TestSignalPlayer {
  prepare(fs) {
    this.fs = fs;
    this.source = "live";
    this.demo = null; this.demoPos = 0;
    this.file = null; this.fileRate = fs; this.filePos = 0;
    this.voices = OPEN_STRINGS.map(() => {
      const string = new PluckedString();
      string.prepare(fs);
      return { string, pendingDelay: -1, pendingHz: 0, pendingVelocity: 0 };
    });
    this.rng = makeRng(7);
    this.progress = 0;
    this.playing = true;
    // chord loop
    this.chordPattern = CHORD_PATTERNS.arpeggio;
    this.chordTempo = 90;
    this.loopChord = -1; this.nextLoopChord = -1; this.loopStep = 0; this.samplesToStep = 0;
  }

  setChordPattern(p) { this.chordPattern = clamp(p | 0, 0, 2); if (this.chordPattern === CHORD_PATTERNS.single) this.loopChord = this.nextLoopChord = -1; }
  setChordTempo(bpm) { this.chordTempo = clamp(bpm, 40, 240); }
  /** Plays a chord with the current pattern; while looping, the new chord takes over on the next eighth note. */
  playChord(i) {
    if (!CHORDS[i]) return;
    if (this.chordPattern === CHORD_PATTERNS.single) { this.strum(i); return; }
    if (this.loopChord < 0) { this.loopChord = i; this.loopStep = 0; this.samplesToStep = 0; }
    else this.nextLoopChord = i;
  }
  /** Stops the chord loop and mutes the strings. */
  stopChord() {
    this.loopChord = this.nextLoopChord = -1;
    for (const v of this.voices) { v.pendingDelay = -1; v.string.damp(); }
  }
  loopingChord() { return this.nextLoopChord >= 0 ? this.nextLoopChord : this.loopChord; }

  runChordLoop(n) {
    if (this.loopChord < 0) return;
    const stepLength = Math.max(1, Math.round((this.fs * 30) / this.chordTempo)); // eighth notes
    while (this.samplesToStep < n) {
      if (this.nextLoopChord >= 0) { this.loopChord = this.nextLoopChord; this.nextLoopChord = -1; this.loopStep = 0; }
      this.playStep(CHORDS[this.loopChord], this.loopStep, this.samplesToStep);
      this.loopStep = (this.loopStep + 1) % 8;
      this.samplesToStep += stepLength;
    }
    this.samplesToStep -= n;
  }

  /** One eighth note of the pattern, starting `offset` samples into this block. */
  playStep(chord, step, offset) {
    const played = [];
    chord.frets.forEach((f, s) => { if (f >= 0) played.push(s); });
    const pluckAt = (s, delay, velocity) => {
      const v = this.voices[s];
      v.pendingDelay = delay; v.pendingHz = fretHz(s, chord.frets[s]); v.pendingVelocity = velocity;
    };
    const muteUnplayed = () => chord.frets.forEach((f, s) => { if (f < 0) this.voices[s].string.damp(); });

    if (this.chordPattern === CHORD_PATTERNS.arpeggio) {
      // bass, G, B, e, alternate bass, G, B, e
      const upper = [-1, 3, 4, 5, -1, 3, 4, 5];
      const bass = played[0];
      const altBass = played.length > 1 && played[1] <= 2 ? played[1] : bass;
      const s = step === 0 ? bass : step === 4 ? altBass : upper[step];
      if (step === 0) muteUnplayed();
      pluckAt(s, offset, step === 0 ? 0.3 : step === 4 ? 0.26 : 0.2);
    } else {
      // down, -, down, up, -, up, down, up
      const kind = [1, 0, 1, 2, 0, 2, 1, 2][step];
      if (kind === 1) {
        muteUnplayed();
        const spacing = Math.floor(0.009 * this.fs);
        played.forEach((s, k) => pluckAt(s, offset + k * spacing, (step === 0 ? 0.19 : 0.16) * (1 - 0.04 * k)));
      } else if (kind === 2) { // up-strums catch only the top four strings, lighter
        const spacing = Math.floor(0.007 * this.fs);
        played.slice(-4).reverse().forEach((s, k) => pluckAt(s, offset + k * spacing, 0.11));
      }
    }
  }
  /** Picking the demo riff or a file also starts it playing. */
  setSource(s) {
    if (s === "demo" && !this.demo) this.demo = renderDemoRiff(this.fs);
    this.source = s;
    if (s !== "live") this.setPlaying(true);
  }
  setFile(samples, rate) { this.file = samples; this.fileRate = rate; this.filePos = 0; this.setSource("file"); }
  /** STOP / PLAY for the demo riff or file. Stopping rewinds, so PLAY starts from the top. Plucks still sound. */
  setPlaying(on) {
    if (!on) { this.demoPos = 0; this.filePos = 0; this.progress = 0; }
    this.playing = on;
  }
  pluck(i) { this.voices[i].pendingDelay = -1; this.voices[i].string.pluck(OPEN_STRINGS[i], 0.35, 3.5, 0.75, this.rng); }
  /** Down-strums CHORDS[i]: strings 12 ms apart, low to high; unplayed strings are muted. */
  strum(i) {
    const chord = CHORDS[i];
    if (!chord) return;
    const spacing = Math.floor(0.012 * this.fs);
    let order = 0;
    chord.frets.forEach((fret, s) => {
      const v = this.voices[s];
      if (fret < 0) { v.pendingDelay = -1; v.string.damp(); return; }
      v.pendingDelay = spacing * order;
      v.pendingHz = fretHz(s, fret);
      v.pendingVelocity = 0.19 * (1 - 0.04 * order); // six strings together peak near -8 dBFS
      ++order;
    });
  }
  process(io, n) {
    if (this.source !== "live" && !this.playing) {
      io.fill(0, 0, n); // stopped: silence, but plucks below still sound
    } else if (this.source === "demo" && this.demo) {
      const d = this.demo;
      for (let i = 0; i < n; ++i) { io[i] = d[this.demoPos]; if (++this.demoPos >= d.length) this.demoPos = 0; }
      this.progress = this.demoPos / d.length;
    } else if (this.source === "file") {
      const s = this.file;
      if (!s || s.length < 4) io.fill(0, 0, n);
      else {
        const size = s.length, step = this.fileRate / this.fs;
        const at = (i) => s[((i % size) + size) % size];
        for (let i = 0; i < n; ++i) {
          const i0 = Math.floor(this.filePos), t = this.filePos - i0;
          const xm1 = at(i0 - 1), x0 = at(i0), x1 = at(i0 + 1), x2 = at(i0 + 2);
          const c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
          io[i] = ((c3 * t + c2) * t + c1) * t + x0;
          this.filePos += step;
          if (this.filePos >= size) this.filePos -= size;
        }
        this.progress = this.filePos / size;
      }
    }
    this.runChordLoop(n);
    for (const v of this.voices) {
      if (!v.string.active && v.pendingDelay < 0) continue;
      for (let i = 0; i < n; ++i) {
        if (v.pendingDelay >= 0 && v.pendingDelay-- === 0) v.string.pluck(v.pendingHz, v.pendingVelocity, 3.0, 0.75, this.rng);
        io[i] += v.string.next();
      }
    }
  }
}

return { PI, dbToGain, gainToDb, Smoothed, Biquad, OnePole, DcBlocker, DelayLine, NoiseGate, OVERSAMPLER_COEFFS, Oversampler4x, Distortion, CabSim, EQ_BANDS, defaultEq, Equalizer, MOD_TYPES, Modulation, Delay, Freeverb, ReverbFx, NUM_SLOTS, MAX_KNOBS, CATEGORY, CATEGORY_NAMES, ENGINE, UNIT, DELAY_NOTE_NAMES, DELAY_NOTE_BEATS, REVERB_NOTE_NAMES, REVERB_NOTE_BEATS, MODELS, modelIndex, modelInfo, knobSkew, knobFromNorm, knobToNorm, knobText, makeSlot, resolveTempo, defaultBoard, defaultParams, Slot, FxChain, PitchDetector, NOTE_NAMES, frequencyToNote, SpectrumAnalyzer, TapTempo, OPEN_STRINGS, fretHz, CHORD_PATTERNS, CHORD_PATTERN_NAMES, CHORDS, makeRng, PluckedString, renderDemoRiff, TestSignalPlayer };
})();

// AudioWorklet processor: runs the whole pedalboard on the audio thread.
// The build prepends dsp.js as `const DSP = (...)()`.
//
// Messages in:  {type:"params", params} | {type:"source", source} | {type:"pluck", index} | {type:"file", samples, rate} | {type:"playing", playing} | {type:"strum", index}
//               {type:"chord", index} | {type:"chordStop"} | {type:"chordSettings", pattern, bpm}
// Messages out: {tuner, analyzer, inPeak, outPeak, progress} every 1024 frames
//               (raw input for the tuner, post-EQ signal for the spectrum analyser)

const BATCH = 1024;

class PedalboardProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const init = (options && options.processorOptions) || {};

    this.chain = new DSP.FxChain();
    this.chain.prepare(sampleRate, 128);
    this.params = init.params || DSP.defaultParams();
    this.chain.setParameters(this.params);
    this.chain.reset(); // pedals start in their switched state, no fade-in

    this.player = new DSP.TestSignalPlayer();
    this.player.prepare(sampleRate);
    if (init.source) this.player.setSource(init.source);
    if (init.playing === false) this.player.setPlaying(false);
    if (init.chordPattern !== undefined) this.player.setChordPattern(init.chordPattern);
    if (init.chordTempo !== undefined) this.player.setChordTempo(init.chordTempo);

    this.mono = new Float32Array(128);
    this.postEq = new Float32Array(128);
    this.postEqLength = 0;
    this.chain.analyzerTap = (data, n) => {
      if (this.postEqLength + n > this.postEq.length) {
        const grown = new Float32Array((this.postEqLength + n) * 2);
        grown.set(this.postEq.subarray(0, this.postEqLength));
        this.postEq = grown;
      }
      this.postEq.set(data.subarray(0, n), this.postEqLength);
      this.postEqLength += n;
    };

    this.newBatch();
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  newBatch() {
    this.tuner = new Float32Array(BATCH);
    this.analyzer = new Float32Array(BATCH);
    this.fill = 0;
    this.inPeak = 0;
    this.outPeak = 0;
  }

  onMessage(m) {
    if (m.type === "params") { this.params = m.params; }
    else if (m.type === "source") { this.player.setSource(m.source); }
    else if (m.type === "pluck") { this.player.pluck(m.index); }
    else if (m.type === "strum") { this.player.strum(m.index); }
    else if (m.type === "chord") { this.player.playChord(m.index); }
    else if (m.type === "chordStop") { this.player.stopChord(); }
    else if (m.type === "chordSettings") { this.player.setChordPattern(m.pattern); this.player.setChordTempo(m.bpm); }
    else if (m.type === "file") { this.player.setFile(m.samples, m.rate); }
    else if (m.type === "playing") { this.player.setPlaying(m.playing); }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0], right = out.length > 1 ? out[1] : null;
    const n = left.length;
    if (this.mono.length < n) this.mono = new Float32Array(n);
    const mono = this.mono.subarray(0, n);

    const live = inputs[0] && inputs[0][0];
    if (live && this.player.source === "live") mono.set(live.subarray(0, n)); else mono.fill(0);
    this.player.process(mono, n);

    this.postEqLength = 0;
    this.chain.setParameters(this.params);
    this.chain.process(mono, left, right, n);

    // hand raw input + post-EQ audio to the page in batches
    for (let i = 0; i < n; ) {
      const take = Math.min(n - i, BATCH - this.fill);
      this.tuner.set(mono.subarray(i, i + take), this.fill);
      this.analyzer.set(this.postEq.subarray(i, i + take), this.fill);
      this.fill += take;
      i += take;
      if (this.fill === BATCH) this.flush();
    }
    this.inPeak = Math.max(this.inPeak, this.chain.inputPeak);
    this.outPeak = Math.max(this.outPeak, this.chain.outputPeak);
    return true;
  }

  flush() {
    this.port.postMessage(
      { tuner: this.tuner, analyzer: this.analyzer, inPeak: this.inPeak, outPeak: this.outPeak, progress: this.player.progress,
        chord: this.player.loopingChord() },
      [this.tuner.buffer, this.analyzer.buffer]
    );
    this.newBatch();
  }
}

registerProcessor("pedalboard", PedalboardProcessor);
