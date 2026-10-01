// The HD500X's Preamp+EQ models (Source/DSP/fx/Eq.h). Stereo (each side filtered on its own), except the mono Vintage Pre.
//   Graphic EQ: 80Hz, 220Hz, 480Hz, 1.1kHz, 2.2kHz         |   Parametric EQ: Lows, Highs, Freq, Q, Gain
//   Studio EQ: Low Freq, Low Gain, Hi Freq, Hi Gain, Gain   |   4 Band Shift EQ: Low, Low Mid, Hi Mid, Hi, Shift
//   Mid Focus EQ: Hi Pass Freq, Hi Pass Q, Low Pass Freq, Low Pass Q, Gain
//   Vintage Pre: Gain, Output, Phase, Hi Pass Filter, Lo Pass Filter
import { PI, clamp, f32, dbToGain, Smoothed, DcBlocker, Oversampler4x } from "./core.js";
import { CATEGORY, ENGINE, model, percent, decibels, freq, choice } from "./modeltypes.js";

const GRAPHIC_EQ = 0, PARAMETRIC_EQ = 1, STUDIO_EQ = 2, FOUR_BAND_SHIFT_EQ = 3, MID_FOCUS_EQ = 4, VINTAGE_PRE = 5;
const SECTION_COUNTS = [5, 3, 2, 4, 2, 2];

const TICK = 16; // samples between filter updates (the filters glide in between)
const MAX_SECTIONS = 5, NUM_PARAMS = 5;
const TINY = f32(1e-20);

// Graphic EQ: the HD500X's five bands, each as wide as its distance to its neighbours
const GRAPHIC_HZ = [80, 220, 480, 1100, 2200], GRAPHIC_Q = [0.9, 1.0, 1.3, 1.5, 1.6];
// Parametric EQ: fixed shelves, Q knob 0 ... 100 % = 0.4 ... 10
const PARAMETRIC_LOW_HZ = 150, PARAMETRIC_HIGH_HZ = 3500, PARAMETRIC_MIN_Q = 0.4, PARAMETRIC_Q_RANGE = 25;
// Studio EQ (API 550B's two mid bands)
const STUDIO_Q = 1.2, STUDIO_KNEE = 1, STUDIO_CEILING = 2;
// 4 Band Shift EQ: band centres at Shift 50 %, and how many octaves each moves per half turn of Shift
const SHIFT_LOW_HZ = 120, SHIFT_LOW_MID_HZ = 400, SHIFT_HI_MID_HZ = 1600, SHIFT_HI_HZ = 5000, SHIFT_MID_Q = 0.9;
const SHIFT_LOW_OCTAVES = -0.7, SHIFT_LOW_MID_OCTAVES = 0.7, SHIFT_HI_MID_OCTAVES = 0.8, SHIFT_HI_OCTAVES = 0.8;
// Mid Focus EQ: Q knobs 0 ... 100 % = 0.35 ... 5.6 (25 % = Butterworth)
const FOCUS_MIN_Q = 0.35, FOCUS_Q_RANGE = 16;
// Vintage Pre: the tube stage
const PRE_MAX_DRIVE_DB = f32(30), PRE_HEADROOM = f32(2), PRE_BIAS = f32(0.2), PRE_MAKEUP = f32(0.7);
const PERCENT = f32(0.01);

// ---- one second-order filter as a state-variable filter: out = m0 * in + m1 * band-pass + m2 * low-pass.
// Coefficients [g, k, m0, m1, m2] are written into a Float32Array (rounded to float like the C++).
// The responses are the RBJ cookbook's; identity is m0 = 1, m1 = m2 = 0.
function makeCoefs(out, g, k, m0, m1, m2) { out[0] = g; out[1] = k; out[2] = m0; out[3] = m1; out[4] = m2; }
const warp = (fs, hz) => Math.tan((PI * clamp(hz, 1, 0.49 * fs)) / fs);

/** Peaking band, boost and cut mirror images; the band gets narrower as the gain grows (Q = width at half the gain in dB). */
function peak(out, fs, hz, q, gainDb) {
  const A = Math.pow(10, gainDb / 40), k = 1 / (q * A);
  makeCoefs(out, warp(fs, hz), k, 1, k * (A * A - 1), 0);
}
/** Constant-Q peaking band: the poles keep the same Q at every boost (the zeros at every cut). */
function constantQPeak(out, fs, hz, q, gainDb) {
  const G = Math.pow(10, gainDb / 20), k = G >= 1 ? 1 / q : 1 / (q * G);
  makeCoefs(out, warp(fs, hz), k, 1, k * (G - 1), 0);
}
const SHELF_K = 1.41421356; // 1 / Q of the shelves (slope 1)
/** `hz` is where the shelf has reached half its gain (in dB). */
function lowShelf(out, fs, hz, gainDb) {
  const A = Math.pow(10, gainDb / 40);
  makeCoefs(out, warp(fs, hz) / Math.sqrt(A), SHELF_K, 1, SHELF_K * (A - 1), A * A - 1);
}
function highShelf(out, fs, hz, gainDb) {
  const A = Math.pow(10, gainDb / 40);
  makeCoefs(out, warp(fs, hz) * Math.sqrt(A), SHELF_K, A * A, SHELF_K * (1 - A) * A, 1 - A * A);
}
function highPass(out, fs, hz, q) { makeCoefs(out, warp(fs, hz), 1 / q, 1, -1 / q, -1); }
function lowPass(out, fs, hz, q) { makeCoefs(out, warp(fs, hz), 1 / q, 0, 0, 1); }

const IDENTITY = new Float32Array(5);

/** One filter for both sides; a new setting glides in over one tick, sample by sample. */
class Section {
  constructor() {
    this.c = new Float32Array(5); this.target = new Float32Array(5); this.step = new Float32Array(5);
    this.a1 = 1; this.a2 = 0; this.a3 = 0; // from g and k
    this.gliding = false;
    this.ic1 = new Float32Array(2); this.ic2 = new Float32Array(2);
    this.setIdentity();
  }
  set(now) {
    const c = this.c;
    c.set(now); this.target.set(now);
    this.a1 = f32(1 / f32(1 + f32(c[0] * f32(c[0] + c[1]))));
    this.a2 = f32(c[0] * this.a1);
    this.a3 = f32(c[0] * this.a2);
    this.gliding = false;
  }
  setIdentity() { IDENTITY[0] = 0.1; IDENTITY[1] = 1; IDENTITY[2] = 1; IDENTITY[3] = IDENTITY[4] = 0; this.set(IDENTITY); }
  clear() { this.ic1[0] = this.ic1[1] = this.ic2[0] = this.ic2[1] = 0; }
  glideTo(next) {
    const c = this.c, step = this.step, scale = 1 / TICK;
    this.target.set(next);
    for (let i = 0; i < 5; ++i) step[i] = f32(next[i] - c[i]) * scale;
    this.gliding = true;
  }
  /** End of a tick: be exactly where the glide was heading. */
  land() { if (this.gliding) this.set(this.target); }
  process(x, offset, n, ch) {
    const c = this.c, end = offset + n;
    let s1 = this.ic1[ch], s2 = this.ic2[ch];
    if (this.gliding) {
      const st = this.step, dg = st[0], dk = st[1], d0 = st[2], d1 = st[3], d2 = st[4];
      let g = c[0], k = c[1], m0 = c[2], m1 = c[3], m2 = c[4];
      for (let i = offset; i < end; ++i) {
        g = f32(g + dg); k = f32(k + dk); m0 = f32(m0 + d0); m1 = f32(m1 + d1); m2 = f32(m2 + d2);
        const b1 = 1 / (1 + g * (g + k));
        const b2 = g * b1;
        const b3 = g * b2;

        const input = x[i];
        const v3 = (input + TINY) - s2;
        const v1 = b1 * s1 + b2 * v3;
        const v2 = s2 + b2 * s1 + b3 * v3;
        s1 = 2 * v1 - s1;
        s2 = 2 * v2 - s2;
        x[i] = m0 * input + m1 * v1 + m2 * v2;
      }
    } else {
      const a1 = this.a1, a2 = this.a2, a3 = this.a3, m0 = c[2], m1 = c[3], m2 = c[4];
      for (let i = offset; i < end; ++i) {
        const input = x[i];
        const v3 = (input + TINY) - s2; // (TINY: keeps the states out of the denormals in silence)
        const v1 = a1 * s1 + a2 * v3;
        const v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2 * v1 - s1;
        s2 = 2 * v2 - s2;
        x[i] = m0 * input + m1 * v1 + m2 * v2;
      }
    }
    this.ic1[ch] = s1; this.ic2[ch] = s2;
  }
  /** After both sides have been processed: move the glide on by `n` samples. */
  advance(n) {
    if (!this.gliding) return;
    const c = this.c, st = this.step;
    for (let i = 0; i < n; ++i) { c[0] += st[0]; c[1] += st[1]; c[2] += st[2]; c[3] += st[3]; c[4] += st[4]; }
  }
}
/** Linear up to `knee`, then bends over smoothly towards `ceiling`. */
function softClip(v, knee, ceiling) {
  const a = Math.abs(v);
  if (a <= knee) return v;
  const range = ceiling - knee;
  const s = knee + range * Math.tanh((a - knee) / range);
  return v < 0 ? -s : s;
}

const logHz = (hz) => f32(Math.log(Math.max(1, hz))); // frequencies glide in octaves

export class EqFx {
  constructor() {
    this.fs = 48000; this.variant = GRAPHIC_EQ;
    this.param = Array.from({ length: NUM_PARAMS }, () => new Smoothed(0));
    this.value = new Float32Array(NUM_PARAMS);
    this.newTarget = new Float32Array(NUM_PARAMS);
    this.outGain = new Smoothed(1); this.drive = new Smoothed(1); this.makeup = new Smoothed(1);
    this.section = Array.from({ length: MAX_SECTIONS }, () => new Section());
    this.coefs = Array.from({ length: MAX_SECTIONS }, () => new Float32Array(5));
    this.tickCount = 0; this.modelChanged = true;
    this.oversampler = new Oversampler4x(TICK);
    this.dc = new DcBlocker();
    this.mono = new Float32Array(TICK);
    this.tubeOffset = 0; this.tubeNorm = 1;
  }

  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    for (const p of this.param) p.reset(this.fs, 0.05);
    this.outGain.reset(this.fs, 0.03); this.drive.reset(this.fs, 0.03); this.makeup.reset(this.fs, 0.03);
    this.oversampler.reset();
    this.dc.prepare(this.fs);
    this.tubeOffset = f32(Math.tanh(PRE_BIAS));
    this.tubeNorm = f32(1 / f32(1 - f32(this.tubeOffset * this.tubeOffset))); // slope 1 for small signals
    this.reset();
  }

  reset() {
    for (let p = 0; p < NUM_PARAMS; ++p) {
      this.param[p].setCurrentAndTarget(this.param[p].target);
      this.value[p] = this.param[p].target;
    }
    this.outGain.setCurrentAndTarget(this.outGain.target);
    this.drive.setCurrentAndTarget(this.drive.target);
    this.makeup.setCurrentAndTarget(this.makeup.target);

    for (const s of this.section) { s.setIdentity(); s.clear(); }
    this.computeSections(false);

    this.oversampler.reset();
    this.dc.reset();
    this.tickCount = 0;
    this.modelChanged = false;
  }

  setModel(variant) {
    this.variant = clamp(variant | 0, 0, 5);
    this.modelChanged = true;
  }

  setParameters(k) {
    const v = this.newTarget;
    const k0 = f32(k[0]), k1 = f32(k[1]), k2 = f32(k[2]), k3 = f32(k[3]), k4 = f32(k[4]);
    v[0] = k0; v[1] = k1; v[2] = k2; v[3] = k3; v[4] = k4;

    switch (this.variant) {
      case PARAMETRIC_EQ:
        v[2] = logHz(k2);
        v[3] = k3 * PERCENT;
        break;
      case STUDIO_EQ:
        v[0] = logHz(k0);
        v[2] = logHz(k2);
        v[4] = 0;
        this.outGain.setTarget(f32(dbToGain(k4)));
        break;
      case FOUR_BAND_SHIFT_EQ:
        v[4] = k4 * PERCENT;
        break;
      case MID_FOCUS_EQ:
        v[0] = logHz(k0);
        v[1] = k1 * PERCENT;
        v[2] = logHz(k2);
        v[3] = k3 * PERCENT;
        v[4] = 0;
        this.outGain.setTarget(f32(dbToGain(k4)));
        break;
      case VINTAGE_PRE: {
        // Gain drives the tube harder; most of the extra level is taken off again behind it. Phase flips the output.
        const driveDb = f32(f32(PRE_MAX_DRIVE_DB * k0) * PERCENT);
        this.drive.setTarget(f32(f32(dbToGain(driveDb)) / PRE_HEADROOM));
        this.makeup.setTarget(f32(PRE_HEADROOM * f32(dbToGain(f32(-PRE_MAKEUP * driveDb)))));
        this.outGain.setTarget(k2 >= 0.5 ? -f32(dbToGain(k1)) : f32(dbToGain(k1)));
        v[0] = v[1] = v[2] = 0;
        v[3] = logHz(k3);
        v[4] = logHz(k4);
        break;
      }
      default: break;
    }

    for (let p = 0; p < NUM_PARAMS; ++p) this.param[p].setTarget(v[p]);
  }

  process(left, right, numSamples) {
    const variant = this.variant, section = this.section, count = SECTION_COUNTS[variant], outGain = this.outGain;

    for (let pos = 0; pos < numSamples;) {
      if (this.tickCount === 0) this.nextTick();

      const n = Math.min(this.tickCount, numSamples - pos);

      if (variant === VINTAGE_PRE) {
        this.processPre(left, right, pos, n);
      } else {
        for (let s = 0; s < count; ++s) {
          section[s].process(left, pos, n, 0);
          section[s].process(right, pos, n, 1);
          section[s].advance(n);
        }

        if (variant === STUDIO_EQ) {
          // the output stage: clean up to full scale, rounds the peaks off when pushed beyond it
          for (let i = pos; i < pos + n; ++i) {
            const gain = outGain.next();
            left[i] = softClip(left[i] * gain, STUDIO_KNEE, STUDIO_CEILING);
            right[i] = softClip(right[i] * gain, STUDIO_KNEE, STUDIO_CEILING);
          }
        } else if (variant === MID_FOCUS_EQ) {
          for (let i = pos; i < pos + n; ++i) {
            const gain = outGain.next();
            left[i] *= gain;
            right[i] *= gain;
          }
        }
      }

      pos += n;
      this.tickCount -= n;
    }
  }

  /** Filter coefficients for the current (gliding) knob values. */
  computeSections(glide) {
    const c = this.coefs, fs = this.fs, v = this.value;

    switch (this.variant) {
      case GRAPHIC_EQ: // five gyrator-style bands, boost and cut symmetrical, +-12 dB
        for (let b = 0; b < 5; ++b) peak(c[b], fs, GRAPHIC_HZ[b], GRAPHIC_Q[b], v[b]);
        break;
      case PARAMETRIC_EQ: // low shelf, high shelf and one band with free frequency, width and gain
        lowShelf(c[0], fs, PARAMETRIC_LOW_HZ, v[0]);
        highShelf(c[1], fs, PARAMETRIC_HIGH_HZ, v[1]);
        peak(c[2], fs, Math.exp(v[2]), PARAMETRIC_MIN_Q * Math.pow(PARAMETRIC_Q_RANGE, v[3]), v[4]);
        break;
      case STUDIO_EQ: // the API 550B's two mid bands, constant-Q, reciprocal boost / cut
        constantQPeak(c[0], fs, Math.exp(v[0]), STUDIO_Q, v[1]);
        constantQPeak(c[1], fs, Math.exp(v[2]), STUDIO_Q, v[3]);
        break;
      case FOUR_BAND_SHIFT_EQ: { // shelves at both ends, two peaking bands between; Shift slides the low band down, the others up
        const turn = 2 * v[4] - 1;
        lowShelf(c[0], fs, SHIFT_LOW_HZ * Math.pow(2, SHIFT_LOW_OCTAVES * turn), v[0]);
        peak(c[1], fs, SHIFT_LOW_MID_HZ * Math.pow(2, SHIFT_LOW_MID_OCTAVES * turn), SHIFT_MID_Q, v[1]);
        peak(c[2], fs, SHIFT_HI_MID_HZ * Math.pow(2, SHIFT_HI_MID_OCTAVES * turn), SHIFT_MID_Q, v[2]);
        highShelf(c[3], fs, SHIFT_HI_HZ * Math.pow(2, SHIFT_HI_OCTAVES * turn), v[3]);
        break;
      }
      case MID_FOCUS_EQ: // 12 dB/octave high-pass and low-pass, each with its own resonance
        highPass(c[0], fs, Math.exp(v[0]), FOCUS_MIN_Q * Math.pow(FOCUS_Q_RANGE, v[1]));
        lowPass(c[1], fs, Math.exp(v[2]), FOCUS_MIN_Q * Math.pow(FOCUS_Q_RANGE, v[3]));
        break;
      default: // Vintage Pre: the filters in front of and behind the tube
        highPass(c[0], fs, Math.exp(v[3]), 0.70710678);
        lowPass(c[1], fs, Math.exp(v[4]), 0.70710678);
        break;
    }

    const count = SECTION_COUNTS[this.variant];
    for (let s = 0; s < count; ++s) {
      if (glide) this.section[s].glideTo(c[s]);
      else this.section[s].set(c[s]);
    }
  }

  nextTick() {
    this.tickCount = TICK;
    for (const s of this.section) s.land();

    let moved = this.modelChanged;
    for (let p = 0; p < NUM_PARAMS; ++p) {
      if (this.modelChanged || this.param[p].isSmoothing()) {
        this.value[p] = this.param[p].skip(TICK);
        moved = true;
      }
    }

    if (moved) {
      this.modelChanged = false;
      this.computeSections(true);
    }
  }

  // Vintage Pre (Requisite Y7 tube mic preamp), mono: high-pass, one tube stage (a biased tanh, at 4x the sample rate), low-pass.
  processPre(left, right, pos, n) {
    const mono = this.mono, drive = this.drive, makeup = this.makeup, outGain = this.outGain, dc = this.dc;
    const tubeOffset = this.tubeOffset, tubeNorm = this.tubeNorm;

    for (let i = 0; i < n; ++i) mono[i] = 0.5 * (left[pos + i] + right[pos + i]);

    this.section[0].process(mono, 0, n, 0);
    this.section[0].advance(n);

    for (let i = 0; i < n; ++i) mono[i] *= drive.next();

    const up = this.oversampler.up(mono, n);
    for (let i = 0; i < 4 * n; ++i) up[i] = (Math.tanh(up[i] + PRE_BIAS) - tubeOffset) * tubeNorm;
    this.oversampler.down(mono, n);

    for (let i = 0; i < n; ++i) mono[i] *= makeup.next();

    this.section[1].process(mono, 0, n, 0);
    this.section[1].advance(n);

    for (let i = 0; i < n; ++i) left[pos + i] = right[pos + i] = dc.process(mono[i]) * outGain.next();
  }
}

const gain = (name, def = 0) => decibels(name, -12, 12, def);
const eq = (key, name, variant, basedOn, knobs) => model(key, name, CATEGORY.eq, ENGINE.eqFx, variant, basedOn, knobs);

/** In the order of the variants. */
export const EQ_MODELS = [
  eq("graphic_eq", "Graphic EQ", GRAPHIC_EQ, "Inspired by the MXR 10-band graphic EQ",
    [gain("80Hz"), gain("220Hz"), gain("480Hz"), gain("1.1kHz"), gain("2.2kHz")]),
  eq("parametric_eq", "Parametric EQ", PARAMETRIC_EQ, "Low shelf, high shelf and one fully parametric band",
    [gain("Lows"), gain("Highs"), freq("Freq", 80, 8000, 800, 800), percent("Q", 35), gain("Gain")]),
  eq("studio_eq", "Studio EQ", STUDIO_EQ, "Inspired by the API 550B",
    [freq("Low Freq", 75, 1000, 500, 275), gain("Low Gain"), freq("Hi Freq", 800, 12500, 1500, 3200), gain("Hi Gain"), gain("Gain")]),
  eq("4_band_shift_eq", "4 Band Shift EQ", FOUR_BAND_SHIFT_EQ, "Four bands whose frequencies the Shift knob spreads apart",
    [gain("Low"), gain("Low Mid"), gain("Hi Mid"), gain("Hi"), percent("Shift", 50)]),
  eq("mid_focus_eq", "Mid Focus EQ", MID_FOCUS_EQ, "High-pass and low-pass with resonance, and make-up gain",
    [freq("Hi Pass Freq", 20, 2000, 130, 200), percent("Hi Pass Q", 25), freq("Low Pass Freq", 500, 20000, 4500, 3200), percent("Low Pass Q", 25), gain("Gain", 2)]),
  eq("vintage_pre", "Vintage Pre", VINTAGE_PRE, "Requisite Y7 tube mic preamp",
    [percent("Gain", 30), decibels("Output", -30, 12, 0), choice("Phase", ["0", "180"], 0),
     freq("Hi Pass Filter", 20, 1000, 20, 140), freq("Lo Pass Filter", 1000, 20000, 20000, 4500)]),
];
