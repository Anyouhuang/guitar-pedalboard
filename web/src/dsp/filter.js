// The HD500X's filter models and guitar synths (Source/DSP/fx/Filter.h), 16 of 17: the Vocoder needs the
// hardware's microphone input. Ported line for line; see the C++ header for what each model is.
import { PI, clamp, f32 } from "./core.js";
import { CATEGORY, ENGINE, model, percent, decibels, hertz, semitones, choice } from "./modeltypes.js";

// Names of the choice knobs
const VOWEL_NAMES = ["A", "E", "I", "O", "U"];
const AUTO_NAMES = ["Sine", "Hold", "Ramp", "Random"];
const SWEEP_NAMES = ["Up", "Up-Down"];
const TYPE_NAMES = ["LP", "BP", "HP"];
const PATTERN_NAMES = ["Rise", "Fall", "Peak", "Zigzag", "Hi-Lo", "Pairs", "Random 1", "Random 2"];
const STEP_NAMES = ["2", "3", "4", "5", "6", "7", "8", "9"];
const RANGE_NAMES = ["Hi", "Lo"];
const LFO_NAMES = ["Ramp Up", "Ramp Down", "Triangle", "Square"];
const MODE_NAMES = ["Up", "Down"];
const SYNTH_WAVE_NAMES = ["Saw", "Square", "Pulse", "Triangle", "Saw+Sqr", "Detune", "Octave", "Sub"];
const ATTACK_WAVE_NAMES = ["Square", "PWM", "Ramp"];

const TWO_PI = 2 * PI;

// Formants F1..F3 of the vowels A E I O U, the level of each formant and the resonators' Q
const VOWEL_HZ = [[730, 1090, 2440], [530, 1840, 2480], [270, 2290, 3010], [570, 840, 2410], [300, 870, 2240]];
const VOWEL_GAIN = [[1, 0.70, 0.25], [1, 0.45, 0.30], [1, 0.35, 0.25], [1, 0.60, 0.15], [1, 0.40, 0.12]];
const VOWEL_Q = [6, 9, 10];

// The Seeker's step patterns 3..8 (0 = lowest, 1 = highest filter position); "Rise" and "Fall" are computed
const SEEKER_TABLE = [
  [0.00, 0.35, 0.70, 1.00, 0.70, 0.35, 0.00, 0.50, 1.00], // Peak
  [0.00, 0.60, 0.20, 0.80, 0.40, 1.00, 0.30, 0.70, 0.50], // Zigzag
  [0.10, 0.90, 0.10, 0.90, 0.30, 0.70, 0.30, 0.70, 0.50], // Hi-Lo
  [0.00, 0.00, 0.50, 0.50, 1.00, 1.00, 0.50, 0.50, 0.25], // Pairs
  [0.62, 0.11, 0.87, 0.35, 0.05, 0.74, 0.48, 0.96, 0.23], // Random 1
  [0.30, 0.95, 0.55, 0.00, 0.80, 0.18, 0.68, 0.42, 1.00], // Random 2
];

function seekerPosition(pattern, step, steps) {
  if (pattern === 0) return step / (steps - 1);
  if (pattern === 1) return 1 - step / (steps - 1);
  return SEEKER_TABLE[pattern - 2][step];
}
const seekerHz = (position) => 300 * Math.pow(8, position);

function smooth01(x) {
  x = clamp(x, 0, 1);
  return x * x * (3 - 2 * x);
}
const choiceOf = (value, maxIndex) => clamp(Math.floor(value + 0.5), 0, maxIndex);

const F04 = f32(0.4);
function softLimit(x) {
  const a = Math.abs(x);
  if (a <= 1.5) return x;
  const y = 1.5 + 2.5 * Math.tanh((a - 1.5) * F04);
  return x < 0 ? -y : y;
}

const lowPassNorm = (k) => 1 / Math.sqrt(1 + 0.25 / k);
const bandPassNorm = (k) => 1.4 * Math.sqrt(k);

// ============================================================================
// A value that moves in a straight line to a new target during each control interval (a float in the C++)
class Ramp {
  constructor() { this.v = 0; this.dv = 0; }
  clear() { this.v = 0; this.dv = 0; }
  aim(target, invSteps, snap) {
    if (snap) { this.v = f32(target); this.dv = 0; }
    else this.dv = f32(f32(f32(target) - this.v) * invSteps);
  }
  step() { this.v = f32(this.v + this.dv); return this.v; }
}

// Coefficients of a state-variable filter, interpolated per sample (floats in the C++)
const G_DEFAULT = f32(0.1);
class SvfCoef {
  constructor() { this.clear(); }
  clear() { this.g = G_DEFAULT; this.k = 1; this.dg = 0; this.dk = 0; this.a1 = 0; this.a2 = 0; this.a3 = 0; }
  aim(gTarget, kTarget, invSteps, snap) {
    if (snap) { this.g = f32(gTarget); this.k = f32(kTarget); this.dg = this.dk = 0; }
    else {
      this.dg = f32(f32(f32(gTarget) - this.g) * invSteps);
      this.dk = f32(f32(f32(kTarget) - this.k) * invSteps);
    }
  }
  step() {
    this.g = f32(this.g + this.dg);
    this.k = f32(this.k + this.dk);
    this.a1 = f32(1 / f32(1 + f32(this.g * f32(this.g + this.k))));
    this.a2 = f32(this.g * this.a1);
    this.a3 = f32(this.g * this.a2);
  }
}

// Topology-preserving state-variable filter
class Svf {
  constructor() { this.s1 = 0; this.s2 = 0; this.lp = 0; this.bp = 0; this.hp = 0; }
  reset() { this.s1 = this.s2 = this.lp = this.bp = this.hp = 0; }
  process(x, c) {
    const v3 = x - this.s2;
    this.bp = c.a1 * this.s1 + c.a2 * v3;
    this.lp = this.s2 + c.a2 * this.s1 + c.a3 * v3;
    this.s1 = 2 * this.bp - this.s1;
    this.s2 = 2 * this.lp - this.s2;
    this.hp = x - c.k * this.bp - this.lp;
  }
  flush() {
    if (Math.abs(this.s1) < 1e-20) this.s1 = 0;
    if (Math.abs(this.s2) < 1e-20) this.s2 = 0;
  }
}

// One-pole smoothing at the control rate
class Slew {
  constructor() { this.v = 0; }
  next(target, coef, snap) {
    this.v = snap ? target : this.v + coef * (target - this.v);
    return this.v;
  }
}

class Lcg {
  constructor() { this.state = 1; }
  next01() {
    this.state = (Math.imul(this.state, 1664525) + 1013904223) >>> 0;
    return (this.state >>> 8) * (1 / 16777216);
  }
}

// ============================================================================
// Finds pick attacks: the peak level jumps well above what it has been lately
class AttackDetector {
  prepare(fs) {
    this.fastRelease = 1 - 1 / (0.020 * fs);
    this.slowRise = 1 / (0.010 * fs);
    this.slowFall = 1 / (0.100 * fs);
    this.holdSamples = Math.floor(0.06 * fs);
    this.reset();
  }
  reset() { this.fast = this.slow = 0; this.hold = 0; this.armed = true; }
  /** `a` = |input|. True on the sample where a new note starts. */
  process(a) {
    this.fast = a > this.fast ? a : this.fast * this.fastRelease;
    if (this.fast < 1e-20) this.fast = 0;
    this.slow += (this.fast > this.slow ? this.slowRise : this.slowFall) * (this.fast - this.slow);
    if (this.slow < 1e-20) this.slow = 0;

    if (this.hold > 0) {
      --this.hold;
    } else if (this.armed) {
      if (this.fast > 0.01 && this.fast > 1.7 * this.slow) {
        this.armed = false;
        this.hold = this.holdSamples;
        return true;
      }
    } else if (this.fast < 1.25 * this.slow) {
      this.armed = true;
    }
    return false;
  }
}

// The playing level for the synths' amplifiers: the highest peak of the last 13.5 ms
class LevelMeter {
  prepare(numBlocks) { this.ring = new Float64Array(Math.max(1, numBlocks)); this.reset(); }
  reset() { this.ring.fill(0); this.pos = 0; this.blockMax = 0; }
  push(a) { if (a > this.blockMax) this.blockMax = a; }
  /** Once per control interval. */
  endBlock() {
    const ring = this.ring;
    ring[this.pos] = this.blockMax;
    this.blockMax = 0;
    if (++this.pos >= ring.length) this.pos = 0;

    let level = 0;
    for (let i = 0; i < ring.length; ++i) if (ring[i] > level) level = ring[i];
    return level;
  }
}

// ============================================================================
// Monophonic pitch tracker for the guitar synths (see the C++ for how it works). Everything is double
// precision with only + - * / and comparisons, so it takes exactly the same decisions as the C++ one.
const FINE_LAGS = 64, CURVE_SIZE = 172, REFINE_LAG = 160, MIN_LAG = 9, MAX_KEYS = 24;
const MIN_WINDOW = 64; // in coarse samples: 8 ms
const FINE_WINDOW = 2 * MIN_WINDOW + FINE_LAGS + 1, COARSE_WINDOW = 2 * REFINE_LAG, FINE_RING = 256, COARSE_RING = 512;
const HOP = 48; // fine samples between two analyses
const OPEN_LEVEL = 0.006, CLOSE_LEVEL = 0.003, CLARITY = 0.7;

const onePole = (fc, fs) => { const w = (TWO_PI * fc) / fs; return w / (1 + w); };

/** NSDF at one lag over the newest samples (`lin`: newest first, `prefix`: their running energy). */
function nsdfAt(lin, prefix, lag, minLength) {
  const length = lag >= minLength ? lag : minLength;

  // four running sums (the same four as in the C++): about three times as fast as one
  let c0 = 0, c1 = 0, c2 = 0, c3 = 0;
  let j = 0;
  for (; j + 3 < length; j += 4) {
    c0 += lin[j] * lin[j + lag];
    c1 += lin[j + 1] * lin[j + 1 + lag];
    c2 += lin[j + 2] * lin[j + 2 + lag];
    c3 += lin[j + 3] * lin[j + 3 + lag];
  }
  for (; j < length; ++j) c0 += lin[j] * lin[j + lag];

  const cross = (c0 + c1) + (c2 + c3);
  const energy = prefix[length] + prefix[length + lag] - prefix[lag];
  return energy > 1e-14 ? (2 * cross) / energy : 0;
}

function peakOffset(before, at, after) {
  const bend = before - 2 * at + after;
  return bend < 0 ? clamp((0.5 * (before - after)) / bend, -0.5, 0.5) : 0;
}

class PitchTracker {
  prepare(fs) {
    this.decimation = Math.max(1, Math.floor(fs / 16000 + 0.5));
    this.invDecimation = 1 / this.decimation;
    this.rate = fs / this.decimation;
    this.preCoef = onePole(2500, fs);
    this.lowCoef = onePole(1000, this.rate);
    this.dcCoef = onePole(45, this.rate);
    this.fineRing = new Float64Array(FINE_RING);
    this.coarseRing = new Float64Array(COARSE_RING);
    this.fineLin = new Float64Array(FINE_WINDOW);
    this.finePrefix = new Float64Array(FINE_WINDOW + 1);
    this.coarseLin = new Float64Array(COARSE_WINDOW);
    this.coarsePrefix = new Float64Array(COARSE_WINDOW + 1);
    this.curve = new Float64Array(CURVE_SIZE);
    this.keyIndex = new Int32Array(MAX_KEYS);
    this.keyValue = new Float64Array(MAX_KEYS);
    this.reset();
  }

  reset() {
    this.fineRing.fill(0);
    this.coarseRing.fill(0);
    this.pre1 = this.pre2 = this.sum = this.dc = this.low1 = this.low2 = this.previous = 0;
    this.decimationCount = this.hopCount = this.finePos = this.coarsePos = 0;
    this.level = 0; // set by the owner once per control interval
    this.gate = this.voiced = false;
    this.period = this.pendingPeriod = 120;
    this.pendingCount = this.steadyCount = 0;
    this.freq = this.rate / this.period; // Hz, valid while `voiced`
  }

  process(x) {
    this.pre1 += this.preCoef * (x - this.pre1) + 1e-25;
    this.pre2 += this.preCoef * (this.pre1 - this.pre2);
    this.sum += this.pre2;
    if (++this.decimationCount < this.decimation) return;

    this.decimationCount = 0;
    const decimated = this.sum * this.invDecimation;
    this.sum = 0;
    this.dc += this.dcCoef * (decimated - this.dc);
    const fine = decimated - this.dc;
    this.fineRing[this.finePos] = fine;
    this.finePos = (this.finePos + 1) & (FINE_RING - 1);

    this.low1 += this.lowCoef * (fine - this.low1) + 1e-25;
    this.low2 += this.lowCoef * (this.low1 - this.low2);
    if ((this.finePos & 1) === 0) {
      this.coarseRing[this.coarsePos] = 0.5 * (this.low2 + this.previous);
      this.coarsePos = (this.coarsePos + 1) & (COARSE_RING - 1);
    }
    this.previous = this.low2;

    if (++this.hopCount < HOP) return;

    this.hopCount = 0;
    this.analyse();
  }

  fineNsdf(lag) { return nsdfAt(this.fineLin, this.finePrefix, lag, 2 * MIN_WINDOW); }
  coarseNsdf(lag) { return nsdfAt(this.coarseLin, this.coarsePrefix, lag, MIN_WINDOW); }

  analyse() {
    if (this.gate) {
      if (this.level < CLOSE_LEVEL) {
        this.gate = this.voiced = false;
        this.pendingCount = this.steadyCount = 0;
      }
    } else if (this.level > OPEN_LEVEL) {
      this.gate = true;
    }

    if (!this.gate) return;

    // the newest samples first, and their running energy
    const fineLin = this.fineLin, finePrefix = this.finePrefix, fineRing = this.fineRing;
    finePrefix[0] = 0;
    for (let i = 0; i < FINE_WINDOW; ++i) {
      const v = fineRing[(this.finePos - 1 - i) & (FINE_RING - 1)];
      fineLin[i] = v;
      finePrefix[i + 1] = finePrefix[i] + v * v;
    }

    const coarseLin = this.coarseLin, coarsePrefix = this.coarsePrefix, coarseRing = this.coarseRing;
    coarsePrefix[0] = 0;
    for (let i = 0; i < COARSE_WINDOW; ++i) {
      const v = coarseRing[(this.coarsePos - 1 - i) & (COARSE_RING - 1)];
      coarseLin[i] = v;
      coarsePrefix[i + 1] = coarsePrefix[i] + v * v;
    }

    const curve = this.curve;
    for (let i = 1; i < FINE_LAGS; ++i) curve[i] = nsdfAt(fineLin, finePrefix, i, 2 * MIN_WINDOW);
    for (let i = FINE_LAGS; i < CURVE_SIZE; ++i) curve[i] = nsdfAt(coarseLin, coarsePrefix, i - FINE_LAGS / 2, MIN_WINDOW);
    // where the two lag grids meet, each side is compared with its own signal's next point
    const aboveLastFine = this.fineNsdf(FINE_LAGS), belowFirstCoarse = this.coarseNsdf(FINE_LAGS / 2 - 1);

    // "key maxima": the highest point of each positive stretch after the curve first went negative
    const keyIndex = this.keyIndex, keyValue = this.keyValue;
    let numKeys = 0, best = 0;
    let bestValue = 0, highest = 0;
    let seenNegative = false, positive = false;

    for (let i = 1; i < CURVE_SIZE - 1; ++i) {
      const v = curve[i];
      if (!seenNegative) {
        seenNegative = v < 0;
        continue;
      }

      if (v > 0) {
        if (!positive) {
          positive = true;
          best = 0;
          bestValue = 0;
        }
        if (v > bestValue && v >= (i === FINE_LAGS ? belowFirstCoarse : curve[i - 1])
                          && v >= (i === FINE_LAGS - 1 ? aboveLastFine : curve[i + 1])) {
          bestValue = v;
          best = i;
        }
      }

      if (positive && (v <= 0 || i === CURVE_SIZE - 2)) {
        positive = false;
        if (best >= MIN_LAG && numKeys < MAX_KEYS) {
          keyIndex[numKeys] = best;
          keyValue[numKeys] = bestValue;
          ++numKeys;
          if (bestValue > highest) highest = bestValue;
        }
      }
    }

    // the first one that is nearly as high as the highest: the shortest period that fits
    let chosen = -1;
    for (let i = 0; i < numKeys && chosen < 0; ++i)
      if (keyValue[i] >= 0.85 * highest) chosen = i;

    if (chosen < 0 || keyValue[chosen] < CLARITY) {
      this.pendingCount = 0;
      return;
    }

    // parabola through the peak ...
    const index = keyIndex[chosen];
    const offset = peakOffset(index === FINE_LAGS ? belowFirstCoarse : curve[index - 1], curve[index],
                              index === FINE_LAGS - 1 ? aboveLastFine : curve[index + 1]);
    let found = index < FINE_LAGS ? index + offset : 2 * (index - FINE_LAGS / 2 + offset);
    let multiple = 1;

    // ... then the same on multiples of the period for a finer reading
    for (;;) {
      const coarse = 2 * found >= FINE_LAGS - 2.5;
      const lag = Math.floor((coarse ? found : 2 * found) + 0.5);
      if (coarse && lag + 2 > REFINE_LAG) break;

      const peak = this.peakNear(coarse, lag);
      if (peak < 0) break;

      found = coarse ? 2 * peak : peak;
      multiple *= 2;
    }

    const candidate = found / multiple;

    if (this.voiced && Math.abs(candidate - this.period) <= 0.06 * this.period) {
      this.period += 0.4 * (candidate - this.period); // the same note: follow it, a little smoothed
      this.pendingCount = 0;
      if (this.steadyCount < 1000) ++this.steadyCount;
    } else {
      this.pendingCount = (this.pendingCount > 0 && Math.abs(candidate - this.pendingPeriod) <= 0.06 * this.pendingPeriod) ? this.pendingCount + 1 : 1;
      this.pendingPeriod = candidate;

      // a settled note needs more evidence before it jumps an octave (the usual tracking error)
      const ratio = candidate / this.period;
      const octave = this.voiced && this.steadyCount > 8 && ((ratio > 1.88 && ratio < 2.12) || (ratio > 0.47 && ratio < 0.53));
      if (this.pendingCount >= (octave ? 4 : 2)) {
        this.period = candidate;
        this.voiced = true;
        this.pendingCount = this.steadyCount = 0;
      }
    }

    this.freq = this.rate / this.period;
  }

  /** The NSDF peak at `lag` or one step beside it, on the fine or the coarse signal; -1 if there is no clear peak. */
  peakNear(coarse, lag) {
    let before = coarse ? this.coarseNsdf(lag - 1) : this.fineNsdf(lag - 1);
    let at = coarse ? this.coarseNsdf(lag) : this.fineNsdf(lag);
    let after = coarse ? this.coarseNsdf(lag + 1) : this.fineNsdf(lag + 1);

    if (after > at) {
      ++lag;
      before = at;
      at = after;
      after = coarse ? this.coarseNsdf(lag + 1) : this.fineNsdf(lag + 1);
    } else if (before > at) {
      --lag;
      after = at;
      at = before;
      before = coarse ? this.coarseNsdf(lag - 1) : this.fineNsdf(lag - 1);
    }

    if (at < CLARITY || at < before || at < after) return -1;

    return lag + peakOffset(before, at, after);
  }
}

// ============================================================================
// Oscillators: `p` = phase 0..1, `dt` = phase step per sample, with polyBLEP / polyBLAMP
function polyBlep(t, dt) {
  if (t < dt) {
    const x = t / dt;
    return x + x - x * x - 1;
  }
  if (t > 1 - dt) {
    const x = (t - 1) / dt;
    return x * x + x + x + 1;
  }
  return 0;
}

function polyBlamp(t, dt) {
  if (t < dt) {
    const x = t / dt - 1;
    return (-x * x * x) / 3;
  }
  if (t > 1 - dt) {
    const x = (t - 1) / dt + 1;
    return (x * x * x) / 3;
  }
  return 0;
}

const sawWave = (p, dt) => 2 * p - 1 - polyBlep(p, dt);

function pulseWave(p, dt, width) {
  let p2 = p + 1 - width;
  if (p2 >= 1) p2 -= 1;
  return sawWave(p, dt) - sawWave(p2, dt);
}

function triangleWave(p, dt) {
  let p2 = p + 0.5;
  if (p2 >= 1) p2 -= 1;
  return 1 - 4 * Math.abs(p - 0.5) + 8 * dt * (polyBlamp(p2, dt) - polyBlamp(p, dt));
}

// ============================================================================
const VOICE_BOX = 0, V_TRON = 1, Q_FILTER = 2, SEEKER = 3, OBI_WAH = 4, TRON_UP = 5, TRON_DOWN = 6, THROBBER = 7, SLOW_FILTER = 8,
      SPIN_CYCLE = 9, COMET_TRAILS = 10, OCTISYNTH = 11, SYNTH_O_MATIC = 12, ATTACK_SYNTH = 13, SYNTH_STRING = 14, GROWLER = 15, NUM_VARIANTS = 16;
const NUM_COMETS = 7;
const COMET_LEFT = Float32Array.of(1.0, 0.43, 0.72, 0.31, 0.52, 0.22, 0.38);
const COMET_RIGHT = Float32Array.of(0.5, 0.85, 0.36, 0.61, 0.26, 0.44, 0.19);
const F001 = f32(0.01);

// make-up gains: every model about as loud as its input at the default knobs
const VOWEL_MAKE_UP = 3.0, COMET_MAKE_UP = 0.7, OCTI_MAKE_UP = 0.15, SYNTH_MAKE_UP = 0.35,
      ATTACK_MAKE_UP = 0.17, STRING_MAKE_UP = 0.22, GROWLER_MAKE_UP = 0.28;

export class FilterFx {
  prepare(sampleRate, maxBlock) {
    this.variant = this.variant || 0;
    this.knobs = this.knobs || Float32Array.of(0, 0, 0, 0, 100);
    this.mixTarget = this.mixTarget === undefined ? 1 : this.mixTarget;
    this.fs = sampleRate;
    this.ctl = 16 * Math.max(1, Math.floor(this.fs / 48000 + 0.5));
    this.invCtl = f32(1 / this.ctl);
    this.ctlTime = this.ctl / this.fs;
    this.fcMax = Math.min(16000, 0.42 * this.fs);
    this.c2 = this.slewCoef(0.002);
    this.c5 = this.slewCoef(0.005);
    this.c10 = this.slewCoef(0.010);
    this.c20 = this.slewCoef(0.020);
    this.c40 = this.slewCoef(0.040);
    this.c120 = this.slewCoef(0.120);
    this.envAttack = 1 / (1 + 0.004 * this.fs);
    this.envRelease = 1 / (1 + 0.120 * this.fs);

    this.svfL = []; this.svfR = []; this.coef = []; this.ramp = []; this.slew = [];
    for (let i = 0; i < NUM_COMETS; ++i) { this.svfL.push(new Svf()); this.svfR.push(new Svf()); this.coef.push(new SvfCoef()); }
    for (let i = 0; i < 3; ++i) this.ramp.push(new Ramp());
    for (let i = 0; i < 7; ++i) this.slew.push(new Slew());
    this.mixRamp = new Ramp();
    this.mixSlew = new Slew();

    this.attack = new AttackDetector();
    this.tracker = new PitchTracker();
    this.meter = new LevelMeter();
    this.lcg = new Lcg();
    this.attack.prepare(this.fs);
    this.tracker.prepare(this.fs);
    this.meter.prepare(Math.ceil(0.0135 / this.ctlTime));
    this.reset();
  }

  reset() {
    this.first = true;
    this.ctlCount = 0;

    for (let i = 0; i < NUM_COMETS; ++i) {
      this.svfL[i].reset();
      this.svfR[i].reset();
      this.coef[i].clear();
    }
    for (const r of this.ramp) r.clear();
    for (const s of this.slew) s.v = 0;
    this.mixRamp.clear();
    this.mixSlew.v = 0;

    this.attack.reset();
    this.tracker.reset();
    this.meter.reset();
    this.lcg.state = 20260930;
    this.randPrev = this.lcg.next01();
    this.randNext = this.lcg.next01();
    this.phase = 0;
    this.sweep = 1;
    this.env = 0;
    this.step = 0;
    this.ph1 = this.ph2 = 0;
    this.inc1 = this.inc2 = 0;
    this.oscHz = 110;
    this.width1 = this.width2 = 0.5;
    this.amp = this.fade = 0;
    this.wave = 0;
    this.wasVoiced = false;
  }

  setModel(variant) { this.variant = clamp(variant | 0, 0, NUM_VARIANTS - 1); }

  setParameters(k) {
    for (let i = 0; i < 5; ++i) this.knobs[i] = k[i];

    this.mixTarget = clamp(f32(this.knobs[4] * F001), 0, 1);
  }

  process(left, right, numSamples) {
    switch (this.variant) {
      case VOICE_BOX:    this.processVowel(left, right, numSamples, false); break;
      case V_TRON:       this.processVowel(left, right, numSamples, true); break;
      case Q_FILTER:     this.processSingle(left, right, numSamples, true, false); break;
      case SEEKER:       this.processSingle(left, right, numSamples, true, false); break;
      case OBI_WAH:      this.processSingle(left, right, numSamples, false, false); break;
      case TRON_UP:      this.processSingle(left, right, numSamples, true, true); break;
      case TRON_DOWN:    this.processSingle(left, right, numSamples, true, true); break;
      case THROBBER:     this.processLowPass(left, right, numSamples, false); break;
      case SLOW_FILTER:  this.processLowPass(left, right, numSamples, true); break;
      case SPIN_CYCLE:   this.processSpin(left, right, numSamples); break;
      case COMET_TRAILS: this.processComet(left, right, numSamples); break;
      case OCTISYNTH:    this.processOcti(left, right, numSamples); break;
      default:           this.processSynth(left, right, numSamples); break;
    }
  }

  /** Coefficient of a one-pole smoother that runs once per control interval. */
  slewCoef(seconds) {
    const x = this.ctlTime / seconds;
    return x / (1 + x);
  }

  gOf(hz) { return Math.tan((PI * clamp(hz, 20, this.fcMax)) / this.fs); }

  /** True when a control interval starts: time for the model's control code. Moves the Mix knob on the way. */
  controlDue() {
    if (this.ctlCount > 0) {
      --this.ctlCount;
      return false;
    }
    this.ctlCount = this.ctl - 1;
    this.mixRamp.aim(this.mixSlew.next(this.mixTarget, this.c10, this.first), this.invCtl, this.first);
    return true;
  }

  // ==========================================================================
  // Voice Box and V-Tron: three parallel formant resonators morphing from the Start to the End vowel
  controlVowel(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c10 = this.c10;
    let start = 0, end = 0;
    let pos = 0;

    if (this.variant === VOICE_BOX) {
      start = choiceOf(knobs[1], 4);
      end = choiceOf(knobs[2], 4);
      this.phase += knobs[0] * this.ctlTime;
      if (this.phase >= 1) {
        this.phase -= 1;
        this.randPrev = this.randNext;
        this.randNext = this.lcg.next01();
      }

      const phase = this.phase;
      switch (choiceOf(knobs[3], 3)) {
        case 0:  pos = 0.5 - 0.5 * Math.cos(TWO_PI * phase); break;
        case 1:  pos = smooth01((0.7 - Math.abs(2 * phase - 1)) * 2.5); break;
        case 2:  pos = phase < 0.85 ? phase / 0.85 : (1 - phase) / 0.15; break;
        default: pos = this.randPrev + (this.randNext - this.randPrev) * smooth01(2 * phase); break;
      }
    } else {
      start = choiceOf(knobs[0], 4);
      end = choiceOf(knobs[1], 4);
      if (this.sweep < 1) this.sweep = Math.min(1, this.sweep + knobs[2] * this.ctlTime);

      pos = choiceOf(knobs[3], 1) === 0 ? smooth01(this.sweep) : 0.5 - 0.5 * Math.cos(TWO_PI * this.sweep);
    }

    pos = slew[0].next(pos, this.c5, snap);

    for (let j = 0; j < 3; ++j) {
      const hz = slew[1 + j].next(VOWEL_HZ[start][j] * Math.pow(VOWEL_HZ[end][j] / VOWEL_HZ[start][j], pos), c10, snap);
      const gain = slew[4 + j].next(VOWEL_GAIN[start][j] + (VOWEL_GAIN[end][j] - VOWEL_GAIN[start][j]) * pos, c10, snap);
      const k = 1 / VOWEL_Q[j];
      this.coef[j].aim(this.gOf(hz), k, invCtl, snap);
      this.ramp[j].aim((j === 1 ? -VOWEL_MAKE_UP : VOWEL_MAKE_UP) * gain * k, invCtl, snap);
    }
  }

  processVowel(left, right, numSamples, triggered) {
    const mix = this.mixRamp, attack = this.attack;
    const c0 = this.coef[0], c1 = this.coef[1], c2 = this.coef[2], r0 = this.ramp[0], r1 = this.ramp[1], r2 = this.ramp[2];
    const l0 = this.svfL[0], l1 = this.svfL[1], l2 = this.svfL[2], q0 = this.svfR[0], q1 = this.svfR[1], q2 = this.svfR[2];

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];
      const mono = f32(0.5 * (inL + inR));

      if (triggered && attack.process(Math.abs(mono))) this.sweep = 0;

      if (this.controlDue()) {
        this.controlVowel(this.first);
        this.first = false;
        for (let j = 0; j < 3; ++j) { this.svfL[j].flush(); this.svfR[j].flush(); }
      }

      c0.step();
      c1.step();
      c2.step();
      const w0 = r0.step(), w1 = r1.step(), w2 = r2.step();
      const mixNow = mix.step();

      if (triggered) { // V-Tron: true stereo
        l0.process(inL, c0);
        l1.process(inL, c1);
        l2.process(inL, c2);
        q0.process(inR, c0);
        q1.process(inR, c1);
        q2.process(inR, c2);
        const wetL = softLimit(w0 * l0.bp + w1 * l1.bp + w2 * l2.bp);
        const wetR = softLimit(w0 * q0.bp + w1 * q1.bp + w2 * q2.bp);
        left[i] = inL + mixNow * (wetL - inL);
        right[i] = inR + mixNow * (wetR - inR);
      } else { // Voice Box: mono effect
        l0.process(mono, c0);
        l1.process(mono, c1);
        l2.process(mono, c2);
        const wet = softLimit(w0 * l0.bp + w1 * l1.bp + w2 * l2.bp);
        left[i] = inL + mixNow * (wet - inL);
        right[i] = inR + mixNow * (wet - inR);
      }
    }
  }

  // ==========================================================================
  // Q Filter, Seeker, Obi Wah, Tron Up / Down: one two-pole state-variable filter with LP / BP / HP outputs
  controlSingle(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c5 = this.c5, c10 = this.c10, c20 = this.c20;
    let hz = 1000, q = 1, gain = 1;
    let type = 1;

    switch (this.variant) {
      case Q_FILTER:
        hz = 80 * Math.pow(100, slew[0].next(knobs[0] * 0.01, c20, snap));
        q = 0.5 * Math.pow(32, slew[1].next(knobs[1] * 0.01, c20, snap));
        gain = slew[2].next(Math.pow(10, knobs[2] * 0.05), c20, snap);
        type = choiceOf(knobs[3], 2);
        break;

      case SEEKER: {
        const steps = 2 + choiceOf(knobs[3], 7);
        this.phase += knobs[0] * this.ctlTime;
        if (this.phase >= 1) {
          this.phase -= 1;
          ++this.step;
        }
        if (this.step >= steps) this.step = 0;

        hz = seekerHz(slew[0].next(seekerPosition(choiceOf(knobs[1], 7), this.step, steps), c5, snap));
        q = 1.5 * Math.pow(10, slew[1].next(knobs[2] * 0.01, c20, snap));
        break;
      }

      case OBI_WAH:
        this.phase += knobs[0] * this.ctlTime;
        if (this.phase >= 1) {
          this.phase -= 1;
          this.randPrev = this.lcg.next01();
        }
        hz = 200 * Math.pow(10, slew[3].next(knobs[1] * 0.01, c20, snap)) * Math.pow(8, slew[0].next(this.randPrev - 0.5, c5, snap));
        q = 1.5 * Math.pow(10, slew[1].next(knobs[2] * 0.01, c20, snap));
        type = choiceOf(knobs[3], 2);
        break;

      default: { // Tron Up / Tron Down
        const drive = this.env / (this.env + 0.2); // 0..1: how hard the strings are picked
        const bottom = slew[3].next(choiceOf(knobs[2], 1) === 0 ? 150 : 60, c20, snap)
                         * Math.pow(10, slew[0].next(knobs[0] * 0.01, c20, snap));
        hz = bottom * Math.pow(2, 3.5 * (this.variant === TRON_UP ? drive : 1 - drive));
        q = 0.7 * Math.pow(20, slew[1].next(knobs[1] * 0.01, c20, snap));
        type = choiceOf(knobs[3], 2);
        break;
      }
    }

    const k = 1 / q;
    this.coef[0].aim(this.gOf(hz), k, invCtl, snap);
    this.ramp[0].aim(slew[4].next(type === 0 ? 1 : 0, c10, snap) * lowPassNorm(k) * gain, invCtl, snap);
    this.ramp[1].aim(slew[5].next(type === 1 ? 1 : 0, c10, snap) * bandPassNorm(k) * gain, invCtl, snap);
    this.ramp[2].aim(slew[6].next(type === 2 ? 1 : 0, c10, snap) * lowPassNorm(k) * gain, invCtl, snap);
  }

  processSingle(left, right, numSamples, stereo, follow) {
    const mix = this.mixRamp, c0 = this.coef[0], r0 = this.ramp[0], r1 = this.ramp[1], r2 = this.ramp[2];
    const l0 = this.svfL[0], q0 = this.svfR[0];
    const envAttack = this.envAttack, envRelease = this.envRelease;

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];
      const mono = f32(0.5 * (inL + inR));

      if (follow) {
        const a = Math.abs(mono);
        this.env += (a > this.env ? envAttack : envRelease) * (a - this.env);
      }

      if (this.controlDue()) {
        this.controlSingle(this.first);
        this.first = false;
        l0.flush();
        q0.flush();
        if (this.env < 1e-20) this.env = 0;
      }

      c0.step();
      const wl = r0.step(), wb = r1.step(), wh = r2.step();
      const mixNow = mix.step();

      if (stereo) {
        l0.process(inL, c0);
        q0.process(inR, c0);
        const wetL = softLimit(wl * l0.lp + wb * l0.bp + wh * l0.hp);
        const wetR = softLimit(wl * q0.lp + wb * q0.bp + wh * q0.hp);
        left[i] = inL + mixNow * (wetL - inL);
        right[i] = inR + mixNow * (wetR - inR);
      } else {
        l0.process(mono, c0);
        const wet = softLimit(wl * l0.lp + wb * l0.bp + wh * l0.hp);
        left[i] = inL + mixNow * (wet - inL);
        right[i] = inR + mixNow * (wet - inR);
      }
    }
  }

  // ==========================================================================
  // Throbber (LFO) and Slow Filter (attack-triggered sweep): four-pole low-pass
  controlLowPass(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c20 = this.c20;
    let hz = 1000;

    if (this.variant === THROBBER) {
      this.phase += knobs[0] * this.ctlTime;
      if (this.phase >= 1) this.phase -= 1;

      const phase = this.phase;
      let lfo = 0;
      switch (choiceOf(knobs[3], 3)) {
        case 0:  lfo = 2 * phase - 1; break;
        case 1:  lfo = 1 - 2 * phase; break;
        case 2:  lfo = 1 - 4 * Math.abs(phase - 0.5); break;
        default: lfo = phase < 0.5 ? 1 : -1; break;
      }

      hz = 150 * Math.pow(20, slew[0].next(knobs[1] * 0.01, c20, snap)) * Math.pow(2, 1.5 * slew[2].next(lfo, this.c5, snap));
    } else {
      if (this.sweep < 1) this.sweep = Math.min(1, this.sweep + knobs[2] * this.ctlTime);

      const pos = slew[2].next(this.sweep, this.c10, snap);
      const up = slew[3].next(choiceOf(knobs[3], 1) === 0 ? 1 : 0, c20, snap);
      const dark = 100 * Math.pow(20, slew[0].next(knobs[0] * 0.01, c20, snap));
      hz = dark * Math.pow(10000 / dark, up * pos + (1 - up) * (1 - pos));
    }

    const k = 1 / (0.7 * Math.pow(16, slew[1].next(knobs[this.variant === THROBBER ? 2 : 1] * 0.01, c20, snap)));
    const g = this.gOf(hz);
    this.coef[0].aim(g, k, invCtl, snap);
    this.coef[1].aim(g, 1.4142, invCtl, snap);
    this.ramp[0].aim(lowPassNorm(k), invCtl, snap);
  }

  processLowPass(left, right, numSamples, triggered) {
    const mix = this.mixRamp, attack = this.attack, c0 = this.coef[0], c1 = this.coef[1], r0 = this.ramp[0];
    const l0 = this.svfL[0], l1 = this.svfL[1], q0 = this.svfR[0], q1 = this.svfR[1];

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];

      if (triggered && attack.process(Math.abs(f32(0.5 * (inL + inR))))) this.sweep = 0;

      if (this.controlDue()) {
        this.controlLowPass(this.first);
        this.first = false;
        for (let j = 0; j < 2; ++j) { this.svfL[j].flush(); this.svfR[j].flush(); }
      }

      c0.step();
      c1.step();
      const gain = r0.step();
      const mixNow = mix.step();

      l0.process(inL, c0);
      l1.process(l0.lp, c1);
      q0.process(inR, c0);
      q1.process(q0.lp, c1);
      const wetL = softLimit(gain * l1.lp);
      const wetR = softLimit(gain * q1.lp);
      left[i] = inL + mixNow * (wetL - inL);
      right[i] = inR + mixNow * (wetR - inR);
    }
  }

  // ==========================================================================
  // Spin Cycle: two wah band-passes, left and right, swept in opposite directions
  controlSpin(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c20 = this.c20;
    this.phase += knobs[0] * (1 + 0.06 * knobs[3] * this.env / (this.env + 0.1)) * this.ctlTime;
    if (this.phase >= 1) this.phase -= 1;

    const lfo = 1.2 * Math.sin(TWO_PI * this.phase);
    const centre = 300 * Math.pow(9, slew[0].next(knobs[1] * 0.01, c20, snap));
    const k = 1 / (1.5 * Math.pow(10, slew[1].next(knobs[2] * 0.01, c20, snap)));
    this.coef[0].aim(this.gOf(centre * Math.pow(2, lfo)), k, invCtl, snap);
    this.coef[1].aim(this.gOf(centre * Math.pow(2, -lfo)), k, invCtl, snap);
    this.ramp[0].aim(bandPassNorm(k), invCtl, snap);
  }

  processSpin(left, right, numSamples) {
    const mix = this.mixRamp, c0 = this.coef[0], c1 = this.coef[1], r0 = this.ramp[0], l0 = this.svfL[0], q0 = this.svfR[0];
    const envAttack = this.envAttack, envRelease = this.envRelease;

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];
      const a = Math.abs(f32(0.5 * (inL + inR)));
      this.env += (a > this.env ? envAttack : envRelease) * (a - this.env);

      if (this.controlDue()) {
        this.controlSpin(this.first);
        this.first = false;
        l0.flush();
        q0.flush();
        if (this.env < 1e-20) this.env = 0;
      }

      c0.step();
      c1.step();
      const gain = r0.step();
      const mixNow = mix.step();

      l0.process(inL, c0);
      q0.process(inR, c1);
      const wetL = softLimit(gain * l0.bp);
      const wetR = softLimit(gain * q0.bp);
      left[i] = inL + mixNow * (wetL - inL);
      right[i] = inR + mixNow * (wetR - inR);
    }
  }

  // ==========================================================================
  // Comet Trails: seven band-passes chasing each other along one sine sweep
  controlComet(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c20 = this.c20;
    this.phase += knobs[0] * this.ctlTime;
    if (this.phase >= 1) this.phase -= 1;

    const centre = 300 * Math.pow(10, slew[0].next(knobs[1] * 0.01, c20, snap));
    const k = 1 / (2 * Math.pow(10, slew[1].next(knobs[2] * 0.01, c20, snap)));
    for (let j = 0; j < NUM_COMETS; ++j)
      this.coef[j].aim(this.gOf(centre * Math.pow(2, 1.5 * Math.sin(TWO_PI * (this.phase - 0.06 * j)))), k, invCtl, snap);

    this.ramp[0].aim(COMET_MAKE_UP * bandPassNorm(k) * slew[2].next(Math.pow(10, knobs[3] * 0.05), c20, snap), invCtl, snap);
  }

  processComet(left, right, numSamples) {
    const mix = this.mixRamp, coef = this.coef, svfL = this.svfL, svfR = this.svfR, r0 = this.ramp[0];

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];

      if (this.controlDue()) {
        this.controlComet(this.first);
        this.first = false;
        for (let j = 0; j < NUM_COMETS; ++j) { svfL[j].flush(); svfR[j].flush(); }
      }

      let sumL = 0, sumR = 0;
      for (let j = 0; j < NUM_COMETS; ++j) {
        const c = coef[j], l = svfL[j], r = svfR[j];
        c.step();
        l.process(inL, c);
        r.process(inR, c);
        sumL += COMET_LEFT[j] * l.bp;
        sumR += COMET_RIGHT[j] * r.bp;
      }

      const gain = r0.step();
      const mixNow = mix.step();
      const wetL = softLimit(gain * sumL);
      const wetR = softLimit(gain * sumR);
      left[i] = inL + mixNow * (wetL - inL);
      right[i] = inR + mixNow * (wetR - inR);
    }
  }

  // ==========================================================================
  // Octisynth: the playing level sets an oscillator's frequency; ring modulator, vibrato, second harmonic
  controlOcti(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c20 = this.c20, env = this.env;
    this.phase += knobs[0] * this.ctlTime;
    if (this.phase >= 1) this.phase -= 1;

    const drive = env / (env + 0.1);
    const hz = 80 * Math.pow(2, 4.5 * drive + 0.005 * knobs[3] * Math.sin(TWO_PI * this.phase));
    this.inc1 = Math.min(0.2, hz / this.fs);

    const k = 1 / (0.7 * Math.pow(16, slew[1].next(knobs[2] * 0.01, c20, snap)));
    this.coef[0].aim(this.gOf(2 * hz), k, invCtl, snap);
    this.ramp[0].aim(OCTI_MAKE_UP * lowPassNorm(k) * drive * smooth01((env - 0.002) / 0.006), invCtl, snap); // the oscillator's level
    this.ramp[1].aim(slew[0].next(knobs[1] * 0.01, c20, snap), invCtl, snap);                               // second harmonic
    this.ramp[2].aim(OCTI_MAKE_UP * lowPassNorm(k) * 6, invCtl, snap);                                      // ring modulator
  }

  processOcti(left, right, numSamples) {
    const mix = this.mixRamp, c0 = this.coef[0], r0 = this.ramp[0], r1 = this.ramp[1], r2 = this.ramp[2], l0 = this.svfL[0];
    const envAttack = this.envAttack, envRelease = this.envRelease;

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];
      const mono = f32(0.5 * (inL + inR));
      const a = Math.abs(mono);
      this.env += (a > this.env ? envAttack : envRelease) * (a - this.env);

      if (this.controlDue()) {
        this.controlOcti(this.first);
        this.first = false;
        l0.flush();
        if (this.env < 1e-20) this.env = 0;
      }

      this.ph1 += this.inc1;
      if (this.ph1 >= 1) this.ph1 -= 1;

      const level = r0.step(), second = r1.step(), ring = r2.step();
      const osc = f32(Math.sin(TWO_PI * this.ph1)) + second * f32(Math.sin(2 * TWO_PI * this.ph1));
      c0.step();
      l0.process(osc * (level + ring * mono), c0);

      const wet = softLimit(l0.lp);
      const mixNow = mix.step();
      left[i] = inL + mixNow * (wet - inL);
      right[i] = inR + mixNow * (wet - inR);
    }
  }

  // ==========================================================================
  // Synth O Matic, Attack Synth, Synth String, Growler: pitch-tracked oscillators, gated by the playing level
  controlSynth(snap) {
    const knobs = this.knobs, slew = this.slew, invCtl = this.invCtl, c2 = this.c2, c20 = this.c20, tracker = this.tracker;
    const level = this.meter.endBlock();
    tracker.level = level;

    const voiced = tracker.voiced;
    const target = tracker.freq * Math.pow(2, knobs[3] / 12);
    if (voiced) this.oscHz = (snap || !this.wasVoiced) ? target : this.oscHz + this.c5 * (target - this.oscHz);
    this.wasVoiced = voiced;

    const open = smooth01((level - 0.003) / 0.009); // fades out before the tracker's gate closes
    let wanted = voiced ? open * level / (level + 0.1) : 0, rise = c2, fall = this.c40, gain = 1;
    let waveKnob = -1;

    switch (this.variant) {
      case SYNTH_O_MATIC: {
        const k = 1 / (0.7 * Math.pow(16, slew[1].next(knobs[1] * 0.01, c20, snap)));
        this.coef[0].aim(this.gOf(100 * Math.pow(80, slew[0].next(knobs[0] * 0.01, c20, snap))), k, invCtl, snap);
        gain = SYNTH_MAKE_UP * lowPassNorm(k);
        waveKnob = choiceOf(knobs[2], 7);
        break;
      }

      case ATTACK_SYNTH: {
        if (!voiced) this.sweep = 0;
        else if (this.sweep < 1) this.sweep = Math.min(1, this.sweep + this.ctlTime / (1.5 * Math.pow(1 / 300, knobs[2] * 0.01)));

        const stop = 200 * Math.pow(40, slew[0].next(knobs[0] * 0.01, c20, snap));
        const g = this.gOf(90 * Math.pow(stop / 90, slew[1].next(this.sweep, this.c5, snap)));
        this.coef[0].aim(g, 0.4, invCtl, snap);
        this.coef[1].aim(g, 1.4142, invCtl, snap);
        gain = ATTACK_MAKE_UP;
        waveKnob = choiceOf(knobs[1], 2);

        this.phase += 0.8 * this.ctlTime;
        if (this.phase >= 1) this.phase -= 1;
        this.width1 = waveKnob === 1 ? 0.5 + 0.4 * Math.sin(TWO_PI * this.phase) : 0.5;
        break;
      }

      case SYNTH_STRING: {
        this.phase += knobs[0] * this.ctlTime;
        if (this.phase >= 1) this.phase -= 1;

        this.width1 = 0.5 + 0.38 * Math.sin(TWO_PI * this.phase);
        this.width2 = 0.5 + 0.38 * Math.sin(TWO_PI * this.phase + 1.9);
        this.coef[0].aim(this.gOf(300 * Math.pow(27, slew[0].next(knobs[1] * 0.01, c20, snap))), 1.25, invCtl, snap);

        const x = this.ctlTime / (0.005 * Math.pow(400, knobs[2] * 0.01));
        rise = x / (1 + x);
        fall = this.c120;
        wanted = voiced ? open * level / (level + 0.04) : 0;
        gain = STRING_MAKE_UP;
        break;
      }

      default: { // Growler
        this.phase += knobs[0] * this.ctlTime;
        if (this.phase >= 1) this.phase -= 1;

        this.width1 = 0.5 + 0.42 * Math.sin(TWO_PI * this.phase);
        const k = 1 / (0.7 * Math.pow(20, slew[1].next(knobs[2] * 0.01, c20, snap)));
        this.coef[0].aim(this.gOf(80 * Math.pow(10, slew[0].next(knobs[1] * 0.01, c20, snap)) * Math.pow(16, this.env / (this.env + 0.1))), k, invCtl, snap);
        gain = GROWLER_MAKE_UP * lowPassNorm(k);
        break;
      }
    }

    // a wave switch fades the oscillator out, changes it and fades back in
    if (waveKnob >= 0) {
      if (snap) { this.wave = waveKnob; this.fade = 1; }
      else if (waveKnob !== this.wave) { this.fade += c2 * (0 - this.fade); if (this.fade < 0.003) this.wave = waveKnob; }
      else this.fade += c2 * (1 - this.fade);
      gain *= this.fade;
    }

    this.amp = snap ? wanted : this.amp + (wanted > this.amp ? rise : fall) * (wanted - this.amp);
    if (this.amp < 1e-20) this.amp = 0;
    this.ramp[0].aim(this.amp * gain, invCtl, snap);

    this.inc1 = Math.min(0.2, this.oscHz / this.fs);
    this.inc2 = this.inc1;
    if (this.variant === SYNTH_O_MATIC) this.inc2 = this.wave === 5 ? 1.007 * this.inc1 : 0.5 * this.inc1;
    else if (this.variant === SYNTH_STRING) { this.inc1 *= 0.998; this.inc2 *= 1.002; }
  }

  processSynth(left, right, numSamples) {
    const mix = this.mixRamp, tracker = this.tracker, meter = this.meter, attack = this.attack, variant = this.variant;
    const c0 = this.coef[0], c1 = this.coef[1], r0 = this.ramp[0], l0 = this.svfL[0], l1 = this.svfL[1];
    const envAttack = this.envAttack, envRelease = this.envRelease;

    for (let i = 0; i < numSamples; ++i) {
      const inL = left[i], inR = right[i];
      const mono = f32(0.5 * (inL + inR));
      const a = Math.abs(mono);
      tracker.process(mono);
      meter.push(a);

      if (variant === ATTACK_SYNTH) {
        if (attack.process(a)) this.sweep = 0;
      } else if (variant === GROWLER) {
        this.env += (a > this.env ? envAttack : envRelease) * (a - this.env);
      }

      if (this.controlDue()) {
        this.controlSynth(this.first);
        this.first = false;
        l0.flush();
        l1.flush();
        if (this.env < 1e-20) this.env = 0;
      }

      const inc1 = this.inc1, inc2 = this.inc2;
      let ph1 = this.ph1 + inc1;
      if (ph1 >= 1) ph1 -= 1;
      let ph2 = this.ph2 + inc2;
      if (ph2 >= 1) ph2 -= 1;
      this.ph1 = ph1;
      this.ph2 = ph2;

      let osc = 0;
      if (variant === SYNTH_O_MATIC) {
        switch (this.wave) {
          case 0:  osc = sawWave(ph1, inc1); break;
          case 1:  osc = pulseWave(ph1, inc1, 0.5); break;
          case 2:  osc = pulseWave(ph1, inc1, 0.2); break;
          case 3:  osc = 1.6 * triangleWave(ph1, inc1); break;
          case 4:  osc = 0.6 * (sawWave(ph1, inc1) + pulseWave(ph1, inc1, 0.5)); break;
          case 5:  osc = 0.6 * (sawWave(ph1, inc1) + sawWave(ph2, inc2)); break;
          case 6: {
            let octave = ph1 + ph1;
            if (octave >= 1) octave -= 1;
            osc = 0.65 * sawWave(ph1, inc1) + 0.5 * sawWave(octave, inc1 + inc1);
            break;
          }
          default: osc = 0.6 * (pulseWave(ph1, inc1, 0.5) + pulseWave(ph2, inc2, 0.5)); break;
        }
      } else if (variant === ATTACK_SYNTH) {
        osc = this.wave === 2 ? sawWave(ph1, inc1) : pulseWave(ph1, inc1, this.width1);
      } else if (variant === SYNTH_STRING) {
        osc = 0.6 * (pulseWave(ph1, inc1, this.width1) + pulseWave(ph2, inc2, this.width2));
      } else {
        osc = 0.6 * pulseWave(ph1, inc1, this.width1) + 0.5 * sawWave(ph1, inc1);
      }

      c0.step();
      l0.process(f32(osc) * r0.step(), c0);
      let out = l0.lp;

      if (variant === ATTACK_SYNTH) {
        c1.step();
        l1.process(out, c1);
        out = l1.lp;
      }

      const wet = softLimit(out);
      const mixNow = mix.step();
      left[i] = inL + mixNow * (wet - inL);
      right[i] = inR + mixNow * (wet - inR);
    }
  }
}

// In the order of the variants
const speed = (def) => hertz("Speed", 0.05, 10, def, 1);
const mixKnob = () => percent("Mix", 100);
const pitch = () => semitones("Pitch", -12, 12, 0);
const type = (def) => choice("Type", TYPE_NAMES, def);
const make = (key, name, variant, basedOn, knobs) => model(key, name, CATEGORY.filter, ENGINE.filterFx, variant, basedOn, knobs);

export const FILTER_MODELS = [
  make("voice_box", "Voice Box", VOICE_BOX, "Talk box: vocoders, vocal tracts and surgical tubing",
    [speed(1.5), choice("Start", VOWEL_NAMES, 0), choice("End", VOWEL_NAMES, 2), choice("Auto", AUTO_NAMES, 0), mixKnob()]),
  make("v_tron", "V-Tron", V_TRON, "Voice Box triggered like a Mu-Tron III",
    [choice("Start", VOWEL_NAMES, 4), choice("End", VOWEL_NAMES, 0), hertz("Speed", 0.2, 20, 5, 3), choice("Mode", SWEEP_NAMES, 0), mixKnob()]),
  make("q_filter", "Q Filter", Q_FILTER, "Parked wah",
    [percent("Freq", 50), percent("Q", 60), decibels("Gain", -12, 12, 0), type(1), mixKnob()]),
  make("seeker", "Seeker", SEEKER, "Z.Vex Seek Wah",
    [hertz("Speed", 0.5, 20, 6, 5), choice("Freq", PATTERN_NAMES, 3), percent("Q", 60), choice("Steps", STEP_NAMES, 6), mixKnob()]),
  make("obi_wah", "Obi Wah", OBI_WAH, "Oberheim voltage-controlled sample & hold filter",
    [hertz("Speed", 0.5, 20, 6, 5), percent("Freq", 50), percent("Q", 60), type(1), mixKnob()]),
  make("tron_up", "Tron Up", TRON_UP, "Mu-Tron III, drive switch up",
    [percent("Freq", 30), percent("Q", 60), choice("Range", RANGE_NAMES, 0), type(1), mixKnob()]),
  make("tron_down", "Tron Down", TRON_DOWN, "Mu-Tron III, drive switch down",
    [percent("Freq", 30), percent("Q", 60), choice("Range", RANGE_NAMES, 0), type(1), mixKnob()]),
  make("throbber", "Throbber", THROBBER, "Electrix Filter Factory, LFO section",
    [speed(2), percent("Freq", 50), percent("Q", 50), choice("Wave", LFO_NAMES, 2), mixKnob()]),
  make("slow_filter", "Slow Filter", SLOW_FILTER, "Line 6 original: attack-triggered low-pass sweep",
    [percent("Freq", 30), percent("Q", 35), hertz("Speed", 0.1, 10, 1.5, 1.5), choice("Mode", MODE_NAMES, 0), mixKnob()]),
  make("spin_cycle", "Spin Cycle", SPIN_CYCLE, "Craig Anderton's Wah/Anti-Wah",
    [speed(1), percent("Freq", 50), percent("Q", 60), percent("VolSens", 30), mixKnob()]),
  make("comet_trails", "Comet Trails", COMET_TRAILS, "Line 6 original: seven filters chasing each other",
    [speed(0.5), percent("Freq", 50), percent("Q", 70), decibels("Gain", -12, 12, 0), mixKnob()]),
  make("octisynth", "Octisynth", OCTISYNTH, "Line 6 original: level-controlled oscillator, ring modulator and vibrato",
    [hertz("Speed", 0.1, 15, 5, 3), percent("Freq", 40), percent("Q", 50), percent("Depth", 30), mixKnob()]),
  make("synth_o_matic", "Synth O Matic", SYNTH_O_MATIC, "Moog modular and Oberheim SEM waveforms",
    [percent("Freq", 60), percent("Q", 45), choice("Wave", SYNTH_WAVE_NAMES, 0), pitch(), mixKnob()]),
  make("attack_synth", "Attack Synth", ATTACK_SYNTH, "Korg X911 guitar synthesizer",
    [percent("Freq", 65), choice("Wave", ATTACK_WAVE_NAMES, 0), percent("Speed", 50), pitch(), mixKnob()]),
  make("synth_string", "Synth String", SYNTH_STRING, "Roland GR-700 guitar synthesizer (strings)",
    [speed(3), percent("Freq", 60), percent("Attack", 50), pitch(), mixKnob()]),
  make("growler", "Growler", GROWLER, "Roland GR-700 tone into a Mu-Tron III",
    [speed(1.5), percent("Freq", 35), percent("Q", 65), pitch(), mixKnob()]),
];
