// The model list and the signal chain (Source/DSP/Models.h, FxChain.*): eight FX slots plus the amp/cab block.
import { clamp, f32, dbToGain, Smoothed } from "./core.js";
import { NoiseGate, Distortion, Equalizer, defaultEq, Modulation, Delay, ReverbFx } from "./engines.js";
import { HumFilter, Denoiser } from "./noise.js";
import { NUM_SLOTS, MAX_KNOBS, CATEGORY, ENGINE, UNIT, model, knobSpec, percent, decibels, millis, hertz, choice,
         DELAY_NOTE_NAMES, DELAY_NOTE_BEATS, REVERB_NOTE_NAMES, REVERB_NOTE_BEATS } from "./modeltypes.js";
import { DynamicsFx, DYNAMICS_MODELS } from "./dynamics.js";
import { ModFx, MOD_MODELS } from "./mod.js";
import { FilterFx, FILTER_MODELS } from "./filter.js";
import { PitchFx, PITCH_MODELS } from "./pitch.js";
import { EqFx, EQ_MODELS } from "./eq.js";
import { DelayFx, DELAY_MODELS } from "./delayfx.js";
import { VerbFx, VERB_MODELS } from "./verb.js";
import { WahFx, WAH_MODELS } from "./wah.js";
import { VolumeFx, VOLUME_MODELS } from "./volume.js";
import { AmpFx, AMP_MODELS } from "./amp.js";
import { CabFx, CAB_MODELS } from "./cab.js";

/** Every model, in a fixed order (append only). Same list as the plugin; compare.mjs checks it. */
export const MODELS = (() => {
  const m = [];
  const add = (key, name, category, engine, variant, basedOn, knobs, extra = {}) =>
    m.push(model(key, name, category, engine, variant, basedOn, knobs, extra));
  add("empty", "Empty", CATEGORY.none, ENGINE.none, 0, "", []);
  add("noise_gate", "Noise Gate", CATEGORY.dynamics, ENGINE.gate, 0, "Noise suppressor with hysteresis",
    [decibels("Threshold", -90, -20, -65, 0.5), millis("Decay", 5, 500, 60, 80)]);

  const drive = (key, name, variant, basedOn, third, driveDefault) =>
    add(key, name, CATEGORY.distortion, ENGINE.distortion, variant, basedOn,
      [percent("Drive", driveDefault), percent("Bass", 50), percent(third, 50), percent("Treble", 50), decibels("Output", -30, 12, 0)]);
  drive("tube_drive", "Tube Drive", 0, "Chandler Tube Driver", "Mid", 50);
  drive("screamer", "Screamer", 1, "Ibanez TS808 Tube Screamer", "Tone", 50);
  drive("overdrive", "Overdrive", 2, "DOD Overdrive/Preamp 250", "Mid", 50);
  drive("classic_dist", "Classic Dist", 3, "Pro Co RAT", "Filter", 50);
  drive("heavy_dist", "Heavy Dist", 4, "BOSS MT-2 Metal Zone", "Mid", 60);
  drive("color_drive", "Color Drive", 5, "Colorsound Overdriver", "Mid", 50);
  drive("buzz_saw", "Buzz Saw", 6, "Maestro Fuzz-Tone FZ-1", "Mid", 60);
  drive("facial_fuzz", "Facial Fuzz", 7, "Arbiter Fuzz Face", "Mid", 60);
  drive("jumbo_fuzz", "Jumbo Fuzz", 8, "Vox Tone Bender", "Mid", 60);
  drive("fuzz_pi", "Fuzz Pi", 9, "Electro-Harmonix Big Muff Pi", "Mid", 60);
  add("jet_fuzz", "Jet Fuzz", CATEGORY.distortion, ENGINE.distortion, 10, "Roland AP-7 Jet Phaser",
    [percent("Drive", 60), percent("Fdbk", 50), percent("Tone", 50), hertz("Speed", 0.05, 8, 0.4, 1), decibels("Output", -30, 12, 0)]);
  drive("line6_drive", "Line 6 Drive", 11, "Line 6 original: Mid morphs '70s fuzz > modern high gain > Tone Bender grit", "Mid", 50);
  drive("line6_dist", "Line 6 Distortion", 12, "Line 6 original: massive, over-the-top gain", "Mid", 60);
  drive("sub_oct_fuzz", "Sub Octave Fuzz", 13, "PAiA Roctave Divider", "Sub", 60);
  drive("octave_fuzz", "Octave Fuzz", 14, "Tycobrahe Octavia", "Mid", 60);

  const mod = (key, name, variant, basedOn, third) =>
    add(key, name, CATEGORY.modulation, ENGINE.modulation, variant, basedOn,
      [hertz("Speed", 0.05, 10, 0.8, 1), percent("Depth", 50), percent(third, 50)], { stereo: true });
  mod("chorus", "Chorus", 0, "Stereo chorus", "Mix");
  mod("flanger", "Flanger", 1, "Stereo flanger", "Mix");
  mod("phaser", "Classic Phaser", 2, "6-stage phaser", "Mix"); // "Phaser" is the HD500X one (phaser_hd)
  mod("tremolo", "Tremolo", 3, "Tremolo, sine to square", "Shape");

  add("analog_delay", "Analog Delay", CATEGORY.delay, ENGINE.delay, 0, "Stereo delay, darker repeats",
    [millis("Time", 20, 2000, 500, 400), choice("Note", DELAY_NOTE_NAMES, 1), percent("Feedback", 35, 95), percent("Mix", 35),
     knobSpec("Tone", 0, 10, 6, 0, 0.1, UNIT.knob)],
    { timeKnob: 0, noteKnob: 1, noteBeats: DELAY_NOTE_BEATS, stereo: true, trails: true });
  add("room_reverb", "Room Reverb", CATEGORY.reverb, ENGINE.reverb, 0, "Freeverb room / hall",
    [percent("Size", 55), percent("Damp", 45), percent("Mix", 25), millis("Pre-Delay", 0, 500, 0, 120), choice("Note", REVERB_NOTE_NAMES, 0)],
    { timeKnob: 3, noteKnob: 4, noteBeats: REVERB_NOTE_BEATS, stereo: true, trails: true });

  // the fx/ engines, each with its own model list (same order as Source/DSP/Models.h)
  m.push(...DYNAMICS_MODELS);
  m.push(...MOD_MODELS);
  m.push(...FILTER_MODELS);
  m.push(...PITCH_MODELS);
  m.push(...EQ_MODELS);
  m.push(...DELAY_MODELS);
  m.push(...VERB_MODELS);
  m.push(...WAH_MODELS);
  m.push(...VOLUME_MODELS);
  return m;
})();
const MODEL_INDEX = new Map(MODELS.map((m, i) => [m.key, i]));
export const modelIndex = (key) => (typeof key === "number" ? clamp(key | 0, 0, MODELS.length - 1) : MODEL_INDEX.get(key) ?? 0);
export const modelInfo = (key) => MODELS[modelIndex(key)];

/** A slot holding `key` with every knob at its default. */
export function makeSlot(key, on) {
  const knobs = new Array(MAX_KNOBS).fill(0);
  modelInfo(key).knobs.forEach((k, i) => (knobs[i] = k.def));
  return { on, model: MODELS[modelIndex(key)].key, knobs };
}
/** Tempo sync: when the note knob is not "ms", the time knob follows the tempo. */
export function resolveTempo(slot, bpm) {
  const m = modelInfo(slot.model);
  if (!(bpm > 0)) return slot;
  for (const [timeKnob, noteKnob] of [[m.timeKnob, m.noteKnob], [m.timeKnob2, m.noteKnob2]]) {
    if (timeKnob < 0 || noteKnob < 0) continue;
    const note = Math.round(slot.knobs[noteKnob]);
    if (note > 0) {
      const spec = m.knobs[timeKnob];
      slot.knobs[timeKnob] = f32(clamp((60000 / bpm) * m.noteBeats[note], spec.min, spec.max));
    }
  }
  return slot;
}

// The amp block's own lists: amp models and speaker cabinets (with their microphones)
export const AMPS = AMP_MODELS;
export const CABS = CAB_MODELS;
export const MAX_AMP_KNOBS = 12;
export const MAX_CAB_KNOBS = 8;
export const DEFAULT_CAB_KEY = "cab_412_classic"; // the cab this project has always used
const AMP_INDEX = new Map(AMPS.map((m, i) => [m.key, i]));
const CAB_INDEX = new Map(CABS.map((m, i) => [m.key, i]));
/** Index of an amp / cab key, or -1 for none ("" or unknown). */
export const ampIndex = (key) => AMP_INDEX.get(key) ?? -1;
export const cabIndex = (key) => CAB_INDEX.get(key) ?? -1;

/** The cabinet an amp is usually played through (the HD500X selects it with the amp), or "" to keep the current one. */
const USUAL_CABS = {
  blackface_double_normal: "cab_212_blackface",
  blackface_double_vibrato: "cab_212_blackface",
  hiway_100: "cab_412_hiway",
  super_o: "cab_6x9_super_o",
  gibtone_185: "cab_112_field_coil",
  tweed_b_man_normal: "cab_410_tweed",
  tweed_b_man_bright: "cab_410_tweed",
  blackface_lux_normal: "cab_112_bf_lux",
  blackface_lux_vibrato: "cab_112_bf_lux",
  divide_9_15: "cab_112_celest_12h",
  phd_motorway: "cab_212_phd_ported",
  class_a_15: "cab_112_blue_bell",
  class_a_30_tb: "cab_212_silver_bell",
  brit_j_45_normal: "cab_412_greenback",
  brit_j_45_bright: "cab_412_greenback",
  plexi_lead_100_normal: "cab_412_blackback",
  plexi_lead_100_bright: "cab_412_blackback",
  brit_p_75_normal: "cab_412_greenback",
  brit_p_75_bright: "cab_412_greenback",
  brit_j_800: "cab_412_brit_t75",
  bomber_uber: "cab_412_uber",
  treadplate: "cab_412_tread_v30",
  angel_f_ball: "cab_412_xxl_v30",
  line6_elektrik: "cab_412_xxl_v30",
  solo_100_clean: "cab_412_tread_v30",
  solo_100_crunch: "cab_412_tread_v30",
  solo_100_od: "cab_412_tread_v30",
  line6_doom: "cab_412_uber",
  line6_epic: "cab_412_xxl_v30",
  flip_top: "cab_115_flip_top",
  pv_panama: "cab_412_tread_v30",
  mahadeva: "cab_412_tread_v30",
  brit_2204: "cab_412_brit_t75",
  line6_insane: "cab_412_uber",
  line6_big_bottom: "cab_412_uber",
  line6_variaced_plexi: "cab_412_greenback",
  line6_purge: "cab_412_xxl_v30",
  line6_aggro: "cab_412_tread_v30",
  line6_smash: "cab_412_brit_t75",
  line6_octone: "cab_412_greenback",
  jazz_rivet: "cab_212_jazz_rivet",
  small_tweed: "cab_108_small_tweed",
  mandarin_80: "cab_412_greenback",
  a30_fawn_nrm: "cab_212_silver_bell",
  a30_fawn_brt: "cab_212_silver_bell",
  black_panel_pete: "cab_212_blackface",
  line6_acoustic: "cab_212_jazz_rivet",
  svt_nrm: "cab_810_sv_beast",
  svt_brt: "cab_810_sv_beast",
  g_cougar_800: "cab_410_rhino",
};
export const defaultCabFor = (ampKey) => USUAL_CABS[ampKey] || "";

/** An amp block with this amp and cab ("" = none), every knob at its default. */
export function makeAmp(ampKey, cabKey, on = true) {
  const amp = ampIndex(ampKey), cab = cabIndex(cabKey);
  const ampKnobs = new Array(MAX_AMP_KNOBS).fill(0), cabKnobs = new Array(MAX_CAB_KNOBS).fill(0);
  if (amp >= 0) AMPS[amp].knobs.forEach((k, i) => (ampKnobs[i] = k.def));
  if (cab >= 0) CABS[cab].knobs.forEach((k, i) => (cabKnobs[i] = k.def));
  return { on, amp: amp >= 0 ? ampKey : "", cab: cab >= 0 ? cabKey : "", ampKnobs, cabKnobs };
}

/** The board as it first opens: Noise Gate > Screamer > cab > Chorus (off) > Analog Delay (off) > Room Reverb. */
export function defaultBoard() {
  const slots = [makeSlot("noise_gate", true), makeSlot("screamer", true), makeSlot("chorus", false),
                 makeSlot("analog_delay", false), makeSlot("room_reverb", true), makeSlot("empty", false),
                 makeSlot("empty", false), makeSlot("empty", false)];
  return { inputGainDb: 0, outputGainDb: 0, mute: false, amp: makeAmp("", DEFAULT_CAB_KEY, true), ampPosition: 2,
           eqOn: true, eq: defaultEq(), humMode: 0, denoise: 0, slots };
}
export const defaultParams = defaultBoard;

class Fade {
  constructor(fs) { this.amount = new Smoothed(0); this.amount.reset(fs, 0.03); }
  set(on) { this.amount.setTarget(on ? 1 : 0); }
  isOff() { return !this.amount.isSmoothing() && this.amount.current <= 0; }
  isFullyOn() { return !this.amount.isSmoothing() && this.amount.current >= 1; }
}

const KIND = { none: 0, first: 1, insert: 2, send: 3 };
const kindOf = (engine) =>
  engine === ENGINE.none ? KIND.none
  : engine <= ENGINE.reverb ? KIND.first
  : engine === ENGINE.delayFx || engine === ENGINE.reverbFx ? KIND.send : KIND.insert;

/** One FX slot: an instance of every engine; picking another model fades the old one out, then the new one in.
    Three kinds of engine (see Source/DSP/FxChain.h): the first engines, inserts (with a warm-up for
    modulation, filter and pitch) and sends (delay, reverb: wet only, their tails ring out when switched off). */
export class Slot {
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.gate = new NoiseGate(); this.gate.prepare(sampleRate);
    this.distortion = new Distortion(); this.distortion.prepare(sampleRate, maxBlock);
    this.modulation = new Modulation(); this.modulation.prepare(sampleRate);
    this.delay = new Delay(); this.delay.prepare(sampleRate);
    this.reverb = new ReverbFx(); this.reverb.prepare(sampleRate, maxBlock);

    // the fx/ engines, by ENGINE number. Unlike the C++ (which owns them all up front, so the audio thread never
    // allocates), each one is created the first time its slot needs it: a phone need not hold 8 of everything.
    this.maxBlock = maxBlock;
    this.refs = new Array(ENGINE.cabFx + 1).fill(null);
    this.makers = new Array(ENGINE.cabFx + 1).fill(null);
    this.makers[ENGINE.dynamicsFx] = () => new DynamicsFx();
    this.makers[ENGINE.modFx] = () => new ModFx();
    this.makers[ENGINE.filterFx] = () => new FilterFx();
    this.makers[ENGINE.pitchFx] = () => new PitchFx();
    this.makers[ENGINE.eqFx] = () => new EqFx();
    this.makers[ENGINE.delayFx] = () => new DelayFx();
    this.makers[ENGINE.reverbFx] = () => new VerbFx();
    this.makers[ENGINE.wahFx] = () => new WahFx();
    this.makers[ENGINE.volumeFx] = () => new VolumeFx();

    this.fade = new Fade(sampleRate);
    this.send = new Smoothed(0); this.send.reset(sampleRate, 0.03);
    this.mix = new Smoothed(0); this.mix.reset(sampleRate, 0.05);
    this.active = this.requested = 0;
    this.on = false; this.needsReset = false;
    this.warmupLeft = 0; this.running = false; this.silentSamples = 0;
    this.knobs = new Float32Array(MAX_KNOBS); this.pendingKnobs = new Float32Array(MAX_KNOBS);
    this.reset();
  }
  ref() {
    const engine = MODELS[this.active].engine;
    if (!this.refs[engine] && this.makers[engine]) {
      this.refs[engine] = this.makers[engine]();
      this.refs[engine].prepare(this.fs, this.maxBlock);
    }
    return this.refs[engine];
  }
  reset() {
    for (const e of [this.gate, this.distortion, this.modulation, this.delay, this.reverb]) e.reset();
    if (this.active !== this.requested) this.knobs.set(this.pendingKnobs);
    this.active = this.requested;
    this.needsReset = false;
    this.warmupLeft = 0;
    this.silentSamples = 0;
    this.configure(); this.resetEngine(); this.configure();

    const info = MODELS[this.active], kind = kindOf(info.engine);
    const audible = kind === KIND.send || (kind === KIND.first && info.trails) ? true : this.on;
    this.fade.amount.setCurrentAndTarget(kind !== KIND.none && audible ? 1 : 0);
    this.running = kind === KIND.send && this.on;
    this.send.setCurrentAndTarget(this.running ? 1 : 0);
    if (kind === KIND.send && this.ref()) this.mix.setCurrentAndTarget(f32(this.ref().getMix()));
  }
  setParameters(p) {
    this.requested = modelIndex(p.model);
    this.on = !!p.on;
    if (this.requested === this.active) this.knobs.set(p.knobs); else this.pendingKnobs.set(p.knobs);

    const activeKind = kindOf(MODELS[this.active].engine);
    if (activeKind === KIND.none || (activeKind === KIND.send && !this.running && this.requested !== this.active))
      this.fade.amount.setCurrentAndTarget(0);
    if (this.requested !== this.active && this.fade.isOff()) this.start();
    this.configure();

    const info = MODELS[this.active], same = this.requested === this.active;
    switch (kindOf(info.engine)) {
      case KIND.none: this.fade.set(false); break;
      case KIND.first: this.fade.set(same && (this.on || info.trails)); break;
      case KIND.send: this.fade.set(same); break;
      default:
        if (!(same && this.on)) this.fade.set(false);
        else if (!this.needsReset && this.warmupLeft === 0) this.fade.set(true);
    }
  }
  start() {
    this.active = this.requested;
    this.knobs.set(this.pendingKnobs);
    this.warmupLeft = 0;
    this.running = false;
    this.silentSamples = 0;
    if (kindOf(MODELS[this.active].engine) === KIND.first) {
      this.needsReset = false;
      this.configure(); this.resetEngine(); this.configure();
    } else this.needsReset = true;
  }
  configure() {
    const info = MODELS[this.active], k = this.knobs, enabled = this.on && this.active === this.requested;
    switch (info.engine) {
      case ENGINE.none: break;
      case ENGINE.gate: this.gate.setParameters(k[0], k[1]); break;
      case ENGINE.distortion: this.distortion.setModel(info.variant); this.distortion.setParameters(k); break;
      case ENGINE.modulation: this.modulation.setParameters(info.variant, k[0], f32(k[1] / 100), f32(k[2] / 100)); break;
      case ENGINE.delay: this.delay.setParameters(enabled, k[0], f32(k[2] / 100), f32(k[3] / 100), f32(k[4] / 10)); break;
      case ENGINE.reverb: this.reverb.setParameters(enabled, f32(k[0] / 100), f32(k[1] / 100), f32(k[2] / 100), k[3]); break;
      default: { const e = this.ref(); if (e) { e.setModel(info.variant); e.setParameters(k); } }
    }
  }
  resetEngine() {
    switch (MODELS[this.active].engine) {
      case ENGINE.none: break;
      case ENGINE.gate: this.gate.reset(); break;
      case ENGINE.distortion: this.distortion.reset(); break;
      case ENGINE.modulation: this.modulation.restart(); break;
      case ENGINE.delay: this.delay.reset(); break;
      case ENGINE.reverb: this.reverb.reset(); break;
      default: { const e = this.ref(); if (e) e.reset(); }
    }
  }
  process(left, right, n, scratch) {
    const info = MODELS[this.active];
    // the original chorus / flanger lines always hold the slot's recent input (see the C++)
    if (info.engine !== ENGINE.modulation || this.fade.isOff()) this.modulation.feed(left, right, n);
    switch (kindOf(info.engine)) {
      case KIND.none: break;
      case KIND.first: this.processFirst(left, right, n, scratch); break;
      case KIND.insert: if (this.ref()) this.processInsert(left, right, n, scratch); break;
      default: if (this.ref()) this.processSend(left, right, n, scratch);
    }
  }
  crossfade(left, right, n, scratch) {
    const a = this.fade.amount, dl = scratch.dryLeft, dr = scratch.dryRight;
    for (let i = 0; i < n; ++i) {
      const g = a.next();
      left[i] = dl[i] + g * (left[i] - dl[i]);
      right[i] = dr[i] + g * (right[i] - dr[i]);
    }
  }
  processFirst(left, right, n, scratch) {
    const info = MODELS[this.active];
    if (this.fade.isOff()) { this.needsReset = true; return; }
    if (this.needsReset) { if (info.engine !== ENGINE.modulation) this.resetEngine(); this.needsReset = false; }

    const full = this.fade.isFullyOn();
    if (!full) { scratch.dryLeft.set(left.subarray(0, n)); scratch.dryRight.set(right.subarray(0, n)); }
    if (info.stereo) {
      if (info.engine === ENGINE.modulation) this.modulation.process(left, right, n);
      else if (info.engine === ENGINE.delay) this.delay.process(left, right, n);
      else this.reverb.process(left, right, n);
    } else {
      const m = scratch.mid;
      for (let i = 0; i < n; ++i) m[i] = 0.5 * (left[i] + right[i]);
      if (info.engine === ENGINE.gate) this.gate.process(m, n); else this.distortion.process(m, n);
      for (let i = 0; i < n; ++i) left[i] = right[i] = m[i];
    }
    if (!full) this.crossfade(left, right, n, scratch);
  }
  processInsert(left, right, n, scratch) {
    const e = this.ref(), wanted = this.on && this.requested === this.active;
    if (!wanted && this.fade.isOff()) { this.needsReset = true; this.warmupLeft = 0; return; }

    if (this.needsReset) {
      e.reset();
      this.needsReset = false;
      const engine = MODELS[this.active].engine;
      const needsWarmup = engine === ENGINE.modFx || engine === ENGINE.filterFx || engine === ENGINE.pitchFx;
      this.warmupLeft = needsWarmup ? Math.floor(0.05 * this.fs) : 0;
      if (this.warmupLeft === 0) this.fade.set(wanted);
    }
    if (this.warmupLeft > 0) {
      const wl = scratch.wetLeft.subarray(0, n), wr = scratch.wetRight.subarray(0, n);
      wl.set(left.subarray(0, n)); wr.set(right.subarray(0, n));
      e.process(wl, wr, n);
      this.warmupLeft = Math.max(0, this.warmupLeft - n);
      if (this.warmupLeft === 0) this.fade.set(wanted);
      return;
    }
    if (this.fade.isFullyOn()) { e.process(left, right, n); return; }
    scratch.dryLeft.set(left.subarray(0, n)); scratch.dryRight.set(right.subarray(0, n));
    e.process(left, right, n);
    this.crossfade(left, right, n, scratch);
  }
  processSend(left, right, n, scratch) {
    const e = this.ref(), enabled = this.on && this.requested === this.active;
    if (!this.running) {
      if (!enabled) return;
      e.reset();
      this.running = true;
      this.needsReset = false;
      this.silentSamples = 0;
      this.send.setCurrentAndTarget(0);
      this.mix.setCurrentAndTarget(f32(e.getMix()));
    } else if (this.needsReset) { e.reset(); this.needsReset = false; }

    this.send.setTarget(enabled ? 1 : 0);
    this.mix.setTarget(f32(e.getMix()));

    const gain = scratch.gain, dl = scratch.dryLeft, dr = scratch.dryRight;
    const wl = scratch.wetLeft.subarray(0, n), wr = scratch.wetRight.subarray(0, n);
    for (let i = 0; i < n; ++i) {
      const g = this.send.next();
      gain[i] = g;
      dl[i] = left[i]; dr[i] = right[i];
      wl[i] = left[i] * g; wr[i] = right[i] * g;
    }
    if (e.processDry) e.processDry(dl.subarray(0, n), dr.subarray(0, n), n);
    e.process(wl, wr, n);

    const fullFade = this.fade.isFullyOn();
    let peak = 0;
    for (let i = 0; i < n; ++i) {
      const m = this.mix.next();
      const f = fullFade ? 1 : this.fade.amount.next();
      const dryGain = Math.min(1, 2 - 2 * m), wetGain = Math.min(1, 2 * m);
      const a = gain[i] * f, l = wl[i], r = wr[i];
      left[i] += a * (dl[i] * dryGain - left[i]) + f * wetGain * l;
      right[i] += a * (dr[i] * dryGain - right[i]) + f * wetGain * r;
      peak = Math.max(peak, Math.abs(l), Math.abs(r));
    }
    if (!enabled && !this.send.isSmoothing()) {
      this.silentSamples = peak < 1e-5 ? this.silentSamples + n : 0;
      if (this.silentSamples > e.getTailSeconds() * this.fs) this.running = false;
    } else this.silentSamples = 0;
  }
}

/** The amp block: amp model -> speaker cabinet + microphone (both mono; either can be "none").
    Switching it, picking another amp or cab, or moving it crossfades through the dry signal. */
class AmpBlock {
  prepare(sampleRate, maxBlock) {
    this.amp = new AmpFx(); this.amp.prepare(sampleRate, maxBlock);
    this.cab = new CabFx(); this.cab.prepare(sampleRate, maxBlock);
    this.fade = new Fade(sampleRate);
    const none = () => ({ on: true, amp: -1, cab: -1, ampKnobs: new Float32Array(MAX_AMP_KNOBS), cabKnobs: new Float32Array(MAX_CAB_KNOBS) });
    this.active = none(); this.requested = none();
    this.needsReset = true; this.held = false;
    this.reset();
  }
  static copy(from, to) {
    to.on = from.on; to.amp = from.amp; to.cab = from.cab;
    to.ampKnobs.set(from.ampKnobs); to.cabKnobs.set(from.cabKnobs);
  }
  reset() {
    AmpBlock.copy(this.requested, this.active);
    this.configure();
    this.amp.reset(); this.cab.reset();
    this.needsReset = false;
    const a = this.active;
    this.fade.amount.setCurrentAndTarget(a.on && !this.held && (a.amp >= 0 || a.cab >= 0) ? 1 : 0);
  }
  setParameters(p, hold) {
    const r = this.requested, a = this.active;
    r.on = !!p.on; r.amp = ampIndex(p.amp); r.cab = cabIndex(p.cab);
    r.ampKnobs.set(p.ampKnobs); r.cabKnobs.set(p.cabKnobs);
    this.held = hold;
    const same = () => r.amp === a.amp && r.cab === a.cab;
    if (same()) AmpBlock.copy(r, a);
    else if (this.fade.isOff()) { AmpBlock.copy(r, a); this.needsReset = true; }
    this.configure();
    this.fade.set(r.on && !hold && same() && (a.amp >= 0 || a.cab >= 0));
  }
  configure() {
    const a = this.active;
    if (a.amp >= 0) { this.amp.setModel(a.amp); this.amp.setParameters(a.ampKnobs); }
    if (a.cab >= 0) { this.cab.setModel(a.cab); this.cab.setParameters(a.cabKnobs); }
  }
  isOff() { return this.fade.isOff(); }
  process(left, right, n, scratch) {
    if (this.fade.isOff()) { this.needsReset = true; return; }
    if (this.needsReset) { this.amp.reset(); this.cab.reset(); this.needsReset = false; }
    const full = this.fade.isFullyOn(), dl = scratch.dryLeft, dr = scratch.dryRight;
    if (!full) { dl.set(left.subarray(0, n)); dr.set(right.subarray(0, n)); }
    if (this.active.amp >= 0) this.amp.process(left, right, n);
    if (this.active.cab >= 0) this.cab.process(left, right, n);
    if (!full) {
      for (let i = 0; i < n; ++i) {
        const g = this.fade.amount.next();
        left[i] = dl[i] + g * (left[i] - dl[i]);
        right[i] = dr[i] + g * (right[i] - dr[i]);
      }
    }
  }
}

/** input -> 8 slots, with the amp/cab block between them -> global EQ -> output. Mono in, stereo out. */
export class FxChain {
  prepare(sampleRate, maxBlock) {
    this.fs = sampleRate;
    this.maxBlock = Math.max(1, maxBlock);
    this.slots = Array.from({ length: NUM_SLOTS }, () => { const s = new Slot(); s.prepare(sampleRate, this.maxBlock); return s; });
    this.ampPosition = this.requestedAmpPosition = 2;
    this.ampBlock = new AmpBlock(); this.ampBlock.prepare(sampleRate, this.maxBlock);
    this.hum = new HumFilter(); this.hum.prepare(sampleRate);
    this.denoiser = new Denoiser(); this.denoiser.prepare(sampleRate);
    this.eqLeft = new Equalizer(); this.eqLeft.prepare(sampleRate);
    this.eqRight = new Equalizer(); this.eqRight.prepare(sampleRate);
    this.eqFade = new Fade(sampleRate);
    this.eqNeedsReset = true;
    this.inputGain = new Smoothed(1); this.outputGain = new Smoothed(1);
    this.inputGain.reset(sampleRate, 0.05); this.outputGain.reset(sampleRate, 0.05);
    this.mono = new Float32Array(this.maxBlock);
    const buffer = () => new Float32Array(this.maxBlock);
    this.scratch = { dryLeft: buffer(), dryRight: buffer(), mid: buffer(), wetLeft: buffer(), wetRight: buffer(), gain: buffer() };
    this.spareR = new Float32Array(this.maxBlock);
    this.analyzerTap = null;
    this.reset();
  }
  reset() {
    for (const s of this.slots) s.reset();
    this.ampPosition = this.requestedAmpPosition;
    this.ampBlock.reset();
    this.hum.reset();
    this.denoiser.reset();
    this.eqLeft.reset(); this.eqRight.reset();
    this.eqNeedsReset = false;
    this.eqFade.amount.setCurrentAndTarget(this.eqFade.amount.target);
    this.inputGain.setCurrentAndTarget(this.inputGain.target);
    this.outputGain.setCurrentAndTarget(this.outputGain.target);
    this.inputPeak = this.outputPeak = 0;
  }
  setParameters(p) {
    this.inputGain.setTarget(f32(dbToGain(p.inputGainDb)));
    this.outputGain.setTarget(p.mute ? 0 : f32(dbToGain(p.outputGainDb)));
    for (let s = 0; s < NUM_SLOTS; ++s) this.slots[s].setParameters(p.slots[s]);
    this.requestedAmpPosition = clamp(p.ampPosition | 0, 0, NUM_SLOTS);
    if (this.requestedAmpPosition !== this.ampPosition && this.ampBlock.isOff()) this.ampPosition = this.requestedAmpPosition;
    this.ampBlock.setParameters(p.amp, this.requestedAmpPosition !== this.ampPosition);
    this.eqFade.set(p.eqOn);
    this.eqLeft.setParameters(p.eq);
    this.eqRight.setParameters(p.eq);
    this.hum.setMode(p.humMode || 0);
    this.denoiser.setAmount(p.denoise || 0);
  }
  getActiveModel(slot) { return MODELS[this.slots[slot].active].key; }
  /** Mono in, stereo out. outRight may be null for a mono mix. */
  process(input, outLeft, outRight, n) {
    this.inputPeak = this.outputPeak = 0;
    for (let offset = 0; offset < n; offset += this.maxBlock) {
      const len = Math.min(this.maxBlock, n - offset);
      const L = outLeft.subarray(offset, offset + len);
      const R = outRight ? outRight.subarray(offset, offset + len) : this.spareR.subarray(0, len);
      this.processChunk(input.subarray(offset, offset + len), L, R, len);
      if (!outRight) for (let i = 0; i < len; ++i) L[i] = 0.5 * (L[i] + R[i]);
    }
  }
  processChunk(input, left, right, n) {
    const m = this.mono;
    for (let i = 0; i < n; ++i) {
      m[i] = input[i] * this.inputGain.next();
      this.inputPeak = Math.max(this.inputPeak, Math.abs(m[i]));
    }
    this.hum.process(m, n); // before anything can amplify the hum
    for (let i = 0; i < n; ++i) left[i] = right[i] = m[i];
    for (let s = 0; s < NUM_SLOTS; ++s) {
      if (s === this.ampPosition) this.ampBlock.process(left, right, n, this.scratch);
      this.slots[s].process(left, right, n, this.scratch);
    }
    if (this.ampPosition >= NUM_SLOTS) this.ampBlock.process(left, right, n, this.scratch);
    this.runEq(left, right, n);
    this.denoiser.process(left, right, n);
    if (this.analyzerTap) {
      const mid = this.scratch.mid;
      for (let i = 0; i < n; ++i) mid[i] = 0.5 * (left[i] + right[i]);
      this.analyzerTap(mid, n);
    }
    for (let i = 0; i < n; ++i) {
      const g = this.outputGain.next();
      left[i] = clamp(left[i] * g, -2, 2);
      right[i] = clamp(right[i] * g, -2, 2);
      this.outputPeak = Math.max(this.outputPeak, Math.abs(left[i]), Math.abs(right[i]));
    }
  }
  runEq(left, right, n) {
    if (this.eqFade.isOff()) { this.eqNeedsReset = true; return; }
    if (this.eqNeedsReset) { this.eqLeft.reset(); this.eqRight.reset(); this.eqNeedsReset = false; }
    if (this.eqFade.isFullyOn()) { this.eqLeft.process(left, n); this.eqRight.process(right, n); return; }
    const dl = this.scratch.dryLeft, dr = this.scratch.dryRight;
    dl.set(left.subarray(0, n)); dr.set(right.subarray(0, n));
    this.eqLeft.process(left, n); this.eqRight.process(right, n);
    for (let i = 0; i < n; ++i) {
      const a = this.eqFade.amount.next();
      left[i] = dl[i] + a * (left[i] - dl[i]);
      right[i] = dr[i] + a * (right[i] - dr[i]);
    }
  }
}

