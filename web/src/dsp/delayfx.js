// The HD500X's 19 Delay models (Source/DSP/fx/DelayFx.h), ported line for line.
// Returns the wet signal only; processDry() adds the original unit's preamp colour to the slot's dry signal
// for Tube Echo, Tape Echo, Sweep Echo and Echo Platter.
//
// Knobs: Time, Note, Fdbk, <two of the model's own>, Mix (the Note knob is ignored here).
// Stereo Delay: L Time, L Note, L Fdbk, R Time, R Note, R Fdbk, Mix.
import { Biquad, OnePole, PI, clamp, f32 } from "./core.js";
import { CATEGORY, ENGINE, DELAY_NOTE_NAMES, DELAY_NOTE_BEATS, model, millis, percent, hertz, choice } from "./modeltypes.js";

const TWO_PI = 6.283185307179586, LN2 = 0.6931471805599453;

const V = { pingPong: 0, dynamicDly: 1, stereoDelay: 2, digitalDelay: 3, digMod: 4, reverse: 5, loRes: 6, tubeEcho: 7, tubeEchoDry: 8,
            tapeEcho: 9, tapeEchoDry: 10, sweepEcho: 11, sweepEchoDry: 12, echoPlatter: 13, echoPlatterDry: 14, analogMod: 15,
            analogEcho: 16, autoVolume: 17, multiHead: 18, numVariants: 19 };

/** Linear parameter ramp kept in double: steps through exactly the same values as the C++ one. */
class Ramp {
  constructor() { this.current = 0; this.target = 0; this.step = 0; this.countdown = 0; this.steps = 1; }
  prepare(fs, seconds) { this.steps = Math.max(1, Math.trunc(seconds * fs)); }
  set(v) {
    if (v === this.target) return;
    this.target = v;
    this.countdown = this.steps;
    this.step = (this.target - this.current) / this.steps;
  }
  snap() { this.current = this.target; this.countdown = 0; }
  next() {
    if (this.countdown > 0) {
      if (--this.countdown === 0) this.current = this.target;
      else this.current += this.step;
    }
    return this.current;
  }
  isZero() { return this.countdown === 0 && this.current === 0; }
}

/** Circular delay line over a piece of the engine's memory. read(d) / readInt(d): the sample pushed d pushes ago. */
class Line {
  constructor() { this.data = new Float32Array(8); this.size = 8; this.writePos = 0; }
  push(x) {
    this.data[this.writePos] = x;
    if (++this.writePos === this.size) this.writePos = 0;
  }
  readInt(d) {
    let i = this.writePos - d;
    if (i < 0) i += this.size;
    return this.data[i];
  }
  /** 4-point Hermite; an integer d returns the stored sample exactly. */
  read(d) {
    const size = this.size, data = this.data;
    if (d < 2) d = 2;
    else if (d > size - 4) d = size - 4;

    const di = Math.floor(d);
    const frac = f32(d - di);
    let i0 = this.writePos - di;  if (i0 < 0) i0 += size;
    let im = i0 + 1;              if (im >= size) im -= size;
    let i1 = i0 - 1;              if (i1 < 0) i1 += size;
    let i2 = i1 - 1;              if (i2 < 0) i2 += size;

    const xm1 = data[im], x0 = data[i0], x1 = data[i1], x2 = data[i2];
    const c1 = 0.5 * (x1 - xm1);
    const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * frac + c2) * frac + c1) * frac + x0;
  }
}

/** One read position of a digital delay; a new time is reached by crossfading to a second read position. */
class Tap {
  constructor() { this.current = 9600; this.next = 9600; this.target = 9600; this.count = 0; }
}

/** Transparent up to +-1, then bends over towards +-1.5: keeps a digital loop at 100 % feedback bounded. */
function softLimit(x) {
  const a = Math.abs(x);
  if (a <= 1) return x;
  const over = a - 1, y = 1 + over / (1 + 2 * over);
  return x < 0 ? -y : y;
}

/** tanh with a bias: tape (bias 0) or a tube / FET / BBD stage. Unity gain for small signals. */
class Saturator {
  constructor() { this.gain = 1; this.shift = 0; this.offset = 0; this.norm = 1; }
  set(drive, bias) {
    const o = Math.tanh(drive * bias);
    this.gain = f32(drive);
    this.shift = f32(drive * bias);
    this.offset = f32(Math.tanh(this.shift));
    this.norm = f32(1 / (drive * (1 - o * o)));
  }
  process(x) { return (Math.tanh(this.gain * x + this.shift) - this.offset) * this.norm; }
}

/** Peak filter (the same response as Biquad.setPeak) as a state-variable filter: precise in float at low centres. */
class Bell {
  constructor() { this.a1 = 1; this.a2 = 0; this.a3 = 0; this.peak = 0; this.s1 = 0; this.s2 = 0; }
  set(fs, hz, q, gainDb) {
    const A = Math.pow(10, gainDb / 40), k = 1 / (q * A);
    const g = Math.tan(PI * clamp(hz, 1, 0.49 * fs) / fs), k1 = 1 / (1 + g * (g + k));
    this.a1 = f32(k1);
    this.a2 = f32(g * k1);
    this.a3 = f32(g * g * k1);
    this.peak = f32(k * (A * A - 1));
  }
  reset() { this.s1 = this.s2 = 0; }
  process(x) {
    const v3 = x - this.s2, v1 = this.a1 * this.s1 + this.a2 * v3, v2 = this.s2 + this.a2 * this.s1 + this.a3 * v3;
    this.s1 = 2 * v1 - this.s1;
    this.s2 = 2 * v2 - this.s2;
    return x + this.peak * v1;
  }
}

// What makes each tape / platter / BBD echo its own (see Voicing in DelayFx.h)
const voicing = (lowPassHz, lowPassQ, highPassHz, bumpHz, bumpDb, bumpQ, preHz, driveMin, driveMax, bias, wowHz, wowDeviation, drift, dry) =>
  ({ lowPassHz, lowPassQ, highPassHz, bumpHz, bumpDb, bumpQ, preHz, driveMin, driveMax, bias, wowHz, wowDeviation, drift, dry });
const VOICING_EP1     = voicing(3800, 0.65, 85,  120, 2.5, 0.9,  0,    0.7, 3.2, 0.12,   [0.55, 1.9, 7.3], [0.0055, 0.0022, 0.002], 0.0022, 1);
const VOICING_EP3     = voicing(4600, 0.70, 65,  100, 2.0, 1.0,  0,    0.9, 0.9, 0.03,   [0.55, 1.9, 7.3], [0.0055, 0.0022, 0.002], 0.0022, 2);
const VOICING_SWEEP   = voicing(3800, 0.65, 85,  120, 2.5, 0.9,  0,    1.4, 1.4, 0.12,   [0.55, 1.9, 7.3], [0.0055, 0.0022, 0.002], 0.0022, 1);
const VOICING_PLATTER = voicing(5000, 0.70, 160, 1800, 1.5, 0.8, 0,    0.6, 2.6, 0.10,   [0.37, 2.6, 9.5], [0.003, 0.006, 0.003],   0.002,  3);
const VOICING_DMM     = voicing(3600, 0.75, 60,  0, 0, 1.0,      5000, 1.15, 1.15, 0.08, [1, 1, 1],        [0, 0, 0],               0,      0);
const VOICING_DM2     = voicing(2900, 0.80, 110, 0, 0, 1.0,      4000, 1.7, 1.7, 0.12,   [1, 1, 1],        [0, 0, 0],               0,      0);
const VOICING_SWELL   = voicing(5000, 0.70, 50,  100, 1.5, 1.0,  0,    0.9, 0.9, 0,      [0.55, 1.9, 7.3], [0.0055, 0.0022, 0.002], 0.0022, 0);
const VOICING_RE101   = voicing(4200, 0.70, 95,  95, 3.0, 1.0,   0,    1.1, 1.1, 0.05,   [0.45, 1.3, 8.9], [0.005, 0.0025, 0.002],  0.0025, 0);

const EQ_RANGE_DB = 9, EQ_BASS_HZ = 250, EQ_TREBLE_HZ = 2500;
const SWEEP_CENTRE_HZ = 800, SWEEP_OCTAVES = 2.2, SWEEP_DAMPING = 0.4;
const SWELL_ON = 0.006, SWELL_OFF = 0.002, SWELL_ONSET_RATIO = 1.6;
const HEAD_RATIO = [0.25, 0.5, 0.75, 1.0], HEAD_MIX_KNEE = f32(0.6), HEAD_MIX_SCALE = f32(1 / f32(0.6));
const REGEN_KNEE = 0.85, REGEN_MAX = 1.2; // tape, platter and BBD echoes regenerate past unity at the top of the Fdbk knob
const NUM_KNOBS_READ = 7;

const toSeconds = (ms) => clamp(ms, 20, 2000) * 0.001;
const toUnit = (percentValue) => clamp(percentValue * 0.01, 0, 1);

/** How many repeats after the first one until they are 60 dB down. */
function repeats(fb) {
  if (fb >= 0.999) return 1.0e6;
  return Math.log(0.001) / Math.log(Math.max(fb, 1.0e-6));
}

/** Moves `value` towards `target` by at most `maxStep`. */
function approach(value, target, maxStep) {
  if (value === target) return value;
  return target > value ? Math.min(target, value + maxStep) : Math.max(target, value - maxStep);
}

/** A bass and a treble shelf: out = in + amount * (low- / high-passed in); states at index `s`. */
function shelfPair(x, lowState, highState, s, bass, treble, lowCoef, highCoef) {
  let v = (x - lowState[s]) * lowCoef, low = v + lowState[s];
  lowState[s] = low + v;
  x += bass * low;

  v = (x - highState[s]) * highCoef;
  low = v + highState[s];
  highState[s] = low + v;
  return x + treble * (x - low);
}

export class DelayFx {
  constructor() {
    this.fs = 48000;
    this.variant = V.digitalDelay;
    this.mix = f32(0.35); this.tail = 1;
    this.knobs = new Float32Array([500, 1, 40, 50, 50, 35, 35]); // the last setParameters, for prepare()

    this.memory = new Float32Array(16);
    this.line = [new Line(), new Line()]; this.whole = new Line();
    this.lineSize = 0;

    // times and feedback
    this.timeTarget = 9600; this.glide = 9600; // samples
    this.glideCoef = 0;
    this.tap = [new Tap(), new Tap()];
    this.xfadeLength = 1; this.xfadeStep = 1;
    this.feedback = [new Ramp(), new Ramp()];

    // modulation: chorus LFO, tape wow / flutter / drift
    this.lfoPhase = 0; this.lfoInc = 0;
    this.modAmp = new Ramp();
    this.wowPhase = new Float64Array(3); this.wowInc = new Float64Array(3);
    this.wowAmp = [new Ramp(), new Ramp(), new Ramp()]; this.driftAmp = new Ramp();
    this.noiseState = 0; this.noiseCount = 0; this.noiseInterval = 1;
    this.noiseTarget = 0; this.noise1 = 0; this.noise2 = 0; this.noiseCoef = 0;

    // loop colour
    this.lowPass = [new Biquad(), new Biquad()]; this.bump = [new Bell(), new Bell()];
    this.highPass = [new OnePole(), new OnePole()]; this.preFilter = [new OnePole(), new OnePole()];
    this.shelfLow = new Float32Array(4); this.shelfHigh = new Float32Array(4); // [inside the loop, on the output] x [side]
    this.saturator = new Saturator();
    this.drive = 0; this.driveTarget = 0; this.bbdTime = -1;
    this.bassGain = new Ramp(); this.trebleGain = new Ramp(); this.bassCoef = new Ramp(); this.trebleCoef = new Ramp(); this.outGain = new Ramp();

    // Ping Pong
    this.spreadDirect = new Ramp(); this.spreadCross = new Ramp();

    // Dynamic Dly
    this.duckThreshold = 0.03; this.duckDepth = 0; this.duckEnv = 0; this.duckGain = 1;
    this.duckAttack = 0; this.duckRelease = 0; this.duckClose = 0; this.duckOpen = 0;

    // Lo Res Delay
    this.tone = 1; this.toneTarget = 1; this.toneState = new Float64Array(2); this.quantScale = 8388608;
    this.toneCoef = 1; this.toneOpen = true;

    // Reverse
    this.reversePos = 0; this.reverseLength = 9600; this.reversePrevious = 9600; this.reverseTarget = 9600;
    this.reverseFade = 1; this.reverseFadeMax = 1;

    // Sweep Echo
    this.sweepPhase = 0; this.sweepInc = 0;
    this.sweepDepth = new Ramp(); this.sweepMix = new Ramp();
    this.sweep1 = new Float32Array(2); this.sweep2 = new Float32Array(2);

    // Auto-Volume Echo
    this.swellFast = 0; this.swellSlow = 0; this.swellPos = 0; this.swellGain = 0;
    this.swellRise = 0.001; this.swellDipStep = 0.01; this.swellCloseStep = 0.001;
    this.swellFastRelease = 0; this.swellSlowAttack = 0; this.swellSlowRelease = 0;
    this.swellHold = 0; this.swellHoldSamples = 0;
    this.swellOpen = false; this.swellDipping = false;
    this.directGain = new Ramp(); this.echoGain = new Ramp();

    // Multi-Head
    this.headGain = [new Ramp(), new Ramp(), new Ramp(), new Ramp()];

    // the dry signal's preamp (processDry)
    this.dryKind = 0;
    this.dryEq = [new Biquad(), new Biquad()];
    this.dryHighPass = [new OnePole(), new OnePole()]; this.dryLowPass = [new OnePole(), new OnePole()];
    this.drySaturator = new Saturator();
    this.dryDrive = 0; this.dryGain = 1;

    this.ramps = [this.feedback[0], this.feedback[1], this.modAmp, this.bassGain, this.trebleGain, this.bassCoef, this.trebleCoef,
                  this.spreadDirect, this.spreadCross, this.wowAmp[0], this.wowAmp[1], this.wowAmp[2], this.driftAmp, this.sweepDepth,
                  this.sweepMix, this.directGain, this.echoGain, this.outGain,
                  this.headGain[0], this.headGain[1], this.headGain[2], this.headGain[3]];
  }

  prepare(sampleRate, maxBlock) {
    const fs = this.fs = sampleRate;

    // 2 s of delay plus room for wow / chorus; Reverse reads up to twice its chunk length back,
    // so it uses both halves as one line
    const lineSize = this.lineSize = Math.trunc(2.1 * fs) + 64;
    this.memory = new Float32Array(lineSize * 2);
    this.line[0].data = this.memory.subarray(0, lineSize);  this.line[0].size = lineSize;
    this.line[1].data = this.memory.subarray(lineSize);     this.line[1].size = lineSize;
    this.whole.data = this.memory;                          this.whole.size = lineSize * 2;

    this.xfadeLength = Math.max(1, Math.trunc(0.04 * fs));
    this.xfadeStep = f32(1 / this.xfadeLength);
    this.reverseFadeMax = Math.max(1, Math.trunc(0.02 * fs));

    for (const r of this.ramps) r.prepare(fs, 0.04);

    this.glideCoef = this.stepFor(0.2);
    this.duckAttack = this.stepFor(0.002);  this.duckRelease = this.stepFor(0.12);
    this.duckClose = this.stepFor(0.012);   this.duckOpen = this.stepFor(0.3);
    this.swellFastRelease = f32(Math.exp(-1 / (0.03 * fs)));
    this.swellSlowAttack = this.stepFor(0.03);  this.swellSlowRelease = this.stepFor(0.15);
    this.swellDipStep = 1 / (0.004 * fs);
    this.swellCloseStep = 1 / (0.15 * fs);
    this.swellHoldSamples = Math.trunc(0.08 * fs);
    this.noiseInterval = Math.max(1, Math.trunc(fs / 40));
    this.noiseCoef = f32(1 - Math.exp(-TWO_PI * 1.2 / fs));

    this.updateVoicing();
    this.setParameters(this.knobs); // everything measured in samples depends on the rate
    this.reset();
  }

  reset() {
    this.memory.fill(0);
    this.line[0].writePos = this.line[1].writePos = this.whole.writePos = 0;

    for (let c = 0; c < 2; ++c) {
      this.lowPass[c].reset();  this.highPass[c].reset();  this.bump[c].reset();  this.preFilter[c].reset();
      this.dryEq[c].reset();    this.dryHighPass[c].reset(); this.dryLowPass[c].reset();
      this.sweep1[c] = this.sweep2[c] = 0;
      this.toneState[c] = 0;
      const t = this.tap[c];
      t.current = t.next = t.target;
      t.count = 0;
    }
    this.shelfLow.fill(0); this.shelfHigh.fill(0);

    this.glide = this.timeTarget;
    this.lfoPhase = this.sweepPhase = 0;
    this.wowPhase.fill(0);
    this.noiseState = 0x2545f491;
    this.noiseCount = 0;
    this.noiseTarget = this.noise1 = this.noise2 = 0;

    this.duckEnv = 0;
    this.duckGain = 1;
    this.swellFast = this.swellSlow = this.swellPos = this.swellGain = 0;
    this.swellOpen = this.swellDipping = false;
    this.swellHold = 0;

    this.reversePos = 0;
    this.reverseLength = this.reversePrevious = this.reverseTarget;
    this.reverseFade = Math.min(Math.trunc(this.reverseLength / 4), this.reverseFadeMax);

    this.updateBlock(0, true);

    for (const r of this.ramps) r.snap();
  }

  setModel(variant) {
    this.variant = clamp(variant | 0, 0, V.numVariants - 1);
    this.updateVoicing();
  }

  setParameters(k) {
    const fs = this.fs, variant = this.variant, q = this.knobs;
    if (k !== q)
      for (let i = 0; i < NUM_KNOBS_READ; ++i) q[i] = k[i] === undefined ? 0 : k[i]; // (rounds to float, as the C++ receives them)

    const twoTimes = variant === V.stereoDelay;
    const seconds = toSeconds(q[0]);
    const fb = toUnit(q[2]);
    const a = twoTimes ? 0 : q[3], b = twoTimes ? 0 : q[4];
    const mixValue = toUnit(q[twoTimes ? 6 : 5]);
    const regenerates = variant >= V.tubeEcho && variant !== V.autoVolume;
    const loopGain = regenerates && fb > REGEN_KNEE ? REGEN_KNEE + (fb - REGEN_KNEE) * (REGEN_MAX - REGEN_KNEE) / (1 - REGEN_KNEE) : fb;
    let first = seconds, spacing = seconds, tailFeedback = Math.min(1, loopGain); // for getTailSeconds()
    let fbRight = fb, wowDepth = 0, loopScale = 1;
    const tap = this.tap;

    this.mix = f32(mixValue);
    this.timeTarget = seconds * fs;
    tap[0].target = tap[1].target = Math.floor(seconds * fs + 0.5);

    switch (variant) {
      case V.pingPong: {
        // Offset: the right delay as a percentage of the left one. Spread: mono .. hard left / right.
        const right = Math.max(0.001, seconds * toUnit(q[3]));
        const angle = (1 - toUnit(q[4])) * (PI / 4);
        tap[1].target = Math.floor(right * fs + 0.5);
        this.spreadDirect.set(Math.cos(angle));
        this.spreadCross.set(Math.sin(angle));
        first = spacing = seconds + right;
        break;
      }

      case V.dynamicDly: {
        // Thresh: -60 .. 0 dB. Ducking: how far the echoes are turned down (100 % = muted).
        const keep = 1 - toUnit(q[4]);
        this.duckThreshold = Math.pow(10, (-60 + 0.6 * clamp(a, 0, 100)) / 20);
        this.duckDepth = 1 - keep * keep;
        break;
      }

      case V.stereoDelay: {
        const secondsRight = toSeconds(q[3]);
        fbRight = toUnit(q[5]);
        tap[1].target = Math.floor(secondsRight * fs + 0.5);
        if (secondsRight * (1 + repeats(fbRight)) > seconds * (1 + repeats(fb))) {
          first = spacing = secondsRight;
          tailFeedback = fbRight;
        }
        break;
      }

      case V.digitalDelay:
      case V.tapeEcho:
      case V.tapeEchoDry:
      case V.analogEcho: {
        // Shelves, flat at 50 %. A cut sits inside the loop (every repeat loses a little more), a boost behind it
        // (every repeat gets it once), so the loop gain never exceeds Fdbk.
        const bassDb = (clamp(a, 0, 100) - 50) / 50 * EQ_RANGE_DB;
        const trebleDb = (clamp(b, 0, 100) - 50) / 50 * EQ_RANGE_DB;
        const bassRatio = Math.pow(10, bassDb / 20), trebleRatio = Math.pow(10, trebleDb / 20);
        this.bassGain.set(bassRatio);
        this.trebleGain.set(trebleRatio);

        // out = in + (gain - 1) * low-passed (or high-passed) in, the corner placed so that boost and cut are mirror images
        this.bassCoef.set(this.onePoleCoef(EQ_BASS_HZ / Math.sqrt(bassRatio)));
        this.trebleCoef.set(this.onePoleCoef(EQ_TREBLE_HZ * Math.sqrt(trebleRatio)));
        wowDepth = variant === V.tapeEcho || variant === V.tapeEchoDry ? 0.18 : 0;
        break;
      }

      case V.digMod:
      case V.analogMod: {
        // Depth is the pitch deviation (up to +-1.2 %); the sweep is capped at 5 ms so slow speeds stay a chorus
        const rate = clamp(a, 0.05, 10);
        this.lfoInc = TWO_PI * rate / fs;
        this.modAmp.set(Math.min(Math.min(0.005, 0.25 * seconds), toUnit(q[4]) * 0.012 / (TWO_PI * rate)) * fs);
        break;
      }

      case V.reverse: {
        const rate = clamp(a, 0.05, 10);
        this.lfoInc = TWO_PI * rate / fs;
        this.modAmp.set(Math.min(0.003, toUnit(q[4]) * 0.010 / (TWO_PI * rate)) * fs);
        this.reverseTarget = tap[0].target;
        first = spacing = 2 * seconds; // a chunk is heard up to twice its length after it went in
        break;
      }

      case V.loRes:
        this.toneTarget = toUnit(q[3]);
        this.quantScale = 1 << (5 + clamp(Math.floor(b + 0.5), 0, 18)); // 2^(bits - 1)
        break;

      case V.tubeEcho:
      case V.tubeEchoDry:
      case V.echoPlatter:
      case V.echoPlatterDry:
        wowDepth = toUnit(q[3]);
        this.driveTarget = toUnit(q[4]);
        break;

      case V.sweepEcho:
      case V.sweepEchoDry: {
        const depth = toUnit(q[4]);
        this.sweepInc = TWO_PI * clamp(a, 0.05, 10) / fs;
        this.sweepDepth.set(depth * SWEEP_OCTAVES);
        this.sweepMix.set(Math.min(1, depth * 5)); // no sweep, no filter: plain EP-1 echoes
        wowDepth = 0.15;
        this.driveTarget = 0.4; // the dry path's preamp (processDry)
        break;
      }

      case V.autoVolume:
        // The swell has to replace the dry signal, so this model does its own mixing: it returns the swelled
        // signal and the echoes already balanced by Mix, and getMix() tells the slot "all wet".
        wowDepth = toUnit(q[3]);
        this.swellRise = f32(1 / (0.02 * Math.pow(100, toUnit(q[4])) * fs)); // 20 ms .. 2 s
        this.directGain.set(Math.min(1, 2 - 2 * mixValue));
        this.echoGain.set(Math.min(1, 2 * mixValue));
        this.mix = 1;
        break;

      case V.multiHead: {
        const heads = clamp(Math.floor(a + 0.5), 0, 3) | (clamp(Math.floor(b + 0.5), 0, 3) << 2);
        let count = 0, last = 3;
        for (let h = 0; h < 4; ++h)
          if ((heads >> h) & 1) { ++count; last = h; }

        // The mix of the selected heads (each at 1 / sqrt (heads)) is what gets recorded again, as on the real
        // unit: more heads, denser repeats. It is scaled so that the loop gain is Fdbk when all heads add up.
        const gain = count > 0 ? 1 / Math.sqrt(count) : 0;
        for (let h = 0; h < 4; ++h)
          this.headGain[h].set(((heads >> h) & 1) ? gain : 0);

        loopScale = count > 0 ? gain : 1;
        wowDepth = 0.2;
        first = spacing = seconds * HEAD_RATIO[last];
        break;
      }

      default:
        break;
    }

    this.feedback[0].set(loopGain * loopScale);
    this.feedback[1].set(fbRight);
    this.setWow(wowDepth, seconds);
    this.tail = f32(clamp(first + spacing * repeats(tailFeedback) + 0.1, first + 0.1, 20));
  }

  /** Wet signal only, in place. */
  process(left, right, n) {
    this.updateBlock(n, false);

    switch (this.variant) {
      case V.pingPong:      this.processPingPong(left, right, n); break;
      case V.dynamicDly:
      case V.stereoDelay:
      case V.digitalDelay:
      case V.digMod:        this.processDigital(left, right, n); break;
      case V.reverse:       this.processReverse(left, right, n); break;
      case V.loRes:         this.processLoRes(left, right, n); break;
      case V.multiHead:     this.processMultiHead(left, right, n); break;
      default:              this.processAnalog(left, right, n); break;
    }
  }

  /** The slot's dry signal, in place: Tube Echo, Tape Echo, Sweep Echo and Echo Platter pass it through the
      original unit's preamp; every other model (and the "Dry" variants) leave it untouched. */
  processDry(left, right, n) {
    if (this.dryKind === 0) return;

    const moved = approach(this.dryDrive, this.driveTarget, 8 * n / this.fs);
    if (moved !== this.dryDrive) {
      this.dryDrive = moved;
      this.updateDrySaturator();
    }

    const sat = this.drySaturator, gain = this.dryGain;
    for (let c = 0; c < 2; ++c) {
      const io = c === 0 ? left : right, eq = this.dryEq[c], hp = this.dryHighPass[c], lp = this.dryLowPass[c];
      for (let i = 0; i < n; ++i) {
        let x = eq.process(io[i]);
        x = hp.highPass(sat.process(x));
        io[i] = lp.lowPass(x) * gain;
      }
    }
  }

  /** 0..1 from the Mix knob. (Auto-Volume Echo reports 1 and balances swell and echoes itself.) */
  getMix() { return this.mix; }

  /** About how long the repeats need to fall by 60 dB at the current Time and Fdbk. */
  getTailSeconds() { return this.tail; }

  // ---- private ----
  stepFor(seconds) { return f32(1 - Math.exp(-1 / (seconds * this.fs))); }

  /** Coefficient of a one-pole (TPT) low-pass at `hz`. */
  onePoleCoef(hz) {
    const g = Math.tan(PI * clamp(hz, 1, 0.49 * this.fs) / this.fs);
    return g / (1 + g);
  }

  voicing() {
    switch (this.variant) {
      case V.tapeEcho: case V.tapeEchoDry:        return VOICING_EP3;
      case V.sweepEcho: case V.sweepEchoDry:      return VOICING_SWEEP;
      case V.echoPlatter: case V.echoPlatterDry:  return VOICING_PLATTER;
      case V.analogMod:                           return VOICING_DMM;
      case V.analogEcho:                          return VOICING_DM2;
      case V.autoVolume:                          return VOICING_SWELL;
      case V.multiHead:                           return VOICING_RE101;
      default:                                    return VOICING_EP1;
    }
  }

  /** The fixed filters of the selected model. */
  updateVoicing() {
    const v = this.voicing(), fs = this.fs, variant = this.variant;
    const dryThrough = variant === V.tubeEcho || variant === V.tapeEcho || variant === V.sweepEcho || variant === V.echoPlatter;
    const dryKind = this.dryKind = dryThrough ? v.dry : 0;

    for (let c = 0; c < 2; ++c) {
      this.lowPass[c].setLowPass(fs, v.lowPassHz, v.lowPassQ);
      this.highPass[c].setCutoff(fs, v.highPassHz);
      this.bump[c].set(fs, v.bumpHz > 0 ? v.bumpHz : 100, v.bumpQ, v.bumpDb);
      this.preFilter[c].setCutoff(fs, v.preHz > 0 ? v.preHz : 5000);

      // the dry signal's way through the unit
      if (dryKind === 2)       this.dryEq[c].setHighShelf(fs, 2200, 2.5);  // EP-3: the FET preamp's treble lift
      else if (dryKind === 3)  this.dryEq[c].setPeak(fs, 1200, 0.7, 2.0);  // Echorec: mid-forward valve mixer
      else                     this.dryEq[c].setPeak(fs, 1000, 0.7, 0.0);  // EP-1: flat before the tube
      this.dryHighPass[c].setCutoff(fs, dryKind === 3 ? 90 : (dryKind === 2 ? 45 : 30));
      this.dryLowPass[c].setCutoff(fs, dryKind === 3 ? 6500 : (dryKind === 2 ? 16000 : 9000));
    }

    this.dryGain = dryKind === 2 ? f32(0.93) : 1;
    this.bbdTime = -1;
  }

  updateDrySaturator() {
    if (this.dryKind === 2)       this.drySaturator.set(0.45, 0.05);
    else if (this.dryKind === 3)  this.drySaturator.set(0.5 + 1.0 * this.dryDrive, 0.10);
    else                          this.drySaturator.set(0.5 + 1.2 * this.dryDrive, 0.12);
  }

  /** Transport speed errors -> delay modulation. A speed error a sin (w t) moves an echo of length T by
      (2 a / w) sin (w T / 2): slow wow grows with the delay time, fast flutter does not. */
  setWow(depth, seconds) {
    const v = this.voicing(), fs = this.fs;
    for (let j = 0; j < 3; ++j) {
      const w = TWO_PI * v.wowHz[j];
      this.wowAmp[j].set(depth * (2 * v.wowDeviation[j] / w) * Math.abs(Math.sin(0.5 * w * seconds)) * fs);
      this.wowInc[j] = w / fs;
    }
    this.driftAmp.set(depth * v.drift * 8 * Math.min(seconds, 0.5) * fs);
  }

  /** Once per block: knob values that are slewed at block rate and the filters that follow them.
      `force` (from reset) jumps to the targets and recalculates everything. */
  updateBlock(numSamples, force) {
    const fs = this.fs, n = numSamples / fs, v = this.voicing();

    // record-path saturation (Drive)
    const drive = approach(this.drive, this.driveTarget, force ? 1.0e9 : 8 * n);
    if (drive !== this.drive || force) {
      this.drive = drive;
      const hasDriveKnob = v.driveMax > v.driveMin;
      const g = v.driveMin + (v.driveMax - v.driveMin) * (hasDriveKnob ? drive : 0);
      this.saturator.set(g, v.bias);
      this.outGain.set(hasDriveKnob ? Math.pow(g, 0.25) : 1); // a hot tape returns a little louder
    }

    if (force) {
      this.dryDrive = this.driveTarget;
      this.updateDrySaturator();
    }

    // Lo Res Delay's Tone: 500 Hz .. 20 kHz, wide open at 100 %
    const tone = approach(this.tone, this.toneTarget, force ? 1.0e9 : 6 * n);
    if (tone !== this.tone || force) {
      this.tone = tone;
      const g = Math.tan(PI * Math.min(500 * Math.pow(40, tone), 0.49 * fs) / fs);
      this.toneCoef = f32(g / (1 + g));
      this.toneOpen = tone >= 0.995;
    }

    // BBD: a longer delay means a slower clock and a lower reconstruction filter
    if (this.variant === V.analogMod || this.variant === V.analogEcho) {
      if (force) this.glide = this.timeTarget;

      if (this.bbdTime < 0 || Math.abs(this.glide - this.bbdTime) > 0.01 * this.bbdTime) {
        this.bbdTime = this.glide;
        const hz = v.lowPassHz * Math.min(1, Math.pow(0.3 * fs / this.glide, 0.4));
        this.lowPass[0].setLowPass(fs, hz, v.lowPassQ);
        this.lowPass[1].setLowPass(fs, hz, v.lowPassQ);
      }
    }
  }

  /** Whole-sample read with the crossfade to a new time. */
  readTap(l, t) {
    if (t.count === 0) {
      if (t.target === t.current) return l.readInt(t.current);
      t.next = t.target;
      t.count = this.xfadeLength;
    }

    const from = l.readInt(t.current), to = l.readInt(t.next);
    const g = f32((this.xfadeLength - t.count) * this.xfadeStep);
    if (--t.count === 0) t.current = t.next;
    return from + (to - from) * g;
  }

  /** The same with a modulated (fractional) read position. */
  readTapAt(l, t, offset) {
    if (t.count === 0) {
      if (t.target === t.current) return l.read(t.current + offset);
      t.next = t.target;
      t.count = this.xfadeLength;
    }

    const from = l.read(t.current + offset), to = l.read(t.next + offset);
    const g = f32((this.xfadeLength - t.count) * this.xfadeStep);
    if (--t.count === 0) t.current = t.next;
    return from + (to - from) * g;
  }

  advanceLfo() {
    this.lfoPhase += this.lfoInc;
    if (this.lfoPhase >= TWO_PI) this.lfoPhase -= TWO_PI;
  }

  /** Dynamic Dly: gain for the echoes, from the level of what is being played right now. */
  nextDuckGain(l, r) {
    const level = Math.max(Math.abs(l), Math.abs(r));
    this.duckEnv += (level - this.duckEnv) * (level > this.duckEnv ? this.duckAttack : this.duckRelease);

    const amount = clamp((this.duckEnv - 0.5 * this.duckThreshold) / this.duckThreshold, 0, 1);
    const target = 1 - amount * this.duckDepth;
    this.duckGain += (target - this.duckGain) * (target < this.duckGain ? this.duckClose : this.duckOpen); // down fast, bloom slowly
    return this.duckGain;
  }

  /** Auto-Volume Echo: gain that closes at every new note and fades it in (exactly the C++ arithmetic). */
  nextSwellGain(l, r) {
    const level = 0.5 * (Math.abs(l) + Math.abs(r));
    this.swellFast = level > this.swellFast ? level : this.swellFast * this.swellFastRelease;
    this.swellSlow += (this.swellFast - this.swellSlow) * (this.swellFast > this.swellSlow ? this.swellSlowAttack : this.swellSlowRelease);
    if (this.swellHold > 0) --this.swellHold;

    if (this.swellFast < SWELL_OFF) {
      this.swellOpen = false;
    } else if (this.swellFast > SWELL_ON && this.swellHold === 0 && (!this.swellOpen || this.swellFast > SWELL_ONSET_RATIO * this.swellSlow)) {
      this.swellOpen = true;
      this.swellDipping = true; // a new note: down in 4 ms, then up over the Swell time
      this.swellHold = this.swellHoldSamples;
    }

    if (this.swellDipping) {
      this.swellGain -= this.swellDipStep;
      if (this.swellGain <= 0) {
        this.swellGain = this.swellPos = 0;
        this.swellDipping = false;
      }
    } else {
      this.swellPos = this.swellOpen ? Math.min(1, this.swellPos + this.swellRise) : Math.max(0, this.swellPos - this.swellCloseStep);
      this.swellGain = this.swellPos * this.swellPos;
    }
    return this.swellGain;
  }

  /** Wow, flutter and a slow random drift of the transport, as a delay offset in samples. */
  nextWow() {
    const wowAmp = this.wowAmp, wowPhase = this.wowPhase, wowInc = this.wowInc;
    let offset = 0;
    for (let j = 0; j < 3; ++j) {
      offset += wowAmp[j].next() * Math.sin(wowPhase[j]);
      wowPhase[j] += wowInc[j];
      if (wowPhase[j] >= TWO_PI) wowPhase[j] -= TWO_PI;
    }

    if (--this.noiseCount <= 0) {
      this.noiseCount = this.noiseInterval;
      this.noiseState = (Math.imul(this.noiseState, 1664525) + 1013904223) >>> 0;
      this.noiseTarget = (this.noiseState >>> 8) * (2 / 16777216) - 1;
    }
    this.noise1 += this.noiseCoef * (this.noiseTarget - this.noise1);
    this.noise2 += this.noiseCoef * (this.noise1 - this.noise2);
    return offset + this.driftAmp.next() * this.noise2;
  }

  wowIsOn() {
    return !(this.wowAmp[0].isZero() && this.wowAmp[1].isZero() && this.wowAmp[2].isZero() && this.driftAmp.isZero());
  }

  /** Tape-style time change: the delay glides to its target, which bends the pitch of what is in the line. */
  nextGlide() {
    this.glide += clamp((this.timeTarget - this.glide) * this.glideCoef, -1, 0.66);
    return this.glide;
  }

  // Dynamic Dly, Stereo Delay, Digital Delay, Dig Dly W/Mod: one clean line per side
  processDigital(left, right, n) {
    const variant = this.variant, line = this.line, tap = this.tap, feedback = this.feedback;
    const shelfLow = this.shelfLow, shelfHigh = this.shelfHigh;
    const useEq = variant === V.digitalDelay, useMod = variant === V.digMod, ducking = variant === V.dynamicDly;

    for (let i = 0; i < n; ++i) {
      const outputGain = ducking ? f32(this.nextDuckGain(left[i], right[i])) : 1;
      let bass = 1, treble = 1, lowCoef = 0, highCoef = 0;
      let offset0 = 0, offset1 = 0;

      if (useEq) {
        bass = f32(this.bassGain.next());      lowCoef = f32(this.bassCoef.next());
        treble = f32(this.trebleGain.next());  highCoef = f32(this.trebleCoef.next());
      }

      if (useMod) {
        const amp = this.modAmp.next();
        offset0 = amp * Math.sin(this.lfoPhase); // the right side a quarter cycle ahead: the chorus spreads out
        offset1 = amp * Math.cos(this.lfoPhase);
        this.advanceLfo();
      }

      for (let c = 0; c < 2; ++c) {
        const io = c === 0 ? left : right;
        let wet = useMod ? this.readTapAt(line[c], tap[c], c === 0 ? offset0 : offset1) : this.readTap(line[c], tap[c]);
        if (useEq) // cuts, inside the loop
          wet = shelfPair(wet, shelfLow, shelfHigh, c, Math.min(bass, 1) - 1, Math.min(treble, 1) - 1, lowCoef, highCoef);

        line[c].push(softLimit(io[i] + f32(feedback[c].next()) * wet));

        if (useEq) // boosts, on the way out
          wet = shelfPair(wet, shelfLow, shelfHigh, 2 + c, Math.max(bass, 1) - 1, Math.max(treble, 1) - 1, lowCoef, highCoef);
        io[i] = wet * outputGain;
      }
    }
  }

  // Ping Pong: the mono sum goes into the left line, its output into the right line (first "ping", then
  // "pong" at the same level), and the right line's output back into the left one, turned down by Fdbk.
  processPingPong(left, right, n) {
    const line0 = this.line[0], line1 = this.line[1], tap0 = this.tap[0], tap1 = this.tap[1];
    for (let i = 0; i < n; ++i) {
      const input = 0.5 * (left[i] + right[i]);
      const ping = this.readTap(line0, tap0), pong = this.readTap(line1, tap1);
      const fb = f32(this.feedback[0].next());
      const direct = f32(this.spreadDirect.next()), cross = f32(this.spreadCross.next());

      line0.push(softLimit(input + fb * pong));
      line1.push(ping);
      left[i] = direct * ping + cross * pong;
      right[i] = direct * pong + cross * ping;
    }
  }

  // Lo Res Delay: what goes into the line is rounded to the chosen word length (no dither, like an early
  // digital delay), again on every trip round the loop. Same double arithmetic as the C++, so it rounds the same way.
  processLoRes(left, right, n) {
    const line = this.line, tap = this.tap, feedback = this.feedback, toneState = this.toneState;
    const coef = this.toneCoef, xfadeLength = this.xfadeLength, toneOpen = this.toneOpen, quantScale = this.quantScale;

    for (let i = 0; i < n; ++i)
      for (let c = 0; c < 2; ++c) {
        const io = c === 0 ? left : right, t = tap[c], l = line[c];
        let wet;

        if (t.count === 0 && t.target === t.current) {
          wet = l.readInt(t.current);
        } else {
          if (t.count === 0) {
            t.next = t.target;
            t.count = xfadeLength;
          }

          const from = l.readInt(t.current), to = l.readInt(t.next);
          wet = from + (to - from) * ((xfadeLength - t.count) / xfadeLength);
          if (--t.count === 0) t.current = t.next;
        }

        // Tone: one-pole low-pass on the repeats (inside the loop: every repeat is a little darker)
        const v = (wet - toneState[c]) * coef, low = v + toneState[c];
        toneState[c] = low + v;
        if (!toneOpen) wet = low;

        const input = softLimit(io[i] + feedback[c].next() * wet);
        l.push(Math.floor(input * quantScale + 0.5) / quantScale);
        io[i] = wet;
      }
  }

  // Reverse: the read position runs backwards from "now" for one chunk (Time), then jumps back to "now".
  // At each jump the old read head keeps running underneath the new one for an equal-power crossfade.
  processReverse(left, right, n) {
    const whole = this.whole;
    for (let i = 0; i < n; ++i) {
      const input = 0.5 * (left[i] + right[i]);
      const offset = this.modAmp.next() * (1 + Math.sin(this.lfoPhase)); // chorus: the read head wanders a little
      this.advanceLfo();

      let wet = whole.read(2 + 2 * this.reversePos + offset);
      if (this.reversePos < this.reverseFade) {
        const x = 0.5 * PI * (this.reversePos + 0.5) / this.reverseFade;
        const old = whole.read(2 + 2 * (this.reversePrevious + this.reversePos) + offset);
        wet = f32(Math.cos(x)) * old + f32(Math.sin(x)) * wet;
      }
      wet = f32(wet);

      whole.push(softLimit(input + f32(this.feedback[0].next()) * wet));
      left[i] = right[i] = wet;

      if (++this.reversePos >= this.reverseLength) {
        this.reversePos = 0;
        this.reversePrevious = this.reverseLength;
        this.reverseLength = this.reverseTarget; // a new Time is taken up here
        this.reverseFade = Math.min(Math.trunc(this.reverseLength / 4), this.reverseFadeMax);
      }
    }
  }

  // Multi-Head: one tape, one record head, four playback heads. The selected heads are mixed, and that mix
  // is also what is fed back to the record head.
  processMultiHead(left, right, n) {
    const line0 = this.line[0], headGain = this.headGain, lowPass = this.lowPass[0], highPass = this.highPass[0], bump = this.bump[0];
    const wow = this.wowIsOn();

    for (let i = 0; i < n; ++i) {
      const input = 0.5 * (left[i] + right[i]);
      const d = this.nextGlide() + (wow ? this.nextWow() : 0);
      let wet = 0;

      for (let h = 0; h < 4; ++h) {
        const g = headGain[h].next();
        if (g !== 0) wet += f32(g) * line0.read(d * HEAD_RATIO[h]);
      }

      wet = HEAD_MIX_KNEE * softLimit(wet * HEAD_MIX_SCALE);
      wet = bump.process(highPass.highPass(lowPass.process(wet)));
      line0.push(this.saturator.process(input + f32(this.feedback[0].next()) * wet));
      left[i] = right[i] = wet;
    }
  }

  // Tube / Tape / Sweep Echo, Echo Platter, Analog W/Mod, Analog Echo, Auto-Volume Echo: per side
  //   read (gliding, wobbling) -> playback filters -> out
  //   in + Fdbk * out -> saturation -> line
  // so every repeat is filtered and saturated once more than the one before.
  processAnalog(left, right, n) {
    const variant = this.variant, v = this.voicing(), fs = this.fs;
    const line = this.line, lowPass = this.lowPass, highPass = this.highPass, bump = this.bump, preFilter = this.preFilter;
    const shelfLow = this.shelfLow, shelfHigh = this.shelfHigh, sweep1 = this.sweep1, sweep2 = this.sweep2, saturator = this.saturator;
    const wow = this.wowIsOn(), sineMod = variant === V.analogMod, useBump = v.bumpDb !== 0, usePre = v.preHz > 0;
    const useEq = variant === V.tapeEcho || variant === V.tapeEchoDry || variant === V.analogEcho;
    const sweeping = variant === V.sweepEcho || variant === V.sweepEchoDry, swelling = variant === V.autoVolume;
    const damping = f32(SWEEP_DAMPING);

    for (let i = 0; i < n; ++i) {
      let d = this.nextGlide();
      if (wow) d += this.nextWow();

      if (sineMod) {
        d += this.modAmp.next() * Math.sin(this.lfoPhase);
        this.advanceLfo();
      }

      const fb = f32(this.feedback[0].next()), level = f32(this.outGain.next());
      let bass = 1, treble = 1, lowCoef = 0, highCoef = 0;
      let swell = 1, direct = 0, echo = 1;
      let a1 = 0, a2 = 0, a3 = 0, sweepAmount = 0;

      if (useEq) {
        bass = f32(this.bassGain.next());      lowCoef = f32(this.bassCoef.next());
        treble = f32(this.trebleGain.next());  highCoef = f32(this.trebleCoef.next());
      }

      if (swelling) {
        swell = f32(this.nextSwellGain(left[i], right[i]));
        direct = f32(this.directGain.next());
        echo = f32(this.echoGain.next());
      }

      if (sweeping) {
        // state-variable filter, the cutoff swept up and down by a sine
        const octaves = this.sweepDepth.next() * Math.sin(this.sweepPhase);
        this.sweepPhase += this.sweepInc;
        if (this.sweepPhase >= TWO_PI) this.sweepPhase -= TWO_PI;

        const g = Math.tan(PI * Math.min(SWEEP_CENTRE_HZ * Math.exp(LN2 * octaves), 0.45 * fs) / fs);
        const k1 = 1 / (1 + g * (g + SWEEP_DAMPING));
        a1 = f32(k1);
        a2 = f32(g * k1);
        a3 = f32(g * g * k1);
        sweepAmount = f32(this.sweepMix.next());
      }

      for (let c = 0; c < 2; ++c) {
        const io = c === 0 ? left : right;
        const input = f32(io[i] * swell);
        let wet = highPass[c].highPass(lowPass[c].process(line[c].read(d)));
        if (useBump) wet = bump[c].process(wet);

        if (useEq) // cuts, inside the loop
          wet = shelfPair(wet, shelfLow, shelfHigh, c, Math.min(bass, 1) - 1, Math.min(treble, 1) - 1, lowCoef, highCoef);

        let record = saturator.process(input + fb * wet);
        if (usePre) record = preFilter[c].lowPass(record);
        line[c].push(record);

        if (useEq) // boosts, on the way out
          wet = shelfPair(wet, shelfLow, shelfHigh, 2 + c, Math.max(bass, 1) - 1, Math.max(treble, 1) - 1, lowCoef, highCoef);

        if (sweeping) {
          const v3 = wet - sweep2[c];
          const v1 = a1 * sweep1[c] + a2 * v3;
          const v2 = sweep2[c] + a2 * sweep1[c] + a3 * v3;
          sweep1[c] = 2 * v1 - sweep1[c];
          sweep2[c] = 2 * v2 - sweep2[c];
          wet += sweepAmount * (0.5 * v2 + damping * v1 - wet); // some low-pass under the band-pass peak
        }

        io[i] = swelling ? input * direct + wet * echo : wet * level;
      }
    }
  }
}

// ---- the model list (in the order of the variants) ----
const HEADS_12 = ["Off", "1", "2", "1+2"], HEADS_34 = ["Off", "3", "4", "3+4"];
const BIT_NAMES = ["6 bit", "7 bit", "8 bit", "9 bit", "10 bit", "11 bit", "12 bit", "13 bit", "14 bit", "15 bit",
                   "16 bit", "17 bit", "18 bit", "19 bit", "20 bit", "21 bit", "22 bit", "23 bit", "24 bit"];

const timeKnob = (name, def) => millis(name, 20, 2000, def, 500);
const noteKnob = (name, def) => choice(name, DELAY_NOTE_NAMES, def);
const speedKnob = (name, def) => hertz(name, 0.05, 10, def, 1);
const SYNC = { timeKnob: 0, noteKnob: 1, noteBeats: DELAY_NOTE_BEATS, trails: true };

/** Time, Note, Fdbk, the model's two controls, Mix. */
const delayModel = (key, name, variant, basedOn, timeMs, fdbk, first, second, mix) =>
  model(key, name, CATEGORY.delay, ENGINE.delayFx, variant, basedOn,
        [timeKnob("Time", timeMs), noteKnob("Note", 1), percent("Fdbk", fdbk), first, second, percent("Mix", mix)], SYNC);

export const DELAY_MODELS = [
  delayModel("ping_pong", "Ping Pong", V.pingPong, "Line 6 original", 400, 45, percent("Offset", 100), percent("Spread", 100), 35),
  delayModel("dynamic_dly", "Dynamic Dly", V.dynamicDly, "TC Electronic 2290", 450, 40, percent("Thresh", 50), percent("Ducking", 60), 40),
  // two times, each with its own tempo sync (dotted eighth against a quarter by default)
  model("stereo_delay", "Stereo Delay", CATEGORY.delay, ENGINE.delayFx, V.stereoDelay, "Line 6 high-res digital delay",
        [timeKnob("L Time", 375), noteKnob("L Note", 2), percent("L Fdbk", 35), timeKnob("R Time", 500), noteKnob("R Note", 1), percent("R Fdbk", 35), percent("Mix", 35)],
        { ...SYNC, timeKnob2: 3, noteKnob2: 4 }),
  delayModel("digital_delay", "Digital Delay", V.digitalDelay, "Line 6 original", 500, 40, percent("Bass", 50), percent("Treble", 50), 35),
  delayModel("dig_dly_w_mod", "Dig Dly W/Mod", V.digMod, "Line 6 original", 450, 40, speedKnob("ModSpd", 0.8), percent("Depth", 50), 35),
  delayModel("reverse", "Reverse", V.reverse, "Line 6 original", 800, 20, speedKnob("ModSpd", 0.5), percent("Depth", 20), 50),
  delayModel("lo_res_delay", "Lo Res Delay", V.loRes, "Line 6 original", 400, 40, percent("Tone", 60), choice("Res", BIT_NAMES, 4), 35),
  delayModel("tube_echo", "Tube Echo", V.tubeEcho, "'63 Maestro EP-1 Echoplex", 350, 45, percent("Wow/Flt", 35), percent("Drive", 40), 35),
  delayModel("tube_echo_dry", "Tube Echo Dry", V.tubeEchoDry, "'63 Maestro EP-1 Echoplex", 350, 45, percent("Wow/Flt", 35), percent("Drive", 40), 35),
  delayModel("tape_echo", "Tape Echo", V.tapeEcho, "Maestro EP-3 Echoplex", 380, 40, percent("Bass", 50), percent("Treble", 50), 35),
  delayModel("tape_echo_dry", "Tape Echo Dry", V.tapeEchoDry, "Maestro EP-3 Echoplex", 380, 40, percent("Bass", 50), percent("Treble", 50), 35),
  delayModel("sweep_echo", "Sweep Echo", V.sweepEcho, "Line 6 original (EP-1 with a sweeping filter)", 400, 45, speedKnob("Swp Spd", 0.4), percent("Swp Dep", 60), 40),
  delayModel("sweep_echo_dry", "Sweep Echo Dry", V.sweepEchoDry, "Line 6 original (EP-1 with a sweeping filter)", 400, 45, speedKnob("Swp Spd", 0.4), percent("Swp Dep", 60), 40),
  delayModel("echo_platter", "Echo Platter", V.echoPlatter, "Binson EchoRec", 300, 50, percent("Wow/Flt", 30), percent("Drive", 35), 40),
  delayModel("echo_platter_dry", "Echo Platter Dry", V.echoPlatterDry, "Binson EchoRec", 300, 50, percent("Wow/Flt", 30), percent("Drive", 35), 40),
  delayModel("analog_w_mod", "Analog W/Mod", V.analogMod, "Electro-Harmonix Deluxe Memory Man", 400, 40, speedKnob("ModSpd", 0.6), percent("Depth", 35), 35),
  delayModel("analog_echo", "Analog Echo", V.analogEcho, "Boss DM-2", 300, 35, percent("Bass", 50), percent("Treble", 50), 35),
  delayModel("auto_volume_echo", "Auto-Volume Echo", V.autoVolume, "Line 6 original", 450, 35, percent("ModDep", 30), percent("Swell", 50), 40),
  delayModel("multi_head", "Multi-Head", V.multiHead, "Roland RE-101 Space Echo", 480, 35, choice("Heads 1-2", HEADS_12, 2), choice("Heads 3-4", HEADS_34, 2), 35),
];
