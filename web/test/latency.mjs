// How much latency does each pedal add? Cross-correlates the dry test input with the C++ renders in
// test_output/reference (and with the JavaScript port) and reports the lag with the best match.
// Usage: node web/test/latency.mjs   (after running PedalTest)
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import * as dsp from "../src/dsp.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const ref = join(root, "test_output", "reference");
const readF32 = (f) => { const b = readFileSync(f); return new Float32Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)); };
const manifest = JSON.parse(readFileSync(join(ref, "manifest.json"), "utf8"));
const input = readF32(join(ref, "input.f32"));
const fs = manifest.sampleRate;

/** Lag (in samples, 0..maxLag) at which `out` best matches `inp`, from normalised cross-correlation. */
export function bestLag(inp, out, stride = 1, maxLag = 480, from = 4800, length = 96000) {
  let best = 0, bestScore = -Infinity;
  for (let lag = 0; lag <= maxLag; ++lag) {
    let xy = 0, yy = 0;
    for (let i = from; i < from + length; ++i) {
      const x = inp[i], y = out[(i + lag) * stride];
      xy += x * y; yy += y * y;
    }
    const score = xy / Math.sqrt(yy + 1e-20);
    if (score > bestScore) { bestScore = score; best = lag; }
  }
  return best;
}

let failures = 0;
console.log("== Latency added by each pedal (C++ renders) ==");
// The Octave Fuzz rectifies the signal (that's how it makes the octave up), so its output no longer
// correlates with the input; it runs through the same 4x oversampler as the other models.
const uncorrelated = /octave_fuzz/;
const names = manifest.cases.map((c) => c.name)
  .filter((n) => /^(00_clean|01_cab_only|drv_|eq_smile|overdrive_hot)/.test(n) && !uncorrelated.test(n));
for (const name of names) {
  const out = readF32(join(ref, `${name}.f32`)); // interleaved stereo
  const lag = bestLag(input, out, 2);
  const ms = (lag / fs) * 1000;
  console.log(`  ${name.padEnd(18)} ${String(lag).padStart(3)} samples = ${ms.toFixed(2)} ms`);
  if (ms > 1) { failures++; console.log(`  FAIL: ${name} adds more than 1 ms`); }
}

console.log("\n== Same, JavaScript version (each distortion model alone) ==");
for (const model of dsp.MODELS.filter((m) => m.category === dsp.CATEGORY.distortion && !uncorrelated.test(m.key))) {
  const p = { ...dsp.defaultBoard(), amp: dsp.makeAmp("", "", false), eqOn: false };
  p.slots = p.slots.map(() => dsp.makeSlot("empty", false));
  p.slots[0] = dsp.makeSlot(model.key, true);
  const chain = new dsp.FxChain();
  chain.prepare(fs, 256);
  chain.setParameters(p);
  chain.reset();
  const n = 110000, L = new Float32Array(n), R = new Float32Array(n);
  for (let pos = 0; pos < n; pos += 256) {
    chain.setParameters(p);
    chain.process(input.subarray(pos, pos + 256), L.subarray(pos, pos + 256), R.subarray(pos, pos + 256), Math.min(256, n - pos));
  }
  const lag = bestLag(input, L);
  console.log(`  ${model.key.padEnd(18)} ${String(lag).padStart(3)} samples = ${((lag / fs) * 1000).toFixed(2)} ms`);
  if (lag / fs > 0.001) { failures++; console.log(`  FAIL: ${model.key} adds more than 1 ms`); }
}

console.log(`\n${failures === 0 ? "ALL CHECKS PASSED" : failures + " CHECK(S) FAILED"}`);
process.exit(failures ? 1 : 0);
