// The HD500X's 22 modulation models (Source/DSP/fx/Mod.h), ported line for line: see the C++ for what each
// model is and what was modelled. LFO phases and everything that steers a filter or a delay are computed in
// doubles on both sides and rounded to float at the same points (f32), so the two versions stay together.
import { clamp, f32, PI } from "./core.js";
import { CATEGORY, ENGINE, choice, hertz, model, percent } from "./modeltypes.js";

const STEP_NAMES = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "Mute", "Skip", "Full"];
const PAN_NAMES = ["Left", "Center", "Right"];
const SWEEP_NAMES = ["Up", "Down", "Stereo"];
const STAGE_NAMES = ["4", "8", "12", "16"];
const SWITCH_NAMES = ["Off", "On"];
const CHORUS_NAMES = ["Chorus", "Vibrato"];
const HARMONIC_NAMES = ["Even", "Odd"];
const ROTOR_NAMES = ["Slow", "Fast"];

const TWO_PI = 2 * PI;
const STEP_MUTE = 16, STEP_SKIP = 17;
const HILBERT_SECTIONS = 6;
const BARBER_STAGES = 8;

// variants (ModFx::Variant)
const PATTERN_TREMOLO = 0, PANNER = 1, BIAS_TREMOLO = 2, OPTO_TREMOLO = 3, SCRIPT_PHASE = 4, PANNED_PHASER = 5, BARBERPOLE_PHASER = 6,
  DUAL_PHASER = 7, U_VIBE = 8, PHASER = 9, PITCH_VIBRATO = 10, DIMENSION = 11, ANALOG_CHORUS = 12, TRI_CHORUS = 13, ANALOG_FLANGER = 14,
  JET_FLANGER = 15, AC_FLANGER = 16, FLANGER_80A = 17, FREQUENCY_SHIFTER = 18, RING_MODULATOR = 19, ROTARY_DRUM = 20, ROTARY_DRUM_HORN = 21,
  NUM_VARIANTS = 22;

// smoothed values
const SM_DEPTH = 0, SM_FEEDBACK = 1, SM_SHAPE = 2, SM_A = 3, SM_B = 4, SM_C = 5, SM_DRY = 6, SM_WET = 7, SM_DRY_R = 8, SM_WET_R = 9,
  SM_TAP0 = 10, SM_DIR_L = 14, SM_DIR_R = 15, SM_TONE = 16, NUM_SMOOTHED = 17;

const PANNED_FEEDBACK = 0.3, PANNED_FEEDBACK_F = f32(0.3);
const BIAS_REST = 1, BIAS_SWING = 4, BIAS_SCALE = f32(0.1464466);
const SOFT_CLIP_CUBIC = f32(4 / 27);
const SQRT2 = 1.4142135623730951, SIN120 = 0.8660254037844386;
const VIBE_RATIO = [0.3133, 0.02136, 10, 1]; // 4.7 nF / the stage's capacitor

/** fx::Smoothed (linear), stepping in single precision exactly like the C++: the ramps steer delay times and
    filter coefficients here, where the rounding of a double-precision ramp would be heard in the comparison. */
const FLT_EPS = 1.1920929e-7, FLT_MIN = 1.17549435e-38;
class Ramp {
  constructor() { this.current = 0; this.target = 0; this.step = 0; this.countdown = 0; this.stepsToTarget = 0; }
  reset(sampleRate, seconds) { this.stepsToTarget = Math.floor(seconds * sampleRate); this.setCurrentAndTarget(this.target); }
  setCurrentAndTarget(v) { this.target = this.current = v; this.countdown = 0; }
  /** v: already rounded to float */
  setTarget(v) {
    const d = Math.abs(f32(v - this.target));
    if (d <= FLT_MIN || d <= f32(FLT_EPS * Math.max(Math.abs(v), Math.abs(this.target)))) return;
    if (this.stepsToTarget <= 0) { this.setCurrentAndTarget(v); return; }
    this.target = v;
    this.countdown = this.stepsToTarget;
    this.step = f32(f32(this.target - this.current) / this.countdown);
  }
  isSmoothing() { return this.countdown > 0; }
  next() {
    if (this.countdown <= 0) return this.target;
    if (--this.countdown > 0) this.current = f32(this.current + this.step);
    else this.current = this.target;
    return this.current;
  }
}

/** (tan w - 1) / (tan w + 1) with sin and cos as polynomials: first-order all-pass coefficient. */
function allpassCoef(w) {
  const w2 = w * w;
  const s = w * (1 + w2 * (-1 / 6 + w2 * (1 / 120 + w2 * (-1 / 5040 + w2 * (1 / 362880)))));
  const c = 1 + w2 * (-1 / 2 + w2 * (1 / 24 + w2 * (-1 / 720 + w2 * (1 / 40320 + w2 * (-1 / 3628800)))));
  return (s - c) / (s + c);
}

const pct = (k, i) => k[i] / 100;
const pick = (k, i, count) => clamp((k[i] + 0.5) | 0, 0, count - 1);

const sineToSquare = (s, g) => (s * (1 + g)) / (1 + g * Math.abs(s));
const squareEdge = (rateHz) => clamp(60 / rateHz, 1, 4000);

/** Triangle (0) -> sine (0.5) -> square (1), -1..1, phase 0..1. */
function morphLfo(p, shape, edge) {
  const s = Math.sin(TWO_PI * p);
  if (shape < 0.5) {
    let q = p + 0.25;
    if (q >= 1) q -= 1;
    const tri = 1 - 4 * Math.abs(q - 0.5);
    return tri + 2 * shape * (s - tri);
  }
  const amount = 2 * shape - 1;
  return sineToSquare(s, amount * amount * edge);
}

/** A chain of identical first-order all-pass stages; states z[offset .. offset + count). */
function allpassChain(x, a, z, offset, count) {
  for (let s = offset; s < offset + count; ++s) {
    const y = a * x + z[s];
    z[s] = x - a * y;
    x = y;
  }
  return x;
}

function softClip(x) {
  x = x > 1.5 ? 1.5 : (x < -1.5 ? -1.5 : x);
  return x - SOFT_CLIP_CUBIC * x * x * x;
}

function tubeClip(x) {
  x = x > 3 ? 3 : (x < -3 ? -3 : x);
  const x2 = x * x;
  return (x * (27 + x2)) / (27 + 9 * x2);
}

function biasStage(x, b) {
  const p = b + 2 * x, q = b - 2 * x;
  return BIAS_SCALE * (p - q + Math.sqrt(p * p + 1) - Math.sqrt(q * q + 1));
}

function balance(x) {
  const t = x < 0.5 ? 2 * x : 1;
  return f32(t * (2 - t));
}

/** Power-of-two delay buffer, 4-point Hermite read. */
class Line {
  constructor() { this.buffer = new Float32Array(16); this.mask = 15; this.write = 0; }
  prepare(samples) {
    let size = 16;
    while (size < samples + 8) size <<= 1;
    this.buffer = new Float32Array(size);
    this.mask = size - 1;
    this.write = 0;
  }
  reset() { this.buffer.fill(0); this.write = 0; }
  push(x) { this.buffer[this.write] = x; this.write = (this.write + 1) & this.mask; }
  /** The sample pushed `delay` pushes ago (1 = the latest), delay >= 2. */
  read(delay) {
    const di = delay | 0, frac = delay - di, b = this.buffer, mask = this.mask, at = this.write - di;
    const xm1 = b[(at + 1) & mask], x0 = b[at & mask], x1 = b[(at - 1) & mask], x2 = b[(at - 2) & mask];
    const c1 = 0.5 * (x1 - xm1);
    const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * frac + c2) * frac + c1) * frac + x0;
  }
}

/** One path of the 90-degree phase splitter; h[o ..] holds the last two samples of the input and of every section. */
function hilbertPath(x, c, h, o) {
  for (let k = 0; k < HILBERT_SECTIONS; ++k) {
    const y = c[k] * (x + h[o + 2 * k + 3]) - h[o + 2 * k + 1];
    h[o + 2 * k + 1] = h[o + 2 * k];
    h[o + 2 * k] = x;
    x = y;
  }
  h[o + 2 * HILBERT_SECTIONS + 1] = h[o + 2 * HILBERT_SECTIONS];
  h[o + 2 * HILBERT_SECTIONS] = x;
  return x;
}

/** One microphone looking at a rotor (Doppler, loudness, brightness). */
function rotorTap(line, facing, depth, baseSamples, swingSamples, split, state, index, shadowLow, shadowHigh) {
  const s = line.read(f32(baseSamples + swingSamples * depth * (1 - facing)));
  const low = state[index] + split * (s - state[index]);
  state[index] = low;
  const away = f32(0.5 * depth * (1 - facing));
  return low * (1 - shadowLow * away) + (s - low) * (1 - shadowHigh * away);
}

// maxDelayMs, ratio, exponential, lowPassHz, drive, offset, compressor, mono
const FLANGER_VOICINGS = [
  { maxDelayMs: 10, ratio: 20, exponential: false, lowPassHz: 10000, drive: 0.5, offset: 0.25, compressor: false, mono: false },   // Analog Flanger
  { maxDelayMs: 12.25, ratio: 35, exponential: true, lowPassHz: 9000, drive: 0.5, offset: 0.5, compressor: true, mono: false },    // Jet Flanger
  { maxDelayMs: 10, ratio: 20, exponential: false, lowPassHz: 6500, drive: 1, offset: 0.125, compressor: false, mono: false },     // AC Flanger
  { maxDelayMs: 12.25, ratio: 35, exponential: true, lowPassHz: 6000, drive: 1, offset: 0, compressor: true, mono: true },         // 80A Flanger
];

const COMP_THRESHOLD = f32(0.1), SHADOW = { drumLow: f32(0.45), drumHigh: f32(0.85), hornLow: f32(0.5), hornHigh: f32(0.9), bassLow: f32(0.4), bassHigh: f32(0.7) };

export class ModFx {
  prepare(sampleRate, maxBlock) {
    this.variant = this.variant || 0;
    this.fs = sampleRate;
    this.invFs = 1 / sampleRate;
    const fs = this.fs;

    if (!this.sm) {
      // from the knobs
      this.rateHz = 1; this.rate2Hz = 0; this.volSens = 0; this.riseStep = 1; this.drumTarget = 0.67; this.hornTarget = 0.8;
      this.steps = new Int32Array(4);
      this.toneCoef = 1; this.wetLowPass = 1;
      this.sm = [];
      for (let i = 0; i < NUM_SMOOTHED; ++i) this.sm.push(new Ramp());
      this.lines = [new Line(), new Line()];
      this.barberCoef = new Float32Array(BARBER_STAGES);
      this.hilbertI = new Float32Array(HILBERT_SECTIONS); this.hilbertQ = new Float32Array(HILBERT_SECTIONS);
      // state
      this.apL = new Float32Array(16); this.apR = new Float32Array(16);
      this.lp = new Float32Array(10);
      this.hilbert = new Float32Array(4 * (2 * HILBERT_SECTIONS + 2));
      this.hilbertDelay = new Float32Array(2);
    }

    for (const line of this.lines) line.prepare(Math.floor(0.026 * fs));
    for (const s of this.sm) s.reset(fs, 0.04);

    this.envAttack = f32(this.glide(0.005));
    this.envRelease = f32(this.glide(0.25));
    this.compAttack = f32(this.glide(0.005));
    this.compRelease = f32(this.glide(0.12));
    this.tremCoef = f32(this.glide(0.0012));
    this.ldrFast = this.glide(0.004);
    this.ldrSlow = this.glide(0.045);
    this.cellUp = this.glide(0.008);
    this.cellDown = this.glide(0.02);
    this.lampUp = this.glide(0.012);
    this.lampDown = this.glide(0.035);
    this.fbLowPass = f32(this.lowPass(6000));
    this.fbHighPass = f32(this.lowPass(100));

    // Barberpole: eight fixed all-pass stages spread evenly (in octaves) from 100 Hz to 6.4 kHz
    for (let i = 0; i < BARBER_STAGES; ++i) {
      const t = Math.tan((PI * 100 * Math.pow(64, i / 7)) / fs);
      this.barberCoef[i] = (t - 1) / (t + 1);
    }

    this.designHilbert();
    this.reset();
  }

  reset() {
    for (const s of this.sm) s.setCurrentAndTarget(s.target);
    for (const line of this.lines) line.reset();

    this.phase = this.phase2 = this.seqPhase = 0;
    this.ldrA = this.ldrB = this.lamp = this.rise = 0;
    this.drumAngle = this.hornAngle = 0;
    this.drumSpeed = this.drumTarget;
    this.hornSpeed = this.hornTarget;
    this.env = this.compEnv = this.fbL = this.fbR = 0;
    this.apL.fill(0); this.apR.fill(0); this.lp.fill(0); this.hilbert.fill(0); this.hilbertDelay.fill(0);

    this.seqStep = this.steps[0] === STEP_SKIP ? this.nextStep(0) : 0;
    this.trem1 = this.trem2 = ModFx.stepGain(this.steps[this.seqStep], 0);
  }

  setModel(variant) { this.variant = clamp(variant | 0, 0, NUM_VARIANTS - 1); }

  setParameters(k) {
    const variant = this.variant;
    this.volSens = 0;

    switch (variant) {
      case PATTERN_TREMOLO:
        this.rateHz = k[0];
        for (let i = 0; i < 4; ++i) this.steps[i] = pick(k, 1 + i, 19);
        break;

      case PANNER:
      case BIAS_TREMOLO:
      case OPTO_TREMOLO:
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_SHAPE, pct(k, 2));
        this.volSens = 3 * pct(k, 3);
        this.set(SM_DRY, 1 - pct(k, 4));
        this.set(SM_WET, pct(k, 4));
        break;

      case SCRIPT_PHASE:
        this.rateHz = k[0];
        break;

      case PANNED_PHASER: {
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        // the panner sweeps the left half, everything or the right half; with Pan Spd at 0 it parks there
        const pan = pick(k, 2, 3);
        this.rate2Hz = k[3];
        const moving = Math.min(1, this.rate2Hz / 0.05);
        this.set(SM_A, 0.5 * pan + moving * (0.25 - 0.25 * pan));
        this.set(SM_B, moving * (pan === 1 ? 0.5 : 0.25));
        this.setMix(pct(k, 4), PANNED_FEEDBACK, true);
        break;
      }

      case BARBERPOLE_PHASER: {
        this.rateHz = k[0];
        const mode = pick(k, 2, 3);
        const feedback = 0.8 * pct(k, 1) * Math.min(1, 2 * pct(k, 3));
        this.set(SM_FEEDBACK, feedback);
        this.set(SM_DIR_L, mode === 1 ? -1 : 1);
        this.set(SM_DIR_R, mode === 0 ? 1 : -1);
        this.setMix(pct(k, 3), feedback, true);
        break;
      }

      case DUAL_PHASER:
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_FEEDBACK, 0.8 * pct(k, 2));
        this.set(SM_SHAPE, pct(k, 3));
        this.setMix(pct(k, 4), 0.8 * pct(k, 2), true);
        break;

      case U_VIBE:
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_FEEDBACK, 0.7 * pct(k, 2));
        this.volSens = 3 * pct(k, 3);
        this.setMix(pct(k, 4), 0.7 * pct(k, 2), true);
        break;

      case PHASER: {
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_FEEDBACK, 0.8 * pct(k, 2));
        const stages = pick(k, 3, 4);
        for (let t = 0; t < 4; ++t) this.set(SM_TAP0 + t, t === stages ? 1 : 0);
        this.setMix(pct(k, 4), 0.8 * pct(k, 2), true);
        break;
      }

      case PITCH_VIBRATO: {
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        const slow = 1 - pct(k, 2);
        this.riseStep = this.invFs / (0.02 + 3 * slow * slow);
        this.volSens = 3 * pct(k, 3);
        this.setMix(pct(k, 4), 0, false);
        this.wetLowPass = f32(this.lowPass(5500));
        break;
      }

      case DIMENSION: {
        // the four mode buttons add up: more modulation with every button, and button 4 doubles the speed
        const s1 = pick(k, 0, 2), s2 = pick(k, 1, 2), s3 = pick(k, 2, 2), s4 = pick(k, 3, 2);
        const amount = s1 + 2 * s2 + 3 * s3 + 4 * s4;
        this.set(SM_DEPTH, 0.002 * (1 - Math.exp(-amount / 3.5)));
        this.rateHz = s4 !== 0 ? 0.5 : 0.25;
        this.setMix(amount > 0 ? pct(k, 4) : 0, 0, false);
        this.wetLowPass = f32(this.lowPass(9000));
        this.toneCoef = f32(this.lowPass(200));
        break;
      }

      case ANALOG_CHORUS: {
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        const vibrato = pick(k, 2, 2);
        this.set(SM_A, vibrato);
        this.set(SM_B, 0.0032 * Math.min(1, 1.2 / this.rateHz)); // chorus swing (s): held back at high speeds
        this.set(SM_TONE, this.lowPass(1200 * Math.pow(10, pct(k, 3))));
        this.wetLowPass = f32(this.lowPass(8000));
        const mix = pct(k, 4);
        if (vibrato !== 0) {
          // the CE-1's vibrato has no dry signal: from 50 % up the Mix knob leaves it that way
          const angle = 0.5 * PI * Math.min(1, 2 * mix);
          this.set(SM_DRY, Math.cos(angle));
          this.set(SM_WET, Math.sin(angle));
          this.set(SM_DRY_R, Math.cos(angle));
          this.set(SM_WET_R, Math.sin(angle));
        } else {
          // chorus on the left output, the untouched signal on the right (the CE-1's two jacks)
          this.set(SM_DRY, Math.cos(0.5 * PI * mix));
          this.set(SM_WET, Math.sin(0.5 * PI * mix));
          this.set(SM_DRY_R, 1);
          this.set(SM_WET_R, 0);
        }
        break;
      }

      case TRI_CHORUS:
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_A, pct(k, 2));
        this.set(SM_B, pct(k, 3));
        this.set(SM_C, Math.min(1, 1.5 / this.rateHz)); // the swing is held back at high speeds
        this.setMix(pct(k, 4), 0, false);
        this.wetLowPass = f32(this.lowPass(9000));
        break;

      case ANALOG_FLANGER:
      case JET_FLANGER:
      case AC_FLANGER:
      case FLANGER_80A: {
        const voicing = FLANGER_VOICINGS[variant - ANALOG_FLANGER];
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_A, pct(k, 3));
        let feedback = (variant === ANALOG_FLANGER ? 0.85 : variant === FLANGER_80A ? 0.93 : 0.9) * pct(k, 2);
        if (variant === FLANGER_80A && pick(k, 4, 2) === 1) feedback = -feedback; // Odd: the regeneration is inverted
        this.set(SM_FEEDBACK, feedback);

        // dry and delayed signal at equal power, turned down by what the regeneration adds
        const mix = variant === ANALOG_FLANGER || variant === JET_FLANGER ? pct(k, 4) : 0.5;
        const c = Math.cos(0.5 * PI * mix), s = Math.sin(0.5 * PI * mix);
        const norm = 1 / Math.sqrt(c * c + (s * s) / (1 - feedback * feedback));
        this.set(SM_DRY, c * norm);
        this.set(SM_WET, s * norm);
        this.set(SM_B, Math.min(1, 2 * mix));
        this.wetLowPass = f32(this.lowPass(voicing.lowPassHz));
        break;
      }

      case FREQUENCY_SHIFTER: {
        this.rateHz = k[0];
        const mode = pick(k, 1, 3);
        this.set(SM_DIR_L, mode === 1 ? -1 : 1);
        this.set(SM_DIR_R, mode === 0 ? 1 : -1);
        this.setMix(pct(k, 2), 0, false);
        break;
      }

      case RING_MODULATOR:
        this.rateHz = k[0];
        this.set(SM_DEPTH, pct(k, 1));
        this.set(SM_SHAPE, pct(k, 2));
        this.set(SM_A, pct(k, 3));
        this.setMix(pct(k, 4), 0, false);
        break;

      case ROTARY_DRUM:
      case ROTARY_DRUM_HORN: {
        const fast = pick(k, 0, 2) === 1;
        this.drumTarget = fast ? 5.7 : 0.67;
        this.hornTarget = fast ? 6.8 : 0.8;
        this.set(SM_DEPTH, pct(k, 1));
        if (variant === ROTARY_DRUM) this.set(SM_TONE, this.lowPass(1500 * Math.pow(6, pct(k, 2))));
        else this.set(SM_A, pct(k, 2));
        // the tube amp: more gain into the clipping, turned back down so a full-level note stays where it was
        const gain = 1 + 7 * pct(k, 3) * pct(k, 3);
        this.set(SM_B, gain);
        this.set(SM_C, 0.4 / tubeClip(0.4 * gain));
        this.setMix(pct(k, 4), 0, false);
        break;
      }

      default:
        break;
    }
  }

  process(left, right, n) {
    switch (this.variant) {
      case PATTERN_TREMOLO: this.processPattern(left, right, n); break;
      case PANNER: this.processPanner(left, right, n); break;
      case BIAS_TREMOLO: this.processBias(left, right, n); break;
      case OPTO_TREMOLO: this.processOpto(left, right, n); break;
      case SCRIPT_PHASE: this.processScript(left, right, n); break;
      case PANNED_PHASER: this.processPanned(left, right, n); break;
      case BARBERPOLE_PHASER: this.processBarberpole(left, right, n); break;
      case DUAL_PHASER: this.processDual(left, right, n); break;
      case U_VIBE: this.processVibe(left, right, n); break;
      case PHASER: this.processPhaser(left, right, n); break;
      case PITCH_VIBRATO: this.processVibrato(left, right, n); break;
      case DIMENSION: this.processDimension(left, right, n); break;
      case ANALOG_CHORUS: this.processChorus(left, right, n); break;
      case TRI_CHORUS: this.processTriChorus(left, right, n); break;
      case ANALOG_FLANGER:
      case JET_FLANGER:
      case AC_FLANGER:
      case FLANGER_80A: this.processFlanger(left, right, n, FLANGER_VOICINGS[this.variant - ANALOG_FLANGER]); break;
      case FREQUENCY_SHIFTER: this.processShifter(left, right, n); break;
      case RING_MODULATOR: this.processRing(left, right, n); break;
      case ROTARY_DRUM: this.processDrum(left, right, n); break;
      case ROTARY_DRUM_HORN: this.processLeslie(left, right, n); break;
      default: break;
    }
  }

  // ---- helpers
  set(index, value) { this.sm[index].setTarget(f32(value)); }

  glide(seconds) { return 1 - Math.exp(-1 / (seconds * this.fs)); }

  /** Coefficient c of the one-pole low-pass y += c (x - y) that is 3 dB down at `hz` at any sample rate. */
  lowPass(hz) {
    const b = 2 - Math.cos((TWO_PI * Math.min(hz, 0.45 * this.fs)) / this.fs);
    return 1 - (b - Math.sqrt(b * b - 1));
  }

  setMix(mix, feedback, makeUp) {
    const c = Math.cos(0.5 * PI * mix), s = Math.sin(0.5 * PI * mix);
    const wetGain = makeUp ? 1 + feedback : 1;
    const norm = 1 / Math.sqrt(c * c + (s * s * (1 + feedback)) / (1 - feedback));
    this.sm[SM_DRY].setTarget(f32(c * norm));
    this.sm[SM_WET].setTarget(f32(s * wetGain * norm));
  }

  designHilbert() {
    const count = 2 * HILBERT_SECTIONS, order = 2 * count + 1;
    const transition = 60 / this.fs;
    let k = Math.tan(((1 - transition * 2) * PI) / 4);
    k *= k;
    const root = Math.pow(1 - k * k, 0.25);
    const e = (0.5 * (1 - root)) / (1 + root), e4 = e * e * e * e;
    const q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));

    for (let index = 0; index < count; ++index) {
      const c = index + 1;
      let num = 0, den = 0.5, sign = 1;
      for (let i = 0; i < 14; ++i) {
        num += sign * Math.pow(q, i * (i + 1)) * Math.sin(((2 * i + 1) * c * PI) / order);
        sign = -sign;
      }
      sign = -1;
      for (let i = 1; i < 14; ++i) {
        den += sign * Math.pow(q, i * i) * Math.cos((2 * i * c * PI) / order);
        sign = -sign;
      }
      num *= Math.pow(q, 0.25);
      const ww = (num / den) * (num / den);
      const x = Math.sqrt((1 - ww * k) * (1 - ww / k)) / (1 + ww);
      const coef = (1 - x) / (1 + x);
      if ((index & 1) !== 0) this.hilbertQ[index >> 1] = coef;
      else this.hilbertI[index >> 1] = coef;
    }
  }

  /** VolSens: the louder the playing, the faster the LFO. Returns the phase step. */
  sensedIncrement(mono) {
    const a = Math.abs(mono), env = this.env;
    this.env = f32(env + f32((a > env ? this.envAttack : this.envRelease) * f32(a - env)));
    return this.rateHz * this.invFs * (1 + this.volSens * Math.min(1, 2.5 * this.env));
  }

  // ---- Pattern Tremolo
  nextStep(from) {
    for (let i = 1; i <= 4; ++i) if (this.steps[(from + i) & 3] !== STEP_SKIP) return (from + i) & 3;
    return from;
  }

  static stepGain(step, position) {
    if (step === STEP_MUTE) return 0;
    if (step > STEP_MUTE) return 1; // Full (and a step switched to Skip while it plays)
    const pulse = position * (step + 1);
    return pulse - Math.floor(pulse) < 0.5 ? 1 : 0;
  }

  processPattern(l, r, n) {
    const inc = this.rateHz * this.invFs, steps = this.steps, tremCoef = this.tremCoef;
    let seqPhase = this.seqPhase, seqStep = this.seqStep, trem1 = this.trem1, trem2 = this.trem2;
    for (let i = 0; i < n; ++i) {
      seqPhase += inc;
      if (seqPhase >= 1) {
        seqPhase -= 1;
        seqStep = this.nextStep(seqStep);
      }
      const target = ModFx.stepGain(steps[seqStep], seqPhase);
      trem1 = f32(trem1 + tremCoef * (target - trem1));
      trem2 = f32(trem2 + tremCoef * (trem1 - trem2));
      l[i] *= trem2;
      r[i] *= trem2;
    }
    this.seqPhase = seqPhase; this.seqStep = seqStep; this.trem1 = trem1; this.trem2 = trem2;
  }

  // ---- Panner
  processPanner(l, r, n) {
    const sm = this.sm, shapeS = sm[SM_SHAPE], depthS = sm[SM_DEPTH], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const edge = squareEdge(this.rateHz);
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += this.sensedIncrement(f32(0.5 * (inL + inR)));
      if (phase >= 1) phase -= 1;
      const u = morphLfo(phase, shapeS.next(), edge);
      const angle = 0.25 * PI * (1 + depthS.next() * u);
      const gl = f32(SQRT2 * Math.cos(angle)), gr = f32(SQRT2 * Math.sin(angle));
      const dry = dryS.next(), wet = wetS.next();
      l[i] = inL * (dry + wet * gl);
      r[i] = inR * (dry + wet * gr);
    }
    this.phase = phase;
  }

  // ---- Bias Tremolo
  processBias(l, r, n) {
    const sm = this.sm, shapeS = sm[SM_SHAPE], depthS = sm[SM_DEPTH], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const edge = squareEdge(this.rateHz);
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += this.sensedIncrement(f32(0.5 * (inL + inR)));
      if (phase >= 1) phase -= 1;
      const amount = shapeS.next(), g = amount * amount * edge;
      const swing = BIAS_SWING * depthS.next();
      const uL = sineToSquare(Math.sin(TWO_PI * phase), g), uR = sineToSquare(Math.cos(TWO_PI * phase), g);
      const bL = f32(BIAS_REST - swing * (0.5 - 0.5 * uL)), bR = f32(BIAS_REST - swing * (0.5 - 0.5 * uR));
      const dry = dryS.next(), wet = wetS.next();
      l[i] = dry * inL + wet * biasStage(inL, bL);
      r[i] = dry * inR + wet * biasStage(inR, bR);
    }
    this.phase = phase;
  }

  // ---- Opto Tremolo
  processOpto(l, r, n) {
    const sm = this.sm, shapeS = sm[SM_SHAPE], depthS = sm[SM_DEPTH], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const edge = squareEdge(this.rateHz), ldrFast = this.ldrFast, ldrSlow = this.ldrSlow;
    let phase = this.phase, ldrA = this.ldrA;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += this.sensedIncrement(f32(0.5 * (inL + inR)));
      if (phase >= 1) phase -= 1;
      const amount = shapeS.next();
      const drive = sineToSquare(Math.sin(TWO_PI * phase), 2 + amount * amount * edge);
      const light = drive > 0.25 ? (drive - 0.25) / 0.75 : 0;
      ldrA += (light > ldrA ? ldrFast : ldrSlow) * (light - ldrA);
      const level = depthS.next();
      const gain = f32(1 / (1 + 24 * level * level * ldrA * ldrA));
      const g = dryS.next() + wetS.next() * gain;
      l[i] = inL * g;
      r[i] = inR * g;
    }
    this.phase = phase; this.ldrA = ldrA;
  }

  // ---- Script Phase
  processScript(l, r, n) {
    const inc = this.rateHz * this.invFs, low = (PI * 200) / this.fs, apL = this.apL, apR = this.apR, gain = f32(0.6);
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      const tri = 1 - Math.abs(2 * phase - 1);
      const bent = 0.5 * (tri + tri * tri * (3 - 2 * tri));
      const a = f32(allpassCoef(low * Math.exp(2.1972245773362196 * bent))); // ln 9
      l[i] = gain * (inL + allpassChain(inL, a, apL, 0, 4));
      r[i] = gain * (inR + allpassChain(inR, a, apR, 0, 4));
    }
    this.phase = phase;
  }

  // ---- Panned Phaser
  processPanned(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], aS = sm[SM_A], bS = sm[SM_B], dryS = sm[SM_DRY], wetS = sm[SM_WET], apL = this.apL;
    const inc = this.rateHz * this.invFs, panInc = this.rate2Hz * this.invFs, centre = (PI * 550) / this.fs;
    let phase = this.phase, phase2 = this.phase2, fbL = this.fbL;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      phase2 += panInc;
      if (phase2 >= 1) phase2 -= 1;
      const a = f32(allpassCoef(centre * Math.exp(1.1090354888959124 * depthS.next() * Math.sin(TWO_PI * phase)))); // +-1.6 octaves
      fbL = f32(allpassChain(0.5 * (inL + inR) + PANNED_FEEDBACK_F * fbL, a, apL, 0, 4));
      const pan = aS.next() + bS.next() * Math.sin(TWO_PI * phase2);
      const dry = dryS.next(), wet = wetS.next() * fbL;
      l[i] = dry * inL + wet * balance(1 - pan);
      r[i] = dry * inR + wet * balance(pan);
    }
    this.phase = phase; this.phase2 = phase2; this.fbL = fbL;
  }

  // ---- Barberpole Phaser
  processBarberpole(l, r, n) {
    const sm = this.sm, fbS = sm[SM_FEEDBACK], dryS = sm[SM_DRY], wetS = sm[SM_WET], dirLS = sm[SM_DIR_L], dirRS = sm[SM_DIR_R];
    const inc = this.rateHz * this.invFs, coef = this.barberCoef, apL = this.apL, apR = this.apR, lp = this.lp;
    const hI = this.hilbertI, hQ = this.hilbertQ, h = this.hilbert, hd = this.hilbertDelay;
    const fbLowPass = this.fbLowPass, fbHighPass = this.fbHighPass;
    let phase = this.phase, fbL = this.fbL, fbR = this.fbR;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      const c = f32(Math.cos(TWO_PI * phase)), s = f32(Math.sin(TWO_PI * phase));
      const fb = fbS.next(), dry = dryS.next(), wet = wetS.next();
      const dirL = dirLS.next(), dirR = dirRS.next();

      let x = inL + fb * fbL;
      for (let k = 0; k < BARBER_STAGES; ++k) {
        const y = coef[k] * x + apL[k];
        apL[k] = x - coef[k] * y;
        x = y;
      }
      let si = hilbertPath(x, hI, h, 0);
      let sq = hd[0];
      hd[0] = hilbertPath(x, hQ, h, 14);
      const wetL = si * c - dirL * sq * s;
      lp[0] += fbLowPass * (wetL - lp[0]);
      lp[1] += fbHighPass * (lp[0] - lp[1]);
      fbL = f32(lp[0] - lp[1]);

      x = inR + fb * fbR;
      for (let k = 0; k < BARBER_STAGES; ++k) {
        const y = coef[k] * x + apR[k];
        apR[k] = x - coef[k] * y;
        x = y;
      }
      si = hilbertPath(x, hI, h, 28);
      sq = hd[1];
      hd[1] = hilbertPath(x, hQ, h, 42);
      const wetR = si * c - dirR * sq * s;
      lp[2] += fbLowPass * (wetR - lp[2]);
      lp[3] += fbHighPass * (lp[2] - lp[3]);
      fbR = f32(lp[2] - lp[3]);

      l[i] = dry * inL + wet * wetL;
      r[i] = dry * inR + wet * wetR;
    }
    this.phase = phase; this.fbL = fbL; this.fbR = fbR;
  }

  // ---- Dual Phaser
  processDual(l, r, n) {
    const sm = this.sm, shapeS = sm[SM_SHAPE], depthS = sm[SM_DEPTH], fbS = sm[SM_FEEDBACK], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const inc = this.rateHz * this.invFs, centre = (PI * 420) / this.fs, edge = squareEdge(this.rateHz);
    const cellUp = this.cellUp, cellDown = this.cellDown, apL = this.apL, apR = this.apR;
    let phase = this.phase, ldrA = this.ldrA, ldrB = this.ldrB, fbL = this.fbL, fbR = this.fbR;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i], m = f32(0.5 * (inL + inR));
      phase += inc;
      if (phase >= 1) phase -= 1;
      const amount = shapeS.next();
      const u = sineToSquare(Math.sin(TWO_PI * phase), amount * amount * edge);
      ldrA += (u > ldrA ? cellUp : cellDown) * (u - ldrA);
      ldrB += (-u > ldrB ? cellUp : cellDown) * (-u - ldrB);
      const range = 1.178350206951907 * depthS.next(); // +-1.7 octaves
      const aA = f32(allpassCoef(centre * Math.exp(range * ldrA))), aB = f32(allpassCoef(centre * Math.exp(range * ldrB)));
      const fb = fbS.next(), dry = dryS.next(), wet = wetS.next();
      fbL = f32(allpassChain(m + fb * fbL, aA, apL, 0, 6));
      fbR = f32(allpassChain(m + fb * fbR, aB, apR, 0, 6));
      l[i] = dry * inL + wet * fbL;
      r[i] = dry * inR + wet * fbR;
    }
    this.phase = phase; this.ldrA = ldrA; this.ldrB = ldrB; this.fbL = fbL; this.fbR = fbR;
  }

  // ---- U-Vibe
  processVibe(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], fbS = sm[SM_FEEDBACK], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const base = (PI * 600) / this.fs, lampUp = this.lampUp, lampDown = this.lampDown, apL = this.apL;
    let phase = this.phase, lamp = this.lamp, fbL = this.fbL;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i], m = f32(0.5 * (inL + inR));
      phase += this.sensedIncrement(m);
      if (phase >= 1) phase -= 1;
      const glow = 0.5 + 0.5 * Math.sin(TWO_PI * phase);
      lamp += (glow > lamp ? lampUp : lampDown) * (glow - lamp);
      const w = base * Math.exp(2.772588722239781 * depthS.next() * (lamp * lamp - 0.45)); // 4 octaves
      const fb = fbS.next();
      let x = m + fb * fbL;
      for (let k = 0; k < 4; ++k) {
        const a = f32(allpassCoef(Math.min(1.35, w * VIBE_RATIO[k])));
        const y = a * x + apL[k];
        apL[k] = x - a * y;
        x = y;
      }
      fbL = x = f32(x);
      const dry = dryS.next(), wet = wetS.next() * x;
      l[i] = dry * inL + wet;
      r[i] = dry * inR + wet;
    }
    this.phase = phase; this.lamp = lamp; this.fbL = fbL;
  }

  // ---- Phaser
  processPhaser(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], fbS = sm[SM_FEEDBACK], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const inc = this.rateHz * this.invFs, centre = (PI * 600) / this.fs, apL = this.apL, apR = this.apR;
    let taps = 1;
    for (let t = 1; t < 4; ++t) if (sm[SM_TAP0 + t].target > 0 || sm[SM_TAP0 + t].isSmoothing()) taps = t + 1;

    let phase = this.phase, fbL = this.fbL, fbR = this.fbR;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i], m = f32(0.5 * (inL + inR));
      phase += inc;
      if (phase >= 1) phase -= 1;
      const range = 1.1090354888959124 * depthS.next();
      const aL = f32(allpassCoef(centre * Math.exp(range * Math.sin(TWO_PI * phase))));
      const aR = f32(allpassCoef(centre * Math.exp(range * Math.cos(TWO_PI * phase))));
      const fb = fbS.next(), dry = dryS.next(), wet = wetS.next();
      let xl = m + fb * fbL, xr = m + fb * fbR, outL = 0, outR = 0;
      for (let t = 0; t < 4; ++t) {
        const gain = sm[SM_TAP0 + t].next();
        if (t < taps) {
          xl = allpassChain(xl, aL, apL, 4 * t, 4);
          xr = allpassChain(xr, aR, apR, 4 * t, 4);
          outL += gain * xl;
          outR += gain * xr;
        }
      }
      fbL = outL = f32(outL);
      fbR = outR = f32(outR);
      l[i] = dry * inL + wet * outL;
      r[i] = dry * inR + wet * outR;
    }
    this.phase = phase; this.fbL = fbL; this.fbR = fbR;
  }

  // ---- Pitch Vibrato
  processVibrato(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const fs = this.fs, line = this.lines[0], lp = this.lp, wetLowPass = this.wetLowPass, riseStep = this.riseStep;
    let phase = this.phase, rise = this.rise;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i], m = f32(0.5 * (inL + inR));
      rise = Math.min(1, rise + riseStep);
      const ramp = rise * rise * (3 - 2 * rise);
      phase += this.sensedIncrement(m) * (0.5 + 0.5 * ramp);
      if (phase >= 1) phase -= 1;
      const w = line.read(f32(fs * (0.005 + 0.0016 * depthS.next() * ramp * Math.sin(TWO_PI * phase))));
      line.push(softClip(m));
      lp[0] += wetLowPass * (w - lp[0]);
      lp[1] += wetLowPass * (lp[0] - lp[1]);
      const dry = dryS.next(), wet = wetS.next() * lp[1];
      l[i] = dry * inL + wet;
      r[i] = dry * inR + wet;
    }
    this.phase = phase; this.rise = rise;
  }

  // ---- Dimension
  processDimension(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const inc = this.rateHz * this.invFs, fs = this.fs, line = this.lines[0], lp = this.lp;
    const wetLowPass = this.wetLowPass, toneCoef = this.toneCoef, cross = f32(0.4);
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i], m = f32(0.5 * (inL + inR));
      phase += inc;
      if (phase >= 1) phase -= 1;
      const swing = depthS.next() * (1 - 4 * Math.abs(phase - 0.5));
      const a = line.read(f32(fs * (0.008 + swing))), b = line.read(f32(fs * (0.008 - swing)));
      lp[4] += toneCoef * (m - lp[4]);
      line.push(softClip(m - lp[4]));
      lp[0] += wetLowPass * (a - lp[0]);
      lp[1] += wetLowPass * (lp[0] - lp[1]);
      lp[2] += wetLowPass * (b - lp[2]);
      lp[3] += wetLowPass * (lp[2] - lp[3]);
      const dry = dryS.next(), wet = wetS.next();
      l[i] = dry * inL + wet * (lp[1] - cross * lp[3]);
      r[i] = dry * inR + wet * (lp[3] - cross * lp[1]);
    }
    this.phase = phase;
  }

  // ---- Analog Chorus
  processChorus(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], aS = sm[SM_A], bS = sm[SM_B], toneS = sm[SM_TONE];
    const dryS = sm[SM_DRY], wetS = sm[SM_WET], dryRS = sm[SM_DRY_R], wetRS = sm[SM_WET_R];
    const inc = this.rateHz * this.invFs, fs = this.fs, line = this.lines[0], lp = this.lp, wetLowPass = this.wetLowPass;
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      const vibrato = aS.next();
      const tri = 1 - 4 * Math.abs(phase - 0.5);
      const lfo = tri + vibrato * (-Math.cos(TWO_PI * phase) - tri);
      const chorusSwing = bS.next();
      const swing = depthS.next() * (chorusSwing + vibrato * (0.0024 - chorusSwing));
      const w = line.read(f32(fs * (0.007 - 0.002 * vibrato + swing * lfo)));
      line.push(softClip(f32(0.5 * (inL + inR))));
      lp[0] += wetLowPass * (w - lp[0]);
      lp[1] += wetLowPass * (lp[0] - lp[1]);
      lp[2] += toneS.next() * (lp[1] - lp[2]);
      l[i] = dryS.next() * inL + wetS.next() * lp[2];
      r[i] = dryRS.next() * inR + wetRS.next() * lp[2];
    }
    this.phase = phase;
  }

  // ---- Tri Chorus
  processTriChorus(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], aS = sm[SM_A], bS = sm[SM_B], cS = sm[SM_C], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const fs = this.fs, inc = this.rateHz * this.invFs, inc2 = 5.3 * inc, line = this.lines[0], lp = this.lp, wetLowPass = this.wetLowPass;
    const slowSwing = 0.0022 * fs, quickSwing = 0.00012 * fs, side = f32(0.75), both = f32(0.7);
    let phase = this.phase, phase2 = this.phase2;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      phase2 += inc2;
      if (phase2 >= 1) phase2 -= 1;
      const s1 = Math.sin(TWO_PI * phase), c1 = Math.cos(TWO_PI * phase);
      const s2 = Math.sin(TWO_PI * phase2), c2 = Math.cos(TWO_PI * phase2);
      const scale = cS.next(), slow = slowSwing * scale, quick = quickSwing * scale;
      const d1 = depthS.next(), d2 = aS.next(), d3 = bS.next();
      const t1 = line.read(f32(0.006 * fs + d1 * (slow * s1 + quick * s2)));
      const t2 = line.read(f32(0.008 * fs + d2 * (slow * (-0.5 * s1 + SIN120 * c1) + quick * (-0.5 * s2 - SIN120 * c2))));
      const t3 = line.read(f32(0.0105 * fs + d3 * (slow * (-0.5 * s1 - SIN120 * c1) + quick * (-0.5 * s2 + SIN120 * c2))));
      line.push(softClip(f32(0.5 * (inL + inR))));
      const wl = side * (t1 + both * t2), wr = side * (t3 + both * t2);
      lp[0] += wetLowPass * (wl - lp[0]);
      lp[1] += wetLowPass * (lp[0] - lp[1]);
      lp[2] += wetLowPass * (wr - lp[2]);
      lp[3] += wetLowPass * (lp[2] - lp[3]);
      const dry = dryS.next(), wet = wetS.next();
      l[i] = dry * inL + wet * lp[1];
      r[i] = dry * inR + wet * lp[3];
    }
    this.phase = phase; this.phase2 = phase2;
  }

  // ---- the four flangers
  processFlanger(l, r, n, v) {
    const sm = this.sm, depthS = sm[SM_DEPTH], aS = sm[SM_A], bS = sm[SM_B], fbS = sm[SM_FEEDBACK], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const inc = this.rateHz * this.invFs, maxDelay = 0.001 * v.maxDelayMs * this.fs, lnRatio = Math.log(v.ratio);
    const drive = v.drive, invDrive = f32(1 / v.drive), lp = this.lp, wetLowPass = this.wetLowPass;
    const line0 = this.lines[0], line1 = this.lines[1], compAttack = this.compAttack, compRelease = this.compRelease;
    const exponential = v.exponential, ratio = v.ratio, makeUp = f32(1.25);
    let phase = this.phase, compEnv = this.compEnv;
    for (let i = 0; i < n; ++i) {
      let inL = l[i], inR = r[i];
      const m = f32(0.5 * (inL + inR));
      let feed = m;
      if (v.compressor) {
        // the compressor in front: 2:1 above -20 dB, 2 dB of make-up (see the C++)
        const a = Math.abs(m);
        compEnv = f32(compEnv + f32((a > compEnv ? compAttack : compRelease) * f32(a - compEnv)));
        const g = f32(makeUp * f32(Math.sqrt(f32(COMP_THRESHOLD / Math.max(compEnv, COMP_THRESHOLD)))));
        const dryGain = f32(1 + f32(bS.next() * f32(g - 1)));
        feed = f32(m * g);
        inL = f32(inL * dryGain);
        inR = f32(inR * dryGain);
      }
      phase += inc;
      if (phase >= 1) phase -= 1;
      const depth = depthS.next(), manual = aS.next();
      const fb = fbS.next(), dry = dryS.next(), wet = wetS.next();

      let cv = manual + depth * (1 - Math.abs(2 * phase - 1) - manual);
      let w = invDrive * line0.read(f32(exponential ? maxDelay * Math.exp(-lnRatio * cv) : maxDelay / (1 + (ratio - 1) * cv)));
      lp[0] += wetLowPass * (w - lp[0]);
      lp[1] += wetLowPass * (lp[0] - lp[1]);
      line0.push(softClip(drive * (feed + fb * lp[1])));

      if (v.mono) {
        l[i] = r[i] = dry * 0.5 * (inL + inR) + wet * lp[1];
        continue;
      }

      let pr = phase + v.offset;
      if (pr >= 1) pr -= 1;
      cv = manual + depth * (1 - Math.abs(2 * pr - 1) - manual);
      w = invDrive * line1.read(f32(exponential ? maxDelay * Math.exp(-lnRatio * cv) : maxDelay / (1 + (ratio - 1) * cv)));
      lp[2] += wetLowPass * (w - lp[2]);
      lp[3] += wetLowPass * (lp[2] - lp[3]);
      line1.push(softClip(drive * (feed + fb * lp[3])));

      l[i] = dry * inL + wet * lp[1];
      r[i] = dry * inR + wet * lp[3];
    }
    this.phase = phase; this.compEnv = compEnv;
  }

  // ---- Frequency Shifter
  processShifter(l, r, n) {
    const sm = this.sm, dryS = sm[SM_DRY], wetS = sm[SM_WET], dirLS = sm[SM_DIR_L], dirRS = sm[SM_DIR_R];
    const inc = this.rateHz * this.invFs, hI = this.hilbertI, hQ = this.hilbertQ, h = this.hilbert, hd = this.hilbertDelay;
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      const c = f32(Math.cos(TWO_PI * phase)), s = f32(Math.sin(TWO_PI * phase));
      const dry = dryS.next(), wet = wetS.next();
      const dirL = dirLS.next(), dirR = dirRS.next();

      let si = hilbertPath(inL, hI, h, 0);
      let sq = hd[0];
      hd[0] = hilbertPath(inL, hQ, h, 14);
      l[i] = dry * inL + wet * (si * c - dirL * sq * s);

      si = hilbertPath(inR, hI, h, 28);
      sq = hd[1];
      hd[1] = hilbertPath(inR, hQ, h, 42);
      r[i] = dry * inR + wet * (si * c - dirR * sq * s);
    }
    this.phase = phase;
  }

  // ---- Ring Modulator
  processRing(l, r, n) {
    const sm = this.sm, shapeS = sm[SM_SHAPE], depthS = sm[SM_DEPTH], aS = sm[SM_A], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const inc = this.rateHz * this.invFs, fs = this.fs, line = this.lines[0];
    let phase = this.phase;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      phase += inc;
      if (phase >= 1) phase -= 1;
      const amount = shapeS.next(), g = 12 * amount * amount, depth = depthS.next();
      const carL = sineToSquare(Math.sin(TWO_PI * phase), g), carR = sineToSquare(Math.cos(TWO_PI * phase), g);
      const fmL = line.read(f32(fs * (0.0006 + 0.00045 * depth * carL)));
      const fmR = line.read(f32(fs * (0.0006 + 0.00045 * depth * carR)));
      line.push(f32(0.5 * (inL + inR)));
      const amL = inL * f32(1 - depth + depth * carL), amR = inR * f32(1 - depth + depth * carR);
      const blend = aS.next(), dry = dryS.next(), wet = wetS.next();
      l[i] = dry * inL + wet * (amL + blend * (fmL - amL));
      r[i] = dry * inR + wet * (amR + blend * (fmR - amR));
    }
    this.phase = phase;
  }

  // ---- Rotary Drum
  processDrum(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], bS = sm[SM_B], cS = sm[SM_C], toneS = sm[SM_TONE], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const fs = this.fs, invFs = this.invFs, line = this.lines[0], lp = this.lp, target = this.drumTarget;
    const up = this.glide(0.5), down = this.glide(0.8), base = 0.001 * fs, swing = 0.00035 * fs;
    const band = f32(this.lowPass(5000)), split = f32(this.lowPass(1200));
    let speed = this.drumSpeed, angle = this.drumAngle;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      const x = tubeClip(f32(0.5 * (inL + inR)) * bS.next()) * cS.next();
      lp[0] += band * (x - lp[0]);
      lp[1] += band * (lp[0] - lp[1]);
      line.push(lp[1]);
      speed += (target > speed ? up : down) * (target - speed);
      angle += speed * invFs;
      if (angle >= 1) angle -= 1;
      const depth = depthS.next();
      const c = Math.cos(TWO_PI * angle), s = Math.sin(TWO_PI * angle);
      const micL = rotorTap(line, c, depth, base, swing, split, lp, 4, SHADOW.drumLow, SHADOW.drumHigh);
      const micR = rotorTap(line, -s, depth, base, swing, split, lp, 5, SHADOW.drumLow, SHADOW.drumHigh);
      const tone = toneS.next();
      lp[2] += tone * (micL - lp[2]);
      lp[3] += tone * (micR - lp[3]);
      const dry = dryS.next(), wet = wetS.next() * f32(1 + 0.3 * depth);
      l[i] = dry * inL + wet * lp[2];
      r[i] = dry * inR + wet * lp[3];
    }
    this.drumSpeed = speed; this.drumAngle = angle;
  }

  // ---- Rotary Drm/Hrn
  processLeslie(l, r, n) {
    const sm = this.sm, depthS = sm[SM_DEPTH], aS = sm[SM_A], bS = sm[SM_B], cS = sm[SM_C], dryS = sm[SM_DRY], wetS = sm[SM_WET];
    const fs = this.fs, invFs = this.invFs, line0 = this.lines[0], line1 = this.lines[1], lp = this.lp;
    const hornTarget = this.hornTarget, drumTarget = this.drumTarget;
    const hornUp = this.glide(0.3), hornDown = this.glide(0.5), drumUp = this.glide(1.5), drumDown = this.glide(1.2);
    const base = 0.001 * fs, hornSwing = 0.0005 * fs, drumSwing = 0.00025 * fs;
    const cross = f32(this.lowPass(800)), top = f32(this.lowPass(7000));
    const hornSplit = f32(this.lowPass(2500)), drumSplit = f32(this.lowPass(400));
    let hornSpeed = this.hornSpeed, hornAngle = this.hornAngle, drumSpeed = this.drumSpeed, drumAngle = this.drumAngle;
    for (let i = 0; i < n; ++i) {
      const inL = l[i], inR = r[i];
      const x = f32(tubeClip(f32(0.5 * (inL + inR)) * bS.next()) * cS.next());
      // 12 dB per octave both ways; the horn is wired out of phase, as such a crossover needs to add up flat
      lp[0] += cross * (x - lp[0]);
      lp[1] += cross * (lp[0] - lp[1]);
      const h1 = f32(x - lp[8]);
      lp[8] += cross * h1;
      const h2 = f32(h1 - lp[9]);
      lp[9] += cross * h2;
      lp[2] += top * (-h2 - lp[2]);
      line0.push(lp[2]);
      line1.push(lp[1]);
      hornSpeed += (hornTarget > hornSpeed ? hornUp : hornDown) * (hornTarget - hornSpeed);
      hornAngle += hornSpeed * invFs;
      if (hornAngle >= 1) hornAngle -= 1;
      drumSpeed += (drumTarget > drumSpeed ? drumUp : drumDown) * (drumTarget - drumSpeed);
      drumAngle += drumSpeed * invFs;
      if (drumAngle >= 1) drumAngle -= 1;
      const drumDepth = depthS.next(), hornDepth = aS.next();
      const hc = Math.cos(TWO_PI * hornAngle), hs = Math.sin(TWO_PI * hornAngle);
      const dc = Math.cos(TWO_PI * drumAngle), ds = Math.sin(TWO_PI * drumAngle);
      const hornGain = f32(1 + 0.35 * hornDepth), drumGain = f32(1 + 0.2 * drumDepth);
      const micL = hornGain * rotorTap(line0, hc, hornDepth, base, hornSwing, hornSplit, lp, 4, SHADOW.hornLow, SHADOW.hornHigh)
                 + drumGain * rotorTap(line1, dc, drumDepth, base, drumSwing, drumSplit, lp, 6, SHADOW.bassLow, SHADOW.bassHigh);
      const micR = hornGain * rotorTap(line0, -hs, hornDepth, base, hornSwing, hornSplit, lp, 5, SHADOW.hornLow, SHADOW.hornHigh)
                 + drumGain * rotorTap(line1, ds, drumDepth, base, drumSwing, drumSplit, lp, 7, SHADOW.bassLow, SHADOW.bassHigh);
      const dry = dryS.next(), wet = wetS.next();
      l[i] = dry * inL + wet * micL;
      r[i] = dry * inR + wet * micR;
    }
    this.hornSpeed = hornSpeed; this.hornAngle = hornAngle; this.drumSpeed = drumSpeed; this.drumAngle = drumAngle;
  }
}

const speed = (def) => hertz("Speed", 0.05, 10, def, 1);
const mod = (key, name, variant, basedOn, knobs) => model(key, name, CATEGORY.modulation, ENGINE.modFx, variant, basedOn, knobs);

/** In the order of the variants. */
export const MOD_MODELS = [
  mod("pattern_tremolo", "Pattern Tremolo", PATTERN_TREMOLO, "Inspired by Lightfoot Labs Goatkeeper",
    [speed(2), choice("Step 1", STEP_NAMES, 0), choice("Step 2", STEP_NAMES, 1), choice("Step 3", STEP_NAMES, 3), choice("Step 4", STEP_NAMES, 1)]),
  mod("panner", "Panner", PANNER, "Auto-panner",
    [speed(1.5), percent("Depth", 100), percent("Shape", 50), percent("VolSens", 0), percent("Mix", 100)]),
  mod("bias_tremolo", "Bias Tremolo", BIAS_TREMOLO, "1960 Vox AC-15 tremolo",
    [speed(4), percent("Level", 50), percent("Shape", 0), percent("VolSens", 0), percent("Mix", 100)]),
  mod("opto_tremolo", "Opto Tremolo", OPTO_TREMOLO, "Blackface Fender optical tremolo",
    [speed(5), percent("Level", 60), percent("Shape", 30), percent("VolSens", 0), percent("Mix", 100)]),
  mod("script_phase", "Script Phase", SCRIPT_PHASE, "MXR Phase 90 (script logo)",
    [speed(0.7)]),
  mod("panned_phaser", "Panned Phaser", PANNED_PHASER, "Ibanez Flying Pan",
    [speed(0.5), percent("Depth", 70), choice("Pan", PAN_NAMES, 1), hertz("Pan Spd", 0, 10, 0.3, 1), percent("Mix", 50)]),
  mod("barberpole_phaser", "Barberpole Phaser", BARBERPOLE_PHASER, "Modular-synth barberpole phaser",
    [speed(0.3), percent("Fdbk", 40), choice("Mode", SWEEP_NAMES, 0), percent("Mix", 50)]),
  mod("dual_phaser", "Dual Phaser", DUAL_PHASER, "Mu-Tron Bi-Phase",
    [speed(0.35), percent("Depth", 75), percent("Fdbk", 45), percent("LFO Shp", 0), percent("Mix", 50)]),
  mod("u_vibe", "U-Vibe", U_VIBE, "Uni-Vibe",
    [speed(1.6), percent("Depth", 80), percent("Fdbk", 0), percent("VolSens", 0), percent("Mix", 50)]),
  mod("phaser_hd", "Phaser", PHASER, "Inspired by MXR Phase 90",
    [speed(0.5), percent("Depth", 70), percent("Fdbk", 40), choice("Stages", STAGE_NAMES, 0), percent("Mix", 50)]),
  mod("pitch_vibrato", "Pitch Vibrato", PITCH_VIBRATO, "Boss VB-2",
    [speed(5), percent("Depth", 40), percent("Rise", 75), percent("VolSens", 0), percent("Mix", 100)]),
  mod("dimension", "Dimension", DIMENSION, "Roland Dimension D",
    [choice("Sw1", SWITCH_NAMES, 0), choice("Sw2", SWITCH_NAMES, 0), choice("Sw3", SWITCH_NAMES, 1), choice("Sw4", SWITCH_NAMES, 0), percent("Mix", 50)]),
  mod("analog_chorus", "Analog Chorus", ANALOG_CHORUS, "Boss CE-1 Chorus Ensemble",
    [speed(0.6), percent("Depth", 50), choice("Ch Vib", CHORUS_NAMES, 0), percent("Tone", 50), percent("Mix", 50)]),
  mod("tri_chorus", "Tri Chorus", TRI_CHORUS, "Song Bird / DyTronics Tri-Stereo Chorus",
    [speed(0.5), percent("Depth", 60), percent("Depth2", 50), percent("Depth3", 60), percent("Mix", 50)]),
  mod("analog_flanger", "Analog Flanger", ANALOG_FLANGER, "Inspired by MXR Flanger",
    [speed(0.3), percent("Depth", 70), percent("Fdbk", 50), percent("Manual", 30), percent("Mix", 50)]),
  mod("jet_flanger", "Jet Flanger", JET_FLANGER, "Inspired by A/DA Flanger",
    [speed(0.2), percent("Depth", 85), percent("Fdbk", 65), percent("Manual", 30), percent("Mix", 50)]),
  mod("ac_flanger", "AC Flanger", AC_FLANGER, "MXR Flanger",
    [speed(0.35), percent("Width", 60), percent("Regen", 55), percent("Manual", 25)]),
  mod("80a_flanger", "80A Flanger", FLANGER_80A, "A/DA Flanger",
    [speed(0.25), percent("Range", 80), percent("Enhance", 60), percent("Manual", 30), choice("Even Odd", HARMONIC_NAMES, 0)]),
  mod("frequency_shifter", "Frequency Shifter", FREQUENCY_SHIFTER, "Modular-synth frequency shifter",
    [hertz("Freq", 0, 2000, 12, 40), choice("Mode", SWEEP_NAMES, 0), percent("Mix", 50)]),
  mod("ring_modulator", "Ring Modulator", RING_MODULATOR, "Ring modulator",
    [hertz("Speed", 1, 2000, 120, 100), percent("Depth", 100), percent("Shape", 0), percent("AM/FM", 0), percent("Mix", 50)]),
  mod("rotary_drum", "Rotary Drum", ROTARY_DRUM, "Fender Vibratone",
    [choice("Speed", ROTOR_NAMES, 1), percent("Depth", 70), percent("Tone", 60), percent("Drive", 25), percent("Mix", 80)]),
  mod("rotary_drm_hrn", "Rotary Drm/Hrn", ROTARY_DRUM_HORN, "Leslie 145",
    [choice("Speed", ROTOR_NAMES, 0), percent("Depth", 70), percent("Horn Dep", 70), percent("Drive", 30), percent("Mix", 100)]),
];
