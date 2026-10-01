// Checks the JavaScript DSP (web/src/dsp.js) against the C++ plugin.
//   1. Replays the C++ test input through FxChain with the exact same parameters and block size,
//      and compares every sample with the C++ render (test_output/reference, written by PedalTest).
//   2. Repeats the C++ checks that don't need reference data: tuner, plucked strings, analyser,
//      tap tempo, demo riff, file playback at a different sample rate.
//
// Usage (from the repo root, after running PedalTest):  node web/test/compare.mjs
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import * as dsp from "../src/dsp.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const refDir = join(root, "test_output", "reference");
let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.log(`  FAIL: ${what}`); } };
const readF32 = (file) => { const b = readFileSync(file); return new Float32Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)); };
const db = (x) => (x > 0 ? 20 * Math.log10(x) : -999);

// ---------------------------------------------------------------------------
console.log("== JavaScript vs C++ (same input, same parameters) ==");
const manifest = JSON.parse(readFileSync(join(refDir, "manifest.json"), "utf8"));
const input = readF32(join(refDir, "input.f32"));
const { sampleRate: fs, blockSize } = manifest;

// the model list must be the same as the plugin's (same order, knobs, ranges)
{
  const near = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(a), Math.abs(b));
  let mismatches = 0;
  const lists = [[manifest.models, dsp.MODELS], [manifest.amps, dsp.AMPS], [manifest.cabs, dsp.CABS]];
  for (const [wanted, have] of lists) wanted.forEach((want, i) => {
    const got = have[i];
    const same = got && ["key", "name", "category", "engine", "variant", "basedOn", "timeKnob", "noteKnob", "timeKnob2", "noteKnob2", "stereo", "trails"].every((f) => got[f] === want[f])
      && got.knobs.length === want.knobs.length
      && want.knobs.every((k, j) => {
        const g = got.knobs[j];
        return g.name === k.name && g.unit === k.unit && ["min", "max", "def", "centre", "step"].every((f) => near(g[f], k[f]))
          && JSON.stringify(g.choices) === JSON.stringify(k.choices);
      });
    if (!same) { mismatches++; console.log(`  model ${i} (${want.key}) differs`); }
  });
  console.log(`  model lists: ${manifest.models.length} models, ${manifest.amps.length} amps, ${manifest.cabs.length} cabs in the plugin; `
    + `${dsp.MODELS.length}, ${dsp.AMPS.length}, ${dsp.CABS.length} here; ${mismatches} different`);
  check(mismatches === 0 && lists.every(([wanted, have]) => wanted.length === have.length), "model lists match the plugin");
}

for (const c of manifest.cases) {
  const expected = readF32(join(refDir, `${c.name}.f32`));
  const n = expected.length / 2; // some cases only keep their first seconds
  const L = new Float32Array(n), R = new Float32Array(n);
  const chain = new dsp.FxChain();
  chain.prepare(fs, blockSize);
  chain.setParameters(c.params);
  chain.reset();

  // scripted cases change the parameters at block starts, the same way the C++ render does
  const paramsAt = (pos) => {
    let p = c.params;
    for (const s of c.schedule || []) if (pos >= s.atSample) p = s.params;
    return p;
  };

  const t0 = performance.now();
  for (let pos = 0; pos < n; pos += blockSize) {
    const len = Math.min(blockSize, n - pos);
    chain.setParameters(paramsAt(pos));
    chain.process(input.subarray(pos, pos + len), L.subarray(pos, pos + len), R.subarray(pos, pos + len), len);
  }
  const ms = performance.now() - t0;

  let sig = 0, err = 0, maxErr = 0;
  for (let i = 0; i < n; ++i) {
    for (const [got, want] of [[L[i], expected[2 * i]], [R[i], expected[2 * i + 1]]]) {
      const e = got - want;
      sig += want * want; err += e * e; maxErr = Math.max(maxErr, Math.abs(e));
    }
  }
  const relDb = db(Math.sqrt(err / Math.max(sig, 1e-30)));
  const realtime = (n / fs) * 1000 / ms;
  console.log(`  ${c.name.padEnd(28)} difference ${relDb.toFixed(1).padStart(7)} dB   max ${maxErr.toExponential(1)}   (${realtime.toFixed(0)}x realtime)`);
  check(relDb < -60, `${c.name} differs from the C++ version`);
}

// ---------------------------------------------------------------------------
console.log("\n== Tuner hears the plucked strings in tune ==");
for (const rate of [44100, 48000, 96000]) {
  let worst = 0;
  dsp.OPEN_STRINGS.forEach((hz, s) => {
    const string = new dsp.PluckedString();
    string.prepare(rate);
    string.pluck(hz, 0.35, 3.5, 0.75, dsp.makeRng(s + 1));
    const audio = new Float32Array(Math.floor(rate * 0.4));
    for (let i = 0; i < audio.length; ++i) audio[i] = string.next();
    const det = new dsp.PitchDetector();
    det.prepare(rate);
    det.pushSamples(audio.subarray(Math.floor(rate * 0.1)));
    const got = det.detect();
    worst = Math.max(worst, got > 0 ? Math.abs(1200 * Math.log2(got / hz)) : 9999);
  });
  console.log(`  worst @ ${rate} Hz: ${worst.toFixed(2)} cents`);
  check(worst < 2, `plucked strings in tune @ ${rate}`);
}

console.log("\n== Spectrum analyser ==");
for (const [hz, amp] of [[82.41, 0.5], [440, 0.5], [1000, 0.25], [5000, 0.1]]) {
  const a = new dsp.SpectrumAnalyzer();
  a.setSampleRate(48000);
  const sine = new Float32Array(8192).map((_, i) => amp * Math.sin((2 * Math.PI * hz * i) / 48000));
  a.push(sine);
  a.process();
  let peak = 0;
  for (let c = 1; c < a.columns.length; ++c) if (a.columns[c] > a.columns[peak]) peak = c;
  const peakHz = dsp.SpectrumAnalyzer.columnFrequency(peak);
  console.log(`  ${hz} Hz @ ${db(amp).toFixed(1)} dBFS -> ${peakHz.toFixed(1)} Hz, ${a.columns[peak].toFixed(1)} dBFS`);
  check(Math.abs(Math.log2(peakHz / hz)) < 0.05 && Math.abs(a.columns[peak] - db(amp)) < 1.6, `analyser at ${hz} Hz`);
}

console.log("\n== Tap tempo ==");
{
  const t = new dsp.TapTempo();
  let beat = 0;
  for (let i = 0; i < 5; ++i) beat = t.tap(1000 + 500 * i);
  console.log(`  taps every 500 ms -> ${beat} ms`);
  check(beat === 500, "tap tempo average");
  beat = t.tap(3000 + 3000);
  check(beat === 0, "tap tempo restarts after a pause");
}

console.log("\n== Worklet-style use: 128-sample blocks, parameters sent as a plain object ==");
{
  // same sequence as worklet.js: defaults at start-up, then the page's params with MUTE on
  const chain = new dsp.FxChain();
  chain.prepare(48000, 128);
  chain.setParameters(dsp.defaultParams());
  chain.reset();
  const riff = dsp.renderDemoRiff(48000);
  const params = structuredClone({ ...dsp.defaultParams(), mute: true });
  const L = new Float32Array(128), R = new Float32Array(128);
  let peakAfter = 0, inPeak = 0;
  for (let pos = 0; pos + 128 <= 96000; pos += 128) {
    chain.setParameters(params);
    chain.process(riff.subarray(pos, pos + 128), L, R, 128);
    if (pos > 48000) { peakAfter = Math.max(peakAfter, chain.outputPeak); inPeak = Math.max(inPeak, chain.inputPeak); }
  }
  console.log(`  MUTE on: input peak ${db(inPeak).toFixed(1)} dBFS, output peak ${db(peakAfter).toFixed(1)} dBFS`);
  check(peakAfter === 0, "MUTE must silence the output");
}

console.log("\n== Demo riff and file player ==");
{
  const riff = dsp.renderDemoRiff(48000);
  let peak = 0;
  for (const s of riff) peak = Math.max(peak, Math.abs(s));
  console.log(`  riff ${(riff.length / 48000).toFixed(1)} s, peak ${db(peak).toFixed(1)} dBFS`);
  check(Math.abs(db(peak) + 8) < 0.5, "demo riff level");

  const player = new dsp.TestSignalPlayer();
  player.prepare(48000);
  const tone = new Float32Array(44100 * 2).map((_, i) => 0.3 * Math.sin((2 * Math.PI * 196 * i) / 44100));
  player.setFile(tone, 44100);
  const out = new Float32Array(48000 * 3);
  for (let pos = 0; pos < out.length; pos += 128) player.process(out.subarray(pos, pos + 128), 128);
  const det = new dsp.PitchDetector();
  det.prepare(48000);
  det.pushSamples(out.subarray(Math.floor(48000 * 2.4)));
  const got = det.detect();
  console.log(`  44.1 kHz file on a 48 kHz context: ${got.toFixed(2)} Hz (expected 196.00)`);
  check(Math.abs(1200 * Math.log2(got / 196)) < 2, "file playback pitch");

  // STOP silences the riff (plucks still sound); PLAY starts again from the top
  const p2 = new dsp.TestSignalPlayer();
  p2.prepare(48000);
  p2.setSource("demo");
  const block = new Float32Array(4800);
  p2.process(block, 4800);
  p2.setPlaying(false);
  p2.process(block, 4800);
  const silent = block.every((x) => x === 0);
  p2.pluck(0);
  p2.process(block, 4800);
  const pluckSounds = block.some((x) => x !== 0);
  const p3 = new dsp.TestSignalPlayer();
  p3.prepare(48000);
  p3.setSource("demo");
  p3.process(block, 4800);
  p3.setPlaying(false);
  p3.setPlaying(true);
  const head = new Float32Array(256);
  p3.process(head, 256);
  const restarts = head[0] === riff[0] && head[255] === riff[255];
  console.log(`  STOP: silent ${silent}, plucks still sound ${pluckSounds}, PLAY restarts from the top ${restarts}`);
  check(silent && pluckSounds && restarts, "STOP / PLAY transport");
  p2.setSource("demo");
  check(p2.playing, "choosing DEMO RIFF starts it again");
}

console.log("\n== Chords ==");
{
  const expected = { C: [0, 4, 7], D: [2, 6, 9], E: [4, 8, 11], F: [5, 9, 0], G: [7, 11, 2], A: [9, 1, 4], Am: [9, 0, 4], Dm: [2, 5, 9], Em: [4, 7, 11] };
  const openMidi = [40, 45, 50, 55, 59, 64];
  let allRight = true, worst = 0;
  for (const chord of dsp.CHORDS) {
    const got = new Set();
    chord.frets.forEach((fret, s) => {
      if (fret < 0) return;
      const midi = openMidi[s] + fret;
      got.add(midi % 12);
      worst = Math.max(worst, Math.abs(1200 * Math.log2(dsp.fretHz(s, fret) / (440 * Math.pow(2, (midi - 69) / 12)))));
    });
    const want = new Set(expected[chord.name]);
    const right = got.size === want.size && [...got].every((p) => want.has(p));
    allRight &&= right;
    check(right, `chord shape ${chord.name}`);
  }
  console.log(`  all ${dsp.CHORDS.length} shapes have the right notes: ${allRight}, worst tuning ${worst.toFixed(2)} cents`);

  // strum G, then C: the low E (fretted G2 in the G chord, muted in C) must be damped
  const p = new dsp.TestSignalPlayer();
  p.prepare(48000);
  p.strum(4);
  const out = new Float32Array(48000);
  for (let pos = 0; pos < out.length; pos += 128) p.process(out.subarray(pos, pos + 128), 128);
  const peak = Math.max(...out.map(Math.abs));
  const lowEWasRinging = p.voices[0].string.active;
  p.strum(0);
  const more = new Float32Array(0.2 * 48000);
  for (let pos = 0; pos < more.length; pos += 128) p.process(more.subarray(pos, pos + 128), 128);
  console.log(`  strum G: peak ${db(peak).toFixed(1)} dBFS; low E damped when strumming C: ${lowEWasRinging && !p.voices[0].string.active}`);
  check(peak > 0.1 && peak < 1, "strummed chord level");
  check(lowEWasRinging && !p.voices[0].string.active, "muted strings are damped");
}

console.log("\n== Chord loop ==");
{
  const fs = 48000;
  const run = (p, seconds) => {
    const out = new Float32Array(Math.floor(fs * seconds));
    for (let pos = 0; pos < out.length; pos += 128) p.process(out.subarray(pos, Math.min(out.length, pos + 128)), Math.min(128, out.length - pos));
    return out;
  };
  const onsets = (x) => {
    const frame = Math.floor(0.002 * fs), e = new Float64Array(Math.floor(x.length / frame));
    for (let k = 0; k < e.length; ++k) for (let i = 1; i < frame; ++i) { const d = x[k * frame + i] - x[k * frame + i - 1]; e[k] += d * d; }
    const top = Math.max(...e), times = [];
    for (let k = 0; k < e.length; ++k) {
      let before = 1e-12;
      for (let j = Math.max(0, k - 5); j < k; ++j) before = Math.max(before, e[j]);
      if (e[k] > 3 * before && e[k] > 0.02 * top && (!times.length || k * 0.002 - times[times.length - 1] > 0.08)) times.push(k * 0.002);
    }
    return times;
  };
  // arpeggio: every eighth note; strum loop: one strum on every beat
  for (const [name, pattern, bpm, perBar, gridBeats] of [["arpeggio @ 120 BPM", 0, 120, 8, 0.5], ["arpeggio @ 75 BPM", 0, 75, 8, 0.5],
                                                         ["strum loop @ 120 BPM", 1, 120, 4, 1], ["strum loop @ 75 BPM", 1, 75, 4, 1]]) {
    const p = new dsp.TestSignalPlayer();
    p.prepare(fs);
    p.setChordPattern(pattern);
    p.setChordTempo(bpm);
    p.playChord(4);
    const t = onsets(run(p, (4 * 60 / bpm) * 2));
    const grid = (gridBeats * 60) / bpm;
    const worst = Math.max(...t.map((x) => Math.abs(x - grid * Math.round(x / grid))));
    console.log(`  ${name.padEnd(22)}${t.length} notes in 2 bars (expected ${2 * perBar}), worst timing error ${(worst * 1000).toFixed(1)} ms`);
    check(t.length === 2 * perBar && worst < 0.003, `chord loop ${name}`);
  }
  const p = new dsp.TestSignalPlayer();
  p.prepare(fs);
  p.setChordTempo(120);
  p.playChord(0);
  const a = run(p, 0.6);
  p.playChord(4);
  const b = run(p, 0.6);
  const all = new Float32Array(a.length + b.length); all.set(a); all.set(b, a.length);
  const onGrid = !onsets(all).some((x) => Math.abs(x - 0.6) < 0.05);
  p.stopChord();
  run(p, 0.3);
  const tail = Math.max(...run(p, 0.2).map(Math.abs));
  console.log(`  chord change waits for the next eighth: ${onGrid}, 0.3 s after STOP: ${db(tail).toFixed(1)} dBFS`);
  check(onGrid && tail < 1e-4, "chord change quantised, STOP mutes");

  // strum loop: a chord asked for between beats comes in on the next beat, not on the "and"
  const s = new dsp.TestSignalPlayer();
  s.prepare(fs);
  s.setChordPattern(1);
  s.setChordTempo(120);
  s.playChord(0);
  const sa = run(s, 0.6); // beats at 0, 0.5, 1.0 s; the "and" at 0.75 s
  s.playChord(4);
  const sb = run(s, 0.6);
  const sall = new Float32Array(sa.length + sb.length); sall.set(sa); sall.set(sb, sa.length);
  const st = onsets(sall);
  const nextBeat = st.some((x) => Math.abs(x - 1) < 0.01) && !st.some((x) => x > 0.55 && x < 0.95);
  console.log(`  strum loop: chord change comes in on the next beat: ${nextBeat}`);
  check(nextBeat, "strum loop: chord change waits for the next beat");
}

console.log(`\n${failures === 0 ? "ALL CHECKS PASSED" : failures + " CHECK(S) FAILED"}`);
process.exit(failures === 0 ? 0 : 1);
