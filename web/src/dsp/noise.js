// Noise reduction (Source/DSP/NoiseReduction.h): a mains-hum filter for the input and a hiss reducer for the output.
import { PI, clamp, f32, dbToGain, gainToDb, Smoothed, OnePole } from "./core.js";

export const HUM_MODES = ["Off", "50 Hz", "60 Hz"];

/** Narrow notches on 50 or 60 Hz and its first harmonics (mono, in double precision like the C++). */
export class HumFilter {
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.amount = new Smoothed(0);
    this.amount.reset(sampleRate, 0.03);
    this.notches = Array.from({ length: 6 }, () => ({ b0: 1, b1: 0, b2: 0, a1: 0, a2: 0, z1: 0, z2: 0 }));
    this.wanted = this.wanted || 0;
    this.tuned = this.tuned || 2;
    this.needsClear = false;
    this.design();
    this.reset();
  }
  reset() {
    this.clear();
    if (this.wanted !== 0) this.tuned = this.wanted;
    this.design();
    this.amount.setCurrentAndTarget(this.wanted !== 0 ? 1 : 0);
  }
  isIdle() { return !this.amount.isSmoothing() && this.amount.current <= 0; }
  clear() { for (const n of this.notches) n.z1 = n.z2 = 0; }
  static bandwidthHz(k) { return 2 + 0.4 * (k - 1); }
  design() {
    const mains = this.tuned === 1 ? 50 : 60;
    for (let k = 1; k <= this.notches.length; ++k) {
      const f = mains * k, w0 = (2 * PI * f) / this.fs;
      const alpha = Math.sin(w0) / (2 * (f / HumFilter.bandwidthHz(k))), a0 = 1 + alpha, n = this.notches[k - 1];
      n.b0 = 1 / a0;
      n.b1 = (-2 * Math.cos(w0)) / a0;
      n.b2 = 1 / a0;
      n.a1 = n.b1;
      n.a2 = (1 - alpha) / a0;
    }
  }
  setMode(mode) {
    this.wanted = clamp(mode | 0, 0, 2);
    if (this.wanted !== 0 && this.wanted !== this.tuned && this.isIdle()) { this.tuned = this.wanted; this.design(); this.clear(); }
    this.amount.setTarget(this.wanted !== 0 && this.wanted === this.tuned ? 1 : 0);
  }
  process(data, n) {
    if (this.isIdle()) { this.needsClear = true; return; }
    if (this.needsClear) { this.clear(); this.needsClear = false; }
    const notches = this.notches, amount = this.amount;
    for (let i = 0; i < n; ++i) {
      const x = data[i];
      let y = x;
      for (let k = 0; k < notches.length; ++k) {
        const c = notches[k], out = c.b0 * y + c.z1;
        c.z1 = c.b1 * y - c.a1 * out + c.z2;
        c.z2 = c.b2 * y - c.a2 * out;
        y = out;
      }
      data[i] = x + amount.next() * (y - x);
    }
  }
}

/** Hiss reduction for the output (stereo): open while you play, a sliding low-pass and up to 12 dB less
    level as the sound dies away. amount: 0 % = off. */
export class Denoiser {
  prepare(sampleRate) {
    this.fs = sampleRate;
    this.attack = f32(Math.exp(-1 / (0.002 * sampleRate)));
    this.release = f32(Math.exp(-1 / (0.12 * sampleRate)));
    this.filters = [new OnePole(), new OnePole(), new OnePole(), new OnePole()];
    this.mix = new Smoothed(0); this.mix.reset(sampleRate, 0.03);
    this.gain = new Smoothed(1); this.gain.reset(sampleRate, 0.02);
    this.amount = 0; this.thresholdDb = -75; this.needsReset = false;
    this.reset();
  }
  reset() {
    for (const f of this.filters) f.reset();
    this.env = 1;
    this.counter = 0;
    this.mix.setCurrentAndTarget(this.mix.target);
    this.gain.setCurrentAndTarget(1);
    this.retune(1);
  }
  setAmount(percent) {
    this.amount = f32(clamp(percent, 0, 100));
    this.thresholdDb = f32(-75 + 0.4 * this.amount);
    this.mix.setTarget(this.amount > 0 ? 1 : 0);
  }
  retune(open) {
    const cutoff = 1000 * Math.pow((0.49 * this.fs) / 1000, open);
    for (const f of this.filters) f.setCutoff(this.fs, cutoff);
  }
  process(left, right, n) {
    if (!this.mix.isSmoothing() && this.mix.current <= 0) { this.needsReset = true; return; }
    if (this.needsReset) {
      for (const f of this.filters) f.reset();
      this.env = 1; this.counter = 0;
      this.gain.setCurrentAndTarget(1);
      this.retune(1);
      this.needsReset = false;
    }
    const [f0, f1, f2, f3] = this.filters, attack = this.attack, release = this.release;
    let env = this.env;
    for (let i = 0; i < n; ++i) {
      const l = left[i], r = right[i];
      const level = Math.max(Math.abs(l), Math.abs(r));
      env = f32(level + (level > env ? attack : release) * (env - level));
      if (this.counter === 0) {
        const x = f32(clamp((f32(gainToDb(env)) - this.thresholdDb) / 18, 0, 1));
        const open = f32(x * x * (3 - 2 * x));
        this.retune(open);
        this.gain.setTarget(f32(dbToGain(-12 * (1 - open))));
      }
      this.counter = (this.counter + 1) & 15;
      const g = this.gain.next(), m = this.mix.next();
      const yl = f1.lowPass(f0.lowPass(l)) * g;
      const yr = f3.lowPass(f2.lowPass(r)) * g;
      left[i] = l + m * (yl - l);
      right[i] = r + m * (yr - r);
    }
    this.env = env;
  }
}
