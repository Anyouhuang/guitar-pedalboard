// The acoustic guitar simulator (Source/DSP/fx/Acoustic.h): makes a solid-body electric sound like a steel-string
// acoustic, in the manner of the BOSS AC-2 / AC-3 pedals. Our own addition: the HD500X has none. Mono.
// Knobs: Type (Standard / Jumbo / Enhanced / Piezo), Body, Top, Level.
import { clamp, f32, dbToGain, PI, Biquad, OnePole, Smoothed } from "./core.js";
import { CATEGORY, ENGINE, model, percent, choice, levelDb } from "./modeltypes.js";

// The bands, fixed: three body resonances of a dreadnought and of a jumbo (air, top plate, back), the midrange a
// magnetic pickup has too much of, an under-saddle piezo's "quack", presence, air. [Hz, Q]
const BAND_SPECS = [[105, 3.5], [205, 4.5], [400, 5], [88, 3.5], [175, 4.5], [330, 5], [1200, 0.5], [1600, 1.6], [3200, 1.6], [9000, 1.2]];
const DREAD_AIR = 0, JUMBO_AIR = 3, MIDS = 6, QUACK = 7, PRESENCE = 8, AIR = 9, NUM_BANDS = 10;
const LOW_CUT_HZ = 75, TOP_HZ = 7000;

// One voicing per Type. Gains are what is added to the dry signal (0.8 = +5 dB at the band's centre).
// `body` is scaled by the Body knob, `top` and `air` by the Top knob (x 1 at 50 %, x 2 at 100 %).
const VOICES = [
  { body: [0.80, 0.70, 0.25], bigBody: false, mids: -0.62, quack: 0, presence: 0, top: 1.8, air: 0.6, snap: 1.0, trimDb: -3.0 },    // Standard
  { body: [1.10, 0.90, 0.30], bigBody: true, mids: -0.66, quack: 0, presence: 0, top: 1.0, air: 0.2, snap: 0.8, trimDb: -0.5 },     // Jumbo
  { body: [0.70, 0.60, 0.20], bigBody: false, mids: -0.62, quack: 0, presence: 0.60, top: 2.1, air: 0.8, snap: 2.0, trimDb: -6.0 }, // Enhanced
  { body: [0.30, 0.35, 0.15], bigBody: false, mids: -0.35, quack: 0.60, presence: 0, top: 2.3, air: 1.0, snap: 1.5, trimDb: -6.0 }, // Piezo
].map((v) => ({ body: v.body.map(f32), bigBody: v.bigBody, mids: f32(v.mids), quack: f32(v.quack), presence: f32(v.presence),
                top: f32(v.top), air: f32(v.air), snap: f32(v.snap), trimDb: f32(v.trimDb) }));

const FAST_ATTACK = 0.0003, FAST_RELEASE = 0.012, SLOW_ATTACK = 0.025, SLOW_RELEASE = 0.1, SNAP_SMOOTH = 0.0015;
const QUIETEST = f32(2e-4); // (-74 dB) below this the followers no longer count as a note starting
const TINY = 1e-20;         // keeps filters and followers out of the denormals in silence
const stepFor = (seconds, fs) => f32(1 - Math.exp(-1 / (seconds * fs)));

/** A band-pass with 0 dB at its centre (RBJ). Added to the dry signal it lifts that band, subtracted it scoops
    it, and the bands never cancel each other: so the knobs move gains only, never coefficients. */
class Band {
  constructor() { this.b0 = 0; this.a1 = 0; this.a2 = 0; this.z1 = 0; this.z2 = 0; }
  set(fs, hz, q) {
    const w0 = (2 * PI * clamp(hz, 1, 0.45 * fs)) / fs;
    const alpha = Math.sin(w0) / (2 * q), a0 = 1 + alpha;
    this.b0 = f32(alpha / a0);
    this.a1 = f32((-2 * Math.cos(w0)) / a0);
    this.a2 = f32((1 - alpha) / a0);
  }
  reset() { this.z1 = this.z2 = 0; }
  process(x) {
    const y = this.b0 * x + this.z1;
    this.z1 = this.z2 - this.a1 * y;
    this.z2 = -this.b0 * x - this.a2 * y;
    return y;
  }
}

export class AcousticFx {
  constructor() {
    this.lowCut = new Biquad();
    this.band = BAND_SPECS.map(() => new Band());
    this.gain = BAND_SPECS.map(() => new Smoothed(0));
    this.topFilter = new OnePole();
    this.topGain = new Smoothed(0); this.snapGain = new Smoothed(0); this.level = new Smoothed(1);
    this.fastUp = 0; this.fastDown = 0; this.slowUp = 0; this.slowDown = 0; this.snapStep = 0;
    this.fast = 0; this.slow = 0; this.snapNow = 0;
  }
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.lowCut.setHighPass(sampleRate, LOW_CUT_HZ, 0.7071);
    for (let b = 0; b < NUM_BANDS; ++b) {
      this.band[b].set(sampleRate, BAND_SPECS[b][0], BAND_SPECS[b][1]);
      this.gain[b].reset(sampleRate, 0.04);
    }
    this.topFilter.setCutoff(sampleRate, TOP_HZ);
    this.topGain.reset(sampleRate, 0.04); this.snapGain.reset(sampleRate, 0.04); this.level.reset(sampleRate, 0.04);

    this.fastUp = stepFor(FAST_ATTACK, sampleRate);
    this.fastDown = stepFor(FAST_RELEASE, sampleRate);
    this.slowUp = stepFor(SLOW_ATTACK, sampleRate);
    this.slowDown = stepFor(SLOW_RELEASE, sampleRate);
    this.snapStep = stepFor(SNAP_SMOOTH, sampleRate);
    this.reset();
  }
  reset() {
    this.lowCut.reset();
    this.topFilter.reset();
    for (let b = 0; b < NUM_BANDS; ++b) {
      this.band[b].reset();
      this.gain[b].setCurrentAndTarget(this.gain[b].target);
    }
    this.topGain.setCurrentAndTarget(this.topGain.target);
    this.snapGain.setCurrentAndTarget(this.snapGain.target);
    this.level.setCurrentAndTarget(this.level.target);
    this.fast = this.slow = this.snapNow = 0;
  }
  setModel(variant) {} // one model
  setParameters(k) {
    const voice = VOICES[clamp(Math.round(k[0]), 0, VOICES.length - 1)];
    const body = f32(2 * clamp(f32(k[1] / 100), 0, 1));
    const top = f32(2 * clamp(f32(k[2] / 100), 0, 1));
    for (let i = 0; i < 3; ++i) {
      this.gain[DREAD_AIR + i].setTarget(voice.bigBody ? 0 : f32(voice.body[i] * body));
      this.gain[JUMBO_AIR + i].setTarget(voice.bigBody ? f32(voice.body[i] * body) : 0);
    }
    this.gain[MIDS].setTarget(voice.mids);
    this.gain[QUACK].setTarget(voice.quack);
    this.gain[PRESENCE].setTarget(voice.presence);
    this.gain[AIR].setTarget(f32(voice.air * top));

    this.topGain.setTarget(f32(voice.top * top));
    this.snapGain.setTarget(voice.snap);
    this.level.setTarget(f32(dbToGain(f32(k[3] + voice.trimDb))));
  }
  process(left, right, n) {
    const band = this.band, gain = this.gain, lowCut = this.lowCut, topFilter = this.topFilter;
    const topGain = this.topGain, snapGain = this.snapGain, level = this.level;
    const fastUp = this.fastUp, fastDown = this.fastDown, slowUp = this.slowUp, slowDown = this.slowDown, snapStep = this.snapStep;
    let fast = this.fast, slow = this.slow, snapNow = this.snapNow;

    for (let i = 0; i < n; ++i) {
      const x = lowCut.process(0.5 * (left[i] + right[i]) + TINY) + TINY;

      // the pick attack: for the few hundredths of a second in which the fast follower is ahead of the slow one
      const a = Math.abs(x) + TINY;
      fast += (a > fast ? fastUp : fastDown) * (a - fast);
      slow += (a > slow ? slowUp : slowDown) * (a - slow);
      const ahead = clamp((2 * (fast - slow)) / (fast + slow + QUIETEST) - 0.25, 0, 1);
      snapNow += snapStep * (ahead + TINY - snapNow);

      let y = x;
      for (let b = 0; b < NUM_BANDS; ++b) y += gain[b].next() * band[b].process(x);
      y += topGain.next() * (1 + snapGain.next() * snapNow) * topFilter.highPass(x);

      left[i] = right[i] = y * level.next();
    }
    this.fast = fast; this.slow = slow; this.snapNow = snapNow;
  }
}

/** In the order of the variants. */
export const ACOUSTIC_MODELS = [
  model("acoustic_sim", "Acoustic Sim", CATEGORY.eq, ENGINE.acousticFx, 0,
    "Acoustic simulator pedal (BOSS AC-3 style); our own addition, not an HD500X model",
    [choice("Type", ["Standard", "Jumbo", "Enhanced", "Piezo"], 0), percent("Body", 50), percent("Top", 50), levelDb()]),
];
