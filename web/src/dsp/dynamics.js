// The HD500X's Dynamics models without the Noise Gate (Source/DSP/fx/Dynamics.h). All mono.
//   Hard Gate: Open Threshold, Close Threshold, Hold, Decay   |   Tube Comp: Threshold, Level
//   Red Comp / Blue Comp / Blue Comp Treb: Sustain, Level     |   Vetta Comp: Sensitivity, Level
//   Vetta Juice: Amount, Level                                |   Boost Comp: Drive, Bass, Comp, Treble, Output
import { clamp, f32, dbToGain, Smoothed, Biquad, OnePole, DcBlocker } from "./core.js";
import { CATEGORY, ENGINE, model, percent, decibels, millis } from "./modeltypes.js";

const HARD_GATE = 0, TUBE_COMP = 1, RED_COMP = 2, BLUE_COMP = 3, BLUE_COMP_TREB = 4, VETTA_COMP = 5, VETTA_JUICE = 6, BOOST_COMP = 7;

/** Linear up to `knee`, then bends over smoothly towards `ceiling`. */
function softClip(v, knee, ceiling) {
  const a = Math.abs(v);
  if (a <= knee) return v;
  const range = ceiling - knee;
  const s = knee + range * Math.tanh((a - knee) / range);
  return v < 0 ? -s : s;
}
/** Per-sample step of a one-pole follower with the time constant `seconds`. */
const stepFor = (seconds, fs) => f32(1 - Math.exp(-1 / (seconds * fs)));

// Tube Comp (LA-2A)
const TUBE_ATTACK = 0.010, TUBE_RELEASE = 0.060, TUBE_LAG = 0.004, TUBE_MEM_CHARGE = 0.8, TUBE_MEM_RELEASE = 2.5;
const TUBE_MEM_SHARE = f32(0.6), TUBE_DETECTOR = f32(0.837), TUBE_REFERENCE = 0.15;
// Red Comp (Dyna Comp)
const RED_THRESHOLD = f32(0.25), RED_STIFFNESS = f32(8), RED_LEVEL = f32(0.62);
const RED_ATTACK = 0.005, RED_RELEASE = 0.45, RED_TONE = 5500, RED_MAX_GAIN_DB = 34;
// Blue Comp (CS-1)
const BLUE_THRESHOLD = f32(0.22), BLUE_DETECTOR = f32(0.934), BLUE_LEVEL = f32(0.56);
const BLUE_ATTACK = 0.005, BLUE_RELEASE = 0.15, BLUE_LAG = 0.003, BLUE_MAX_GAIN_DB = 26;
// Vetta Comp / Vetta Juice
const VETTA_RATIO = f32(2.35), VETTA_KNEE = 6, JUICE_THRESHOLD_DB = -44, JUICE_KNEE = 12, JUICE_MIN_SLOPE = f32(0.1);
const VETTA_DETECTOR = 0.010, VETTA_ATTACK = 0.008, VETTA_RELEASE = 0.15, JUICE_ATTACK = 0.003, JUICE_RELEASE = 0.25;
// Boost Comp
const BOOST_THRESHOLD = f32(0.30), BOOST_STIFFNESS = f32(8), BOOST_COMP_GAIN_DB = 20, BOOST_DRIVE_DB = 26, BOOST_EQ_DB = 12;
const BOOST_BASS_HZ = 120, BOOST_TREBLE_HZ = 3000;

const RED_SIDE = f32(RED_STIFFNESS / RED_THRESHOLD), BLUE_SIDE = f32(1 / f32(BLUE_DETECTOR * BLUE_THRESHOLD));
const C06 = f32(0.6), C07 = f32(0.7), C14 = f32(1.4), C16 = f32(1.6), C006 = f32(0.06);
const DB_TO_NEPER = f32(0.115129255), EQ_STEP = f32(0.02), PERCENT = f32(0.01);
// added to the signal so that followers and filters settle at 1e-20 in silence, not in the (slow) denormals
const TINY = f32(1e-20), FLUSH_BELOW = f32(1e-15);

export class DynamicsFx {
  constructor() {
    this.fs = 48000; this.variant = HARD_GATE;
    this.level = new Smoothed(1); this.pre = new Smoothed(1); this.side = new Smoothed(0); this.slope = new Smoothed(0);
    this.makeup = new Smoothed(1); this.drive = new Smoothed(1); this.bass = new Smoothed(1); this.treble = new Smoothed(1);
    this.smoothers = [this.level, this.pre, this.side, this.slope, this.makeup, this.drive, this.bass, this.treble];
    this.cachedKnob = -1e9; this.sideTarget = 0; this.makeupTarget = 1;
    this.attack = 0; this.release = 0; this.lagStep = 0; this.memCharge = 0; this.memRelease = 0; this.knee = 6;
    this.env = 0; this.mem = 0; this.lag = 0; this.cap = 0; this.gainReduction = 0;
    this.openThreshold = f32(0.01); this.closeThreshold = f32(0.005); this.decayFactor = f32(0.999); this.openStep = f32(0.02);
    this.detectorRelease = f32(0.01); this.gateGain = 0;
    this.holdSamples = 0; this.holdCount = 0; this.detectorHold = 0; this.detectorCount = 0; this.gateOpen = false;
    this.dc = new DcBlocker();
    this.redToneFilter = new OnePole(); this.bassFilter = new OnePole(); this.trebleFilter = new OnePole();
    this.blueTrebleShelf = new Biquad();
  }

  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    for (const s of this.smoothers) s.reset(this.fs, 0.03);

    this.dc.prepare(this.fs);
    this.redToneFilter.setCutoff(this.fs, RED_TONE);
    this.blueTrebleShelf.setHighShelf(this.fs, 3000, 7);
    this.bassFilter.setCutoff(this.fs, BOOST_BASS_HZ);
    this.trebleFilter.setCutoff(this.fs, BOOST_TREBLE_HZ);

    this.detectorHold = Math.floor(0.010 * this.fs);
    this.detectorRelease = stepFor(0.002, this.fs);
    this.openStep = f32(1 / (0.001 * this.fs));

    this.configure();
    this.reset();
  }

  reset() {
    for (const s of this.smoothers) s.setCurrentAndTarget(s.target);
    this.env = this.mem = this.lag = this.cap = this.gainReduction = 0;
    this.gateGain = 0;
    this.gateOpen = false;
    this.holdCount = this.detectorCount = 0;
    this.dc.reset();
    this.redToneFilter.reset();
    this.blueTrebleShelf.reset();
    this.bassFilter.reset();
    this.trebleFilter.reset();
  }

  setModel(variant) {
    this.variant = clamp(variant | 0, 0, 7);
    this.configure();
  }

  setParameters(k) {
    const k0 = f32(k[0]), k1 = f32(k[1]);
    switch (this.variant) {
      case HARD_GATE: {
        this.openThreshold = f32(dbToGain(k0));
        this.closeThreshold = f32(dbToGain(Math.min(k0, k1))); // never above the open threshold
        this.holdSamples = Math.round(f32(k[2]) * 0.001 * this.fs);
        // Decay: the time the gain takes to fall 60 dB once the hold has run out
        this.decayFactor = f32(Math.exp(-6.907755 / (Math.max(1, f32(k[3])) * 0.001 * this.fs)));
        break;
      }
      case TUBE_COMP: {
        if (k0 !== this.cachedKnob) {
          this.cachedKnob = k0;
          // gain = 1 / (1 + (y / T)^2) with y the compressed signal: y (1 + (y/T)^2) = x, 3:1 far above T
          const threshold = Math.pow(10, k0 / 20);
          this.sideTarget = f32(1 / (TUBE_DETECTOR * threshold));
          // make-up tied to the threshold: whatever the cell takes off a signal at the reference level
          const invT2 = 1 / (threshold * threshold);
          let y = Math.min(TUBE_REFERENCE, Math.cbrt(TUBE_REFERENCE / invT2));
          for (let i = 0; i < 6; ++i) y -= (y + invT2 * y * y * y - TUBE_REFERENCE) / (1 + 3 * invT2 * y * y);
          this.makeupTarget = f32(TUBE_REFERENCE / y);
        }
        this.side.setTarget(this.sideTarget);
        this.makeup.setTarget(this.makeupTarget);
        this.level.setTarget(f32(dbToGain(k1)));
        break;
      }
      case RED_COMP: {
        this.pre.setTarget(f32(dbToGain(f32(f32(RED_MAX_GAIN_DB * k0) * PERCENT))));
        this.level.setTarget(f32(f32(dbToGain(k1)) * RED_LEVEL));
        break;
      }
      case BLUE_COMP:
      case BLUE_COMP_TREB: {
        this.pre.setTarget(f32(dbToGain(f32(f32(BLUE_MAX_GAIN_DB * k0) * PERCENT))));
        this.level.setTarget(f32(f32(dbToGain(k1)) * BLUE_LEVEL));
        break;
      }
      case VETTA_COMP: {
        this.side.setTarget(f32(f32(-50 * k0) * PERCENT)); // Sensitivity: threshold 0 ... -50 dBFS
        this.slope.setTarget(f32(1 - f32(1 / VETTA_RATIO)));
        this.level.setTarget(f32(dbToGain(k1)));
        break;
      }
      case VETTA_JUICE: {
        this.side.setTarget(JUICE_THRESHOLD_DB);
        this.slope.setTarget(f32(f32(f32(1 - JUICE_MIN_SLOPE) * k0) * PERCENT)); // Amount: 1:1 ... 10:1
        this.level.setTarget(f32(dbToGain(k1)));
        break;
      }
      default: { // Boost Comp
        const amount = f32(f32(k[2]) * PERCENT);
        this.drive.setTarget(f32(dbToGain(f32(f32(BOOST_DRIVE_DB * k0) * PERCENT))));
        this.bass.setTarget(f32(dbToGain(f32(f32(BOOST_EQ_DB * f32(k1 - 50)) * EQ_STEP))));
        this.pre.setTarget(f32(dbToGain(f32(BOOST_COMP_GAIN_DB * amount))));
        this.side.setTarget(f32(f32(amount * BOOST_STIFFNESS) / BOOST_THRESHOLD));
        this.treble.setTarget(f32(dbToGain(f32(f32(BOOST_EQ_DB * f32(f32(k[3]) - 50)) * EQ_STEP))));
        this.level.setTarget(f32(dbToGain(f32(k[4]))));
        break;
      }
    }
  }

  process(left, right, n) {
    switch (this.variant) {
      case HARD_GATE: this.processGate(left, right, n); break;
      case TUBE_COMP: this.processTube(left, right, n); break;
      case RED_COMP: this.processRed(left, right, n); break;
      case BLUE_COMP:
      case BLUE_COMP_TREB: this.processBlue(left, right, n); break;
      case VETTA_COMP:
      case VETTA_JUICE: this.processVetta(left, right, n); break;
      default: this.processBoost(left, right, n); break;
    }
  }

  /** Time constants of the selected model. */
  configure() {
    const fs = this.fs;
    this.cachedKnob = -1e9;
    switch (this.variant) {
      case TUBE_COMP:
        this.attack = stepFor(TUBE_ATTACK, fs); this.release = stepFor(TUBE_RELEASE, fs);
        this.lagStep = stepFor(TUBE_LAG, fs);
        this.memCharge = stepFor(TUBE_MEM_CHARGE, fs); this.memRelease = stepFor(TUBE_MEM_RELEASE, fs);
        break;
      case RED_COMP:
      case BOOST_COMP:
        this.attack = stepFor(RED_ATTACK, fs); this.release = stepFor(RED_RELEASE, fs);
        break;
      case BLUE_COMP:
      case BLUE_COMP_TREB:
        this.attack = stepFor(BLUE_ATTACK, fs); this.release = stepFor(BLUE_RELEASE, fs);
        this.lagStep = stepFor(BLUE_LAG, fs);
        break;
      case VETTA_COMP:
        this.attack = stepFor(VETTA_ATTACK, fs); this.release = stepFor(VETTA_RELEASE, fs);
        this.lagStep = stepFor(VETTA_DETECTOR, fs); this.knee = VETTA_KNEE;
        break;
      case VETTA_JUICE:
        this.attack = stepFor(JUICE_ATTACK, fs); this.release = stepFor(JUICE_RELEASE, fs);
        this.lagStep = stepFor(VETTA_DETECTOR, fs); this.knee = JUICE_KNEE;
        break;
      default: break;
    }
  }

  // Hard Gate: opens above one threshold, closes below another, waits for the Hold time, then fades out over Decay.
  processGate(left, right, n) {
    const openThreshold = this.openThreshold, closeThreshold = this.closeThreshold, holdSamples = this.holdSamples;
    const detectorHold = this.detectorHold, detectorRelease = this.detectorRelease, openStep = this.openStep, decayFactor = this.decayFactor;
    let env = this.env, detectorCount = this.detectorCount, gateOpen = this.gateOpen, holdCount = this.holdCount, gateGain = this.gateGain;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]);
      const a = Math.abs(x) + TINY;

      // level detector: keeps each peak for 10 ms, then lets go quickly
      if (a >= env) { env = a; detectorCount = detectorHold; }
      else if (detectorCount > 0) --detectorCount;
      else env -= env * detectorRelease;

      if (env >= openThreshold) { gateOpen = true; holdCount = holdSamples; }
      else if (gateOpen) {
        if (env >= closeThreshold) holdCount = holdSamples;
        else if (holdCount > 0) --holdCount;
        else gateOpen = false;
      }

      if (gateOpen) gateGain = Math.min(1, gateGain + openStep); // opens within 1 ms
      else {
        gateGain *= decayFactor;
        if (gateGain < 1e-5) gateGain = 0;
      }
      left[i] = right[i] = x * gateGain;
    }
    this.env = env; this.detectorCount = detectorCount; this.gateOpen = gateOpen; this.holdCount = holdCount; this.gateGain = gateGain;
  }

  // Tube Comp (LA-2A): optical cell in a feedback loop, two-stage release, make-up tied to the threshold, tube output.
  processTube(left, right, n) {
    const side = this.side, makeup = this.makeup, level = this.level, dc = this.dc;
    const attack = this.attack, release = this.release, memCharge = this.memCharge, memRelease = this.memRelease, lagStep = this.lagStep;
    let env = this.env, mem = this.mem, lag = this.lag;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]) + TINY;
      const y = x / (1 + lag * lag);

      const u = side.next() * Math.abs(y);
      env += (u - env) * (u > env ? attack : release);

      const memTarget = TUBE_MEM_SHARE * env;
      mem += (memTarget - mem) * (memTarget > mem ? memCharge : memRelease);

      lag += (Math.max(env, mem) - lag) * lagStep; // the photoresistor lags behind the light

      let v = softClip(y * makeup.next(), C07, C14);
      v -= C006 * v * v;
      left[i] = right[i] = dc.process(v) * level.next();
    }
    this.env = env; this.mem = mem; this.lag = lag;
  }

  // Red Comp (Dyna Comp): OTA pulled down by the rectified output; fast attack, long release, rolled-off top.
  processRed(left, right, n) {
    const pre = this.pre, level = this.level, tone = this.redToneFilter, attack = this.attack, release = this.release;
    let cap = this.cap;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]) + TINY;
      const y = softClip(x * pre.next() * Math.exp(-cap), C06, 1); // runs into the rails on hard attacks

      const over = (Math.abs(y) - RED_THRESHOLD) * RED_SIDE;
      if (over > cap) cap += (over - cap) * attack;
      else cap -= cap * release;

      left[i] = right[i] = tone.lowPass(y) * level.next();
    }
    this.cap = cap < FLUSH_BELOW ? 0 : cap;
  }

  // Blue Comp / Blue Comp Treb (CS-1): photocoupler feedback loop, about 4:1, quicker release; treble switch = lift above 3 kHz.
  processBlue(left, right, n) {
    const pre = this.pre, level = this.level, shelf = this.blueTrebleShelf, attack = this.attack, release = this.release, lagStep = this.lagStep;
    const trebleSwitch = this.variant === BLUE_COMP_TREB;
    let env = this.env, lag = this.lag;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]) + TINY;
      const y = softClip(x * pre.next() / (1 + lag * lag * lag), C06, 1);

      const u = Math.abs(y) * BLUE_SIDE;
      env += (u - env) * (u > env ? attack : release);
      lag += (env - lag) * lagStep;

      left[i] = right[i] = (trebleSwitch ? shelf.process(y) : y) * level.next();
    }
    this.env = env; this.lag = lag;
  }

  // Vetta Comp (fixed 2.35:1, Sensitivity = threshold) and Vetta Juice (fixed threshold, Amount = ratio): feed-forward.
  processVetta(left, right, n) {
    const side = this.side, slope = this.slope, level = this.level, attack = this.attack, release = this.release, lagStep = this.lagStep;
    const knee = this.knee, halfKnee = 0.5 * knee;
    let env = this.env, gainReduction = this.gainReduction;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]) + TINY;
      const a = Math.abs(x);
      env = a > env ? a : env - env * lagStep; // peak reading

      const over = 20 * Math.log10(Math.max(env, 1e-6)) - side.next();
      let above = 0;
      if (over >= halfKnee) above = over;
      else if (over > -halfKnee) above = (over + halfKnee) * (over + halfKnee) / (2 * knee);

      const target = above * slope.next();
      gainReduction += (target - gainReduction) * (target > gainReduction ? attack : release);

      left[i] = right[i] = softClip(x * level.next() * Math.exp(-DB_TO_NEPER * gainReduction), 1, 2);
    }
    this.env = env; this.gainReduction = gainReduction < FLUSH_BELOW ? 0 : gainReduction;
  }

  // Boost Comp (Micro Amp): Dyna-style squash (Comp), clean gain of up to 26 dB (Drive), Bass / Treble shelves.
  processBoost(left, right, n) {
    const pre = this.pre, side = this.side, drive = this.drive, bass = this.bass, treble = this.treble, level = this.level;
    const bassFilter = this.bassFilter, trebleFilter = this.trebleFilter, attack = this.attack, release = this.release;
    let cap = this.cap;
    for (let i = 0; i < n; ++i) {
      const x = 0.5 * (left[i] + right[i]) + TINY;
      let y = x * pre.next() * Math.exp(-cap);

      const over = (Math.abs(y) - BOOST_THRESHOLD) * side.next();
      if (over > cap) cap += (over - cap) * attack;
      else cap -= cap * release;

      y *= drive.next();
      y += (bass.next() - 1) * bassFilter.lowPass(y);
      y += (treble.next() - 1) * trebleFilter.highPass(y);

      left[i] = right[i] = softClip(y, 1, C16) * level.next();
    }
    this.cap = cap < FLUSH_BELOW ? 0 : cap;
  }
}

/** In the order of the variants. */
export const DYNAMICS_MODELS = [
  model("hard_gate", "Hard Gate", CATEGORY.dynamics, ENGINE.dynamicsFx, HARD_GATE, "Line 6 original: gate with separate open / close thresholds, hold and decay",
    [decibels("Open Threshold", -90, 0, -45, 0.5), decibels("Close Threshold", -90, 0, -55, 0.5), millis("Hold", 0, 1000, 50, 150), millis("Decay", 1, 2000, 80, 200)]),
  model("tube_comp", "Tube Comp", CATEGORY.dynamics, ENGINE.dynamicsFx, TUBE_COMP, "Teletronix LA-2A",
    [decibels("Threshold", -40, 0, -20, 0.5), decibels("Level", -30, 12, 0)]),
  model("red_comp", "Red Comp", CATEGORY.dynamics, ENGINE.dynamicsFx, RED_COMP, "MXR Dyna Comp",
    [percent("Sustain", 50), decibels("Level", -30, 12, 0)]),
  model("blue_comp", "Blue Comp", CATEGORY.dynamics, ENGINE.dynamicsFx, BLUE_COMP, "Boss CS-1 Compression Sustainer, treble switch off",
    [percent("Sustain", 50), decibels("Level", -30, 12, 0)]),
  model("blue_comp_treb", "Blue Comp Treb", CATEGORY.dynamics, ENGINE.dynamicsFx, BLUE_COMP_TREB, "Boss CS-1 Compression Sustainer, treble switch on",
    [percent("Sustain", 50), decibels("Level", -30, 12, 0)]),
  model("vetta_comp", "Vetta Comp", CATEGORY.dynamics, ENGINE.dynamicsFx, VETTA_COMP, "Line 6 Vetta II compressor (fixed 2.35:1)",
    [percent("Sensitivity", 50), decibels("Level", -30, 12, 5)]),
  model("vetta_juice", "Vetta Juice", CATEGORY.dynamics, ENGINE.dynamicsFx, VETTA_JUICE, "Line 6 Vetta II compressor (variable ratio, 30 dB of gain)",
    [percent("Amount", 50), decibels("Level", 0, 30, 13)]),
  model("boost_comp", "Boost Comp", CATEGORY.dynamics, ENGINE.dynamicsFx, BOOST_COMP, "Inspired by the MXR Micro Amp, plus Dyna-style compression",
    [percent("Drive", 20), percent("Bass", 50), percent("Comp", 25), percent("Treble", 50), decibels("Output", -30, 12, -5)]),
];
