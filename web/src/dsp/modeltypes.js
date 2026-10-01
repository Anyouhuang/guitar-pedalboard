// Model list types (Source/DSP/ModelTypes.h): categories, engines, knob units and the knob constructors
// every effect file uses to describe its models. Numbering matches the C++ enums: only ever append.
import { clamp } from "./core.js";

export const NUM_SLOTS = 8;
export const MAX_KNOBS = 8;
export const CATEGORY = { none: 0, dynamics: 1, distortion: 2, modulation: 3, delay: 4, reverb: 5, filter: 6, pitch: 7, eq: 8, wah: 9, volume: 10,
                          amp: 11, cab: 12 }; // amp / cab: the amp block's own lists, not offered in the FX slots
export const CATEGORY_NAMES = ["Empty", "Dynamics", "Distortion", "Modulation", "Delay", "Reverb", "Filter", "Pitch", "Preamp+EQ", "Wah", "Volume/Pan", "Amp", "Cab"];
/** The order the categories are listed in, as on the HD500X. */
export const CATEGORY_ORDER = [0, 1, 2, 3, 6, 7, 8, 4, 5, 10, 9];
export const ENGINE = { none: 0, gate: 1, distortion: 2, modulation: 3, delay: 4, reverb: 5,
                        dynamicsFx: 6, modFx: 7, filterFx: 8, pitchFx: 9, eqFx: 10, delayFx: 11, reverbFx: 12, wahFx: 13, volumeFx: 14,
                        ampFx: 15, cabFx: 16 };
export const UNIT = { knob: 0, percent: 1, db: 2, ms: 3, hz: 4, choice: 5, semitones: 6, freq: 7 };

// Knob constructors, same names and arguments as the C++ ones
export const knobSpec = (name, min, max, def, centre, step, unit, choices) => ({ name, min, max, def, centre, step, unit, choices: choices || [] });
export const knob10 = (name, def) => knobSpec(name, 0, 10, def, 0, 0.1, UNIT.knob);
export const percent = (name, def, max = 100) => knobSpec(name, 0, max, def, 0, 1, UNIT.percent);
export const decibels = (name, min, max, def, step = 0.1) => knobSpec(name, min, max, def, 0, step, UNIT.db);
export const levelDb = (def = 0) => decibels("Level", -30, 12, def);
export const millis = (name, min, max, def, centre) => knobSpec(name, min, max, def, centre, 1, UNIT.ms);
export const hertz = (name, min, max, def, centre) => knobSpec(name, min, max, def, centre, 0.01, UNIT.hz);
export const freq = (name, min, max, def, centre) => knobSpec(name, min, max, def, centre, 1, UNIT.freq);
export const semitones = (name, min, max, def, step = 1) => knobSpec(name, min, max, def, 0, step, UNIT.semitones);
export const choice = (name, choices, def) => knobSpec(name, 0, choices.length - 1, def, 0, 1, UNIT.choice, choices);

export const DELAY_NOTE_NAMES = ["ms", "1/4", "1/8.", "1/8", "1/8T", "1/16"];
export const DELAY_NOTE_BEATS = [0, 1, 0.75, 0.5, 1 / 3, 0.25];
export const REVERB_NOTE_NAMES = ["ms", "1/32", "1/16", "1/8", "1/4"];
export const REVERB_NOTE_BEATS = [0, 0.125, 0.25, 0.5, 1];

/** One entry of a model list. `extra`: timeKnob, noteKnob, noteBeats, timeKnob2, noteKnob2, stereo, trails. */
export const model = (key, name, category, engine, variant, basedOn, knobs, extra = {}) =>
  ({ key, name, category, engine, variant, basedOn, knobs, timeKnob: -1, noteKnob: -1, noteBeats: null,
     timeKnob2: -1, noteKnob2: -1, stereo: false, trails: false, ...extra });

// knob travel (0..1) <-> value, juce::NormalisableRange style
export function knobSkew(spec) { return spec.centre > 0 ? Math.log(0.5) / Math.log((spec.centre - spec.min) / (spec.max - spec.min)) : 1; }
export function knobFromNorm(spec, n) {
  let p = clamp(n, 0, 1);
  if (spec.centre > 0 && p > 0) p = Math.exp(Math.log(p) / knobSkew(spec));
  let v = spec.min + (spec.max - spec.min) * p;
  if (spec.step > 0) v = spec.min + spec.step * Math.round((v - spec.min) / spec.step);
  return clamp(v, spec.min, spec.max);
}
export function knobToNorm(spec, v) {
  const p = clamp((v - spec.min) / (spec.max - spec.min), 0, 1);
  return spec.centre > 0 ? Math.pow(p, knobSkew(spec)) : p;
}
export function knobText(spec, v) {
  switch (spec.unit) {
    case UNIT.percent: return `${Math.round(v)} %`;
    case UNIT.db: return `${v.toFixed(1)} dB`;
    case UNIT.ms: return `${Math.round(v)} ms`;
    case UNIT.hz: return `${v.toFixed(2)} Hz`;
    case UNIT.choice: return spec.choices[clamp(Math.round(v), 0, spec.choices.length - 1)];
    case UNIT.semitones: return `${v > 0 ? "+" : ""}${Number.isInteger(spec.step) ? Math.round(v) : v.toFixed(1)} st`;
    case UNIT.freq: return v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${Math.round(v)} Hz`;
    default: return v.toFixed(1);
  }
}
