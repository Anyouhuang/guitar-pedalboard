// The HD500X's reverb models (Source/DSP/fx/Verb.h), ported line for line. Stereo; returns the wet signal only.
// Knobs:  Decay, PreDelay (ms), Tone, Mix   |   Particle Verb: Dwell, Condition, Gain, Mix
//
// Three structures:
// - a feedback delay network (8 modulated lines, Hadamard matrix, an all-pass in every line, decay per band)
//   behind a pre-delay, early-reflection taps and input diffusers: Plate, Room, Chamber, Hall, Echo (plus a
//   ping-pong echo), Tile, Cave, Ducking (plus a ducker), Octo (plus a feedback path through octave-up shifters)
// - springs: recirculating delays with chains of first-order all-passes (dispersion), run at about 9.6 kHz
// - Particle Verb: two cross-coupled all-pass chains with a pitch shifter in each feedback path
import { clamp, f32, PI, Biquad, Smoothed } from "./core.js";
import { CATEGORY, ENGINE, model, percent, millis, choice } from "./modeltypes.js";

const TICK = 32;        // control rate: knob smoothing and LFOs move every 32 samples (any host block size)
const MAX_EARLY = 12;   // early-reflection taps per side
const MAX_STAGES = 160; // all-pass stages per half spring

const FDN = 0, SPRINGS = 1, PARTICLE = 2;             // type
const PLAIN = 0, ECHOES = 1, DUCKER = 2, OCTAVE = 3;  // extra

// the delay lines, all inside one buffer
const D_PRE_L = 0, D_PRE_R = 1, D_EARLY_L = 2, D_EARLY_R = 3, D_DIFF = 4, D_LINE = 12, D_AP = 20, D_ECHO_L = 28, D_ECHO_R = 29, D_SHIFT_A = 30, NUM_DELAYS = 32;

// everything in the signal path that follows Decay and Tone, ramped from tick to tick.
// [0..31] per line: gain of the highs, mids - highs, lows - mids, loss of the all-pass
// (springs and Particle Verb keep their loop gains in [0..3] and the losses of their all-passes in [24..31])
const C_DAMP = 32, C_SHIFT = 33, C_ECHO = 34, C_TONE = 35, NUM_COEFS = 36;

const AP_SPREAD = Float32Array.from([1.0, 1.19, 1.41, 1.69, 1.93, 2.23, 2.53, 2.83]);
const RATE_SPREAD = Float32Array.from([1.0, 1.31, 0.77, 1.63]);
const SHIFT_MS = Float32Array.from([61.3, 70.9]); // pitch shifter windows
const PARTICLE_AP_MS = Float32Array.from([23.7, 37.1, 53.9, 71.3, 26.3, 34.7, 57.7, 67.9]);
const PARTICLE_DELAY_MS = Float32Array.from([97.1, 113.3]);
const SPRING_IN = Float32Array.from([1.0, -0.9, 0.8]);
const SPRING_OUT_L3 = Float32Array.from([1.0, 0.5, -0.5]), SPRING_OUT_R3 = Float32Array.from([0.5, -0.5, 1.0]);
const SPRING_OUT_L2 = Float32Array.from([1.0, 0.6, 0.0]), SPRING_OUT_R2 = Float32Array.from([-0.6, 1.0, 0.0]);

// The C++ keeps the numbers of a program as floats: round them the same way.
const rounded = (v) => (Array.isArray(v) ? v.map(rounded) : f32(v));
const program = (p) => { const out = {}; for (const name in p) out[name] = rounded(p[name]); return out; };

// What makes one model: the structure (type / extra) and the sizes of its space. The same table as in Verb.h
// (see there for what the fields mean); unused arrays are left short.
const PROGRAMS = [
  // Plate - a studio plate (EMT 140 style): no separate reflections, the full density from the first
  // milliseconds (four diffusers, short output taps), bright, the lows ring longer than the highs.
  program({ type: FDN, extra: PLAIN, rtMin: 0.4, rtMax: 6, level: 0.192, toneHz: 12000, lowCutHz: 120, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [23.9, 28.1, 31.7, 37.3, 41.9, 47.1, 53.3, 59.9], tapMs: [0.3, 0.5, 2.9, 3.7, 6.1, 7.3, 9.7, 11.3], apMs: 3.7, apGain: 0.55, numDiff: 4,
    diffMs: [1.9, 3.7, 5.9, 8.3, 2.3, 3.1, 6.7, 7.9], diffGain: 0.7, dampHz: 8000, hfRatio: 0.6, bassHz: 350, bassRatio: 1.3, modMs: 0.25,
    modHz: 0.9, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []], numSprings: 0, stages: 0, springMs: [0],
    springCoef: 0, drive: 0 }),

  // Room - a small studio live room: mostly early reflections (12 taps per side in the first 60 ms),
  // a short, quieter tail behind them.
  program({ type: FDN, extra: PLAIN, rtMin: 0.15, rtMax: 1.6, level: 0.184, toneHz: 7000, lowCutHz: 90, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [17.3, 20.9, 23.9, 27.7, 31.9, 35.3, 39.7, 44.9], tapMs: [0.3, 0.5, 3.1, 4.3, 7.7, 9.1, 13.3, 15.1], apMs: 2.3, apGain: 0.5, numDiff: 3,
    diffMs: [2.1, 4.9, 7.1, 0, 2.7, 4.3, 7.7, 0], diffGain: 0.6, dampHz: 5000, hfRatio: 0.5, bassHz: 200, bassRatio: 0.85, modMs: 0.2, modHz: 0.7,
    tankLevel: 0.8, earlyLevel: 1, numEarly: 12,
    earlyMs: [[2.9, 6.7, 9.5, 13.3, 17.9, 21.1, 26.3, 30.7, 36.1, 41.9, 49.3, 57.1], [3.7, 5.9, 10.9, 14.7, 16.3, 22.9, 25.1, 32.3, 37.7, 43.3, 47.9, 59.3]],
    earlyGain: [[0.84, -0.71, 0.66, 0.58, -0.52, 0.47, -0.41, 0.38, 0.33, -0.29, 0.25, -0.21], [0.8, 0.74, -0.63, 0.57, 0.54, -0.45, 0.42, -0.36, 0.34, 0.28, -0.26, 0.2]],
    numSprings: 0, stages: 0, springMs: [0], springCoef: 0, drive: 0 }),

  // Chamber - an elongated echo chamber (hallway, stairwell): a few short lines for the cross-section and
  // long ones for the length, reflections that come back in pairs along the long axis.
  program({ type: FDN, extra: PLAIN, rtMin: 0.4, rtMax: 5, level: 0.194, toneHz: 7500, lowCutHz: 90, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [14.3, 17.9, 21.1, 26.9, 33.7, 47.9, 63.1, 83.3], tapMs: [0.3, 0.5, 5.3, 6.7, 11.3, 14.9, 23.3, 31.1], apMs: 2.9, apGain: 0.55,
    numDiff: 3, diffMs: [3.1, 5.3, 9.1, 0, 3.7, 4.7, 9.7, 0], diffGain: 0.65, dampHz: 5500, hfRatio: 0.5, bassHz: 250, bassRatio: 1.1, modMs: 0.3,
    modHz: 0.6, tankLevel: 1, earlyLevel: 0.6, numEarly: 8,
    earlyMs: [[4.1, 11.3, 21.7, 26.3, 43.1, 48.7, 64.9, 86.3], [5.3, 9.7, 22.9, 27.7, 41.9, 50.3, 66.7, 84.1]],
    earlyGain: [[0.7, -0.62, 0.66, 0.5, -0.52, 0.4, 0.38, -0.28], [0.68, 0.6, -0.64, 0.52, 0.5, -0.41, 0.36, 0.29]], numSprings: 0, stages: 0,
    springMs: [0], springCoef: 0, drive: 0 }),

  // Hall - a concert hall: long lines, sparse soft reflections, output taps deep inside the lines so the
  // sound builds up slowly; long smooth tail, warm (long bass decay, damped highs).
  program({ type: FDN, extra: PLAIN, rtMin: 0.8, rtMax: 10, level: 0.265, toneHz: 6500, lowCutHz: 80, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [41.3, 47.9, 56.3, 63.7, 72.1, 81.7, 90.3, 101.9], tapMs: [0.4, 0.6, 17.1, 21.3, 33.7, 38.9, 55.3, 61.1], apMs: 5.3, apGain: 0.6,
    numDiff: 4, diffMs: [4.7, 8.9, 13.3, 19.1, 5.3, 8.3, 14.1, 18.1], diffGain: 0.7, dampHz: 3800, hfRatio: 0.35, bassHz: 250, bassRatio: 1.35,
    modMs: 0.5, modHz: 0.45, tankLevel: 1, earlyLevel: 0.35, numEarly: 8,
    earlyMs: [[8.3, 19.1, 27.7, 38.9, 51.1, 63.7, 79.3, 97.1], [10.1, 17.3, 29.9, 41.3, 47.9, 66.1, 82.7, 94.3]],
    earlyGain: [[0.5, -0.45, 0.48, 0.4, -0.36, 0.33, -0.28, 0.24], [0.48, 0.46, -0.44, 0.41, 0.37, -0.32, 0.27, -0.25]], numSprings: 0, stages: 0,
    springMs: [0], springCoef: 0, drive: 0 }),

  // Echo - Line 6 original: a ping-pong echo (its time follows PreDelay) whose repeats are heard on their
  // own and also feed a lush, strongly modulated tail.
  program({ type: FDN, extra: ECHOES, rtMin: 0.8, rtMax: 8, level: 0.203, toneHz: 6500, lowCutHz: 100, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [31.1, 36.7, 42.9, 49.3, 55.7, 62.3, 69.1, 76.9], tapMs: [0.4, 0.6, 9.1, 11.7, 19.3, 23.9, 31.7, 37.3], apMs: 4.3, apGain: 0.6,
    numDiff: 4, diffMs: [3.7, 6.7, 10.1, 14.9, 4.1, 6.1, 11.3, 13.7], diffGain: 0.7, dampHz: 4500, hfRatio: 0.45, bassHz: 250, bassRatio: 1.1,
    modMs: 0.8, modHz: 0.55, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []], numSprings: 0, stages: 0,
    springMs: [0], springCoef: 0, drive: 0 }),

  // Tile - a small tiled room: hard walls, so strong, bright, clearly separate reflections (little
  // diffusion), short lines, hardly any damping, a bright ring around 3 kHz.
  program({ type: FDN, extra: PLAIN, rtMin: 0.2, rtMax: 2.5, level: 0.161, toneHz: 11000, lowCutHz: 140, peakHz: 3200, peakDb: 3, peakQ: 0.9,
    lineMs: [11.9, 14.3, 16.9, 19.7, 22.7, 26.3, 30.1, 34.7], tapMs: [0.3, 0.5, 2.3, 3.1, 5.9, 7.1, 10.3, 12.7], apMs: 1.7, apGain: 0.45, numDiff: 2,
    diffMs: [1.3, 3.1, 0, 0, 1.7, 2.9, 0, 0], diffGain: 0.5, dampHz: 9000, hfRatio: 0.75, bassHz: 250, bassRatio: 0.8, modMs: 0.12, modHz: 1.1,
    tankLevel: 0.7, earlyLevel: 1, numEarly: 8,
    earlyMs: [[2.3, 5.3, 8.9, 12.1, 17.3, 23.9, 31.1, 39.7], [3.1, 4.7, 9.7, 13.9, 16.1, 25.3, 29.9, 41.3]],
    earlyGain: [[0.9, -0.78, 0.7, 0.61, -0.52, 0.44, -0.37, 0.3], [0.88, 0.8, -0.68, 0.6, 0.55, -0.43, 0.38, -0.29]], numSprings: 0, stages: 0,
    springMs: [0], springCoef: 0, drive: 0 }),

  // Cave - Line 6 original: a huge cavern. Very long lines, far-wall slaps up to a quarter of a second
  // away, dark (strong damping), a booming low-mid resonance and a bass that rings longest.
  program({ type: FDN, extra: PLAIN, rtMin: 1.5, rtMax: 14, level: 0.277, toneHz: 3200, lowCutHz: 60, peakHz: 280, peakDb: 5, peakQ: 1.4,
    lineMs: [61.7, 73.3, 84.1, 97.3, 109.9, 124.7, 139.1, 157.3], tapMs: [0.5, 0.7, 29.3, 37.1, 61.3, 71.9, 97.3, 113.1], apMs: 7.1, apGain: 0.6,
    numDiff: 4, diffMs: [7.9, 12.7, 19.9, 27.1, 8.9, 11.3, 21.1, 25.9], diffGain: 0.7, dampHz: 2200, hfRatio: 0.3, bassHz: 300, bassRatio: 1.6,
    modMs: 0.7, modHz: 0.3, tankLevel: 1, earlyLevel: 0.5, numEarly: 6,
    earlyMs: [[23.3, 57.1, 89.9, 131.3, 187.7, 251.9], [31.7, 49.3, 97.1, 139.7, 173.9, 263.3]],
    earlyGain: [[0.55, -0.5, 0.46, 0.4, -0.34, 0.28], [0.53, 0.5, -0.45, 0.41, 0.35, -0.27]], numSprings: 0, stages: 0, springMs: [0], springCoef: 0,
    drive: 0 }),

  // Ducking - a hall whose level is pulled down by an envelope follower on the dry signal and swells
  // back when the playing stops.
  program({ type: FDN, extra: DUCKER, rtMin: 0.8, rtMax: 10, level: 0.255, toneHz: 7000, lowCutHz: 90, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [37.9, 44.3, 51.7, 59.3, 67.7, 76.1, 85.9, 95.3], tapMs: [0.4, 0.6, 13.3, 16.9, 27.1, 32.3, 45.7, 51.1], apMs: 4.9, apGain: 0.6,
    numDiff: 4, diffMs: [4.3, 7.9, 12.1, 17.3, 4.9, 7.3, 12.9, 16.7], diffGain: 0.7, dampHz: 4200, hfRatio: 0.4, bassHz: 250, bassRatio: 1.2,
    modMs: 0.5, modHz: 0.5, tankLevel: 1, earlyLevel: 0.3, numEarly: 6,
    earlyMs: [[9.1, 20.3, 31.9, 44.3, 58.7, 77.9], [11.3, 18.7, 33.1, 46.1, 56.3, 80.3]],
    earlyGain: [[0.5, -0.46, 0.44, 0.38, -0.33, 0.27], [0.48, 0.47, -0.42, 0.39, 0.32, -0.28]], numSprings: 0, stages: 0, springMs: [0],
    springCoef: 0, drive: 0 }),

  // Octo - Line 6 original: a shimmer. What comes out of two of the eight lines goes through an octave-up
  // pitch shifter and back into the network, so every layer grows another one an octave above it.
  program({ type: FDN, extra: OCTAVE, rtMin: 1.5, rtMax: 15, level: 0.193, toneHz: 8000, lowCutHz: 120, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [37.1, 43.3, 50.9, 57.7, 65.3, 73.9, 81.1, 89.9], tapMs: [0.4, 0.6, 12.7, 15.1, 25.3, 29.9, 41.9, 47.3], apMs: 4.7, apGain: 0.6,
    numDiff: 4, diffMs: [6.1, 10.7, 16.3, 23.9, 6.7, 9.7, 17.9, 22.3], diffGain: 0.7, dampHz: 6000, hfRatio: 0.6, bassHz: 250, bassRatio: 1,
    modMs: 0.6, modHz: 0.5, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []], numSprings: 0, stages: 0,
    springMs: [0], springCoef: 0, drive: 0 }),

  // Spring - a studio spring reverb: three springs of different length, moderate dispersion, clean drive,
  // the springs coupled to each other so the echoes blur quickly.
  program({ type: SPRINGS, extra: PLAIN, rtMin: 0.8, rtMax: 4, level: 0.579, toneHz: 5200, lowCutHz: 80, peakHz: 0, peakDb: 0, peakQ: 1, lineMs: [0],
    tapMs: [0], apMs: 0, apGain: 0, numDiff: 0, diffMs: [0, 0, 0, 0, 0, 0, 0, 0], diffGain: 0, dampHz: 1500, hfRatio: 0.55, bassHz: 0, bassRatio: 1,
    modMs: 0, modHz: 0, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []], numSprings: 3, stages: 64,
    springMs: [29.3, 36.7, 43.1], springCoef: 0.5, drive: 0 }),

  // '63 Spring - Fender 6G15 tube reverb unit: a two-spring tank driven by a tube (asymmetric soft
  // clipping), strong dispersion (the "drip"), band-limited to the tank's 100 Hz .. 4 kHz.
  program({ type: SPRINGS, extra: PLAIN, rtMin: 1, rtMax: 5, level: 0.661, toneHz: 4200, lowCutHz: 130, peakHz: 0, peakDb: 0, peakQ: 1, lineMs: [0],
    tapMs: [0], apMs: 0, apGain: 0, numDiff: 0, diffMs: [0, 0, 0, 0, 0, 0, 0, 0], diffGain: 0, dampHz: 1500, hfRatio: 0.45, bassHz: 0, bassRatio: 1,
    modMs: 0, modHz: 0, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []], numSprings: 2, stages: 150,
    springMs: [33.1, 41.3, 0], springCoef: 0.5, drive: 1.4 }),

  // Particle Verb - Line 6 original: two cross-coupled chains of long modulated all-passes (a pad with a
  // slow attack) with a pitch shifter in each feedback path: off (Stable), a few cents up per trip
  // (Critical), or swinging over an octave with more feedback and modulation (Hazard).
  program({ type: PARTICLE, extra: PLAIN, rtMin: 2, rtMax: 30, level: 0.754, toneHz: 9000, lowCutHz: 100, peakHz: 0, peakDb: 0, peakQ: 1,
    lineMs: [0], tapMs: [0], apMs: 0, apGain: 0.62, numDiff: 4, diffMs: [6.1, 10.7, 16.3, 23.9, 6.7, 9.7, 17.9, 22.3], diffGain: 0.7, dampHz: 5000,
    hfRatio: 1, bassHz: 0, bassRatio: 1, modMs: 1.2, modHz: 0.45, tankLevel: 1, earlyLevel: 0, numEarly: 0, earlyMs: [[], []], earlyGain: [[], []],
    numSprings: 0, stages: 0, springMs: [0], springCoef: 0, drive: 0 }),
];

const zap = (v) => (v > 1.0e-18 || v < -1.0e-18 ? v : 0); // no denormals in the loops
const whole = (v) => Math.floor(v + 0.5);
const poleCoef = (hz, fs) => f32(1 - Math.exp((-2 * PI * Math.min(hz, 0.45 * fs)) / fs));

function nextPrime(n) {
  if (n < 3) return 3;
  for (n |= 1; ; n += 2) {
    let prime = true;
    for (let d = 3; d * d <= n; d += 2) if (n % d === 0) { prime = false; break; }
    if (prime) return n;
  }
}

export class VerbFx {
  constructor() {
    this.fs = 48000;
    this.variant = 0; this.activeVariant = 0;
    this.prog = PROGRAMS[0];

    // knobs
    this.decayTarget = 0.5; this.toneTarget = 0.5; this.decaySm = 0.5; this.toneSm = 0.5; this.smoothCoef = 0.02;
    this.preTarget = 0; this.echoTarget = 4800; this.condition = 0; this.activeCondition = 0; this.activeEcho = 4800;
    this.mix = f32(0.3);
    this.driveGain = new Smoothed(1);

    // the delay memory
    this.pool = new Float32Array(64); this.chain = new Float32Array(6 * MAX_STAGES);
    this.bs = new Int32Array(NUM_DELAYS); this.mk = new Int32Array(NUM_DELAYS);
    this.wp = 0; this.wpLow = 0; this.tickPos = 0;
    this.sxL = new Float32Array(TICK); this.sxR = new Float32Array(TICK);
    this.syL = new Float32Array(TICK); this.syR = new Float32Array(TICK);
    this.duckGain = new Float32Array(TICK);

    // pre-delay, echo
    this.preCur = 0; this.preNext = 0; this.preFade = 0; this.echoCur = 4800; this.echoNext = 4800; this.echoFade = 0; this.fadeLen = 1920;
    this.fadeStep = 0; this.echoCoef = 0; this.echoLp = 0;
    this.echoOn = false; this.duckOn = false; this.shiftOn = false; this.voiceOn = false;

    // feedback delay network
    this.numEarly = 0; this.numDiff = 0;
    this.lineLen = new Int32Array(8); this.apLen = new Int32Array(8); this.tapLen = new Int32Array(8); this.diffLen = new Int32Array(8);
    this.earlyLen = new Int32Array(2 * MAX_EARLY); this.earlyBase = new Int32Array(2 * MAX_EARLY);
    this.earlyGain = new Float32Array(2 * MAX_EARLY);
    this.loopLen = new Float64Array(8);
    this.coef = new Float32Array(NUM_COEFS); this.coefTarget = new Float32Array(NUM_COEFS); this.coefInc = new Float32Array(NUM_COEFS);
    this.ramping = false;
    this.dampHi = new Float32Array(8); this.dampLo = new Float32Array(8);
    this.loCoef = 0; this.lowCoef = 0; this.lowState = new Float32Array(2);
    this.diffGain = f32(0.7); this.apGain = 0.5; this.tankLevel = 1;
    this.o = new Float32Array(8);

    // modulation
    this.modPhase = new Float64Array(4); this.modStep = new Float64Array(4); this.modBase = new Float64Array(4);
    this.modDelay = new Float64Array(4); this.modInc = new Float64Array(4); this.depthSm = 0;

    // pitch shifters (Octo, Particle Verb)
    this.shiftPhase = new Float64Array(2); this.shiftRate = new Float64Array(2); this.hazardPhase = new Float64Array(2);
    this.shiftLen = new Int32Array(2);
    this.shiftCoef = 0.5; this.shiftPole = new Float32Array(2);
    this.shiftLp = [new Biquad(), new Biquad()]; this.shiftHp = [new Biquad(), new Biquad()];

    // ducker
    this.duckEnv = 0; this.duckAmount = 0; this.duckHold = 0; this.duckAttack = 0; this.duckRelease = 0;

    // springs
    this.decim = 5; this.decPhase = 0; this.numSprings = 2; this.stages = 32; this.springLen = new Int32Array(3);
    this.springCoef = f32(0.6); this.drive = 0; this.driveBias = 0; this.driveNorm = 1; this.lowHp = 0;
    this.springFb = new Float32Array(3); this.springLp = new Float32Array(3);
    this.lowPrev = new Float32Array(2); this.lowCur = new Float32Array(2);
    this.springOutL = new Float32Array(3); this.springOutR = new Float32Array(3);
    this.back = new Float32Array(3);
    this.aaIn = [new Biquad(), new Biquad(), new Biquad()];
    this.aaOut = [new Biquad(), new Biquad(), new Biquad(), new Biquad(), new Biquad(), new Biquad()]; // left, right

    // Particle Verb
    this.loopBack = new Float32Array(2); this.loopHp = new Float32Array(2);
    this.pin = new Float32Array(2); this.early = new Float32Array(2); this.late = new Float32Array(2);

    // output
    this.voice = [new Biquad(), new Biquad()];
    this.tone = new Float32Array(4);
    this.outGain = 1; this.outGainInc = 0; this.outGainTarget = 1;
    this.biquads = [...this.voice, ...this.shiftLp, ...this.shiftHp, ...this.aaIn, ...this.aaOut];
  }

  prepare(sampleRate, maxBlock) {
    const fs = (this.fs = sampleRate);

    // one buffer for every delay line, each a power-of-two stretch of it
    let total = 0;
    for (let d = 0; d < NUM_DELAYS; ++d) {
      const seconds = d < D_EARLY_L ? 0.2 : d < D_DIFF ? 0.28 : d < D_LINE ? 0.03 : d < D_AP ? 0.17 : d < D_ECHO_L ? 0.08 : d < D_SHIFT_A ? 0.51 : 0.09;
      let size = 64;
      while (size < Math.floor(seconds * fs) + 8) size *= 2;
      this.bs[d] = total;
      this.mk[d] = size - 1;
      total += size;
    }
    this.pool = new Float32Array(total);
    this.chain.fill(0);

    this.smoothCoef = 1 / (1 + (0.05 * fs) / TICK);
    this.fadeLen = Math.floor(0.04 * fs);
    this.fadeStep = f32(1 / this.fadeLen);
    this.decim = Math.max(1, Math.floor(fs / 9600 + 0.5));
    this.driveGain.reset(fs, 0.03);
    this.reset();
  }

  reset() {
    this.pool.fill(0);
    this.chain.fill(0);
    this.wp = this.wpLow = this.tickPos = this.decPhase = 0;

    this.activeVariant = this.variant;
    this.prog = PROGRAMS[this.variant];
    this.decaySm = this.decayTarget;
    this.toneSm = this.toneTarget;
    this.activeCondition = this.condition;
    this.activeEcho = this.echoTarget;
    this.preCur = this.preNext = this.preTarget;
    this.echoCur = this.echoNext = this.echoTarget;
    this.preFade = this.echoFade = 0;
    this.driveGain.setCurrentAndTarget(this.driveGain.target);

    this.dampHi.fill(0); this.dampLo.fill(0);
    this.tone.fill(0);
    this.springFb.fill(0); this.springLp.fill(0);
    this.lowState.fill(0); this.loopBack.fill(0); this.loopHp.fill(0); this.lowPrev.fill(0); this.lowCur.fill(0); this.shiftPole.fill(0);
    for (let i = 0; i < 2; ++i) {
      this.voice[i].reset();
      this.shiftLp[i].reset();
      this.shiftHp[i].reset();
      this.shiftPhase[i] = 0.5;
      this.shiftRate[i] = 0;
      this.hazardPhase[i] = 1.3 * i;
    }
    for (const f of this.aaIn) f.reset();
    for (const f of this.aaOut) f.reset();
    this.echoLp = this.lowHp = this.duckEnv = this.duckAmount = 0;

    this.configure();
    this.depthSm = this.depthTarget();
    this.updateCoefficients();
    for (let c = 0; c < NUM_COEFS; ++c) {
      this.coef[c] = this.coefTarget[c];
      this.coefInc[c] = 0;
    }
    this.ramping = false;
    this.outGain = this.outGainTarget;
    this.outGainInc = 0;

    for (let i = 0; i < 4; ++i) {
      this.modPhase[i] = 1.7 * i;
      this.modDelay[i] = this.modBase[i] + this.depthSm * Math.sin(this.modPhase[i]);
      this.modInc[i] = 0;
    }
    if (this.prog.type === PARTICLE && this.condition === 1)
      for (let i = 0; i < 2; ++i) this.shiftRate[i] = this.criticalRate(i);
  }

  setModel(variant) { this.variant = clamp(variant | 0, 0, PROGRAMS.length - 1); }

  setParameters(k) {
    const fs = this.fs;
    this.decayTarget = clamp(f32(k[0]) * 0.01, 0, 1);

    if (PROGRAMS[this.variant].type === PARTICLE) {
      this.condition = clamp(Math.floor(f32(k[1]) + 0.5), 0, 2);
      this.driveGain.setTarget(f32(Math.pow(10, (f32(k[2]) - 50) * 0.012))); // +-12 dB
      this.toneTarget = 0.5;
      this.preTarget = 0;
    } else {
      this.preTarget = Math.floor(clamp(f32(k[1]), 0, 200) * 0.001 * fs + 0.5);
      this.toneTarget = clamp(f32(k[2]) * 0.01, 0, 1);
    }

    this.echoTarget = Math.floor(0.1 * fs + 0.5) + 2 * this.preTarget; // Echo: 100 ms + twice the pre-delay
    this.mix = clamp(f32(f32(k[3]) * f32(0.01)), 0, 1);
  }

  getMix() { return this.mix; }

  /** About the time the tail needs to fall 100 dB at the current knobs (plus the pre-delay). */
  getTailSeconds() {
    const p = PROGRAMS[this.variant];
    let rt = p.rtMin * Math.pow(p.rtMax / p.rtMin, this.decayTarget);
    if (p.type === FDN) rt *= p.extra === OCTAVE ? 1.3 : Math.max(1, p.bassRatio); // Octo: the octave layers ring on

    let tail = rt * (100 / 60) + this.preTarget / this.fs;
    if (p.extra === ECHOES) tail += (2 * this.echoTarget) / this.fs;
    if (p.type === SPRINGS) tail += 0.1;
    return f32(clamp(tail, 0.5, 20));
  }

  process(left, right, numSamples) {
    if (this.activeVariant !== this.variant) this.reset();

    for (let pos = 0; pos < numSamples; ) {
      if (this.tickPos === 0) this.controlTick();

      const n = Math.min(numSamples - pos, TICK - this.tickPos);
      this.inputStage(left, right, pos, n);

      const type = this.prog.type;
      if (type === FDN) this.tankStage(n);
      else if (type === SPRINGS) this.springStage(n);
      else this.particleStage(n);

      this.outputStage(left, right, pos, n);
      this.wp = (this.wp + n) & 0x3fffffff;
      this.tickPos = (this.tickPos + n) & (TICK - 1);
      pos += n;
    }
  }

  // ==========================================================================
  /** Lengths, fixed filters and gains of the current model at this sample rate (no allocation in the C++). */
  configure() {
    const p = this.prog, fs = this.fs, bs = this.bs;
    const perMs = 0.001 * fs;

    this.duckOn = p.extra === DUCKER;
    this.echoOn = p.extra === ECHOES;
    this.shiftOn = p.extra === OCTAVE;
    this.lowCoef = poleCoef(p.lowCutHz, fs);
    this.voiceOn = p.peakDb !== 0;
    if (this.voiceOn) for (const v of this.voice) v.setPeak(fs, p.peakHz, p.peakQ, p.peakDb);

    this.numEarly = 0;
    this.numDiff = p.numDiff;
    this.diffGain = p.diffGain;
    for (let i = 0; i < 8; ++i) this.diffLen[i] = nextPrime(whole(p.diffMs[i] * perMs));
    for (let i = 0; i < 4; ++i) {
      this.modStep[i] = (2 * PI * p.modHz * RATE_SPREAD[i] * TICK) / fs;
      this.modBase[i] = 8;
    }
    for (let i = 0; i < 2; ++i) {
      this.shiftLen[i] = whole(SHIFT_MS[i] * perMs);
      this.shiftLp[i].setLowPass(fs, p.type === PARTICLE ? 5000 : 3400, 0.7071);
    }
    this.shiftCoef = poleCoef(5000, fs);
    for (const f of this.shiftHp) f.setHighPass(fs, 160, 0.7071);

    if (p.type === FDN) {
      this.numEarly = p.numEarly;
      this.apGain = p.apGain;
      this.tankLevel = p.tankLevel;
      this.loCoef = poleCoef(p.bassHz, fs);
      for (let i = 0; i < 8; ++i) {
        this.lineLen[i] = nextPrime(whole(p.lineMs[i] * perMs));
        this.apLen[i] = nextPrime(whole(p.apMs * AP_SPREAD[i] * perMs));
        this.tapLen[i] = clamp(whole(p.tapMs[i] * perMs), 1, Math.floor((this.lineLen[i] * 3) / 4));
        this.loopLen[i] = this.lineLen[i];
        if (i < 4) this.modBase[i] = this.lineLen[i];
      }
      for (let side = 0; side < 2; ++side)
        for (let k = 0; k < this.numEarly; ++k) {
          const e = side * MAX_EARLY + k;
          this.earlyLen[e] = Math.max(1, whole(p.earlyMs[side][k] * perMs));
          this.earlyGain[e] = p.earlyGain[side][k] * p.earlyLevel;
          this.earlyBase[e] = bs[((k & 1) ^ side) !== 0 ? D_EARLY_R : D_EARLY_L]; // every other tap comes from the other side
        }

      if (this.shiftOn) for (let i = 0; i < 2; ++i) this.shiftRate[i] = -1 / this.shiftLen[i]; // one octave up

      this.echoCoef = poleCoef(4500, fs);
      this.duckHold = f32(Math.exp(-1 / (0.05 * fs)));
      this.duckAttack = f32(1 - Math.exp(-1 / (0.01 * fs)));
      this.duckRelease = f32(1 - Math.exp(-1 / (0.25 * fs)));
    } else if (p.type === SPRINGS) {
      const fsLow = fs / this.decim;
      const q6 = [0.5176, 0.7071, 1.9319]; // sixth-order Butterworth
      for (let i = 0; i < 3; ++i) {
        this.aaIn[i].setLowPass(fs, 0.4 * fsLow, q6[i]);
        this.aaOut[i].setLowPass(fs, 0.42 * fsLow, q6[i]);
        this.aaOut[3 + i].setLowPass(fs, 0.42 * fsLow, q6[i]);
      }

      this.numSprings = p.numSprings;
      this.stages = Math.min(p.stages, MAX_STAGES);
      for (let s = 0; s < 3; ++s) {
        this.springOutL[s] = this.numSprings === 3 ? SPRING_OUT_L3[s] : SPRING_OUT_L2[s];
        this.springOutR[s] = this.numSprings === 3 ? SPRING_OUT_R3[s] : SPRING_OUT_R2[s];
      }
      this.springCoef = p.springCoef;
      const lowDelay = (this.stages * (1 - p.springCoef)) / (1 + p.springCoef); // of one chain, at low frequencies
      for (let s = 0; s < this.numSprings; ++s) {
        this.springLen[s] = Math.max(2, whole(p.springMs[s] * 0.001 * fsLow - lowDelay));
        this.apLen[2 * s] = nextPrime(whole(0.37 * p.springMs[s] * 0.001 * fsLow)); // reflections inside the spring
        this.apLen[2 * s + 1] = nextPrime(whole(0.23 * p.springMs[s] * 0.001 * fsLow));
        this.loopLen[s] = (2 * (this.springLen[s] + lowDelay)) / fsLow; // seconds per round trip
      }
      this.lowCoef = poleCoef(p.lowCutHz, fsLow);
      this.loCoef = poleCoef(p.dampHz, fsLow);

      this.drive = p.drive;
      this.driveBias = f32(Math.tanh(0.25));
      this.driveNorm = this.drive > 0 ? f32(1 / (this.drive * (1 - this.driveBias * this.driveBias))) : 1;
    } else {
      for (let side = 0; side < 2; ++side) {
        for (let k = 0; k < 4; ++k) {
          const a = side * 4 + k;
          this.apLen[a] = nextPrime(whole(PARTICLE_AP_MS[a] * perMs));
          if (k < 2) this.modBase[side * 2 + k] = this.apLen[a];
        }
        this.lineLen[side] = nextPrime(whole(PARTICLE_DELAY_MS[side] * perMs));
        this.loopLen[side] = (this.lineLen[side] + 0.5 * this.shiftLen[side]) / fs; // seconds in the delay and the shifter
      }
    }
  }

  depthTarget() {
    const depth = this.prog.modMs * 0.001 * this.fs;
    return this.prog.type === PARTICLE && this.activeCondition === 2 ? 2.5 * depth : depth;
  }

  criticalRate(side) { return (1 - Math.pow(2, 14 / 1200)) / this.shiftLen[side]; }

  /** Everything that follows the Decay and Tone knobs (called at the control rate while they move). */
  updateCoefficients() {
    const p = this.prog, fs = this.fs, target = this.coefTarget;
    let rt = p.rtMin * Math.pow(p.rtMax / p.rtMin, this.decaySm);
    const tilt = Math.pow(4, this.toneSm - 0.5); // 0.5 .. 2

    if (p.type === FDN) {
      const hf = clamp(p.hfRatio * tilt, 0.05, 1);
      target[C_DAMP] = poleCoef(p.dampHz * tilt, fs);
      for (let i = 0; i < 8; ++i) {
        const e = (-3 * this.loopLen[i]) / fs;
        const mid = Math.pow(10, e / rt), high = Math.pow(10, e / (rt * hf)), low = Math.pow(10, e / (rt * p.bassRatio));
        target[i] = 0.35355339059327373 * high;
        target[8 + i] = 0.35355339059327373 * (mid - high);
        target[16 + i] = 0.35355339059327373 * (low - mid);
        target[24 + i] = Math.pow(10, (-3 * this.apLen[i]) / (fs * rt)); // the all-pass loses in proportion to its length too
      }
      target[C_ECHO] = Math.min(0.85, Math.pow(10, (-3 * this.echoTarget) / (fs * rt)));
      target[C_SHIFT] = (2 * (0.75 + 0.5 * this.decaySm)) / Math.sqrt(rt); // about the same share of octave whatever the decay time
    } else if (p.type === SPRINGS) {
      for (let s = 0; s < this.numSprings; ++s) target[s] = -Math.pow(10, (-3 * this.loopLen[s]) / rt); // [0..2]: gain of each spring's loop
      for (let a = 0; a < 6; ++a) target[24 + a] = Math.pow(10, (-3 * this.apLen[a] * this.decim) / (fs * rt));
      target[3] = clamp(p.hfRatio * tilt, 0.1, 0.95); // [3]: how much of the highs survives a round trip
    } else {
      if (this.activeCondition === 2) rt *= 1.5;
      for (let side = 0; side < 2; ++side) target[side] = Math.pow(10, (-3 * this.loopLen[side]) / rt); // [0..1]: gain of each feedback path
      for (let a = 0; a < 8; ++a) target[24 + a] = Math.pow(10, (-3 * this.apLen[a]) / (fs * rt));
    }

    const mid = p.toneHz;
    const hz = this.toneSm < 0.5 ? 900 * Math.pow(mid / 900, 2 * this.toneSm) : mid * Math.pow(18000 / mid, 2 * this.toneSm - 1);
    target[C_TONE] = poleCoef(hz, fs);
    this.outGainTarget = p.level * Math.pow(rt, -0.3); // longer decays build up more level: take some of it back
  }

  controlTick() {
    const fs = this.fs;

    // filter states that have died away become zero (a one-pole can stall on a denormal number)
    for (let i = 0; i < 8; ++i) {
      this.dampHi[i] = zap(this.dampHi[i]);
      this.dampLo[i] = zap(this.dampLo[i]);
    }
    for (let i = 0; i < 4; ++i) this.tone[i] = zap(this.tone[i]);
    for (let i = 0; i < 3; ++i) this.springLp[i] = zap(this.springLp[i]);
    for (let i = 0; i < 2; ++i) {
      this.lowState[i] = zap(this.lowState[i]);
      this.shiftPole[i] = zap(this.shiftPole[i]);
      this.loopHp[i] = zap(this.loopHp[i]);
    }
    this.echoLp = zap(this.echoLp);
    this.lowHp = zap(this.lowHp);
    this.duckAmount = zap(this.duckAmount);
    // (JavaScript only: the C++ runs with denormals flushed to zero, which keeps its biquads clean)
    const biquads = this.biquads;
    for (let i = 0; i < biquads.length; ++i) { biquads[i].z1 = zap(biquads[i].z1); biquads[i].z2 = zap(biquads[i].z2); }

    let moving = false;
    if (this.decaySm !== this.decayTarget) {
      this.decaySm += (this.decayTarget - this.decaySm) * this.smoothCoef;
      if (Math.abs(this.decayTarget - this.decaySm) < 1.0e-4) this.decaySm = this.decayTarget;
      moving = true;
    }
    if (this.toneSm !== this.toneTarget) {
      this.toneSm += (this.toneTarget - this.toneSm) * this.smoothCoef;
      if (Math.abs(this.toneTarget - this.toneSm) < 1.0e-4) this.toneSm = this.toneTarget;
      moving = true;
    }
    if (this.activeCondition !== this.condition || this.activeEcho !== this.echoTarget) {
      this.activeCondition = this.condition;
      this.activeEcho = this.echoTarget; // Echo: the feedback follows the echo time
      moving = true;
    }
    const coef = this.coef, target = this.coefTarget, inc = this.coefInc;
    if (moving) {
      // the loop gains glide to their new values over the next tick (no steps in the feedback)
      this.updateCoefficients();
      for (let c = 0; c < NUM_COEFS; ++c) inc[c] = (target[c] - coef[c]) * (1 / TICK);
      this.ramping = true;
    } else if (this.ramping) {
      for (let c = 0; c < NUM_COEFS; ++c) coef[c] = target[c];
      this.ramping = false;
    }
    this.outGainInc = (this.outGainTarget - this.outGain) * (1 / TICK);

    // pre-delay and echo time change by crossfading to a second tap
    if (this.preFade === 0 && this.preCur !== this.preTarget) { this.preNext = this.preTarget; this.preFade = this.fadeLen; }
    if (this.echoFade === 0 && this.echoCur !== this.echoTarget) { this.echoNext = this.echoTarget; this.echoFade = this.fadeLen; }

    // modulation: four sine LFOs, the delay times move in straight lines between the ticks
    const modPhase = this.modPhase;
    this.depthSm += (this.depthTarget() - this.depthSm) * 0.02;
    for (let i = 0; i < 4; ++i) {
      modPhase[i] += this.modStep[i];
      if (modPhase[i] >= 2 * PI) modPhase[i] -= 2 * PI;
      this.modInc[i] = (this.modBase[i] + this.depthSm * Math.sin(modPhase[i]) - this.modDelay[i]) * (1 / TICK);
    }

    if (this.prog.type === PARTICLE) {
      // Hazard: the two shifters swing between +2 .. +12 and -12 .. +2 semitones
      const hazardPhase = this.hazardPhase, shiftRate = this.shiftRate;
      hazardPhase[0] += (2 * PI * 0.13 * TICK) / fs;
      hazardPhase[1] += (2 * PI * 0.09 * TICK) / fs;
      for (let i = 0; i < 2; ++i) {
        if (hazardPhase[i] >= 2 * PI) hazardPhase[i] -= 2 * PI;

        let goal = 0;
        if (this.activeCondition === 1) goal = this.criticalRate(i);
        else if (this.activeCondition === 2)
          goal = (1 - Math.pow(2, ((i === 0 ? 7 : -5) + (i === 0 ? 5 : 7) * Math.sin(hazardPhase[i])) / 12)) / this.shiftLen[i];

        shiftRate[i] += (goal - shiftRate[i]) * 0.1;
        if (Math.abs(goal - shiftRate[i]) < 1.0e-12) shiftRate[i] = goal;
      }
    }
  }

  // ==========================================================================
  /** Pre-delay (both sides) and the ducker's envelope follower. */
  inputStage(left, right, pos, n) {
    const P = this.pool, sxL = this.sxL, sxR = this.sxR;
    const bL = this.bs[D_PRE_L], bR = this.bs[D_PRE_R], m = this.mk[D_PRE_L];
    const fadeStep = this.fadeStep, duckOn = this.duckOn, duckGain = this.duckGain;

    for (let j = 0; j < n; ++j) {
      const w = this.wp + j;
      const inL = left[pos + j], inR = right[pos + j];
      P[bL + (w & m)] = inL;
      P[bR + (w & m)] = inR;

      let xl = P[bL + ((w - this.preCur) & m)], xr = P[bR + ((w - this.preCur) & m)];
      if (this.preFade > 0) {
        const g = f32(this.preFade * fadeStep);
        const nl = P[bL + ((w - this.preNext) & m)], nr = P[bR + ((w - this.preNext) & m)];
        xl = nl + (xl - nl) * g;
        xr = nr + (xr - nr) * g;
        if (--this.preFade === 0) this.preCur = this.preNext;
      }
      sxL[j] = xl;
      sxR[j] = xr;

      if (duckOn) {
        const level = 0.5 * (Math.abs(inL) + Math.abs(inR));
        this.duckEnv = f32(zap(Math.max(level, this.duckEnv * this.duckHold)));
        const goal = Math.min(1, this.duckEnv * 16); // fully ducked above -24 dBFS
        this.duckAmount = f32(this.duckAmount + (goal - this.duckAmount) * (goal > this.duckAmount ? this.duckAttack : this.duckRelease));
        duckGain[j] = 1 - f32(0.87) * this.duckAmount; // down to -18 dB
      }
    }
  }

  /** Two-tap crossfading pitch shifter on its own delay line; the two windows always add up to one,
      so it never adds energy (it can sit inside a feedback loop). */
  shift(s, x, w) {
    const P = this.pool;
    const b = this.bs[D_SHIFT_A + s], m = this.mk[D_SHIFT_A + s];
    P[b + (w & m)] = x;

    let ph = this.shiftPhase[s] + this.shiftRate[s];
    if (ph < 0) ph += 1;
    else if (ph >= 1) ph -= 1;
    this.shiftPhase[s] = ph;

    const span = this.shiftLen[s];
    const d1 = 2 + span * ph, d2 = 2 + span * (ph < 0.5 ? ph + 0.5 : ph - 0.5);
    const i1 = d1 | 0, i2 = d2 | 0;
    const f1 = f32(d1 - i1), f2 = f32(d2 - i2);
    const a0 = P[b + ((w - i1) & m)], a1 = P[b + ((w - i1 - 1) & m)];
    const c0 = P[b + ((w - i2) & m)], c1 = P[b + ((w - i2 - 1) & m)];
    const x1 = a0 + f1 * (a1 - a0), x2 = c0 + f2 * (c1 - c0);

    const tri = f32(1 - Math.abs(2 * ph - 1));
    const win = tri * tri * (3 - 2 * tri);
    return x2 + win * (x1 - x2);
  }

  // ==========================================================================
  tankStage(n) {
    const P = this.pool, bs = this.bs, mk = this.mk, o = this.o, coef = this.coef, inc = this.coefInc;
    const sxL = this.sxL, sxR = this.sxR, syL = this.syL, syR = this.syR;
    const eM = mk[D_EARLY_L], cM = mk[D_ECHO_L];
    const lowState = this.lowState, lowCoef = this.lowCoef, loCoef = this.loCoef;
    const numEarly = this.numEarly, earlyGain = this.earlyGain, earlyBase = this.earlyBase, earlyLen = this.earlyLen;
    const numDiff = this.numDiff, diffLen = this.diffLen, diffGain = this.diffGain;
    const modDelay = this.modDelay, modInc = this.modInc, lineLen = this.lineLen, apLen = this.apLen, tapLen = this.tapLen;
    const dampHi = this.dampHi, dampLo = this.dampLo, apGain = this.apGain, tankLevel = this.tankLevel;
    const echoOn = this.echoOn, shiftOn = this.shiftOn, fadeStep = this.fadeStep, echoCoef = this.echoCoef;
    const shiftPole = this.shiftPole, shiftCoef = this.shiftCoef, sixth = f32(1 / 6);

    for (let j = 0; j < n; ++j) {
      const w = this.wp + j;
      let xl = sxL[j], xr = sxR[j];
      if (this.ramping) for (let c = 0; c < C_TONE; ++c) coef[c] += inc[c];

      lowState[0] += lowCoef * (xl - lowState[0]);
      lowState[1] += lowCoef * (xr - lowState[1]);
      xl -= lowState[0];
      xr -= lowState[1];

      // early reflections
      let el = 0, er = 0;
      if (numEarly > 0) {
        P[bs[D_EARLY_L] + (w & eM)] = xl;
        P[bs[D_EARLY_R] + (w & eM)] = xr;
        for (let k = 0; k < numEarly; ++k) {
          el += earlyGain[k] * P[earlyBase[k] + ((w - earlyLen[k]) & eM)];
          er += earlyGain[MAX_EARLY + k] * P[earlyBase[MAX_EARLY + k] + ((w - earlyLen[MAX_EARLY + k]) & eM)];
        }
      }

      // Echo: ping-pong repeats (left first), heard directly and sent into the tank
      if (echoOn) {
        let a = P[bs[D_ECHO_L] + ((w - this.echoCur) & cM)], b = P[bs[D_ECHO_R] + ((w - this.echoCur) & cM)];
        if (this.echoFade > 0) {
          const g = f32(this.echoFade * fadeStep);
          const na = P[bs[D_ECHO_L] + ((w - this.echoNext) & cM)], nb = P[bs[D_ECHO_R] + ((w - this.echoNext) & cM)];
          a = na + (a - na) * g;
          b = nb + (b - nb) * g;
          if (--this.echoFade === 0) this.echoCur = this.echoNext;
        }
        this.echoLp = f32(this.echoLp + echoCoef * (b - this.echoLp));
        P[bs[D_ECHO_L] + (w & cM)] = zap(0.5 * (xl + xr) + coef[C_ECHO] * this.echoLp);
        P[bs[D_ECHO_R] + (w & cM)] = zap(coef[C_ECHO] * a);
        el = f32(0.8) * a;
        er = f32(0.8) * b;
        xl += f32(0.7) * a;
        xr += f32(0.7) * b;
      }

      // input diffusers
      for (let k = 0; k < numDiff; ++k) {
        const dl = D_DIFF + k, dr = D_DIFF + 4 + k;
        const zl = P[bs[dl] + ((w - diffLen[k]) & mk[dl])], zr = P[bs[dr] + ((w - diffLen[4 + k]) & mk[dr])];
        const vl = xl - diffGain * zl, vr = xr - diffGain * zr;
        P[bs[dl] + (w & mk[dl])] = zap(vl);
        P[bs[dr] + (w & mk[dr])] = zap(vr);
        xl = zl + diffGain * vl;
        xr = zr + diffGain * vr;
      }

      // the eight lines: four with a slowly moving length, read with third-order Lagrange interpolation
      // (between its two middle points it never has a gain above one, and it keeps the highs)
      for (let i = 0; i < 4; ++i) {
        const d = modDelay[i];
        modDelay[i] = d + modInc[i];
        const di = d | 0;
        const x = f32(d - di), xp = x + 1, xm = x - 1, xn = x - 2;
        const b = bs[D_LINE + i], m = mk[D_LINE + i];
        o[i] = -x * xm * xn * sixth * P[b + ((w - di + 1) & m)] + xp * xm * xn * 0.5 * P[b + ((w - di) & m)]
             - xp * x * xn * 0.5 * P[b + ((w - di - 1) & m)] + xp * x * xm * sixth * P[b + ((w - di - 2) & m)];
      }
      for (let i = 4; i < 8; ++i) o[i] = P[bs[D_LINE + i] + ((w - lineLen[i]) & mk[D_LINE + i])];

      // decay per band: high, mid and low frequencies lose a different amount per trip
      const damp = coef[C_DAMP];
      for (let i = 0; i < 8; ++i) {
        const x = o[i];
        dampHi[i] += damp * (x - dampHi[i]);
        dampLo[i] += loCoef * (x - dampLo[i]);
        o[i] = coef[i] * x + coef[8 + i] * dampHi[i] + coef[16 + i] * dampLo[i];
      }

      // Octo: what comes out of two lines is band-limited, shifted up an octave and fed in again (crosswise),
      // so an octave layer grows on top of every layer. The low-pass ends the climb; the high-pass keeps
      // out what is too slow for the shifter to move (that would be a plain feedback loop)
      if (shiftOn) {
        shiftPole[0] += shiftCoef * (o[6] - shiftPole[0]);
        shiftPole[1] += shiftCoef * (o[7] - shiftPole[1]);
        const upL = this.shift(0, this.shiftHp[0].process(this.shiftLp[0].process(shiftPole[0])), w);
        const upR = this.shift(1, this.shiftHp[1].process(this.shiftLp[1].process(shiftPole[1])), w);
        xl += coef[C_SHIFT] * upR;
        xr += coef[C_SHIFT] * upL;
      }

      // Hadamard matrix (lossless; its 1 / sqrt (8) is part of the gains)
      const a0 = o[0] + o[1], a1 = o[0] - o[1], a2 = o[2] + o[3], a3 = o[2] - o[3];
      const a4 = o[4] + o[5], a5 = o[4] - o[5], a6 = o[6] + o[7], a7 = o[6] - o[7];
      const b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
      const b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
      o[0] = b0 + b4 + xl; o[1] = b1 + b5 + xr; o[2] = b2 + b6 - xl; o[3] = b3 + b7 - xr;
      o[4] = b0 - b4 + xl; o[5] = b1 - b5 + xr; o[6] = b2 - b6 - xl; o[7] = b3 - b7 - xr;

      // an all-pass in front of every line, then into the line
      for (let i = 0; i < 8; ++i) {
        const da = D_AP + i, dl = D_LINE + i;
        const z = coef[24 + i] * P[bs[da] + ((w - apLen[i]) & mk[da])];
        const v = o[i] - apGain * z;
        P[bs[da] + (w & mk[da])] = zap(v);
        P[bs[dl] + (w & mk[dl])] = zap(z + apGain * v);
      }

      const tl = P[bs[D_LINE] + ((w - tapLen[0]) & mk[D_LINE])] - P[bs[D_LINE + 2] + ((w - tapLen[2]) & mk[D_LINE + 2])]
               + P[bs[D_LINE + 4] + ((w - tapLen[4]) & mk[D_LINE + 4])] - P[bs[D_LINE + 6] + ((w - tapLen[6]) & mk[D_LINE + 6])];
      const tr = P[bs[D_LINE + 1] + ((w - tapLen[1]) & mk[D_LINE + 1])] - P[bs[D_LINE + 3] + ((w - tapLen[3]) & mk[D_LINE + 3])]
               + P[bs[D_LINE + 5] + ((w - tapLen[5]) & mk[D_LINE + 5])] - P[bs[D_LINE + 7] + ((w - tapLen[7]) & mk[D_LINE + 7])];

      syL[j] = el + tankLevel * tl;
      syR[j] = er + tankLevel * tr;
    }
  }

  // ==========================================================================
  /** One step of the springs, at the low rate. Each spring: delay and all-pass chain out to the far end
      (the output), delay and chain back, loss, and into the spring (and its neighbours) again. */
  springStep(x) {
    const P = this.pool, bs = this.bs, mk = this.mk, c = this.chain, coef = this.coef;
    const springCoef = this.springCoef, stages = this.stages, springLen = this.springLen, apLen = this.apLen;
    const back = this.back, springFb = this.springFb, springLp = this.springLp;
    const w = this.wpLow;
    this.wpLow = (this.wpLow + 1) & 0x3fffffff;

    this.lowHp = f32(this.lowHp + this.lowCoef * (x - this.lowHp));
    x -= this.lowHp;

    let outL = 0, outR = 0;
    back[0] = back[1] = back[2] = 0;
    for (let s = 0; s < this.numSprings; ++s) {
      const da = D_LINE + 2 * s, db = da + 1;
      let ci = 2 * s * MAX_STAGES;

      P[bs[da] + (w & mk[da])] = zap(x * SPRING_IN[s] + springFb[s]);
      let t = P[bs[da] + ((w - springLen[s]) & mk[da])];
      for (let k = 0; k < stages; ++k) {
        const y = springCoef * t + c[ci + k];
        c[ci + k] = t - springCoef * y;
        t = y;
      }
      outL += this.springOutL[s] * t;
      outR += this.springOutR[s] * t;

      // on the way back: the same again, with an all-pass before and after (the reflections inside
      // a real spring, which blur the repeats a little more on every trip)
      for (let half = 0; half < 2; ++half) {
        const a = 2 * s + half, d = D_AP + a;
        const z = coef[24 + a] * P[bs[d] + ((w - apLen[a]) & mk[d])];
        const v = t - 0.5 * z;
        P[bs[d] + (w & mk[d])] = zap(v);
        t = z + 0.5 * v;
        if (half === 1) break;

        P[bs[db] + (w & mk[db])] = t;
        t = P[bs[db] + ((w - springLen[s]) & mk[db])];
        ci += MAX_STAGES;
        for (let k = 0; k < stages; ++k) {
          const y = springCoef * t + c[ci + k];
          c[ci + k] = t - springCoef * y;
          t = y;
        }
      }
      springLp[s] += this.loCoef * (t - springLp[s]);
      back[s] = springLp[s] + coef[3] * (t - springLp[s]);
    }

    if (this.numSprings === 3) {
      const m = (back[0] + back[1] + back[2]) * f32(2 / 3); // Householder matrix: the springs feed each other
      for (let s = 0; s < 3; ++s) springFb[s] = coef[s] * (back[s] - m);
    } else {
      springFb[0] = coef[0] * (f32(0.8) * back[0] + f32(0.6) * back[1]); // a rotation: the two springs share their mounts
      springFb[1] = coef[1] * (f32(0.8) * back[1] - f32(0.6) * back[0]);
    }

    this.lowPrev[0] = this.lowCur[0];
    this.lowPrev[1] = this.lowCur[1];
    this.lowCur[0] = outL;
    this.lowCur[1] = outR;
  }

  springStage(n) {
    const sxL = this.sxL, sxR = this.sxR, syL = this.syL, syR = this.syR, coef = this.coef, inc = this.coefInc;
    const aaIn = this.aaIn, aaOut = this.aaOut, lowPrev = this.lowPrev, lowCur = this.lowCur;
    const drive = this.drive, driveBias = this.driveBias, driveNorm = this.driveNorm, decim = this.decim;
    const decStep = f32(1 / decim);
    for (let j = 0; j < n; ++j) {
      if (this.ramping) {
        for (let c = 0; c < 4; ++c) coef[c] += inc[c];
        for (let c = 24; c < 32; ++c) coef[c] += inc[c];
      }

      let m = 0.5 * (sxL[j] + sxR[j]);
      if (drive > 0) m = (f32(Math.tanh(f32(drive * m + 0.25))) - driveBias) * driveNorm; // the tube that drives the tank
      m = aaIn[2].process(aaIn[1].process(aaIn[0].process(m)));

      if (++this.decPhase >= decim) {
        this.decPhase = 0;
        this.springStep(f32(m));
      }

      const fr = f32((this.decPhase + 1) * decStep);
      syL[j] = aaOut[2].process(aaOut[1].process(aaOut[0].process(lowPrev[0] + (lowCur[0] - lowPrev[0]) * fr)));
      syR[j] = aaOut[5].process(aaOut[4].process(aaOut[3].process(lowPrev[1] + (lowCur[1] - lowPrev[1]) * fr)));
    }
  }

  // ==========================================================================
  particleStage(n) {
    const P = this.pool, bs = this.bs, mk = this.mk, coef = this.coef, inc = this.coefInc;
    const sxL = this.sxL, sxR = this.sxR, syL = this.syL, syR = this.syR;
    const g = this.prog.apGain, numDiff = this.numDiff, diffLen = this.diffLen, diffGain = this.diffGain;
    const modDelay = this.modDelay, modInc = this.modInc, apLen = this.apLen, lineLen = this.lineLen;
    const pin = this.pin, early = this.early, late = this.late, loopBack = this.loopBack, loopHp = this.loopHp;
    const lowState = this.lowState, lowCoef = this.lowCoef, third = f32(1 / 27);

    for (let j = 0; j < n; ++j) {
      const w = this.wp + j;
      const gain = this.driveGain.next();
      if (this.ramping) {
        coef[0] += inc[0];
        coef[1] += inc[1];
        for (let c = 24; c < 32; ++c) coef[c] += inc[c];
      }

      lowState[0] += lowCoef * (sxL[j] - lowState[0]);
      lowState[1] += lowCoef * (sxR[j] - lowState[1]);
      pin[0] = (sxL[j] - lowState[0]) * gain; pin[1] = (sxR[j] - lowState[1]) * gain;
      early[0] = early[1] = late[0] = late[1] = 0;

      // input diffusers: a dense burst instead of a click, before the long all-passes smear it further
      for (let k = 0; k < numDiff; ++k)
        for (let side = 0; side < 2; ++side) {
          const d = D_DIFF + side * 4 + k;
          const z = P[bs[d] + ((w - diffLen[side * 4 + k]) & mk[d])];
          const v = pin[side] - diffGain * z;
          P[bs[d] + (w & mk[d])] = zap(v);
          pin[side] = z + diffGain * v;
        }

      for (let side = 0; side < 2; ++side) {
        // the input plus what comes back from the other side, soft-limited (this keeps Hazard bounded)
        let s = f32(clamp(pin[side] + loopBack[side], -3, 3));
        s -= s * s * s * third;

        for (let k = 0; k < 4; ++k) {
          const a = side * 4 + k, b = bs[D_AP + a], m = mk[D_AP + a];
          let z;
          if (k < 2) {
            const mi = side * 2 + k;
            const d = modDelay[mi];
            modDelay[mi] = d + modInc[mi];
            const di = d | 0;
            const fr = f32(d - di);
            const z0 = P[b + ((w - di) & m)], z1 = P[b + ((w - di - 1) & m)];
            z = z0 + fr * (z1 - z0);
          } else {
            z = P[b + ((w - apLen[a]) & m)];
          }
          z *= coef[24 + a];

          const v = s - g * z;
          P[b + (w & m)] = zap(v);
          s = z + g * v;
          if (k === 1) early[side] = s;
        }
        late[side] = s;

        // round to the other side: delay, low-pass, pitch shifter, low cut, loss
        const dl = D_LINE + side;
        P[bs[dl] + (w & mk[dl])] = s;
        let t = this.shift(side, this.shiftLp[side].process(P[bs[dl] + ((w - lineLen[side]) & mk[dl])]), w);
        loopHp[side] += lowCoef * (t - loopHp[side]);
        t -= loopHp[side];
        loopBack[side ^ 1] = zap(t * coef[side]);
      }

      syL[j] = late[0] - f32(0.6) * early[1];
      syR[j] = late[1] + f32(0.6) * early[0];
    }
  }

  // ==========================================================================
  /** Voicing EQ, the Tone low-pass (12 dB / octave), level and ducking. */
  outputStage(left, right, pos, n) {
    const syL = this.syL, syR = this.syR, tone = this.tone, coef = this.coef, inc = this.coefInc;
    const voiceOn = this.voiceOn, voice = this.voice, duckOn = this.duckOn, duckGain = this.duckGain;
    for (let j = 0; j < n; ++j) {
      let yl = syL[j], yr = syR[j];
      if (voiceOn) {
        yl = voice[0].process(yl);
        yr = voice[1].process(yr);
      }

      if (this.ramping) coef[C_TONE] += inc[C_TONE];
      const toneCoef = coef[C_TONE];
      tone[0] += toneCoef * (yl - tone[0]);
      tone[1] += toneCoef * (tone[0] - tone[1]);
      tone[2] += toneCoef * (yr - tone[2]);
      tone[3] += toneCoef * (tone[2] - tone[3]);

      this.outGain += this.outGainInc;
      const g = duckOn ? f32(this.outGain) * duckGain[j] : f32(this.outGain);
      left[pos + j] = tone[1] * g;
      right[pos + j] = tone[3] * g;
    }
  }
}

const verb = (key, name, variant, basedOn, decay, preDelay, mix) =>
  model(key, name, CATEGORY.reverb, ENGINE.reverbFx, variant, basedOn,
        [percent("Decay", decay), millis("PreDelay", 0, 200, preDelay, 50), percent("Tone", 50), percent("Mix", mix)], { trails: true });

/** In the order of the variants. */
export const VERB_MODELS = [
  verb("plate", "Plate", 0, "Studio plate reverb", 50, 10, 30),
  verb("room", "Room", 1, "Studio room, mostly early reflections", 45, 5, 35),
  verb("chamber", "Chamber", 2, "Elongated echo chamber (hallway, stairwell)", 50, 15, 30),
  verb("hall", "Hall", 3, "Concert hall", 50, 30, 30),
  verb("echo_verb", "Echo", 4, "Line 6 original: echoes feeding a lush reverb", 50, 90, 30),
  verb("tile", "Tile", 5, "Tiled room (bathroom, shower)", 45, 5, 30),
  verb("cave", "Cave", 6, "Line 6 original: cavernous echo chamber", 45, 50, 30),
  verb("ducking", "Ducking", 7, "Hall with ducking", 55, 30, 40),
  verb("octo", "Octo", 8, "Line 6 original: octave-harmonised decay", 60, 40, 35),
  verb("spring", "Spring", 9, "Studio spring reverb", 50, 0, 30),
  verb("spring_63", "'63 Spring", 10, "1963 Fender tube spring reverb unit (6G15)", 55, 0, 35),
  model("particle_verb", "Particle Verb", CATEGORY.reverb, ENGINE.reverbFx, 11, "Line 6 original: modulated pad reverb",
        [percent("Dwell", 55), choice("Condition", ["Stable", "Critical", "Hazard"], 0), percent("Gain", 50), percent("Mix", 40)], { trails: true }),
];
