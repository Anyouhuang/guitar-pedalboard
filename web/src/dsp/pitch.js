// The HD500X's Pitch models (Source/DSP/fx/Pitch.h): Bass Octaver, Pitch Glide, Smart Harmony. Mono.
//
// Ported line by line. Everything that leads to a decision (tracker, octaver, read-head positions, ratios) is
// double precision arithmetic with + - * / only in both versions, in the same order, so the JavaScript takes
// exactly the decisions of the C++; keep it that way when editing. Only the audio itself differs in precision.
import { PI, clamp, f32, Smoothed } from "./core.js";
import { CATEGORY, ENGINE, model, percent, semitones, choice } from "./modeltypes.js";

const KEY_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];
const SHIFT_NAMES = ["-8th", "-7th", "-6th", "-5th", "-4th", "-3rd", "-2nd", "+2nd", "+3rd", "+4th", "+5th", "+6th", "+7th", "+8th"];
const SCALE_NAMES = ["Major", "Minor", "Pent Major", "Pent Minor", "Harm Minor", "Mel Minor", "Whole Tone", "Diminished"];

/** The interval number of each Shift choice (-3 = a third below, 8 = an octave above). */
const SHIFT_INTERVALS = [-8, -7, -6, -5, -4, -3, -2, 2, 3, 4, 5, 6, 7, 8];
const SCALE_NOTES = [
  [0, 2, 4, 5, 7, 9, 11],     // major
  [0, 2, 3, 5, 7, 8, 10],     // natural minor
  [0, 2, 4, 7, 9],            // major pentatonic
  [0, 3, 5, 7, 10],           // minor pentatonic
  [0, 2, 3, 5, 7, 8, 11],     // harmonic minor
  [0, 2, 3, 5, 7, 9, 11],     // melodic minor (ascending)
  [0, 2, 4, 6, 8, 10],        // whole tone
  [0, 2, 3, 5, 6, 8, 9, 11],  // diminished (whole-half)
];
const PREFERRED = [[2, 1, 3, 0], [4, 3, 5, 2], [5, 6, 4, 7], [7, 6, 8, 5], [9, 8, 10, 7], [11, 10, 9, 12]];

/** Semitones to shift a played note by so that the harmony stays inside the scale (see Pitch.h). */
function harmonySemitones(scale, interval, pitchClass) {
  const notes = SCALE_NOTES[scale], count = notes.length;
  let pc = pitchClass, degree = notes.indexOf(pc);
  for (let distance = 1; degree < 0 && distance <= 6; ++distance) {
    pc = (pitchClass + 12 - distance) % 12;
    degree = notes.indexOf(pc);
    if (degree < 0) { pc = (pitchClass + distance) % 12; degree = notes.indexOf(pc); }
  }

  const size = interval < 0 ? -interval : interval, direction = interval < 0 ? -1 : 1;
  if (size >= 8) return 12 * direction;
  if (size <= 1 || degree < 0) return 0;

  if (count === 7) {
    const target = degree + direction * (size - 1);
    const octave = target >= 7 ? 1 : (target < 0 ? -1 : 0);
    return notes[target - 7 * octave] + 12 * octave - notes[degree];
  }

  for (let i = 0; i < 4; ++i) {
    const shiftBy = direction * PREFERRED[size - 2][i];
    if (notes.indexOf(((pc + shiftBy) % 12 + 12) % 12) >= 0) return shiftBy;
  }
  return direction * PREFERRED[size - 2][0];
}

// ============================================================================
// Maths built from + - * / only: the same bits as the C++ (Math.pow / std::pow may differ)

/** 2^x */
function exp2Exact(x) {
  const whole = Math.floor(x + 0.5);
  const f = (x - whole) * 0.6931471805599453;
  let term = 1, sum = 1;
  for (let i = 1; i <= 13; ++i) { term *= f / i; sum += term; }
  let n = whole;
  for (; n > 0; --n) sum *= 2;
  for (; n < 0; ++n) sum *= 0.5;
  return sum;
}

/** log2 (v), v > 0 */
function log2Exact(v) {
  let exponent = 0;
  while (v >= 1.4142135623730951) { v *= 0.5; exponent += 1; }
  while (v < 0.7071067811865476) { v *= 2; exponent -= 1; }
  const z = (v - 1) / (v + 1), z2 = z * z;
  let sum = 0;
  for (let k = 12; k >= 0; --k) sum = sum * z2 + 1 / (2 * k + 1);
  return exponent + 2 * z * sum * 1.4426950408889634;
}

/** tan (x) for 0 <= x <= 0.8 */
function tanExact(x) {
  const q = x * x;
  const s = x * (1 - q / 6 * (1 - q / 20 * (1 - q / 42 * (1 - q / 72 * (1 - q / 110 * (1 - q / 156))))));
  const c = 1 - q / 2 * (1 - q / 12 * (1 - q / 30 * (1 - q / 56 * (1 - q / 90 * (1 - q / 132 * (1 - q / 182))))));
  return s / c;
}

const TINY = 1e-30; // states below this are set to zero (before they become denormals)
const BUTTERWORTH_K1 = 1.8477590650225735, BUTTERWORTH_K2 = 0.7653668647301796; // 1/Q of a 4-pole

/** Two-pole low-pass (trapezoidal state-variable filter) in double precision: part of the decision path. */
class Svf {
  constructor() { this.a1 = 1; this.a2 = 0; this.a3 = 0; this.s1 = 0; this.s2 = 0; }
  set(g, k) { this.a1 = 1 / (1 + g * (g + k)); this.a2 = g * this.a1; this.a3 = g * this.a2; }
  reset() { this.s1 = this.s2 = 0; }
  flushTiny() {
    if (Math.abs(this.s1) < TINY) this.s1 = 0;
    if (Math.abs(this.s2) < TINY) this.s2 = 0;
  }
  lowPass(x) {
    const v3 = x - this.s2;
    const v1 = this.a1 * this.s1 + this.a2 * v3;
    const v2 = this.s2 + this.a2 * this.s1 + this.a3 * v3;
    this.s1 = 2 * v1 - this.s1;
    this.s2 = 2 * v2 - this.s2;
    return v2;
  }
}

/** The same filter on the audio (float coefficients as in the C++): the shifter's anti-alias filter. */
class AudioSvf extends Svf {
  set(g, k) {
    const d = 1 / (1 + g * (g + k));
    this.a1 = f32(d); this.a2 = f32(g * d); this.a3 = f32(g * g * d);
  }
}

// ============================================================================
/** Follows the period of the input (see Pitch.h): fundamental with hysteresis, best splice lag, pick attacks. */
class Tracker {
  constructor() {
    this.period = 0; this.locked = false; this.tracking = false;
    this.jump = 0; this.jumpCorr = 0; this.onset = false; this.onsetAge = 0;
  }

  prepare(sampleRate) {
    this.decim = Math.max(1, Math.floor(sampleRate / 8000 + 0.5));
    this.rate = sampleRate / this.decim;
    this.maxLag = Math.floor(this.rate / 38 + 0.5);
    this.minLag = Math.max(2, Math.floor(this.rate / 1600));
    this.minWindow = Math.floor(this.rate * 0.004 + 0.5);
    this.hop = Math.floor(this.rate * 0.003 + 0.5);
    this.jumpMinLag = Math.floor(this.rate * 0.007 + 0.5);
    this.span = 2 * this.maxLag;

    this.ring = new Float64Array(this.span * 2);
    this.cumulative = new Float64Array(this.span + 1);
    this.nsdf = new Float64Array(this.maxLag + 2);
    this.peakLags = new Int32Array(this.maxLag + 1);
    this.numHopLevels = 12;
    this.bright = new Float64Array(this.span * 2);
    this.hopLevels = new Float64Array(this.numHopLevels);
    this.hopBrightLevels = new Float64Array(this.numHopLevels);

    const wh = 2 * PI * 30 / sampleRate, wl = 2 * PI * 1200 / sampleRate, wb = 2 * PI * 2000 / sampleRate;
    this.highPassCoef = wh / (1 + wh);
    this.lowPassCoef = wl / (1 + wl);
    this.brightCoef = wb / (1 + wb);
    this.defaultPeriod = sampleRate / 110;
    this.defaultJump = sampleRate * 0.010;
    this.reset();
  }

  reset() {
    this.ring.fill(0); this.nsdf.fill(0); this.bright.fill(0); this.hopLevels.fill(0); this.hopBrightLevels.fill(0);
    this.highPass = this.lowPass1 = this.lowPass2 = this.sum = this.brightLow = this.brightSum = 0;
    this.phase = this.hopCount = this.pos = this.hopLevelPos = 0;
    this.sinceOnset = 1000;
    this.gateOpen = this.locked = this.tracking = this.onset = false;
    this.onsetAge = 0;
    this.period = this.defaultPeriod;
    this.candidate = 0;
    this.candidateCount = this.disagreed = 0;
    this.jump = this.defaultJump;
    this.jumpCorr = 0;
  }

  /** Takes one input sample. Returns 0, 1 (a tick: one decimated sample later) or 2 (a tick with new results). */
  push(x) {
    this.highPass += this.highPassCoef * (x - this.highPass);
    this.lowPass1 += this.lowPassCoef * (x - this.highPass - this.lowPass1);
    this.lowPass2 += this.lowPassCoef * (this.lowPass1 - this.lowPass2);
    this.sum += this.lowPass2;

    // what is above 2 kHz, as energy: a pick attack shows there even while other notes ring on
    this.brightLow += this.brightCoef * (x - this.brightLow);
    this.brightSum += (x - this.brightLow) * (x - this.brightLow);

    if (++this.phase < this.decim) return 0;

    this.phase = 0;
    const v = this.sum / this.decim, e = this.brightSum / this.decim;
    this.sum = this.brightSum = 0;
    this.ring[this.pos] = v;
    this.ring[this.pos + this.span] = v;
    this.bright[this.pos] = e;
    this.bright[this.pos + this.span] = e;
    if (++this.pos === this.span) this.pos = 0;

    if (++this.hopCount < this.hop) return 1;

    this.hopCount = 0;
    this.analyse();
    return 2;
  }

  noPitch() {
    this.candidateCount = 0;
    if (++this.disagreed >= 3) { this.disagreed = 3; this.locked = false; }
    this.tracking = this.locked && this.disagreed < 2;
  }

  /** The peak position between the lags around `lag` (parabola through three points). */
  refine(lag) {
    if (lag <= 1 || lag >= this.maxLag) return lag;
    const a = this.nsdf[lag - 1], b = this.nsdf[lag], c = this.nsdf[lag + 1];
    const curve = a - 2 * b + c;
    if (curve > -1e-12) return lag;
    let d = 0.5 * (a - c) / curve;
    if (d > 0.5) d = 0.5;
    if (d < -0.5) d = -0.5;
    return lag + d;
  }

  /** Looks for a sudden rise at the end of `values` (one per tick; `samples`: they are samples, not energies):
      the last 3 ms are 3 times (5 dB) stronger than any 3 ms of the 36 ms before. Sets `riseAge`: how many
      ticks ago it began (the first value that stands out from those 36 ms). */
  rise(values, samples, lowest, history) {
    const hop = this.hop, span = this.span, newest = this.pos + span - 1;
    this.riseAge = hop;

    let recent = 0;
    for (let k = 0; k < hop; ++k) {
      const v = values[newest - k];
      recent += samples ? v * v : v;
    }
    recent /= hop;

    let loudest = 0;
    for (let i = 0; i < this.numHopLevels; ++i) if (history[i] > loudest) loudest = history[i];

    history[this.hopLevelPos] = recent;
    if (recent <= 3 * loudest + lowest) return false;

    let peak = 0;
    for (let k = 2 * hop; k < 14 * hop && k < span; ++k) {
      const v = values[newest - k], e = samples ? v * v : v;
      if (e > peak) peak = e;
    }

    const threshold = Math.max(1.5 * peak, 3 * loudest + lowest);
    for (let k = 2 * hop - 1; k >= 0; --k) {
      const v = values[newest - k];
      if ((samples ? v * v : v) > threshold) { this.riseAge = k; break; }
    }
    return true;
  }

  analyse() {
    if (Math.abs(this.brightLow) < TINY) this.brightLow = 0;
    if (Math.abs(this.highPass) < TINY) this.highPass = 0;
    if (Math.abs(this.lowPass1) < TINY) this.lowPass1 = 0;
    if (Math.abs(this.lowPass2) < TINY) this.lowPass2 = 0;

    const a = this.ring, base = this.pos; // a[base] is the oldest of the last `span` samples, a[base + span - 1] the newest
    const span = this.span, maxLag = this.maxLag, hop = this.hop, decim = this.decim;
    const cumulative = this.cumulative, nsdf = this.nsdf, peakLags = this.peakLags, hopLevels = this.hopLevels;
    const newest = base + span - 1;

    let energy = 0;
    cumulative[0] = 0;
    for (let k = 0; k < span; ++k) {
      const s = a[newest - k];
      energy += s * s;
      cumulative[k + 1] = energy;
    }

    // A pick attack: a sudden rise of the level, or of the treble only (a note picked while others ring).
    // Not again within 30 ms: the first periods of a note can look like more attacks.
    {
      const low = this.rise(a, true, 4e-6, hopLevels), lowAge = this.riseAge;
      const high = this.rise(this.bright, false, 1e-7, this.hopBrightLevels), brightAge = this.riseAge;
      if (++this.hopLevelPos === this.numHopLevels) this.hopLevelPos = 0;

      this.onset = (low || high) && this.sinceOnset >= 10;
      this.sinceOnset = this.onset ? 0 : (this.sinceOnset < 1000 ? this.sinceOnset + 1 : this.sinceOnset);
      if (this.onset) this.onsetAge = (high ? brightAge + 1 : lowAge + 3) * decim; // + the delay of the filters in front
    }

    // nothing to track below about -60 dB (closes again at -66 dB)
    const level = cumulative[maxLag] / maxLag;
    this.gateOpen = level >= (this.gateOpen ? 2.5e-7 : 1e-6);
    if (!this.gateOpen) {
      this.noPitch();
      this.jump = this.defaultJump;
      this.jumpCorr = 0;
      return;
    }

    const minWindow = this.minWindow;
    for (let lag = 1; lag <= maxLag; ++lag) {
      const n = lag > minWindow ? lag : minWindow;
      const u = base + span - n, v = u - lag;
      let s0 = 0, s1 = 0, s2 = 0, s3 = 0, i = 0;
      for (; i + 3 < n; i += 4) {
        s0 += a[u + i] * a[v + i];
        s1 += a[u + i + 1] * a[v + i + 1];
        s2 += a[u + i + 2] * a[v + i + 2];
        s3 += a[u + i + 3] * a[v + i + 3];
      }
      for (; i < n; ++i) s0 += a[u + i] * a[v + i];

      const power = cumulative[n] + (cumulative[n + lag] - cumulative[lag]);
      nsdf[lag] = power > TINY ? 2 * ((s0 + s1) + (s2 + s3)) / power : 0;
    }

    // the peak of every positive stretch after the one around lag 0
    let t = 1;
    while (t <= maxLag && nsdf[t] > 0) ++t;

    let numPeaks = 0, highest = 0;
    while (t <= maxLag) {
      while (t <= maxLag && nsdf[t] <= 0) ++t;
      if (t > maxLag) break;

      let top = t;
      for (; t <= maxLag && nsdf[t] > 0; ++t) if (nsdf[t] > nsdf[top]) top = t;

      if (top >= this.minLag && top < maxLag) { // at the very end of the range it is not a peak yet
        peakLags[numPeaks++] = top;
        if (nsdf[top] > highest) highest = nsdf[top];
      }
    }

    // ---- where the signal repeats best (at least 7 ms away), for the shifter's splices
    {
      const jumpMinLag = this.jumpMinLag;
      let best = 0;
      for (let p = 0; p < numPeaks; ++p) if (peakLags[p] >= jumpMinLag && nsdf[peakLags[p]] > best) best = nsdf[peakLags[p]];

      let chosen = -1;
      for (let p = 0; p < numPeaks && chosen < 0; ++p)
        if (peakLags[p] >= jumpMinLag && nsdf[peakLags[p]] >= 0.97 * best) chosen = peakLags[p]; // the shortest of the good ones

      if (chosen < 0) {
        chosen = jumpMinLag;
        for (let lag = jumpMinLag + 1; lag < maxLag; ++lag) if (nsdf[lag] > nsdf[chosen]) chosen = lag;
      }

      // right after a pick attack nothing repeats yet: short jumps smear an attack the least
      if (best < 0.5 && this.sinceOnset < 10) chosen = jumpMinLag;

      const value = nsdf[chosen];
      this.jump = this.refine(chosen) * decim;
      this.jumpCorr = value < 0 ? 0 : (value > 1 ? 1 : value);
    }

    // ---- the fundamental
    if (numPeaks === 0 || highest < 0.5) { this.noPitch(); return; }

    let chosen = -1;
    for (let p = 0; p < numPeaks && chosen < 0; ++p) {
      const lag = peakLags[p];
      // a shorter period only replaces the current one when it is clearly a period too (an octave up has
      // even harmonics only); the current one stays as long as its peak is reasonable
      const current = this.locked && Math.abs(this.refine(lag) * decim - this.period) <= 0.06 * this.period;
      if (nsdf[lag] >= (current ? 0.80 : 0.93) * highest) chosen = lag;
    }

    if (chosen < 0) { this.noPitch(); return; }

    const measured = this.refine(chosen) * decim;
    if (this.locked && Math.abs(measured - this.period) <= 0.06 * this.period) {
      this.period = measured;
      this.candidateCount = this.disagreed = 0;
    } else {
      this.candidateCount = this.candidateCount > 0 && Math.abs(measured - this.candidate) <= 0.03 * this.candidate ? this.candidateCount + 1 : 1;
      this.candidate = measured;
      if (this.disagreed < 3) ++this.disagreed;

      if (this.candidateCount >= 2 && nsdf[chosen] >= 0.8) { // seen twice in a row, and clearly periodic: a new note
        this.period = measured;
        this.locked = true;
        this.candidateCount = this.disagreed = 0;
      }
    }
    this.tracking = this.locked && this.disagreed < 2;
  }
}

// ============================================================================
/** Delay-line pitch shifter with splices a whole number of periods apart (see Pitch.h). */
class Shifter {
  constructor() {
    this.antiAlias1 = new AudioSvf(); this.antiAlias2 = new AudioSvf();
    this.ratio = 1; this.target = 1; this.glide = 1; this.filterRatio = -1;
    this.buffer = new Float32Array(64); this.mask = 63; this.fs = 48000;
  }

  prepare(sampleRate) {
    this.fs = sampleRate;
    let size = 64;
    while (size < Math.floor(0.08 * sampleRate) + 64) size *= 2; // 80 ms: the longest delay is about 55 ms

    this.buffer = new Float32Array(size);
    this.mask = size - 1;
    this.minDelay = Math.floor(0.0005 * sampleRate) + 2;
    this.maxDelay = size - 8;
    this.startDelay = this.minDelay + Math.floor(0.005 * sampleRate);
    this.fadeMin = Math.floor(0.001 * sampleRate);
    this.fadeMax = Math.floor(0.010 * sampleRate);
    this.upDelay = this.minDelay + Math.floor(0.0035 * sampleRate);
    this.onsetMargin = Math.floor(0.0005 * sampleRate);
    this.onsetRoom = Math.floor(0.0025 * sampleRate);
    this.attackLength = Math.floor(0.004 * sampleRate);
    this.shortJump = Math.floor(0.002 * sampleRate);
    this.longestDelay = Math.floor(0.045 * sampleRate);
    this.reset();
  }

  /** How fast the ratio follows its target (time constant in seconds). */
  setGlide(seconds) { this.glide = 1 / (1 + seconds * this.fs); }
  setRatio(newRatio) { this.target = newRatio; }

  reset() {
    this.buffer.fill(0);
    this.antiAlias1.reset(); this.antiAlias2.reset();
    this.writePos = 0;
    this.ratio = this.target;
    this.filterRatio = -1;
    this.updateAntiAlias();
    this.delay = this.oldDelay = this.startDelay;
    this.fadeLeft = 0;
    this.fadeLength = 1;
    this.fadeLoss = 0;
    this.onsetPending = this.onsetForced = false;
    this.sinceAttack = 1e9;
  }

  startFade(length, correlation) {
    this.fadeLength = Math.max(1, Math.floor(length));
    this.fadeLeft = this.fadeLength;
    this.fadeLoss = f32(1 - correlation);
  }

  /** Low-pass in front of the delay line: reading faster than the input was written would alias. */
  updateAntiAlias() {
    this.filterRatio = this.ratio;
    const cutoff = Math.min(0.45, 0.40 / Math.max(1, this.ratio)); // as a fraction of the sample rate
    const g = Math.tan(PI * cutoff);
    this.antiAlias1.set(g, BUTTERWORTH_K1);
    this.antiAlias2.set(g, BUTTERWORTH_K2);
  }

  /** The input `d` samples ago (4-point Hermite). */
  read(d) {
    if (d < 2) d = 2;
    if (d > this.maxDelay) d = this.maxDelay;

    const buffer = this.buffer, mask = this.mask;
    const p = this.writePos + mask + 1 - d;
    const i = Math.floor(p);
    const frac = f32(p - i);
    const xm1 = buffer[(i - 1) & mask], x0 = buffer[i & mask], x1 = buffer[(i + 1) & mask], x2 = buffer[(i + 2) & mask];

    const c1 = 0.5 * (x1 - xm1);
    const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * frac + c2) * frac + c1) * frac + x0;
  }

  /** `event`: what Tracker.push returned for this sample (filter updates are done on its ticks). */
  process(x, tracker, event) {
    if (this.ratio !== this.target) {
      this.ratio += (this.target - this.ratio) * this.glide;
      if (Math.abs(this.target - this.ratio) < 1e-9) this.ratio = this.target;
    }
    const ratio = this.ratio;

    if (event !== 0 && ratio !== this.filterRatio) this.updateAntiAlias();

    this.writePos = (this.writePos + 1) & this.mask;
    this.buffer[this.writePos] = this.antiAlias2.lowPass(this.antiAlias1.lowPass(x));

    const drift = 1 - ratio;
    this.delay += drift;
    if (this.fadeLeft > 0) this.oldDelay += drift;

    // A pick attack (the tracker reports it a few ms late): go to just before it, so that it is heard at
    // once and in full. `sinceAttack` then keeps the splices from skipping or repeating it.
    if (this.sinceAttack < 1e8) this.sinceAttack += 1;

    const fadeMin = this.fadeMin, fadeMax = this.fadeMax;
    if (event === 2 && tracker.onset) {
      this.onsetPending = true;
      this.onsetForced = false;
      this.sinceAttack = tracker.onsetAge;

      // Shifting down, in a crossfade to a head that has jumped ahead: if that head was (or will be) still
      // quiet in the middle of the attack's first 2 ms, the attack is as good as skipped, because the old
      // head never gets there. Then finish the crossfade within 1 ms, and go back if half of them is over.
      const past = this.sinceAttack - this.delay;
      if (ratio <= 1 && this.fadeLeft > 0 && this.fadeLeft + (past - 0.5 * this.shortJump) / ratio > 0.5 * this.fadeLength) {
        this.onsetForced = past > 0.5 * this.shortJump;
        if (this.fadeLeft > fadeMin) {
          this.fadeLength *= fadeMin / this.fadeLeft;
          this.fadeLeft = fadeMin;
        }
      }
    }
    if (this.onsetPending) {
      if (this.fadeLeft === 0) {
        const lead = this.onsetMargin + ratio * fadeMin; // the head fades in before it gets to the attack
        let wanted = this.sinceAttack + lead;
        if (wanted < this.minDelay) wanted = this.minDelay;
        let go;

        if (ratio > 1) {
          // far enough back that the head does not have to return before the attack is over
          const room = this.upDelay + this.onsetRoom + (ratio - 1) / ratio * (lead + this.attackLength + 2 * this.shortJump);
          if (wanted < room) wanted = room;
          go = this.delay > this.sinceAttack && Math.abs(this.delay - wanted) > fadeMin; // (not if it has been played)
        } else {
          go = this.onsetForced || this.delay > wanted + fadeMin;
        }

        if (go) {
          this.oldDelay = this.delay;
          this.delay = wanted;
          this.startFade(fadeMin, 0);
        }
        this.onsetPending = false;
      } else if (this.sinceAttack > 2 * fadeMax) {
        this.onsetPending = false;
      }
    }

    if (this.fadeLeft > 0) {
      // one splice at a time
    } else if (ratio > 1) {
      // shifting up: the head catches up with the input, so it has to go back. It turns 3.5 ms before it
      // gets there: an attack is known that much later, and by then it should not have been played.
      const speed = ratio - 1, lag = tracker.jump;
      const past = this.sinceAttack - this.delay; // how far the head is beyond the start of the last attack
      let room = 0.35 * lag; // what the old head still travels during the crossfade
      if (room > speed * fadeMax) room = speed * fadeMax;
      if (room < speed * fadeMin || past < this.attackLength) room = speed * fadeMin; // (an attack is played first)

      if (this.delay <= this.upDelay + room) {
        let back = lag, match = tracker.jumpCorr, length = room / speed;
        if (past > 0 && past - back < this.attackLength) {
          // not as far back: the attack should not be played twice
          back = past - this.attackLength;
          if (back < this.shortJump) back = this.shortJump;
          match = 0;
          if (length > 0.5 * back / speed) length = 0.5 * back / speed;
          if (length < fadeMin) length = fadeMin;
        }

        this.oldDelay = this.delay;
        this.delay += back;
        this.startFade(length, match);
      }
    } else {
      // shifting down: the head falls behind, so it has to skip ahead (not shifting: it stays close);
      // not across an attack, though: that is played to its end first
      const lag = tracker.jump;
      if (this.delay >= this.minDelay + lag && (this.sinceAttack - this.delay >= this.attackLength || this.delay >= this.longestDelay)) {
        let length = ratio < 1 ? 0.35 * lag / (1 - ratio) : fadeMax;
        if (length > fadeMax) length = fadeMax;
        if (length < fadeMin) length = fadeMin;

        this.oldDelay = this.delay;
        this.delay -= lag;
        this.startFade(length, tracker.jumpCorr);
      }
    }

    let out = this.read(this.delay);
    if (this.fadeLeft > 0) {
      const t = f32(this.fadeLeft / this.fadeLength); // the old head's share: 1 -> 0
      const b = t * t * (3 - 2 * t), a = 1 - b;
      out = (a * out + b * this.read(this.oldDelay)) / Math.sqrt(1 - 2 * a * b * this.fadeLoss);
      --this.fadeLeft;
    }
    return out;
  }
}

// ============================================================================
/** Analog-style octave divider: flips the low-passed fundamental every other cycle (see Pitch.h). */
class Octaver {
  constructor() {
    this.extract1 = new Svf(); this.extract2 = new Svf(); this.tone1 = new Svf(); this.tone2 = new Svf();
    this.fs = 48000; this.tone = 2; this.toneTarget = 2; this.filterFrequency = 0; this.filterTone = 0;
  }

  prepare(sampleRate, tickRate) {
    this.fs = sampleRate;
    const w = 2 * PI * 15 / sampleRate;
    this.highPassCoef = w / (1 + w);
    this.glide = 1 / (1 + 0.006 * tickRate);
    this.gateUp = 1 / (0.006 * sampleRate);
    this.gateDown = 1 / (0.004 * sampleRate);
    this.reset();
  }

  /** 0..1: the octave voice's low-pass, from 1x to 5x the octave's frequency. */
  setTone(tone01) { this.toneTarget = exp2Exact(tone01 * 2.321928094887362); }

  reset() {
    this.extract1.reset(); this.extract2.reset(); this.tone1.reset(); this.tone2.reset();
    this.highPass = this.envelope = this.previous = this.gate = this.gateTarget = 0;
    this.sign = 1;
    this.armed = false;
    this.sinceToggle = 0;
    this.frequency = this.frequencyTarget = 110;
    this.tone = this.toneTarget;
    this.holdOff = 0.62 * this.fs / 110;
    this.envelopeDecay = 1 - 110 / (2 * this.fs);
    this.updateFilters();
  }

  /** Once per tracker tick; `analysed`: the tracker has new results. */
  tick(tracker, analysed) {
    if (analysed) {
      if (tracker.locked) {
        this.frequencyTarget = this.fs / tracker.period;
        this.holdOff = 0.62 * tracker.period;
        this.envelopeDecay = 1 - 1 / (2 * tracker.period);
      }
      this.gateTarget = tracker.tracking ? 1 : 0;

      if (Math.abs(this.highPass) < TINY) this.highPass = 0;
      if (Math.abs(this.envelope) < TINY) this.envelope = 0;
      this.extract1.flushTiny(); this.extract2.flushTiny(); this.tone1.flushTiny(); this.tone2.flushTiny();
    }

    if (this.frequency !== this.frequencyTarget) {
      this.frequency += (this.frequencyTarget - this.frequency) * this.glide;
      if (Math.abs(this.frequencyTarget - this.frequency) < 1e-6) this.frequency = this.frequencyTarget;
    }
    if (this.tone !== this.toneTarget) {
      this.tone += (this.toneTarget - this.tone) * this.glide;
      if (Math.abs(this.toneTarget - this.tone) < 1e-6) this.tone = this.toneTarget;
    }
    if (this.frequency !== this.filterFrequency || this.tone !== this.filterTone) this.updateFilters();
  }

  process(x) {
    this.highPass += this.highPassCoef * (x - this.highPass);
    const y = this.extract2.lowPass(this.extract1.lowPass(x - this.highPass));

    const magnitude = Math.abs(y);
    this.envelope = magnitude > this.envelope ? magnitude : this.envelope * this.envelopeDecay;

    if (this.sinceToggle < 1000000000) ++this.sinceToggle;

    // Schmitt trigger: a rising zero crossing counts once the signal has been clearly negative
    if (this.previous < 0 && y >= 0) {
      if (this.armed && this.sinceToggle >= this.holdOff) {
        this.sign = -this.sign;
        this.sinceToggle = 0;
      }
      this.armed = false;
    }
    if (y < -0.2 * this.envelope - 1e-9) this.armed = true;
    this.previous = y;

    if (this.gate < this.gateTarget) { this.gate += this.gateUp; if (this.gate > this.gateTarget) this.gate = this.gateTarget; }
    else if (this.gate > this.gateTarget) { this.gate -= this.gateDown; if (this.gate < this.gateTarget) this.gate = this.gateTarget; }

    return this.gate * this.tone2.lowPass(this.tone1.lowPass(this.sign * y));
  }

  updateFilters() {
    this.filterFrequency = this.frequency;
    this.filterTone = this.tone;

    const limit = 0.2 * this.fs;
    let cutoff = 1.25 * this.frequency; // keeps the fundamental, 16 dB less of the second harmonic
    if (cutoff > limit) cutoff = limit;
    let g = tanExact(PI * cutoff / this.fs);
    this.extract1.set(g, BUTTERWORTH_K1);
    this.extract2.set(g, BUTTERWORTH_K2);

    cutoff = this.tone * 0.5 * this.frequency;
    if (cutoff > limit) cutoff = limit;
    g = tanExact(PI * cutoff / this.fs);
    this.tone1.set(g, BUTTERWORTH_K1);
    this.tone2.set(g, BUTTERWORTH_K2);
  }
}

// ============================================================================
const BASS_OCTAVER = 0, PITCH_GLIDE = 1, SMART_HARMONY = 2;

export class PitchFx {
  constructor() {
    this.variant = BASS_OCTAVER;
    this.fs = 48000;
    this.tracker = new Tracker(); this.shifter = new Shifter(); this.octaver = new Octaver();
    this.dryGain = new Smoothed(1); this.wetGain = new Smoothed(1);
    this.harmonyKey = -1; this.harmonyShift = -1; this.harmonyScale = -1;
    this.ratios = new Float64Array(12).fill(1);
    this.note = 0; this.pitchClass = 0; this.haveNote = false;
  }

  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.tracker.prepare(sampleRate);
    this.shifter.prepare(sampleRate);
    this.octaver.prepare(sampleRate, this.tracker.rate);
    this.dryGain.reset(sampleRate, 0.03);
    this.wetGain.reset(sampleRate, 0.03);
    this.reset();
  }

  reset() {
    this.tracker.reset();
    this.octaver.reset();
    this.note = 0;
    this.haveNote = false;
    this.pitchClass = 0;
    if (this.variant === SMART_HARMONY) this.shifter.setRatio(this.ratios[0]);
    this.shifter.reset();
    this.dryGain.setCurrentAndTarget(this.dryGain.target);
    this.wetGain.setCurrentAndTarget(this.wetGain.target);
  }

  setModel(variant) { this.variant = clamp(variant | 0, 0, 2); }

  setParameters(k) {
    if (this.variant === BASS_OCTAVER) {
      // Bass Octaver - EBS OctaBass: flip-flop octave divider on the low-passed fundamental, Normal and Octave
      // levels, the octave's tone filter
      const tone = f32(clamp(f32(k[0]), 0, 100) / 100);
      const normal = f32(clamp(f32(k[1]), 0, 100) / 100), octave = f32(clamp(f32(k[2]), 0, 100) / 100);
      this.octaver.setTone(tone);
      this.dryGain.setTarget(f32(normal * normal));
      this.wetGain.setTarget(f32(f32(f32(2.4) * octave) * octave));
      return;
    }

    let mix;
    if (this.variant === PITCH_GLIDE) {
      // Pitch Glide - Digitech Whammy: one shifted voice that glides between intervals, chord-tolerant splicing
      const pitch = clamp(f32(k[0]), -24, 24);
      this.shifter.setGlide(0.012);
      this.shifter.setRatio(exp2Exact(pitch / 12));
      mix = k[1];
    } else {
      // Smart Harmony - Eventide H3000 diatonic shift: the interval follows the played note, key and scale
      const key = clamp(Math.floor(f32(f32(k[0]) + 0.5)), 0, KEY_NAMES.length - 1);
      const shift = clamp(Math.floor(f32(f32(k[1]) + 0.5)), 0, SHIFT_NAMES.length - 1);
      const scale = clamp(Math.floor(f32(f32(k[2]) + 0.5)), 0, SCALE_NAMES.length - 1);

      if (key !== this.harmonyKey || shift !== this.harmonyShift || scale !== this.harmonyScale) {
        this.harmonyKey = key; this.harmonyShift = shift; this.harmonyScale = scale;
        for (let pc = 0; pc < 12; ++pc) this.ratios[pc] = exp2Exact(harmonySemitones(scale, SHIFT_INTERVALS[shift], pc) / 12);
        this.pitchClass = this.haveNote ? ((this.note - key) % 12 + 12) % 12 : 0;
      }
      this.shifter.setGlide(0.008);
      this.shifter.setRatio(this.ratios[this.pitchClass]);
      mix = k[3];
    }

    // equal-power mix: a harmony and the dry note together stay at about the input's loudness
    const angle = f32(f32(clamp(f32(mix), 0, 100) / 100) * f32(0.5 * PI));
    this.dryGain.setTarget(f32(Math.cos(angle)));
    this.wetGain.setTarget(f32(Math.sin(angle)));
  }

  /** Smart Harmony: which note is being played (A = 440 Hz), with hysteresis. */
  followNote() {
    const tracker = this.tracker;
    if (!tracker.tracking) return;

    const midi = 69 + 12 * log2Exact(this.fs / (tracker.period * 440));
    if (!this.haveNote || Math.abs(midi - this.note) > 0.6) {
      this.note = Math.floor(midi + 0.5);
      this.haveNote = true;
      this.pitchClass = ((this.note - this.harmonyKey) % 12 + 12) % 12;
      this.shifter.setRatio(this.ratios[this.pitchClass]);
    }
  }

  process(left, right, n) {
    const tracker = this.tracker, dryGain = this.dryGain, wetGain = this.wetGain;

    if (this.variant === BASS_OCTAVER) {
      const octaver = this.octaver;
      for (let i = 0; i < n; ++i) {
        const x = 0.5 * f32(left[i] + right[i]);
        const event = tracker.push(x);
        if (event !== 0) octaver.tick(tracker, event === 2);

        const octave = octaver.process(x);
        left[i] = right[i] = dryGain.next() * x + wetGain.next() * octave;
      }
      return;
    }

    const shifter = this.shifter, harmony = this.variant === SMART_HARMONY;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * f32(left[i] + right[i]);
      const event = tracker.push(x);
      if (harmony && event === 2) this.followNote();

      const wet = shifter.process(x, tracker, event);
      left[i] = right[i] = dryGain.next() * x + wetGain.next() * wet;
    }
  }
}

/** In the order of the variants. */
export const PITCH_MODELS = [
  model("bass_octaver", "Bass Octaver", CATEGORY.pitch, ENGINE.pitchFx, BASS_OCTAVER, "EBS OctaBass",
        [percent("Tone", 50), percent("Normal", 100), percent("Octave", 70)]),
  model("pitch_glide", "Pitch Glide", CATEGORY.pitch, ENGINE.pitchFx, PITCH_GLIDE, "Digitech Whammy",
        [semitones("Pitch", -24, 24, 12, 0.1), percent("Mix", 100)]),
  model("smart_harmony", "Smart Harmony", CATEGORY.pitch, ENGINE.pitchFx, SMART_HARMONY, "Eventide H3000",
        [choice("Key", KEY_NAMES, 0), choice("Shift", SHIFT_NAMES, 8), choice("Scale", SCALE_NAMES, 0), percent("Mix", 50)]),
];
