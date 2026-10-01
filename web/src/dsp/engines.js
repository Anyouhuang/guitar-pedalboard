// The first effect engines: noise gate, the 15 distortion models, cab sim, EQ, and the original
// modulation / delay / reverb (Source/DSP/NoiseGate.h, Distortion.h, CabSim.h, Equalizer.h, Modulation.h, Delay.h, ReverbFx.h).
import { PI, clamp, f32, dbToGain, gainToDb, Smoothed, Biquad, OnePole, DcBlocker, DelayLine, Oversampler4x } from "./core.js";

// ============================================================================
export class NoiseGate {
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
export class Distortion {
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
export class CabSim {
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
export const EQ_BANDS = ["lowCut", "bass", "lowMid", "highMid", "treble", "highCut"];
export const defaultEq = () => ({
  lowCutHz: 20, bassHz: 100, bassDb: 0, lowMidHz: 400, lowMidDb: 0, lowMidQ: 1,
  highMidHz: 2000, highMidDb: 0, highMidQ: 1, trebleHz: 5000, trebleDb: 0, highCutHz: 20000,
});

export class Equalizer {
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
export const MOD_TYPES = ["Chorus", "Flanger", "Phaser", "Tremolo"];

export class Modulation {
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
export class Delay {
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

export class Freeverb {
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

export class ReverbFx {
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

