// The HD500X's eight wahs (Source/DSP/fx/Wah.h). Stereo: one filter per side. Knobs: Position (0 = heel, 100 = toe), Mix.
import { PI, clamp, f32, Smoothed, OnePole } from "./core.js";
import { CATEGORY, ENGINE, model, percent } from "./modeltypes.js";

// One wah: resonance at heel / toe (Hz) and the pot's taper, Q at heel / toe, gain at the peak (dB) at heel / toe,
// level of the flat part below the resonance (dB) at heel / toe (inductor wahs: resonant low-pass + band-pass;
// lowPassPath false = the inductor-less Colorsound's pure band-pass), and the input cap's bass cut (Hz).
const voice = (heelHz, toeHz, taper, heelQ, toeQ, heelPeakDb, toePeakDb, heelLowDb, toeLowDb, lowPassPath, couplingHz) =>
  ({ heelHz, toeHz, taper, heelQ, toeQ, heelPeakDb, toePeakDb, heelLowDb, toeLowDb, lowPassPath, couplingHz });

const VOICES = [
  voice(410, 2050, 1.0, 6.0, 4.2, 11.0, 13.0, -12.0, -17.0, true, 110),    // Fassel: Cry Baby Super / Jen (Fasel inductor)
  voice(300, 1350, 1.1, 4.0, 3.0, 10.5, 11.0, -7.5, -11.5, true, 60),      // Conductor: Maestro Boomerang
  voice(330, 1750, 0.9, 7.0, 4.8, 13.0, 13.0, -11.0, -16.0, true, 90),     // Throaty: RMC Real McCoy
  voice(280, 2400, 1.2, 2.2, 8.5, 6.0, 16.0, -100.0, -100.0, false, 120),  // Colorful: Colorsound (no inductor)
  voice(300, 2800, 1.0, 4.5, 4.5, 10.5, 10.5, -14.0, -14.0, true, 40),     // Vetta Wah: Line 6 original
  voice(440, 1600, 1.0, 5.5, 3.6, 11.0, 11.5, -11.0, -15.0, true, 100),    // Chrome: Vox V847
  voice(360, 2300, 1.35, 3.4, 2.6, 11.5, 12.5, -9.0, -13.0, true, 60),     // Chrome Custom: modded V847
  voice(350, 2200, 1.0, 7.5, 5.0, 12.0, 15.0, -14.0, -20.0, true, 150),    // Weeper: Arbiter / Dunlop Cry Baby
];

const TICK = 16; // samples between coefficient updates (interpolated in between)
const TINY = f32(1e-20);
const PERCENT = f32(0.01);

export class WahFx {
  constructor() {
    this.fs = 48000; this.variant = 0;
    this.position = new Smoothed(f32(0.5)); this.mix = new Smoothed(1);
    this.g = f32(0.1); this.k = f32(0.2); this.lowGain = 0; this.bandGain = 1;
    this.gTarget = f32(0.1); this.kTarget = f32(0.2); this.lowTarget = 0; this.bandTarget = 1;
    this.gStep = 0; this.kStep = 0; this.lowStep = 0; this.bandStep = 0;
    this.tickCount = 0; this.moving = false; this.modelChanged = true;
    this.ic1 = new Float32Array(2); this.ic2 = new Float32Array(2);
    this.coupling = [new OnePole(), new OnePole()];
  }

  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.position.reset(this.fs, 0.02);
    this.mix.reset(this.fs, 0.03);
    this.configure();
    this.reset();
  }

  reset() {
    this.position.setCurrentAndTarget(this.position.target);
    this.mix.setCurrentAndTarget(this.mix.target);

    this.computeTargets(this.position.target);
    this.g = this.gTarget; this.k = this.kTarget; this.lowGain = this.lowTarget; this.bandGain = this.bandTarget;
    this.gStep = this.kStep = this.lowStep = this.bandStep = 0;
    this.tickCount = 0;
    this.moving = this.modelChanged = false;

    for (let ch = 0; ch < 2; ++ch) {
      this.ic1[ch] = this.ic2[ch] = 0;
      this.coupling[ch].reset();
    }
  }

  setModel(variant) {
    this.variant = clamp(variant | 0, 0, VOICES.length - 1);
    this.configure();
  }

  setParameters(knobs) {
    this.position.setTarget(clamp(f32(f32(knobs[0]) * PERCENT), 0, 1));
    this.mix.setTarget(clamp(f32(f32(knobs[1]) * PERCENT), 0, 1));
  }

  process(left, right, n) {
    const mix = this.mix, couplingL = this.coupling[0], couplingR = this.coupling[1];
    let ic1L = this.ic1[0], ic2L = this.ic2[0], ic1R = this.ic1[1], ic2R = this.ic2[1];

    for (let pos = 0; pos < n;) {
      if (this.tickCount === 0) this.nextTick();

      const count = Math.min(this.tickCount, n - pos);
      const gStep = this.gStep, kStep = this.kStep, lowStep = this.lowStep, bandStep = this.bandStep;
      let g = this.g, k = this.k, lowGain = this.lowGain, bandGain = this.bandGain;

      for (let i = pos; i < pos + count; ++i) {
        g = f32(g + gStep); k = f32(k + kStep); lowGain = f32(lowGain + lowStep); bandGain = f32(bandGain + bandStep);
        const a1 = 1 / (1 + g * (g + k));
        const a2 = g * a1;
        const a3 = g * a2;
        const wetMix = mix.next();

        // state-variable filter (trapezoidal integrators): v1 = band-pass, v2 = low-pass
        let dry = left[i];
        let x = couplingL.highPass(dry + TINY) + TINY;
        let v3 = x - ic2L;
        let v1 = a1 * ic1L + a2 * v3;
        let v2 = ic2L + a2 * ic1L + a3 * v3;
        ic1L = 2 * v1 - ic1L;
        ic2L = 2 * v2 - ic2L;
        left[i] = dry + wetMix * (lowGain * v2 + bandGain * v1 - dry);

        dry = right[i];
        x = couplingR.highPass(dry + TINY) + TINY;
        v3 = x - ic2R;
        v1 = a1 * ic1R + a2 * v3;
        v2 = ic2R + a2 * ic1R + a3 * v3;
        ic1R = 2 * v1 - ic1R;
        ic2R = 2 * v2 - ic2R;
        right[i] = dry + wetMix * (lowGain * v2 + bandGain * v1 - dry);
      }

      this.g = g; this.k = k; this.lowGain = lowGain; this.bandGain = bandGain;
      pos += count;
      this.tickCount -= count;
    }

    this.ic1[0] = ic1L; this.ic2[0] = ic2L; this.ic1[1] = ic1R; this.ic2[1] = ic2R;
  }

  configure() {
    for (const c of this.coupling) c.setCutoff(this.fs, VOICES[this.variant].couplingHz);
    this.modelChanged = true;
  }

  /** Filter settings for a pedal position (0..1). */
  computeTargets(pedal) {
    const v = VOICES[this.variant], fs = this.fs;
    const t = Math.pow(pedal, v.taper);
    const hz = Math.min(v.heelHz * Math.pow(v.toeHz / v.heelHz, t), 0.45 * fs);
    const q = v.heelQ * Math.pow(v.toeQ / v.heelQ, t);
    const peak = Math.pow(10, (v.heelPeakDb + (v.toePeakDb - v.heelPeakDb) * t) / 20);
    const low = v.lowPassPath ? Math.pow(10, (v.heelLowDb + (v.toeLowDb - v.heelLowDb) * t) / 20) : 0;

    // at the resonance both outputs are Q times the input, 90 degrees apart: the band-pass share that gives the wanted peak
    const band = Math.sqrt(Math.max(0, (peak / q) * (peak / q) - low * low));

    this.gTarget = f32(Math.tan((PI * hz) / fs));
    this.kTarget = f32(1 / q);
    this.lowTarget = f32(low);
    this.bandTarget = f32(band);
  }

  nextTick() {
    this.tickCount = TICK;

    if (this.moving) { // land exactly on what the last ramp aimed at
      this.g = this.gTarget; this.k = this.kTarget; this.lowGain = this.lowTarget; this.bandGain = this.bandTarget;
      this.gStep = this.kStep = this.lowStep = this.bandStep = 0;
      this.moving = false;
    }

    if (this.position.isSmoothing() || this.modelChanged) {
      this.modelChanged = false;
      this.computeTargets(f32(this.position.skip(TICK)));
      this.gStep = f32((this.gTarget - this.g) * (1 / TICK));
      this.kStep = f32((this.kTarget - this.k) * (1 / TICK));
      this.lowStep = f32((this.lowTarget - this.lowGain) * (1 / TICK));
      this.bandStep = f32((this.bandTarget - this.bandGain) * (1 / TICK));
      this.moving = true;
    }
  }
}

const wah = (key, name, variant, basedOn) =>
  model(key, name, CATEGORY.wah, ENGINE.wahFx, variant, basedOn, [percent("Position", 50), percent("Mix", 100)]);

/** In the order of the variants. */
export const WAH_MODELS = [
  wah("fassel", "Fassel", 0, "Dunlop Cry Baby Super / Jen Super Cry Baby (Fasel inductor)"),
  wah("conductor", "Conductor", 1, "Maestro Boomerang"),
  wah("throaty", "Throaty", 2, "RMC Real McCoy 1"),
  wah("colorful", "Colorful", 3, "Colorsound Wah-Fuzz (wah section, inductor-less)"),
  wah("vetta_wah", "Vetta Wah", 4, "Line 6 original (Vetta II)"),
  wah("chrome", "Chrome", 5, "Vox V847"),
  wah("chrome_custom", "Chrome Custom", 6, "Modded Vox V847"),
  wah("weeper", "Weeper", 7, "Arbiter Cry Baby"),
];
