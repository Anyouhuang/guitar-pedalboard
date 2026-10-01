// Tuner, spectrum analyser, tap tempo and the test signals (no guitar needed).
import { PI, clamp, f32, dbToGain, gainToDb, Smoothed, Biquad, OnePole, DcBlocker, DelayLine } from "./core.js";

// ============================================================================
// Tuner: YIN on input decimated to ~24 kHz
export class PitchDetector {
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

export const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export function frequencyToNote(hz, a4 = 440) {
  const midi = 69 + 12 * Math.log2(hz / a4), nearest = Math.round(midi);
  return { midi: nearest, cents: (midi - nearest) * 100, name: NOTE_NAMES[((nearest % 12) + 12) % 12], octave: Math.floor(nearest / 12) - 1 };
}

// ============================================================================
// FFT analyser -> 240 log-spaced columns (20 Hz .. 20 kHz) in dBFS
export class SpectrumAnalyzer {
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

export class TapTempo {
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
export const OPEN_STRINGS = [82.41, 110.0, 146.83, 196.0, 246.94, 329.63];
export const fretHz = (string, fret) => OPEN_STRINGS[string] * Math.pow(2, fret / 12);

/** How CHORD buttons play. */
export const CHORD_PATTERNS = { arpeggio: 0, strumLoop: 1, single: 2 };
export const CHORD_PATTERN_NAMES = ["Arpeggio", "Strum loop", "Single"];

/** Open-position chord shapes, frets from low E to high e; -1 = string not played (muted). */
export const CHORDS = [
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
export function makeRng(seed) {
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

export class PluckedString {
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
export function renderDemoRiff(fs) {
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
export class TestSignalPlayer {
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
  /** Plays a chord with the current pattern; while looping, the new chord takes over on the next eighth note (strum loop: the next beat). */
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
      // a new chord takes over on the next eighth (the strum loop: on the next beat, so it stays on the beat)
      if (this.nextLoopChord >= 0 && (this.chordPattern !== CHORD_PATTERNS.strumLoop || this.loopStep % 2 === 0)) {
        this.loopChord = this.nextLoopChord; this.nextLoopChord = -1; this.loopStep = 0;
      }
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
    } else if (step % 2 === 0) {
      // one down-strum per beat (quarter notes), beat 1 a little stronger
      muteUnplayed();
      const spacing = Math.floor(0.009 * this.fs);
      played.forEach((s, k) => pluckAt(s, offset + k * spacing, (step === 0 ? 0.19 : 0.16) * (1 - 0.04 * k)));
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
